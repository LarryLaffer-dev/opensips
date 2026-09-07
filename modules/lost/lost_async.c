/*
 * lost module -- async variants of the exported functions
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

/*!
 * \file
 * \brief lost :: async function variants
 * \ingroup lost
 *
 * An emergency call needs two serial HTTP round trips to external servers
 * (LIS, then ECRF).  Doing those synchronously blocks a SIP worker for as long
 * as the remote side takes to answer, which is the worst property of the
 * original Kamailio module.  These variants hand the transfer to the reactor
 * instead:
 *
 *   async(lost_held_query("lis", $var(pidf), $var(url), $var(err)), held_done);
 */

#include "../../async.h"
#include "../../dprint.h"
#include "../../mem/mem.h"

#include "functions.h"
#include "lost_async.h"

/* Which finish step to run once the transfer completes */
typedef int (*lost_finish_f)(struct sip_msg *msg, lost_ctx_t *ctx, int code);

struct lost_async_param {
	lost_ctx_t ctx;
	lost_finish_f finish;
};

static void lost_async_param_free(struct lost_async_param *p)
{
	lost_ctx_free(&p->ctx);
	pkg_free(p);
}

static enum async_ret_code lost_async_resume(
		int fd, struct sip_msg *msg, void *_param)
{
	struct lost_async_param *p = (struct lost_async_param *)_param;
	int rc;

	if(lost_http_resume(fd, msg, &p->ctx.req) == 0) {
		/* not finished; async_status is already ASYNC_CONTINUE */
		return 1;
	}

	rc = p->finish(msg, &p->ctx, p->ctx.req.code);
	lost_async_param_free(p);

	async_status = ASYNC_DONE;
	return rc;
}

static enum async_ret_code lost_async_timeout(
		int fd, struct sip_msg *msg, void *_param)
{
	struct lost_async_param *p = (struct lost_async_param *)_param;
	int rc;

	lost_http_timeout(fd, msg, &p->ctx.req);

	/* run the finish step with a "no response" code so that the output
	 * variables are always written and the script sees a clean failure */
	rc = p->finish(msg, &p->ctx, 0);
	lost_async_param_free(p);

	async_status = ASYNC_DONE;
	return rc;
}

/*
 * Allocate the async state and copy over the context the prepare step built on
 * the stack.  The context holds no self-references, so a shallow copy is fine.
 */
static struct lost_async_param *lost_async_param_new(
		const lost_ctx_t *ctx, lost_finish_f finish)
{
	struct lost_async_param *p;

	p = pkg_malloc(sizeof *p);
	if(p == NULL) {
		LM_ERR("no more pkg memory\n");
		return NULL;
	}

	p->ctx = *ctx;
	p->finish = finish;

	/* urlbuf is referenced by req.url; re-point it at the copy */
	if(ctx->req.url.s == &ctx->urlbuf[0])
		p->ctx.req.url.s = &p->ctx.urlbuf[0];

	return p;
}

/*
 * Common tail of all async variants: start the transfer and either suspend or,
 * if the exchange already completed, finish inline.
 */
static int lost_async_run(struct sip_msg *msg, async_ctx *actx, lost_ctx_t *ctx,
		lost_finish_f finish)
{
	struct lost_async_param *p;
	int fd, rc;

	p = lost_async_param_new(ctx, finish);
	if(p == NULL) {
		lost_ctx_free(ctx);
		async_status = ASYNC_NO_IO;
		return LOST_CLIENT_ERROR;
	}

	fd = lost_http_start(msg, &p->ctx.req);

	if(fd == ASYNC_NO_IO || fd == ASYNC_SYNC) {
		rc = p->finish(msg, &p->ctx, p->ctx.req.code);
		lost_async_param_free(p);

		async_status = fd;
		return rc;
	}

	actx->resume_f = lost_async_resume;
	actx->timeout_f = lost_async_timeout;
	actx->resume_param = p;
	if(p->ctx.req.timeout_s > 0)
		actx->timeout_s = p->ctx.req.timeout_s;

	async_status = fd;
	return 1;
}

int lost_async_held_function(struct sip_msg *_m, async_ctx *_actx, str *_con,
		pv_spec_t *_pidf, pv_spec_t *_url, pv_spec_t *_err, str *_id)
{
	lost_ctx_t ctx;
	int rc;

	memset(&ctx, 0, sizeof ctx);
	ctx.pv1 = _pidf;
	ctx.pv2 = _url;
	ctx.pv3 = _err;

	rc = lost_held_query_prepare(_m, &ctx, _con, _id);
	if(rc != 0) {
		lost_ctx_free(&ctx);
		async_status = ASYNC_NO_IO;
		return rc;
	}

	return lost_async_run(_m, _actx, &ctx, lost_held_query_finish);
}

int lost_async_held_dereference(struct sip_msg *_m, async_ctx *_actx, str *_url,
		pv_spec_t *_pidf, pv_spec_t *_err, str *_rtime, str *_rtype)
{
	lost_ctx_t ctx;
	int rc;

	memset(&ctx, 0, sizeof ctx);
	ctx.pv1 = _pidf;
	ctx.pv2 = _err;

	rc = lost_held_deref_prepare(_m, &ctx, _url, _rtime, _rtype);
	if(rc != 0) {
		lost_ctx_free(&ctx);
		async_status = ASYNC_NO_IO;
		return rc;
	}

	return lost_async_run(_m, _actx, &ctx, lost_held_deref_finish);
}

int lost_async_function(struct sip_msg *_m, async_ctx *_actx, str *_con,
		pv_spec_t *_uri, pv_spec_t *_name, pv_spec_t *_err, str *_pidf,
		str *_urn)
{
	lost_ctx_t ctx;
	int rc;

	memset(&ctx, 0, sizeof ctx);
	ctx.pv1 = _uri;
	ctx.pv2 = _name;
	ctx.pv3 = _err;

	rc = lost_query_prepare(_m, &ctx, _con, _pidf, _urn);
	if(rc != 0) {
		lost_ctx_free(&ctx);
		async_status = ASYNC_NO_IO;
		return rc;
	}

	return lost_async_run(_m, _actx, &ctx, lost_query_finish);
}
