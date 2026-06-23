/*
 * presence_conference module - Conference event package (RFC 4575)
 *
 * Serves the SIP conference event package ("Event: conference",
 * application/conference-info+xml) from the MMTel-AS conference focus
 * (RFC 4579 §4.4 / TS 24.147). The participant roster is sourced live from the
 * FreeSWITCH MRF mixer over the Event Socket Library (ESL), via the freeswitch
 * module's fs_api: the roster is built on demand for each NOTIFY and pushed to
 * existing watchers when the mixer membership changes.
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
#include <libxml/parser.h>
#include <libxml/tree.h>

#include "../../sr_module.h"
#include "../../dprint.h"
#include "../../mem/mem.h"
#include "../../mem/shm_mem.h"
#include "../../str.h"
#include "../../ut.h"
#include "../../parser/parse_event.h"

#include "../presence/bind_presence.h"
#include "../freeswitch/fs_api.h"

#include "presence_conference.h"

static int mod_init(void);

static str *conf_build_notify_body(str *pres_uri, str *subs_body,
		str *ct_type, int *suppress_notify);
static str *conf_body_setversion(subs_t *subs, str *body);
static void pkg_free_w(char *s);

static int conf_notify_f(struct sip_msg *msg, str *presentity);

/* presence + FreeSWITCH bindings */
presence_api_t pres_api;
struct fs_binds fs_api;
pres_ev_t *conf_event;

/* modparams */
static char *mrf_esl_url_param;
static char *conf_user_prefix_param = "conf=";
static int  conf_default_expires = 3600;

static str mrf_esl_url      = {NULL, 0};
static str conf_user_prefix = {NULL, 0};

static const cmd_export_t cmds[] = {
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
		{ MOD_TYPE_DEFAULT, "presence",   DEP_ABORT },
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
	0,                      /* destroy function */
	0,                      /* per-child init function */
	0                       /* reload confirm function */
};

static void pkg_free_w(char *s)
{
	if (s)
		pkg_free(s);
}

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
	int rc = -1;

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
	rc = 0;
	return rc;
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

/* Emit one RFC 4575 <user> block for a FreeSWITCH <member> node. */
static int conf_emit_member(cbuf_t *b, xmlNode *member, str *host, int *count)
{
	char *name = xml_child_text(member, "caller_id_name");
	char *number = xml_child_text(member, "caller_id_number");
	cbuf_t ent = {0, 0, 0};
	int anon, rc = -1;

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
 * roster if the conference/mixer is unavailable) or NULL on hard OOM. */
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
		 * per-watcher patch (aux_body_processing) never overruns into '>' */
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

/* presence build_notify_body callback: build the initial / refresh NOTIFY
 * roster on demand for the subscribing watcher. */
static str *conf_build_notify_body(str *pres_uri, str *subs_body,
		str *ct_type, int *suppress_notify)
{
	str *body;

	body = conf_build_body(pres_uri);
	if (!body)
		return NULL;

	if (pkg_str_dup(ct_type, _str(CONF_CONTENT_TYPE)) < 0) {
		LM_ERR("oom duplicating content-type\n");
		pkg_free(body->s);
		pkg_free(body);
		return NULL;
	}

	return body;
}

/* presence aux_body_processing callback: patch the monotonic per-watcher
 * "version" attribute in place (mirrors presence_reginfo). */
static str *conf_body_setversion(subs_t *subs, str *body)
{
	char *decl_end, *vstart;
	char version[MAX_INT_LEN + 2]; /* digits + closing quote + NUL */
	int version_len;

	if (!body || !body->s || body->len < 40)
		return NULL;

	/* the buffer is NUL-terminated; skip the <?xml ...?> declaration so we do
	 * not match its own version="1.0" */
	decl_end = strchr(body->s, '>');
	vstart = strstr(decl_end ? decl_end + 1 : body->s, "version=");
	if (!vstart) {
		LM_ERR("version attribute not found in conference-info body\n");
		return NULL;
	}
	vstart += 9; /* skip 'version="' */

	version_len = snprintf(version, MAX_INT_LEN + 2, "%d\"", subs->version);
	if (version_len < 0 || version_len >= MAX_INT_LEN + 2) {
		LM_ERR("failed to render version %d\n", subs->version);
		return NULL;
	}
	/* overwrite the placeholder value; pad the remainder with spaces, which
	 * fall between the closing quote and the following state="full" attribute */
	memcpy(vstart, version, version_len);
	memset(vstart + version_len, ' ', (MAX_INT_LEN + 2) - version_len);

	return NULL; /* body modified in place */
}

