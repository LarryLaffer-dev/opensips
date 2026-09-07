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

#ifndef LOST_ASYNC_H
#define LOST_ASYNC_H

#include "../../async.h"
#include "../../parser/msg_parser.h"
#include "../../pvar.h"
#include "../../str.h"

int lost_async_held_function(struct sip_msg *_m, async_ctx *_actx, str *_con,
		pv_spec_t *_pidf, pv_spec_t *_url, pv_spec_t *_err, str *_id);

int lost_async_held_dereference(struct sip_msg *_m, async_ctx *_actx, str *_url,
		pv_spec_t *_pidf, pv_spec_t *_err, str *_rtime, str *_rtype);

int lost_async_function(struct sip_msg *_m, async_ctx *_actx, str *_con,
		pv_spec_t *_uri, pv_spec_t *_name, pv_spec_t *_err, str *_pidf,
		str *_urn);

#endif /* LOST_ASYNC_H */
