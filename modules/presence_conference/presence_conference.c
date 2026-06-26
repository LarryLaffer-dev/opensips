/*
 * presence_conference module - Conference event package (RFC 4575)
 *
 * Serves the SIP conference event package ("Event: conference",
 * application/conference-info+xml) from the MMTel-AS conference focus
 * (RFC 4579 §4.4 / TS 24.147). The participant roster is sourced live from the
 * FreeSWITCH MRF mixer over the Event Socket Library (ESL), via the freeswitch
 * module's fs_api.
 *
 * VoLTE UEs subscribe to the conference state *in-dialog*, reusing the focus
 * INVITE dialog (RFC 4575 §3 / RFC 5057 multiple dialog usages): the SUBSCRIBE
 * carries the To-tag minted by the MRF when it answered the conference INVITE.
 * The OpenSIPS presence engine only serves subscriptions on dialogs it created
 * itself, so it cannot adopt such a foreign-owned dialog (it answers 481). This
 * module therefore implements the notifier directly on top of the dialog the
 * MMTel-AS already tracks for the conference (created in the cfg conf_create
 * route): conference_subscribe() accepts the in-dialog SUBSCRIBE and emits the
 * conference-info+xml NOTIFY using the dialog module's send_indialog_request
 * (which supplies the route set, target and CSeq from the stored dialog), and
 * conference_notify() pushes a fresh roster to every watcher when the mixer
 * membership changes (CUSTOM conference::maintenance).
 *
 * Copyright (C) 2026 ng-voice GmbH
 *
 * This file is part of opensips, a free SIP server.
 *
 * opensips is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * opensips is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <libxml/parser.h>
#include <libxml/tree.h>

#include "../../sr_module.h"
#include "../../dprint.h"
#include "../../mem/mem.h"
#include "../../mem/shm_mem.h"
#include "../../str.h"
#include "../../ut.h"
#include "../../locking.h"
#include "../../hash_func.h"
#include "../../data_lump_rpl.h"
#include "../../parser/msg_parser.h"
#include "../../parser/parse_expires.h"

#include "../dialog/dlg_load.h"
#include "../tm/tm_load.h"
#include "../freeswitch/fs_api.h"

#include "presence_conference.h"

static int mod_init(void);
static void mod_destroy(void);

static int conf_subscribe_f(struct sip_msg *msg, str *presentity);
static int conf_notify_f(struct sip_msg *msg, str *presentity);

/* module bindings */
struct dlg_binds dlg_api;
struct tm_binds  tmb;
struct fs_binds  fs_api;

/* modparams */
static char *mrf_esl_url_param;
static char *conf_user_prefix_param = "conf=";
static int  conf_default_expires = 3600;

static str mrf_esl_url      = {NULL, 0};
static str conf_user_prefix = {NULL, 0};

static const cmd_export_t cmds[] = {
	{"conference_subscribe", (cmd_function)conf_subscribe_f, {
		{CMD_PARAM_STR, 0, 0}, {0, 0, 0}},
		REQUEST_ROUTE},
	{"conference_notify", (cmd_function)conf_notify_f, {
		{CMD_PARAM_STR, 0, 0}, {0, 0, 0}},
		ALL_ROUTES},
	{0, 0, {{0, 0, 0}}, 0}
};

static const param_export_t params[] = {
	{"mrf_esl_url",      STR_PARAM, &mrf_esl_url_param},
	{"conf_user_prefix", STR_PARAM, &conf_user_prefix_param},
	{"default_expires",  INT_PARAM, &conf_default_expires},
	{0, 0, 0}
};

static const dep_export_t deps = {
	{ /* OpenSIPS module dependencies */
		{ MOD_TYPE_DEFAULT, "dialog",     DEP_ABORT },
		{ MOD_TYPE_DEFAULT, "tm",         DEP_ABORT },
		{ MOD_TYPE_DEFAULT, "freeswitch", DEP_ABORT },
		{ MOD_TYPE_NULL, NULL, 0 },
	},
	{ /* modparam dependencies */
		{ NULL, NULL },
	},
};

struct module_exports exports = {
	"presence_conference",  /* module name */
	MOD_TYPE_DEFAULT,       /* class of this module */
	MODULE_VERSION,
	DEFAULT_DLFLAGS,        /* dlopen flags */
	0,                      /* load function */
	&deps,                  /* OpenSIPS module dependencies */
	cmds,                   /* exported functions */
	0,                      /* exported async functions */
	params,                 /* module parameters */
	0,                      /* exported statistics */
	0,                      /* exported MI functions */
	0,                      /* exported pseudo-variables */
	0,                      /* exported transformations */
	0,                      /* extra processes */
	0,                      /* module pre-initialization function */
	mod_init,               /* module initialization function */
	0,                      /* response function */
	mod_destroy,            /* destroy function */
	0,                      /* per-child init function */
	0                       /* reload confirm function */
};

