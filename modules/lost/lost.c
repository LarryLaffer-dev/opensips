/*
 * lost module
 *
 * Copyright (C) 2019 Wolfgang Kampichler
 * DEC112, FREQUENTIS AG
 * Copyright (C) 2026 volte.io (OpenSIPS port)
 *
 * This file is part of opensips, a free SIP server.
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
 * \brief lost :: module interface
 * \ingroup lost
 */

#include <stdio.h>
#include <stdlib.h>

#include "../../sr_module.h"
#include "../../dprint.h"
#include "../../str.h"
#include "../../ut.h"

#include "functions.h"
#include "lost_async.h"
#include "lost_conn.h"
#include "lost_http.h"
#include "lost_stats.h"

/* module parameters, referenced as externs from functions.c/utilities.c */
int lost_recursion = 1;	  /* LoST server may recurse */
int lost_profile = 0;	  /* location profile preference */
int lost_geoloc_type = 0; /* Geolocation header pref: any/value/reference */
int lost_geoloc_order = 0; /* 0: first, 1: last locationValue */
int lost_geoloc_3d = 0;	  /* include altitude */
int lost_verbose = 0;	  /* print LoST response details */
int held_resp_time = 0;	  /* HELD responseTime, ms; 0: emergencyRouting */
int held_exact_type = 0;  /* HELD exact/any locationType match */
int held_post_req = 0;	  /* dereference location using HELD POST */
str held_loc_type = STR_NULL; /* HELD locationType request */

static int mod_init(void);
static int child_init(int rank);
static void mod_destroy(void);

static const dep_export_t deps = {
	{/* OpenSIPS module dependencies */
			{MOD_TYPE_DEFAULT, "rest_client", DEP_ABORT},
			{MOD_TYPE_NULL, NULL, 0}},
	{/* modparam dependencies */
			{NULL, NULL}}};

static const cmd_export_t cmds[] = {
	{"lost_query", (cmd_function)lost_function,
		{{CMD_PARAM_STR, 0, 0},				  /* connection */
		 {CMD_PARAM_VAR, 0, 0},				  /* out: uri */
		 {CMD_PARAM_VAR, 0, 0},				  /* out: name */
		 {CMD_PARAM_VAR, 0, 0},				  /* out: error */
		 {CMD_PARAM_STR | CMD_PARAM_OPT, 0, 0}, /* pidf-lo */
		 {CMD_PARAM_STR | CMD_PARAM_OPT, 0, 0}, /* service urn */
		 {0, 0, 0}},
		REQUEST_ROUTE | ONREPLY_ROUTE | FAILURE_ROUTE | BRANCH_ROUTE
				| LOCAL_ROUTE},
	{"lost_held_query", (cmd_function)lost_held_function,
		{{CMD_PARAM_STR, 0, 0},				  /* connection */
		 {CMD_PARAM_VAR, 0, 0},				  /* out: pidf-lo */
		 {CMD_PARAM_VAR, 0, 0},				  /* out: location uri */
		 {CMD_PARAM_VAR, 0, 0},				  /* out: error */
		 {CMD_PARAM_STR | CMD_PARAM_OPT, 0, 0}, /* device id */
		 {0, 0, 0}},
		REQUEST_ROUTE | ONREPLY_ROUTE | FAILURE_ROUTE | BRANCH_ROUTE
				| LOCAL_ROUTE},
	{"lost_held_dereference", (cmd_function)lost_held_dereference,
		{{CMD_PARAM_STR, 0, 0},				  /* location uri */
		 {CMD_PARAM_VAR, 0, 0},				  /* out: pidf-lo */
		 {CMD_PARAM_VAR, 0, 0},				  /* out: error */
		 {CMD_PARAM_STR | CMD_PARAM_OPT, 0, 0}, /* responseTime */
		 {CMD_PARAM_STR | CMD_PARAM_OPT, 0, 0}, /* locationType */
		 {0, 0, 0}},
		REQUEST_ROUTE | ONREPLY_ROUTE | FAILURE_ROUTE | BRANCH_ROUTE
				| LOCAL_ROUTE},
	{0, 0, {{0, 0, 0}}, 0}};

