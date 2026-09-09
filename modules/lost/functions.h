/*
 * lost module functions
 *
 * Copyright (C) 2019 Wolfgang Kampichler
 * DEC112, FREQUENTIS AG
 *
 * This file is part of opensips, a free SIP server.
 *
 * Derived from the Kamailio "lost" module.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

/*!
 * \file
 * \brief lost :: functions
 * \ingroup lost
 * Module: \ref lost
 */

#ifndef LOST_FUNCTIONS_H
#define LOST_FUNCTIONS_H

#include "../../config.h"
#include "../../parser/msg_parser.h"
#include "../../pvar.h"
#include "../../str.h"

#include "lost_http.h"
#include "utilities.h"

/*
 * Return codes.  OpenSIPS reads a function's return value as a truth value --
 * positive is true, negative is false -- so unlike the Kamailio original the
 * failure codes have to be negative for "if (lost_query(...))" to work.  The
 * magnitudes are kept, so $rc still carries the same detail.
 */
#define LOST_SUCCESS 200
#define LOST_CLIENT_ERROR -400
#define LOST_SERVER_ERROR -500

/*
 * Every exported function performs one HTTP exchange with a LIS or ECRF, so
 * each is split into a prepare step (assemble the request) and a finish step
 * (parse the reply, write the output variables).  The synchronous entry points
 * below run both back to back; the async ones in lost_async.c suspend in
 * between.  This context holds everything that has to survive that gap.
 */
typedef struct lost_ctx {
	lost_http_req_t req; /* the exchange itself */

	/* output variables, in the order of the script parameters */
	pv_spec_t *pv1;
	pv_spec_t *pv2;
	pv_spec_t *pv3;

	/* lost_query() only: needed to re-assemble a findService request when
	 * the mapping server answers with a redirect */
	p_lost_loc_t loc;
	str urnbuf;
	char urlbuf[MAX_URI_SIZE];
} lost_ctx_t;

void lost_ctx_free(lost_ctx_t *ctx);

char *lost_held_type(char *, int *, int *);

/*
 * prepare: 0 on success, a LOST_* code on failure (nothing to finish)
 * finish:  a LOST_* code; consumes and releases @ctx contents
 */
int lost_held_query_prepare(struct sip_msg *_m, lost_ctx_t *ctx, str *_con,
		str *_id);
int lost_held_query_finish(struct sip_msg *_m, lost_ctx_t *ctx, int _code);

int lost_held_deref_prepare(struct sip_msg *_m, lost_ctx_t *ctx, str *_url,
		str *_rtime, str *_rtype);
int lost_held_deref_finish(struct sip_msg *_m, lost_ctx_t *ctx, int _code);

int lost_query_prepare(struct sip_msg *_m, lost_ctx_t *ctx, str *_con,
		str *_pidf, str *_urn);
int lost_query_finish(struct sip_msg *_m, lost_ctx_t *ctx, int _code);

int lost_held_dereference(struct sip_msg *_m, str *_url, pv_spec_t *_pidf,
		pv_spec_t *_err, str *_rtime, str *_rtype);

int lost_held_function(struct sip_msg *_m, str *_con, pv_spec_t *_pidf,
		pv_spec_t *_url, pv_spec_t *_err, str *_id);

int lost_function(struct sip_msg *_m, str *_con, pv_spec_t *_uri,
		pv_spec_t *_name, pv_spec_t *_err, str *_pidf, str *_urn);

#endif