/* ---- small growable pkg string builder ----------------------------------- */

typedef struct {
	char *s;
	int len;
	int cap;
} cbuf_t;

static int cbuf_ensure(cbuf_t *b, int extra)
{
	int ncap;
	char *n;

	/* +1 so the buffer can always be NUL-terminated for strstr() */
	if (b->len + extra + 1 <= b->cap)
		return 0;

	ncap = b->cap ? b->cap : 1024;
	while (ncap < b->len + extra + 1)
		ncap *= 2;

	n = pkg_realloc(b->s, ncap);
	if (!n) {
		LM_ERR("oom growing body buffer to %d\n", ncap);
		return -1;
	}
	b->s = n;
	b->cap = ncap;
	return 0;
}

static int cbuf_append(cbuf_t *b, const char *s, int l)
{
	if (l <= 0)
		return 0;
	if (cbuf_ensure(b, l) < 0)
		return -1;
	memcpy(b->s + b->len, s, l);
	b->len += l;
	return 0;
}

static int cbuf_appends(cbuf_t *b, const char *s)
{
	return cbuf_append(b, s, strlen(s));
}

/* append XML-escaped text/attribute content */
static int cbuf_append_esc(cbuf_t *b, const char *s, int l)
{
	int i;

	if (!s)
		return 0;
	for (i = 0; i < l; i++) {
		switch (s[i]) {
		case '&':  if (cbuf_appends(b, "&amp;")  < 0) return -1; break;
		case '<':  if (cbuf_appends(b, "&lt;")   < 0) return -1; break;
		case '>':  if (cbuf_appends(b, "&gt;")   < 0) return -1; break;
		case '"':  if (cbuf_appends(b, "&quot;") < 0) return -1; break;
		case '\'': if (cbuf_appends(b, "&apos;") < 0) return -1; break;
		default:   if (cbuf_append(b, s + i, 1)  < 0) return -1; break;
		}
	}
	return 0;
}

/* ---- helpers ------------------------------------------------------------- */

/* Extract the conference id from a focus presentity URI of the form
 * "sip:<prefix><id>@host[;params]" (e.g. sip:conf=<id>@conf-factory...).
 * Returns the id substring (pointing into pres_uri) or an empty str if the URI
 * does not carry the configured conference user prefix. */
static str conf_id_from_uri(str *pres_uri)
{
	str id = {NULL, 0};
	char *p, *end, *at;

	if (!pres_uri || !pres_uri->s || pres_uri->len <= 0)
		return id;

	end = pres_uri->s + pres_uri->len;

	p = NULL;
	if (conf_user_prefix.len > 0) {
		char *c;
		for (c = pres_uri->s; c + conf_user_prefix.len <= end; c++) {
			if (strncmp(c, conf_user_prefix.s, conf_user_prefix.len) == 0) {
				p = c + conf_user_prefix.len;
				break;
			}
		}
		if (!p)
			return id;
	} else {
		/* no prefix: take the user part after an optional scheme */
		p = pres_uri->s;
		if (pres_uri->len > 4 && strncasecmp(p, "sip:", 4) == 0)
			p += 4;
	}

	/* id ends at the URI host separator '@' or a parameter ';' */
	id.s = p;
	for (at = p; at < end; at++) {
		if (*at == '@' || *at == ';')
			break;
	}
	id.len = at - p;
	return id;
}

/* Extract the host part of the focus presentity URI (between '@' and ';'),
 * used as the default domain for participant SIP URIs. */
static str conf_host_from_uri(str *pres_uri)
{
	str host = {NULL, 0};
	char *p, *end, *e;

	if (!pres_uri || !pres_uri->s)
		return host;

	end = pres_uri->s + pres_uri->len;
	p = q_memchr(pres_uri->s, '@', pres_uri->len);
	if (!p)
		return host;
	p++;
	host.s = p;
	for (e = p; e < end; e++)
		if (*e == ';' || *e == '>')
			break;
	host.len = e - p;
	return host;
}

/* Run "api conference xml_list" on the MRF ESL socket. On success *reply holds
 * an SHM string the caller must shm_free(); returns -1 on any failure (no
 * connection / command error), leaving *reply zeroed. */
static int conf_esl_list(str *reply)
{
	fs_evs *sock;
	str cmd = str_init("api conference xml_list");

	reply->s = NULL;
	reply->len = 0;

	if (mrf_esl_url.len == 0) {
		LM_ERR("mrf_esl_url not configured\n");
		return -1;
	}

	sock = fs_api.get_evs_by_url(&mrf_esl_url);
	if (!sock) {
		LM_ERR("failed to get ESL socket for %.*s\n",
			mrf_esl_url.len, mrf_esl_url.s);
		return -1;
	}

	if (!(sock->flags & FS_EVS_FL_CONNECTED)) {
		LM_WARN("MRF ESL not connected (%.*s); serving empty roster\n",
			mrf_esl_url.len, mrf_esl_url.s);
		fs_api.put_evs(sock);
		return -1;
	}

	if (fs_api.fs_esl(sock, &cmd, reply) != 0) {
		LM_ERR("ESL command failed: %.*s\n", cmd.len, cmd.s);
		reply->s = NULL;
		reply->len = 0;
		fs_api.put_evs(sock);
		return -1;
	}

	fs_api.put_evs(sock);
	return 0;
}

