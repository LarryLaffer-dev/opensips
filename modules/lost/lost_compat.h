/*
 * lost module -- Kamailio compatibility shims
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

/*
 * This module was ported from Kamailio's "lost" module.  A handful of small
 * conveniences that Kamailio provides in its core have no OpenSIPS
 * counterpart; they are defined here so that the bulk of the ported code can
 * stay close to upstream and remain easy to diff against it.
 */

#ifndef LOST_COMPAT_H
#define LOST_COMPAT_H

#include "../../dprint.h"
#include "../../str.h"

/* Kamailio: core/mem/pkg.h */
#ifndef PKG_MEM_ERROR
#define PKG_MEM_ERROR \
	LM_ERR("could not allocate private memory from pkg pool\n")
#endif

/* Kamailio: core/ut.h -- zero-safe wrapper, for logging maybe-NULL strings */
#ifndef ZSW
#define ZSW(_s) ((_s) ? (_s) : "")
#endif

/* Kamailio: core/str.h -- initialiser for a str over a string literal.
 * OpenSIPS spells this str_init(). */
#ifndef STR_STATIC_INIT
#define STR_STATIC_INIT(_v) {(_v), sizeof(_v) - 1}
#endif

#endif /* LOST_COMPAT_H */
