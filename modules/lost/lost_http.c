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

#include "../../dprint.h"
#include "../../mem/mem.h"

#include "../rest_client/api.h"

#include "lost_http.h"
#include "lost_stats.h"

static rest_client_api_t rcl;

static void lost_stats_inc_request(const lost_http_req_t *req)
{
	if(req->kind == LOST_REQ_HELD)
		inc_stat(lost_stat_held_requests);
	else
		inc_stat(lost_stat_lost_requests);
}

static void lost_stats_inc_failure(const lost_http_req_t *req)
{
	if(req->kind == LOST_REQ_HELD)
		inc_stat(lost_stat_held_failures);
	else
		inc_stat(lost_stat_lost_failures);
}

int lost_http_init(void)
{
	if(load_rest_client_api(&rcl) != 0) {
		LM_ERR("failed to bind the rest_client API. "
			   "Is the rest_client module loaded?\n");
		return -1;
	}

	return 0;
}

/* Resolve the effective target of @req and apply its per-connection settings */
static int lost_http_prepare(
		lost_http_req_t *req, str **url, unsigned int *timeout)
{
	if(req->conn) {
		*url = &req->conn->url;
		*timeout = req->conn->timeout;

		if(req->conn->tls_dom.len > 0) {
			if(!rcl.init_client_tls) {
				LM_ERR("connection '%.*s' needs TLS client domain '%.*s', "
					   "but tls_mgm is not loaded\n",
						req->conn->name.len, req->conn->name.s,
						req->conn->tls_dom.len, req->conn->tls_dom.s);
				return -1;
			}

			if(rcl.init_client_tls(NULL, &req->conn->tls_dom) < 0) {
				LM_ERR("failed to select TLS client domain '%.*s'\n",
						req->conn->tls_dom.len, req->conn->tls_dom.s);
				return -1;
			}
		}
	} else {
		if(req->url.len == 0) {
			LM_ERR("no connection and no URL given\n");
			return -1;
		}

		*url = &req->url;
		*timeout = 0;
	}

	return 0;
}

/* GET when there is no body, POST otherwise -- as the LoST/HELD specs use */
static enum rest_client_method lost_http_method(const lost_http_req_t *req)
{
	return req->body.len > 0 ? REST_CLIENT_POST : REST_CLIENT_GET;
}

static void lost_http_log(const lost_http_req_t *req, const str *url, int rc)
{
	if(rc < 0) {
		LM_ERR("%s %.*s failed, rest_client code %d\n",
				req->body.len > 0 ? "POST" : "GET", url->len, url->s, rc);
		lost_stats_inc_failure(req);
		return;
	}

	LM_DBG("%s %.*s returned %d\n", req->body.len > 0 ? "POST" : "GET",
			url->len, url->s, req->code);

	if(req->code >= 300 || req->code < 100)
		lost_stats_inc_failure(req);
}

int lost_http_query(struct sip_msg *msg, lost_http_req_t *req)
{
	unsigned int timeout;
	str *url;
	int rc;

	req->code = 0;
	req->res = (str)STR_NULL;

	if(lost_http_prepare(req, &url, &timeout) != 0)
		return -1;

	lost_stats_inc_request(req);

	rc = rcl.sync_transfer(lost_http_method(req), msg, url,
			req->body.len > 0 ? &req->body : NULL,
			req->ctype.len > 0 ? &req->ctype : NULL,
			req->hdrs.len > 0 ? &req->hdrs : NULL, &req->res, NULL, &req->code);

	lost_http_log(req, url, rc);

	return rc < 0 ? rc : req->code;
}

int lost_http_start(struct sip_msg *msg, lost_http_req_t *req)
{
	enum async_ret_code out_fd;
	unsigned int timeout;
	str *url;
	int rc;

	req->code = 0;
	req->res = (str)STR_NULL;
	req->handle = NULL;

	if(lost_http_prepare(req, &url, &timeout) != 0)
		return ASYNC_NO_IO;

	req->timeout_s = timeout;

	lost_stats_inc_request(req);

	rc = rcl.start_async(msg, lost_http_method(req), url,
			req->body.len > 0 ? &req->body : NULL,
			req->ctype.len > 0 ? &req->ctype : NULL,
			req->hdrs.len > 0 ? &req->hdrs : NULL, timeout, &req->handle,
			&req->res, NULL, &req->code, &out_fd);

	if(out_fd == ASYNC_NO_IO) {
		lost_http_log(req, url, rc);
		return ASYNC_NO_IO;
	}

	if(out_fd == ASYNC_SYNC) {
		lost_http_log(req, url, rc);
		return ASYNC_SYNC;
	}

	LM_DBG("async %s %.*s started on fd %d\n",
			req->body.len > 0 ? "POST" : "GET", url->len, url->s, out_fd);

	return out_fd;
}

int lost_http_resume(int fd, struct sip_msg *msg, lost_http_req_t *req)
{
	int rc;

	rc = rcl.resume_async(
			fd, msg, req->handle, &req->res, NULL, &req->code);
	if(rc == RCL_CONTINUE)
		return 0;

	/* the handle was released by resume_async() */
	req->handle = NULL;

	if(rc < 0) {
		LM_ERR("async transfer failed, rest_client code %d\n", rc);
		lost_stats_inc_failure(req);
	} else if(req->code >= 300 || req->code < 100) {
		lost_stats_inc_failure(req);
	}

	LM_DBG("async transfer on fd %d returned %d\n", fd, req->code);

	return 1;
}

void lost_http_timeout(int fd, struct sip_msg *msg, lost_http_req_t *req)
{
	if(!req->handle)
		return;

	LM_ERR("async transfer on fd %d timed out\n", fd);
	lost_stats_inc_failure(req);

	rcl.timeout_async(fd, msg, req->handle);
	req->handle = NULL;
}

void lost_http_free(lost_http_req_t *req)
{
	if(req->res.s) {
		pkg_free(req->res.s);
		req->res = (str)STR_NULL;
	}
}