/* True when a participant requested CLIR / privacy and must be anonymized in
 * the roster (TS 24.147 / TS 24.607). FreeSWITCH renders restricted callers
 * with an empty or sentinel caller-id. */
static int conf_is_anonymous(const char *name, const char *number)
{
	if (!number || number[0] == '\0')
		return 1;
	if (!strcasecmp(number, "anonymous") || !strcasecmp(number, "unknown") ||
		!strcasecmp(number, "restricted") || !strcasecmp(number, "private") ||
		!strcasecmp(number, "unavailable"))
		return 1;
	if (name && (!strcasecmp(name, "anonymous") || !strcasecmp(name, "restricted")))
		return 1;
	return 0;
}

/* True if a FreeSWITCH conference name (e.g. "<id>", "<id>@default",
 * "<id>@domain") refers to the target conference id (compares the part before
 * the first '@'). */
static int conf_name_matches(const char *cname, str *id)
{
	const char *at;
	int blen;

	if (!cname || id->len == 0)
		return 0;
	at = strchr(cname, '@');
	blen = at ? (int)(at - cname) : (int)strlen(cname);
	return (blen == id->len && strncmp(cname, id->s, id->len) == 0);
}

/* text content of the first direct child element named "name" (xmlFree it) */
static char *xml_child_text(xmlNode *parent, const char *name)
{
	xmlNode *n;

	for (n = parent->children; n; n = n->next)
		if (n->type == XML_ELEMENT_NODE &&
			xmlStrcasecmp(n->name, (const xmlChar *)name) == 0)
			return (char *)xmlNodeGetContent(n);
	return NULL;
}