/* Same functions, but the HTTP round trip is handed to the reactor */
static const acmd_export_t acmds[] = {
	{"lost_query", (acmd_function)lost_async_function,
		{{CMD_PARAM_STR, 0, 0}, {CMD_PARAM_VAR, 0, 0}, {CMD_PARAM_VAR, 0, 0},
			{CMD_PARAM_VAR, 0, 0}, {CMD_PARAM_STR | CMD_PARAM_OPT, 0, 0},
			{CMD_PARAM_STR | CMD_PARAM_OPT, 0, 0}, {0, 0, 0}}},
	{"lost_held_query", (acmd_function)lost_async_held_function,
		{{CMD_PARAM_STR, 0, 0}, {CMD_PARAM_VAR, 0, 0}, {CMD_PARAM_VAR, 0, 0},
			{CMD_PARAM_VAR, 0, 0}, {CMD_PARAM_STR | CMD_PARAM_OPT, 0, 0},
			{0, 0, 0}}},
	{"lost_held_dereference", (acmd_function)lost_async_held_dereference,
		{{CMD_PARAM_STR, 0, 0}, {CMD_PARAM_VAR, 0, 0}, {CMD_PARAM_VAR, 0, 0},
			{CMD_PARAM_STR | CMD_PARAM_OPT, 0, 0},
			{CMD_PARAM_STR | CMD_PARAM_OPT, 0, 0}, {0, 0, 0}}},
	{0, 0, {{0, 0, 0}}}};

static const param_export_t params[] = {
	{"connection", STR_PARAM | USE_FUNC_PARAM, (void *)lost_conn_add},
	{"exact_type", INT_PARAM, &held_exact_type},
	{"location_type", STR_PARAM, &held_loc_type.s},
	{"post_request", INT_PARAM, &held_post_req},
	{"response_time", INT_PARAM, &held_resp_time},
	{"geoheader_type", INT_PARAM, &lost_geoloc_type},
	{"geoheader_order", INT_PARAM, &lost_geoloc_order},
	{"geoheader_incl_alt", INT_PARAM, &lost_geoloc_3d},
	{"location_profile", INT_PARAM, &lost_profile},
	{"recursion", INT_PARAM, &lost_recursion},
	{"verbose", INT_PARAM, &lost_verbose},
	{0, 0, 0}};

struct module_exports exports = {
	"lost",			  /* module name */
	MOD_TYPE_DEFAULT, /* class of this module */
	MODULE_VERSION,	  /* module version */
	DEFAULT_DLFLAGS,  /* dlopen flags */
	0,				  /* load function */
	&deps,			  /* OpenSIPS module dependencies */
	cmds,			  /* exported functions */
	acmds,			  /* exported async functions */
	params,			  /* exported parameters */
	lost_stats,		  /* exported statistics */
	NULL,			  /* exported MI functions */
	NULL,			  /* exported pseudo-variables */
	NULL,			  /* exported transformations */
	NULL,			  /* extra processes */
	NULL,			  /* module pre-initialization function */
	mod_init,		  /* module initialization function */
	NULL,			  /* response function */
	mod_destroy,	  /* destroy function */
	child_init,		  /* per-child init function */
	NULL			  /* reload confirm function */
};

static int mod_init(void)
{
	LM_DBG("initializing lost module\n");

	if(held_loc_type.s)
		held_loc_type.len = strlen(held_loc_type.s);

	if(lost_profile < 0 || lost_profile > 3) {
		LM_ERR("location_profile must be 0..3\n");
		return -1;
	}
	if(lost_geoloc_type < 0 || lost_geoloc_type > 2) {
		LM_ERR("geoheader_type must be 0..2\n");
		return -1;
	}
	if(held_resp_time < 0) {
		LM_ERR("response_time must not be negative\n");
		return -1;
	}

	if(lost_http_init() < 0) {
		LM_ERR("failed to bind the rest_client API\n");
		return -1;
	}

	return 0;
}

static int child_init(int rank)
{
	return 0;
}

static void mod_destroy(void)
{
	lost_conn_destroy();
}
