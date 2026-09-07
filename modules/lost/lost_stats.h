/*
 * lost module -- statistics
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

#ifndef LOST_STATS_H
#define LOST_STATS_H

#include "../../statistics.h"

/*
 * Emergency call routing depends on two external services, so the health of
 * the HELD and LoST legs is worth monitoring separately -- a rise in
 * lost_failures with flat held_failures points at the ECRF rather than
 * the LIS.
 */

extern stat_var *lost_stat_held_requests;
extern stat_var *lost_stat_held_failures;
extern stat_var *lost_stat_lost_requests;
extern stat_var *lost_stat_lost_failures;

extern const stat_export_t lost_stats[];

#endif /* LOST_STATS_H */