static int conf_hex_val(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* In-place percent-decode (RFC 3986). FreeSWITCH "conference xml_list"
 * URL-encodes caller_id_name / caller_id_number (e.g. "+4915888456043" ->
 * "%2B4915888456043"), so decode before using them to build SIP URIs and
 * display names; otherwise the roster carries malformed entities such as
 * sip:%2B...@host. */
static void conf_url_decode(char *s)
{
	char *r, *w;
	int hi, lo;

	if (!s)
		return;
	for (r = w = s; *r; ) {
		if (*r == '%' && (hi = conf_hex_val(r[1])) >= 0 &&
				(lo = conf_hex_val(r[2])) >= 0) {
			*w++ = (char)((hi << 4) | lo);
			r += 3;
		} else {
			*w++ = *r++;
		}
	}
	*w = '\0';
}

/* Emit one RFC 4575 <user> block for a FreeSWITCH <member> node. */
static int conf_emit_member(cbuf_t *b, xmlNode *member, str *host, int *count)
{
	char *name = xml_child_text(member, "caller_id_name");
	char *number = xml_child_text(member, "caller_id_number");
	cbuf_t ent = {0, 0, 0};
	int anon, rc = -1;

	/* FreeSWITCH URL-encodes these values in xml_list output */
	conf_url_decode(name);
	conf_url_decode(number);

	/* skip non-participant pseudo members (e.g. recorder) */
	if (!number && !name)
		goto done_ok;

	anon = conf_is_anonymous(name, number);

	if (anon) {
		if (cbuf_appends(&ent, "sip:anonymous@anonymous.invalid") < 0)
			goto out;
	} else if (strncasecmp(number, "sip:", 4) == 0 ||
				strncasecmp(number, "tel:", 4) == 0) {
		if (cbuf_appends(&ent, number) < 0)
			goto out;
	} else if (strchr(number, '@')) {
		if (cbuf_appends(&ent, "sip:") < 0 || cbuf_appends(&ent, number) < 0)
			goto out;
	} else {
		if (cbuf_appends(&ent, "sip:") < 0 || cbuf_appends(&ent, number) < 0)
			goto out;
		if (host->len > 0) {
			if (cbuf_appends(&ent, "@") < 0 ||
				cbuf_append(&ent, host->s, host->len) < 0)
				goto out;
		}
	}
	ent.s[ent.len] = '\0';

	if (cbuf_appends(b, "  <user entity=\"") < 0 ||
		cbuf_append_esc(b, ent.s, ent.len) < 0 ||
		cbuf_appends(b, "\" state=\"full\">\r\n") < 0)
		goto out;

	if (!anon && name && name[0]) {
		if (cbuf_appends(b, "   <display-text>") < 0 ||
			cbuf_append_esc(b, name, strlen(name)) < 0 ||
			cbuf_appends(b, "</display-text>\r\n") < 0)
			goto out;
	}

	if (cbuf_appends(b, "   <endpoint entity=\"") < 0 ||
		cbuf_append_esc(b, ent.s, ent.len) < 0 ||
		cbuf_appends(b, "\">\r\n") < 0 ||
		cbuf_appends(b, "    <status>connected</status>\r\n") < 0 ||
		cbuf_appends(b, "   </endpoint>\r\n") < 0 ||
		cbuf_appends(b, "  </user>\r\n") < 0)
		goto out;

	(*count)++;

done_ok:
	rc = 0;
out:
	if (ent.s)
		pkg_free(ent.s);
	if (name)
		xmlFree(name);
	if (number)
		xmlFree(number);
	return rc;
}

/* Recursively emit a <user> for every <member> found under a matched
 * conference subtree. Members are not nested, so do not descend into them. */
static int conf_emit_members(cbuf_t *b, xmlNode *node, str *host, int *count)
{
	xmlNode *n;

	for (n = node; n; n = n->next) {
		if (n->type != XML_ELEMENT_NODE)
			continue;
		if (xmlStrcasecmp(n->name, (const xmlChar *)"member") == 0) {
			if (conf_emit_member(b, n, host, count) < 0)
				return -1;
		} else if (n->children) {
			if (conf_emit_members(b, n->children, host, count) < 0)
				return -1;
		}
	}
	return 0;
}

/* Walk the FreeSWITCH "conference xml_list" tree, emitting members only for the
 * conference whose name matches the target id. */
static int conf_walk(cbuf_t *b, xmlNode *node, str *id, str *host, int *count)
{
	xmlNode *n;

	for (n = node; n; n = n->next) {
		if (n->type != XML_ELEMENT_NODE)
			continue;
		if (xmlStrcasecmp(n->name, (const xmlChar *)"conference") == 0) {
			char *cname = (char *)xmlGetProp(n, (const xmlChar *)"name");
			if (cname && conf_name_matches(cname, id)) {
				if (conf_emit_members(b, n->children, host, count) < 0) {
					xmlFree(cname);
					return -1;
				}
			}
			if (cname)
				xmlFree(cname);
		} else if (n->children) {
			if (conf_walk(b, n->children, id, host, count) < 0)
				return -1;
		}
	}
	return 0;
}

/* Build the full RFC 4575 conference-info+xml roster for a focus URI by
 * querying the FreeSWITCH mixer. Always returns a valid document (an empty
 * roster if the conference/mixer is unavailable) or NULL on hard OOM. The
 * version attribute is left as an 11-char placeholder, patched per NOTIFY by
 * conf_patch_version(). */
static str *conf_build_body(str *pres_uri)
{
	str reply = {NULL, 0};
	str entity, host, id;
	cbuf_t hdr = {0, 0, 0};
	cbuf_t users = {0, 0, 0};
	char ucnt[MAX_INT_LEN + 1];
	int count = 0, ulen;
	str *body = NULL;
	xmlDoc *doc = NULL;

	/* conference focus URI without parameters becomes the entity */
	entity = *pres_uri;
	{
		char *semi = q_memchr(pres_uri->s, ';', pres_uri->len);
		if (semi)
			entity.len = semi - pres_uri->s;
	}
	host = conf_host_from_uri(pres_uri);
	id = conf_id_from_uri(pres_uri);
	if (id.len == 0)
		LM_WARN("presentity %.*s is not a conference URI\n",
			pres_uri->len, pres_uri->s);

	if (id.len > 0 && conf_esl_list(&reply) == 0 && reply.s && reply.len > 0) {
		doc = xmlReadMemory(reply.s, reply.len, "conf.xml", NULL,
			XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_RECOVER);
		if (doc) {
			xmlNode *root = xmlDocGetRootElement(doc);
			if (root && conf_walk(&users, root, &id, &host, &count) < 0)
				LM_ERR("failed building roster users\n");
		} else {
			LM_DBG("no conference XML roster available\n");
		}
	}

	ulen = snprintf(ucnt, sizeof ucnt, "%d", count);

	if (cbuf_appends(&hdr, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n") < 0 ||
		cbuf_appends(&hdr, "<conference-info xmlns=\"" CONF_INFO_NS "\" entity=\"") < 0 ||
		cbuf_append_esc(&hdr, entity.s, entity.len) < 0 ||
		/* version placeholder MUST be followed by another attribute so the
		 * per-NOTIFY patch (conf_patch_version) never overruns into '>' */
		cbuf_appends(&hdr, "\" version=\"" CONF_VERSION_PLACEHOLDER "\" state=\"full\">\r\n") < 0 ||
		cbuf_appends(&hdr, " <conference-description/>\r\n") < 0 ||
		cbuf_appends(&hdr, " <conference-state>\r\n") < 0 ||
		cbuf_appends(&hdr, "  <user-count>") < 0 ||
		cbuf_append(&hdr, ucnt, ulen) < 0 ||
		cbuf_appends(&hdr, "</user-count>\r\n") < 0 ||
		cbuf_appends(&hdr, " </conference-state>\r\n") < 0 ||
		cbuf_appends(&hdr, " <users>\r\n") < 0)
		goto error;

	if (users.len && cbuf_append(&hdr, users.s, users.len) < 0)
		goto error;

	if (cbuf_appends(&hdr, " </users>\r\n") < 0 ||
		cbuf_appends(&hdr, "</conference-info>\r\n") < 0)
		goto error;

	body = pkg_malloc(sizeof *body);
	if (!body) {
		LM_ERR("oom\n");
		goto error;
	}
	hdr.s[hdr.len] = '\0';
	body->s = hdr.s;
	body->len = hdr.len;
	hdr.s = NULL; /* ownership transferred to body */

	LM_DBG("built conference-info for %.*s: %d participant(s)\n",
		pres_uri->len, pres_uri->s, count);

error:
	if (hdr.s)
		pkg_free(hdr.s);
	if (users.s)
		pkg_free(users.s);
	if (doc)
		xmlFreeDoc(doc);
	if (reply.s)
		shm_free(reply.s);
	return body;
}

/* Patch the monotonic "version" attribute placeholder in place (mirrors
 * presence_reginfo). The body buffer is NUL-terminated at body->len. */
static void conf_patch_version(str *body, int version)
{
	char *decl_end, *vstart;
	char vbuf[MAX_INT_LEN + 2]; /* digits + closing quote + NUL */
	int vlen;

	if (!body || !body->s || body->len < 40)
		return;

	/* skip the <?xml ...?> declaration so we do not match version="1.0" */
	decl_end = strchr(body->s, '>');
	vstart = strstr(decl_end ? decl_end + 1 : body->s, "version=");
	if (!vstart) {
		LM_ERR("version attribute not found in conference-info body\n");
		return;
	}
	vstart += 9; /* skip 'version="' */

	vlen = snprintf(vbuf, sizeof vbuf, "%d\"", version);
	if (vlen < 0 || vlen >= (int)sizeof vbuf) {
		LM_ERR("failed to render version %d\n", version);
		return;
	}
	/* overwrite the placeholder value; pad the remainder with spaces, which
	 * fall between the closing quote and the following state="full" attribute */
	memcpy(vstart, vbuf, vlen);
	memset(vstart + vlen, ' ', (MAX_INT_LEN + 2) - vlen);
}

/* ---- subscription registry ----------------------------------------------- */
/* Watchers are keyed by conference id and reference the focus dialog the
 * MMTel-AS tracks for the conference. The registry lets a mixer-membership
 * change (conference::maintenance) fan a fresh roster NOTIFY out to every
 * watcher of that conference. State lives in shared memory because subscribe
 * (SIP worker), notify (FreeSWITCH event route) and cleanup (dialog destroy)
 * run in different processes. */

#define CONF_SUB_HASH 64

typedef struct conf_sub {
	str conf_id;
	str callid;
	unsigned int h_entry;
	unsigned int h_id;
	int version;
	time_t expires;          /* absolute expiry */
	struct conf_sub *next;
} conf_sub_t;

struct conf_target {
	unsigned int h_entry;
	unsigned int h_id;
	int version;
	int expires_left;
};

struct conf_cb_key {
	str conf_id;
	str callid;
};

static conf_sub_t **conf_tbl;
static gen_lock_t  *conf_lock;

static unsigned int conf_bucket(str *id)
{
	return core_hash(id, NULL, CONF_SUB_HASH);
}

static int conf_shm_str(str *dst, str *src)
{
	dst->s = shm_malloc(src->len);
	if (!dst->s)
		return -1;
	memcpy(dst->s, src->s, src->len);
	dst->len = src->len;
	return 0;
}

static int conf_sub_match(conf_sub_t *e, str *conf_id, str *callid)
{
	return e->conf_id.len == conf_id->len &&
		memcmp(e->conf_id.s, conf_id->s, conf_id->len) == 0 &&
		e->callid.len == callid->len &&
		memcmp(e->callid.s, callid->s, callid->len) == 0;
}

/* Insert or refresh a watcher. Returns 1 if newly created, 0 if refreshed,
 * -1 on error. *out_version receives the (incremented) version to send. */
static int conf_sub_upsert(str *conf_id, str *callid, unsigned int h_entry,
		unsigned int h_id, int expires, int *out_version)
{
	unsigned int b = conf_bucket(conf_id);
	conf_sub_t *e;
	int created = 0;

	lock_get(conf_lock);
	for (e = conf_tbl[b]; e; e = e->next)
		if (conf_sub_match(e, conf_id, callid))
			break;

	if (!e) {
		e = shm_malloc(sizeof *e);
		if (!e) {
			lock_release(conf_lock);
			LM_ERR("oom for conference subscription\n");
			return -1;
		}
		memset(e, 0, sizeof *e);
		if (conf_shm_str(&e->conf_id, conf_id) < 0 ||
			conf_shm_str(&e->callid, callid) < 0) {
			if (e->conf_id.s)
				shm_free(e->conf_id.s);
			shm_free(e);
			lock_release(conf_lock);
			LM_ERR("oom for conference subscription keys\n");
			return -1;
		}
		e->h_entry = h_entry;
		e->h_id = h_id;
		e->version = 0;
		e->next = conf_tbl[b];
		conf_tbl[b] = e;
		created = 1;
	} else {
		/* a re-SUBSCRIBE may ride a fresh dialog instance after replication */
		e->h_entry = h_entry;
		e->h_id = h_id;
	}

	e->expires = time(NULL) + (expires > 0 ? expires : 0);
	e->version++;
	*out_version = e->version;
	lock_release(conf_lock);
	return created;
}

static void conf_sub_remove(str *conf_id, str *callid)
{
	unsigned int b = conf_bucket(conf_id);
	conf_sub_t *e, *prev = NULL;

	lock_get(conf_lock);
	for (e = conf_tbl[b]; e; prev = e, e = e->next) {
		if (conf_sub_match(e, conf_id, callid)) {
			if (prev)
				prev->next = e->next;
			else
				conf_tbl[b] = e->next;
			lock_release(conf_lock);
			shm_free(e->conf_id.s);
			shm_free(e->callid.s);
			shm_free(e);
			return;
		}
	}
	lock_release(conf_lock);
}

/* Snapshot all watchers of a conference into a pkg array, bumping each
 * watcher's version. Returns the count (0 if none, -1 on error). */
static int conf_sub_collect(str *conf_id, struct conf_target **out)
{
	unsigned int b = conf_bucket(conf_id);
	conf_sub_t *e;
	struct conf_target *arr;
	time_t now = time(NULL);
	int n = 0, i = 0;

	*out = NULL;

	lock_get(conf_lock);
	for (e = conf_tbl[b]; e; e = e->next)
		if (e->conf_id.len == conf_id->len &&
			memcmp(e->conf_id.s, conf_id->s, conf_id->len) == 0)
			n++;
	if (n == 0) {
		lock_release(conf_lock);
		return 0;
	}
	arr = pkg_malloc(n * sizeof *arr);
	if (!arr) {
		lock_release(conf_lock);
		LM_ERR("oom collecting conference watchers\n");
		return -1;
	}
	for (e = conf_tbl[b]; e && i < n; e = e->next) {
		if (e->conf_id.len == conf_id->len &&
			memcmp(e->conf_id.s, conf_id->s, conf_id->len) == 0) {
			e->version++;
			arr[i].h_entry = e->h_entry;
			arr[i].h_id = e->h_id;
			arr[i].version = e->version;
			arr[i].expires_left = (int)(e->expires - now);
			i++;
		}
	}
	lock_release(conf_lock);
	*out = arr;
	return i;
}

static void conf_cb_key_free(void *param)
{
	struct conf_cb_key *k = (struct conf_cb_key *)param;

	if (!k)
		return;
	if (k->conf_id.s)
		shm_free(k->conf_id.s);
	if (k->callid.s)
		shm_free(k->callid.s);
	shm_free(k);
}

/* Drop the watcher when its focus dialog is destroyed (BYE / timeout). */
static void conf_dlg_destroyed(struct dlg_cell *dlg, int type,
		struct dlg_cb_params *params)
{
	struct conf_cb_key *k;

	if (!params || !params->param)
		return;
	k = (struct conf_cb_key *)*params->param;
	if (!k)
		return;
	conf_sub_remove(&k->conf_id, &k->callid);
}

static int conf_register_destroy_cb(struct dlg_cell *dlg, str *conf_id, str *callid)
{
	struct conf_cb_key *k = shm_malloc(sizeof *k);

	if (!k) {
		LM_ERR("oom for dialog cb key\n");
		return -1;
	}
	memset(k, 0, sizeof *k);
	if (conf_shm_str(&k->conf_id, conf_id) < 0 ||
		conf_shm_str(&k->callid, callid) < 0) {
		conf_cb_key_free(k);
		return -1;
	}
	if (dlg_api.register_dlgcb(dlg, DLGCB_DESTROY, conf_dlg_destroyed, k,
			conf_cb_key_free) < 0) {
		LM_ERR("failed to register dialog destroy callback\n");
		conf_cb_key_free(k);
		return -1;
	}
	return 0;
}

/* ---- NOTIFY emission ----------------------------------------------------- */

/* Send one in-dialog conference-info NOTIFY toward the watcher (caller leg).
 * The dialog module supplies From/To/Call-ID/Contact/Route/CSeq from the
 * tracked focus dialog; we only add the event headers and the body. */
static int conf_send_notify(struct dlg_cell *dlg, str *body, int version,
		int expires, int terminated)
{
	static str met = str_init("NOTIFY");
	static str ct  = str_init(CONF_CONTENT_TYPE);
	char hbuf[160];
	str hdrs;

	conf_patch_version(body, version);

	if (terminated)
		hdrs.len = snprintf(hbuf, sizeof hbuf,
			"Event: " CONF_EVENT_NAME "\r\n"
			"Subscription-State: terminated;reason=timeout\r\n");
	else
		hdrs.len = snprintf(hbuf, sizeof hbuf,
			"Event: " CONF_EVENT_NAME "\r\n"
			"Subscription-State: active;expires=%d\r\n",
			expires > 0 ? expires : 0);
	if (hdrs.len < 0 || hdrs.len >= (int)sizeof hbuf) {
		LM_ERR("conference NOTIFY headers truncated\n");
		return -1;
	}
	hdrs.s = hbuf;

	if (dlg_api.send_indialog_request(dlg, &met, DLG_CALLER_LEG, body, &ct,
			&hdrs, NULL, NULL, NULL) < 0) {
		LM_ERR("failed to send conference NOTIFY\n");
		return -1;
	}
	LM_DBG("sent conference NOTIFY (version=%d, expires=%d, terminated=%d)\n",
		version, expires, terminated);
	return 0;
}

/* ---- exported script functions ------------------------------------------- */

/* conference_subscribe(presentity): accept an in-dialog conference event-package
 * SUBSCRIBE on the focus dialog and emit the initial / refresh roster NOTIFY.
 * The cfg must have already run loose_route() and t_newtran() (the latter
 * absorbs retransmissions). presentity is the conference focus URI ($ru). */
static int conf_subscribe_f(struct sip_msg *msg, str *presentity)
{
	struct dlg_cell *dlg;
	str conf_id, callid;
	str reason_ok = str_init("OK");
	str reason_481 = str_init("Subscription Does Not Exist");
	int expires, version = 0, created;
	int need_unref = 0;
	str *body;
	char *hdr_append = NULL;
	int hlen;

	if (!presentity || presentity->len == 0) {
		LM_ERR("empty presentity\n");
		return -1;
	}

	if (parse_headers(msg, HDR_CALLID_F | HDR_EXPIRES_F, 0) < 0 || !msg->callid) {
		LM_ERR("failed to parse Call-ID/Expires\n");
		return -1;
	}
	callid = msg->callid->body;

	expires = conf_default_expires;
	if (msg->expires && parse_expires(msg->expires) >= 0 && msg->expires->parsed)
		expires = ((exp_body_t *)msg->expires->parsed)->val;

	/* the focus dialog was created by conf_create on the conference INVITE;
	 * the in-dialog SUBSCRIBE matched it during loose_route() */
	dlg = dlg_api.get_dlg ? dlg_api.get_dlg() : NULL;
	if (!dlg) {
		dlg = dlg_api.get_dlg_by_callid(&callid, 1);
		need_unref = (dlg != NULL);
	}
	if (!dlg) {
		LM_ERR("no focus dialog for conference SUBSCRIBE %.*s (callid %.*s)\n",
			presentity->len, presentity->s, callid.len, callid.s);
		tmb.t_reply(msg, 481, &reason_481);
		return -1;
	}

	conf_id = conf_id_from_uri(presentity);
	if (conf_id.len == 0) {
		LM_ERR("presentity %.*s is not a conference URI\n",
			presentity->len, presentity->s);
		if (need_unref)
			dlg_api.dlg_unref(dlg, 1);
		tmb.t_reply(msg, 481, &reason_481);
		return -1;
	}

	/* 200 OK with Expires + Contact (in-dialog: the To-tag is preserved from
	 * the request). Mirrors presence' send_2XX_reply; add_lump_rpl copies. */
	hdr_append = pkg_malloc(9 /*"Expires: "*/ + MAX_INT_LEN + CRLF_LEN
		+ 10 /*"Contact: <"*/ + presentity->len + 1 /*">"*/ + CRLF_LEN);
	if (hdr_append) {
		hlen = snprintf(hdr_append, 9 + MAX_INT_LEN + CRLF_LEN + 10
				+ presentity->len + 1 + CRLF_LEN,
			"Expires: %d\r\nContact: <%.*s>\r\n",
			expires > 0 ? expires : 0, presentity->len, presentity->s);
		if (hlen > 0)
			add_lump_rpl(msg, hdr_append, hlen, LUMP_RPL_HDR);
		pkg_free(hdr_append);
	}
	tmb.t_reply(msg, 200, &reason_ok);

	body = conf_build_body(presentity);
	if (!body) {
		LM_ERR("failed to build conference roster for %.*s\n",
			presentity->len, presentity->s);
		if (need_unref)
			dlg_api.dlg_unref(dlg, 1);
		return -1;
	}

	if (expires <= 0) {
		/* un-SUBSCRIBE: final NOTIFY then drop the watcher */
		conf_send_notify(dlg, body, 0, 0, 1);
		conf_sub_remove(&conf_id, &callid);
	} else {
		created = conf_sub_upsert(&conf_id, &callid, dlg->h_entry, dlg->h_id,
			expires, &version);
		if (created == 1)
			conf_register_destroy_cb(dlg, &conf_id, &callid);
		conf_send_notify(dlg, body, version, expires, 0);
	}

	pkg_free(body->s);
	pkg_free(body);
	if (need_unref)
		dlg_api.dlg_unref(dlg, 1);
	return 1;
}

/* conference_notify(presentity): mixer membership changed; rebuild the roster
 * and push a NOTIFY to every watcher of the focus URI. Called from the
 * FreeSWITCH conference::maintenance event route. */
static int conf_notify_f(struct sip_msg *msg, str *presentity)
{
	str conf_id;
	str *body;
	struct conf_target *tg = NULL;
	int n, i;

	if (!presentity || presentity->len == 0) {
		LM_ERR("empty presentity\n");
		return -1;
	}
	conf_id = conf_id_from_uri(presentity);
	if (conf_id.len == 0) {
		LM_ERR("presentity %.*s is not a conference URI\n",
			presentity->len, presentity->s);
		return -1;
	}

	n = conf_sub_collect(&conf_id, &tg);
	if (n < 0)
		return -1;
	if (n == 0) {
		LM_DBG("no conference watchers for %.*s\n",
			presentity->len, presentity->s);
		return 1;
	}

	body = conf_build_body(presentity);
	if (!body) {
		LM_ERR("failed to build conference roster for %.*s\n",
			presentity->len, presentity->s);
		pkg_free(tg);
		return -1;
	}

	for (i = 0; i < n; i++) {
		struct dlg_cell *dlg = dlg_api.get_dlg_by_ids(tg[i].h_entry,
			tg[i].h_id, 1);
		if (!dlg) {
			LM_DBG("watcher dialog gone (h=%u/%u); skipping\n",
				tg[i].h_entry, tg[i].h_id);
			continue;
		}
		if (tg[i].expires_left <= 0)
			conf_send_notify(dlg, body, tg[i].version, 0, 1);
		else
			conf_send_notify(dlg, body, tg[i].version, tg[i].expires_left, 0);
		dlg_api.dlg_unref(dlg, 1);
	}

	pkg_free(body->s);
	pkg_free(body);
	pkg_free(tg);
	return 1;
}

/* ---- module lifecycle ---------------------------------------------------- */

static int mod_init(void)
{
	if (mrf_esl_url_param)
		init_str(&mrf_esl_url, mrf_esl_url_param);
	init_str(&conf_user_prefix, conf_user_prefix_param);

	if (mrf_esl_url.len == 0)
		LM_WARN("mrf_esl_url is not set; conference rosters will be empty\n");

	if (load_dlg_api(&dlg_api) != 0) {
		LM_ERR("cannot bind dialog API - is dialog loaded?\n");
		return -1;
	}
	if (!dlg_api.send_indialog_request || !dlg_api.get_dlg ||
		!dlg_api.get_dlg_by_callid || !dlg_api.get_dlg_by_ids ||
		!dlg_api.register_dlgcb || !dlg_api.dlg_unref) {
		LM_ERR("dialog API is missing required functions\n");
		return -1;
	}

	if (load_tm_api(&tmb) != 0) {
		LM_ERR("cannot bind tm API - is tm loaded?\n");
		return -1;
	}

	if (load_fs_api(&fs_api) != 0) {
		LM_ERR("cannot bind FreeSWITCH API - is freeswitch loaded?\n");
		return -1;
	}

	conf_tbl = shm_malloc(CONF_SUB_HASH * sizeof *conf_tbl);
	if (!conf_tbl) {
		LM_ERR("oom for conference subscription table\n");
		return -1;
	}
	memset(conf_tbl, 0, CONF_SUB_HASH * sizeof *conf_tbl);

	conf_lock = lock_alloc();
	if (!conf_lock || !lock_init(conf_lock)) {
		LM_ERR("failed to init conference subscription lock\n");
		return -1;
	}

	LM_INFO("presence_conference initialized (mrf_esl_url=%.*s)\n",
		mrf_esl_url.len, mrf_esl_url.s ? mrf_esl_url.s : "");

	return 0;
}

static void mod_destroy(void)
{
	int b;
	conf_sub_t *e, *next;

	if (conf_tbl) {
		for (b = 0; b < CONF_SUB_HASH; b++) {
			for (e = conf_tbl[b]; e; e = next) {
				next = e->next;
				if (e->conf_id.s)
					shm_free(e->conf_id.s);
				if (e->callid.s)
					shm_free(e->callid.s);
				shm_free(e);
			}
		}
		shm_free(conf_tbl);
		conf_tbl = NULL;
	}
	if (conf_lock) {
		lock_destroy(conf_lock);
		lock_dealloc(conf_lock);
		conf_lock = NULL;
	}
}
