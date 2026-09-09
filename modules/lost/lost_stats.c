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

#include "lost_stats.h"

stat_var *lost_stat_held_requests;
stat_var *lost_stat_held_failures;
stat_var *lost_stat_lost_requests;
stat_var *lost_stat_lost_failures;

const stat_export_t lost_stats[] = {
	{"held_requests", STAT_NO_RESET, &lost_stat_held_requests},
	{"held_failures", STAT_NO_RESET, &lost_stat_held_failures},
	{"lost_requests", STAT_NO_RESET, &lost_stat_lost_requests},
	{"lost_failures", STAT_NO_RESET, &lost_stat_lost_failures},
	{0, 0, 0}
};
