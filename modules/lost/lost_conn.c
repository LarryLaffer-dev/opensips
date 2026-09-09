/*
 * lost module -- named LoST/HELD server connections
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

#include <stdlib.h>

#include "../../dprint.h"
#include "../../mem/shm_mem.h"
#include "../../trim.h"
#include "../../ut.h"
#include "../../lib/csv.h"

#include "lost_compat.h"
#include "lost_conn.h"

/* Connections are built at mod_init() and only read afterwards, so a plain
 * shm-backed list needs no locking. */
static lost_conn_t *lost_connections;

static void lost_conn_free(lost_conn_t *conn)
{
	if(!conn)
		return;

	if(conn->name.s)
		shm_free(conn->name.s);
	if(conn->url.s)
		shm_free(conn->url.s);
	if(conn->tls_dom.s)
		shm_free(conn->tls_dom.s);

	shm_free(conn);
}

/* Split "key=value" and hand back both halves, trimmed */
static int lost_conn_split_kv(const str *in, str *key, str *val)
{
	char *eq;

	eq = q_memchr(in->s, '=', in->len);
	if(!eq) {
		LM_ERR("connection property '%.*s' is not a key=value pair\n", in->len,
				in->s);
		return -1;
	}

	key->s = in->s;
	key->len = eq - in->s;
	val->s = eq + 1;
	val->len = in->len - key->len - 1;

	trim(key);
	trim(val);

	if(key->len == 0 || val->len == 0) {
		LM_ERR("connection property '%.*s' has an empty key or value\n",
				in->len, in->s);
		return -1;
	}

	return 0;
}

int lost_conn_add(modparam_t type, void *val)
{
	str in, name, props, prop, key, pval;
	lost_conn_t *conn = NULL;
	csv_record *props_list = NULL, *it;
	char *sep;
	unsigned int timeout = 0;

	if(!val) {
		LM_ERR("null 'connection' parameter\n");
		return -1;
	}

	init_str(&in, (char *)val);

	/* name => properties */
	sep = str_strstr(&in, _str("=>"));
	if(!sep) {
		LM_ERR("bad 'connection' value '%.*s', expected "
			   "'name=>url=...[;timeout=...][;tls_dom=...]'\n",
				in.len, in.s);
		return -1;
	}

	name.s = in.s;
	name.len = sep - in.s;
	props.s = sep + 2;
	props.len = in.len - name.len - 2;

	trim(&name);
	trim(&props);

	if(name.len == 0) {
		LM_ERR("'connection' has an empty name\n");
		return -1;
	}

	if(lost_conn_get(&name)) {
		LM_ERR("duplicate 'connection' name '%.*s'\n", name.len, name.s);
		return -1;
	}

	conn = shm_malloc(sizeof *conn);
	if(!conn) {
		LM_ERR("oom\n");
		return -1;
	}
	memset(conn, 0, sizeof *conn);

	if(shm_nt_str_dup(&conn->name, &name) != 0) {
		LM_ERR("oom\n");
		goto error;
	}

	props_list = __parse_csv_record(&props, 0, ';');
	if(!props_list) {
		LM_ERR("failed to parse properties of connection '%.*s'\n", name.len,
				name.s);
		goto error;
	}

	for(it = props_list; it; it = it->next) {
		prop = it->s;
		trim(&prop);
		if(prop.len == 0)
			continue;

		if(lost_conn_split_kv(&prop, &key, &pval) != 0)
			goto error;

		if(str_casematch(&key, _str("url"))) {
			/* NUL-terminated: handed to libcurl as-is */
			if(shm_nt_str_dup(&conn->url, &pval) != 0) {
				LM_ERR("oom\n");
				goto error;
			}
		} else if(str_casematch(&key, _str("timeout"))) {
			if(str2int(&pval, &timeout) != 0) {
				LM_ERR("connection '%.*s': timeout '%.*s' is not a number\n",
						name.len, name.s, pval.len, pval.s);
				goto error;
			}
			conn->timeout = timeout;
		} else if(str_casematch(&key, _str("tls_dom"))) {
			if(shm_nt_str_dup(&conn->tls_dom, &pval) != 0) {
				LM_ERR("oom\n");
				goto error;
			}
		} else {
			LM_ERR("connection '%.*s': unknown property '%.*s'\n", name.len,
					name.s, key.len, key.s);
			goto error;
		}
	}

	if(conn->url.len == 0) {
		LM_ERR("connection '%.*s' has no 'url' property\n", name.len, name.s);
		goto error;
	}

	free_csv_record(props_list);

	conn->next = lost_connections;
	lost_connections = conn;

	LM_DBG("connection '%.*s': url '%.*s', timeout %u, tls_dom '%.*s'\n",
			conn->name.len, conn->name.s, conn->url.len, conn->url.s,
			conn->timeout, conn->tls_dom.len, ZSW(conn->tls_dom.s));

	return 0;

error:
	if(props_list)
		free_csv_record(props_list);
	lost_conn_free(conn);
	return -1;
}

lost_conn_t *lost_conn_get(const str *name)
{
	lost_conn_t *it;

	if(!name || name->len == 0)
		return NULL;

	for(it = lost_connections; it; it = it->next)
		if(str_match(&it->name, name))
			return it;

	return NULL;
}

void lost_conn_destroy(void)
{
	lost_conn_t *it, *next;

	for(it = lost_connections; it; it = next) {
		next = it->next;
		lost_conn_free(it);
	}

	lost_connections = NULL;
}