/* script function conference_notify(presentity): rebuild the roster from the
 * mixer and push a NOTIFY to every active watcher of the focus URI. Called
 * from the FreeSWITCH conference::maintenance event hook. */
static int conf_notify_f(struct sip_msg *msg, str *presentity)
{
	str *body;

	if (!presentity || presentity->len == 0) {
		LM_ERR("empty presentity\n");
		return -1;
	}

	body = conf_build_body(presentity);
	if (!body) {
		LM_ERR("failed to build conference roster for %.*s\n",
			presentity->len, presentity->s);
		return -1;
	}

	if (pres_api.notify_all_on_publish(presentity, conf_event, body) < 0)
		LM_ERR("failed to notify watchers of %.*s\n",
			presentity->len, presentity->s);

	pkg_free(body->s);
	pkg_free(body);
	return 1;
}

static int conf_add_event(void)
{
	pres_ev_t event;
	event_t ev;

	memset(&event, 0, sizeof event);
	event.name.s = CONF_EVENT_NAME;
	event.name.len = CONF_EVENT_NAME_LEN;
	event.content_type.s = CONF_CONTENT_TYPE;
	event.content_type.len = CONF_CONTENT_TYPE_LEN;
	event.default_expires = conf_default_expires;
	event.type = PUBL_TYPE;
	event.mandatory_body = 0;
	event.mandatory_timeout_notification = 0;
	event.build_notify_body = conf_build_notify_body;
	event.aux_body_processing = conf_body_setversion;
	event.aux_free_body = (free_body_t *)pkg_free_w;
	event.free_body = (free_body_t *)pkg_free_w;

	if (pres_api.add_event(&event) < 0) {
		LM_ERR("failed to register 'conference' event\n");
		return -1;
	}

	memset(&ev, 0, sizeof ev);
	ev.parsed = EVENT_CONFERENCE;
	ev.text = event.name;
	conf_event = pres_api.search_event(&ev);
	if (!conf_event) {
		LM_CRIT("failed to look up the registered 'conference' event\n");
		return -1;
	}

	return 0;
}

static int mod_init(void)
{
	bind_presence_t bind_presence;

	if (mrf_esl_url_param)
		init_str(&mrf_esl_url, mrf_esl_url_param);
	init_str(&conf_user_prefix, conf_user_prefix_param);

	if (mrf_esl_url.len == 0)
		LM_WARN("mrf_esl_url is not set; conference rosters will be empty\n");

	bind_presence = (bind_presence_t)find_export("bind_presence", 0);
	if (!bind_presence) {
		LM_ERR("cannot find presence API export\n");
		return -1;
	}
	if (bind_presence(&pres_api) < 0) {
		LM_ERR("cannot bind presence API\n");
		return -1;
	}
	if (!pres_api.add_event || !pres_api.search_event ||
		!pres_api.notify_all_on_publish) {
		LM_ERR("presence API is missing required functions\n");
		return -1;
	}

	if (load_fs_api(&fs_api) != 0) {
		LM_ERR("cannot bind FreeSWITCH API - is freeswitch loaded?\n");
		return -1;
	}

	if (conf_add_event() < 0)
		return -1;

	LM_INFO("presence_conference initialized (mrf_esl_url=%.*s)\n",
		mrf_esl_url.len, mrf_esl_url.s ? mrf_esl_url.s : "");

	return 0;
}
