/*
 * Copyright (C) 2026 OpenSIPS Solutions
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

#ifndef _REST_CLIENT_API_H_
#define _REST_CLIENT_API_H_

#include "../../str.h"
#include "../../async.h"
#include "../../sr_module.h"
#include "../../parser/msg_parser.h"

/* Currently supported HTTP verbs */
enum rest_client_method {
	REST_CLIENT_GET,
	REST_CLIENT_PUT,
	REST_CLIENT_POST
};
#define rest_client_method_str(_m) ( \
	(_m) == REST_CLIENT_GET ? "GET" : \
	(_m) == REST_CLIENT_POST ? "POST" : "PUT")

/* return codes, shared by the script functions and the module API */
#define RCL_CONTINUE             3 /* module API only: transfer still running */
#define RCL_OK_LOCKED            2
#define RCL_OK                   1
#define RCL_CONNECT_REFUSED     -1
#define RCL_CONNECT_TIMEOUT     -2
#define RCL_TRANSFER_TIMEOUT    -3
#define RCL_ALREADY_CONNECTING  -4
#define RCL_INTERNAL_ERR       -10

/*
 * Module-facing HTTP client API.
 *
 * This is the same cURL machinery that backs the rest_get()/rest_post()
 * script functions -- including the connection reuse, TLS settings, tracing
 * and the async multi-handle pool -- but with results returned as str/int
 * instead of being written into pvars, so that other modules can build
 * protocols on top of it.
 *
 * All returned buffers are pkg-allocated and owned by the caller.
 *
 * Buffer ownership rule: after any call that takes out_body/out_ctype, the
 * caller must pkg_free() whatever came back non-NULL, regardless of the
 * return code.  Passing NULL for an output means "not interested"; the
 * corresponding buffer is then freed internally.
 */

/*
 * sync_transfer - perform a blocking HTTP request
 *
 * @method:    REST_CLIENT_GET / _POST / _PUT
 * @msg:       current SIP message (used for tracing correlation; may be NULL)
 * @url:       HTTP(S) URL; need not be NUL-terminated
 * @body:      request body, or NULL for GET
 * @ctype:     request "Content-Type" value, or NULL
 * @hdrs:      extra request headers as CRLF- or LF-separated "Name: value"
 *             lines, or NULL
 * @out_body:  reply body, or NULL
 * @out_ctype: reply "Content-Type" value, or NULL
 * @out_code:  HTTP status code (0 if unavailable), or NULL
 *
 * Returns one of the RCL_* codes from rest_methods.h.
 */
typedef int (*rcl_sync_transfer_f)(enum rest_client_method method,
		struct sip_msg *msg, const str *url, const str *body, const str *ctype,
		const str *hdrs, str *out_body, str *out_ctype, int *out_code);

/*
 * start_async - launch a non-blocking HTTP request
 *
 * The TCP connect phase is still synchronous (a libcurl limitation); the
 * transfer itself is driven by the OpenSIPS reactor.
 *
 * @timeout_s: max duration for the whole operation, or 0 for the module
 *             default ("curl_timeout")
 * @handle:    out, opaque transfer handle
 * @out_fd:    out, one of:
 *               ASYNC_NO_IO - the request failed outright.  *handle is NULL,
 *                             there is nothing to resume or clean up.
 *               ASYNC_SYNC  - the transfer already completed.  The outputs
 *                             are filled in and *handle is NULL; do not
 *                             resume.
 *               >= 0        - a pollable fd.  *handle is valid and the caller
 *                             must arrange for resume_async() to be called
 *                             (and timeout_async() on expiry), exactly once.
 *
 * The out_* pointers are only used for the ASYNC_SYNC case, so they may point
 * at stack storage.  For the async case, supply storage to resume_async()
 * instead.
 */
typedef int (*rcl_start_async_f)(struct sip_msg *msg,
		enum rest_client_method method, const str *url, const str *body,
		const str *ctype, const str *hdrs, unsigned int timeout_s,
		void **handle, str *out_body, str *out_ctype, int *out_code,
		enum async_ret_code *out_fd);

/*
 * resume_async - collect the result of an async transfer
 *
 * Returns RCL_CONTINUE if the transfer has not finished yet, in which case
 * async_status has already been set to ASYNC_CONTINUE, the outputs are
 * untouched, @handle stays valid, and the caller's own resume function should
 * return 1 immediately so that it gets invoked again.
 *
 * On any other return code the transfer is complete, @handle has been freed
 * and must not be used again.
 *
 * The storage behind out_body/out_ctype/out_code must stay valid until this
 * call returns something other than RCL_CONTINUE.
 */
typedef enum async_ret_code (*rcl_resume_async_f)(int fd, struct sip_msg *msg,
		void *handle, str *out_body, str *out_ctype, int *out_code);

/*
 * timeout_async - abort an async transfer that ran out of time
 *
 * Frees @handle and returns RCL_TRANSFER_TIMEOUT.
 */
typedef enum async_ret_code (*rcl_timeout_async_f)(int fd, struct sip_msg *msg,
		void *handle);

/*
 * init_client_tls - use a tls_mgm client domain for the next transfer
 *
 * Must be called immediately before sync_transfer() or start_async().  The
 * setting applies to that one transfer only.  Requires the tls_mgm module.
 */
typedef int (*rcl_init_client_tls_f)(struct sip_msg *msg,
		const str *tls_client_dom);

typedef struct rest_client_api {
	rcl_sync_transfer_f   sync_transfer;
	rcl_start_async_f     start_async;
	rcl_resume_async_f    resume_async;
	rcl_timeout_async_f   timeout_async;
	rcl_init_client_tls_f init_client_tls;
} rest_client_api_t;

typedef int (*load_rest_client_f)(rest_client_api_t *api);

int load_rest_client(rest_client_api_t *api);

static inline int load_rest_client_api(rest_client_api_t *api)
{
	load_rest_client_f load_f;

	load_f = (load_rest_client_f)find_export("load_rest_client", 0);
	if (!load_f) {
		LM_ERR("cannot find 'load_rest_client'. "
		       "Is the rest_client module loaded?\n");
		return -1;
	}

	return load_f(api);
}

#endif /* _REST_CLIENT_API_H_ */
