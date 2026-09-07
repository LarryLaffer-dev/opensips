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

#ifndef LOST_CONN_H
#define LOST_CONN_H

#include "../../str.h"
#include "../../sr_module.h"

/*
 * Kamailio's lost module addressed LIS/ECRF endpoints by the "httpcon" names
 * defined by its http_client module.  OpenSIPS' rest_client has no equivalent
 * concept, so the named endpoints are defined on this module instead:
 *
 *   modparam("lost", "connection",
 *            "lis=>url=https://lis.example.org/held;timeout=5;tls_dom=lost")
 *
 * The script then refers to a connection by name, exactly as before:
 *
 *   lost_held_query("lis", $var(pidf), $var(url), $var(err));
 */

typedef struct lost_conn {
	str name;
	str url;
	str tls_dom;          /* tls_mgm client domain, or {NULL,0} for none */
	unsigned int timeout; /* seconds; 0 = rest_client default */
	struct lost_conn *next;
} lost_conn_t;

/* "connection" modparam handler */
int lost_conn_add(modparam_t type, void *val);

/* Look up a connection by name; NULL if there is no such connection */
lost_conn_t *lost_conn_get(const str *name);

void lost_conn_destroy(void);

#endif /* LOST_CONN_H */
