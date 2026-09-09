/*
 * lost module -- HTTP transport over the rest_client API
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

#ifndef LOST_HTTP_H
#define LOST_HTTP_H

#include "../../async.h"
#include "../../parser/msg_parser.h"
#include "../../str.h"

#include "lost_conn.h"

/*
 * Kamailio's lost module talked to LIS/ECRF servers through its http_client
 * module.  This is the equivalent glue over OpenSIPS' rest_client, which also
 * gives us non-blocking transfers.
 */

/* Which external service an exchange targets; drives the statistics */
typedef enum lost_req_kind {
	LOST_REQ_HELD, /* LIS, RFC 6155 / RFC 6753 */
	LOST_REQ_LOST, /* ECRF, RFC 5222 */
} lost_req_kind_t;

/* A single HTTP request/response exchange */
typedef struct lost_http_req {
	/* request -- filled in by the caller */
	lost_req_kind_t kind;
	lost_conn_t *conn; /* named connection, or NULL to use .url */
	str url;           /* target URL when .conn is NULL */
	str body;          /* request body; {NULL,0} issues a GET */
	str ctype;         /* request Content-Type */
	str hdrs;          /* extra request headers, LF-separated */

	/* response */
	int code; /* HTTP status code, 0 if the transfer never completed */
	str res;  /* reply body, pkg-allocated; free with lost_http_free() */

	/* internal: in-flight rest_client transfer */
	void *handle;
	unsigned int timeout_s; /* effective timeout, set by lost_http_start() */
} lost_http_req_t;

/* Bind the rest_client API; call from mod_init() */
int lost_http_init(void);

/*
 * Perform @req synchronously.  Returns the HTTP status code, or a negative
 * value if the exchange never produced a response.  @req->res always has to
 * be released with lost_http_free().
 */
int lost_http_query(struct sip_msg *msg, lost_http_req_t *req);

/*
 * Start @req without blocking.
 *
 * Returns the fd to wait on, or:
 *   ASYNC_SYNC  - the exchange already completed; @req holds the result
 *   ASYNC_NO_IO - the request failed to start; nothing to resume
 */
int lost_http_start(struct sip_msg *msg, lost_http_req_t *req);

/*
 * Collect the result of a request started with lost_http_start().
 *
 * Returns 1 when the exchange is complete (@req holds the result), or 0 if it
 * is still in progress, in which case async_status has already been set to
 * ASYNC_CONTINUE and the caller must return 1 to be resumed again.
 */
int lost_http_resume(int fd, struct sip_msg *msg, lost_http_req_t *req);

/* Abandon a request started with lost_http_start() after a timeout */
void lost_http_timeout(int fd, struct sip_msg *msg, lost_http_req_t *req);

/* Release the reply buffer of @req */
void lost_http_free(lost_http_req_t *req);

#endif /* LOST_HTTP_H */
