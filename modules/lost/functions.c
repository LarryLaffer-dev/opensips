/*
 * lost module functions
 *
 * Copyright (C) 2023 Wolfgang Kampichler
 * DEC112, FREQUENTIS AG
 *
 * This file is part of Kamailio, a free SIP server.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Kamailio is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * Kamailio is distributed in the hope that it will be useful,
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
 * \brief Kamailio lost :: functions
 * \ingroup lost
 * Module: \ref lost
 */
/*****************/

#include "../../mod_fix.h"
#include "../../pvar.h"
#include "../../route_struct.h"
#include "../../ut.h"
#include "../../trim.h"
#include "../../mem/mem.h"
#include "../../parser/msg_parser.h"
#include "../../parser/parse_body.h"

#include "functions.h"
#include "lost_compat.h"
#include "lost_conn.h"
#include "lost_http.h"
#include "pidf.h"
#include "utilities.h"
#include "response.h"
#include "naptr.h"

#define HELD_DEFAULT_TYPE "geodetic locationURI"
#define HELD_DEFAULT_TYPE_LEN (sizeof(HELD_DEFAULT_TYPE) - 1)

#define NAPTR_LOST_SERVICE_HTTP "LoST:http"
#define NAPTR_LOST_SERVICE_HTTPS "LoST:https"
#define NAPTR_LIS_SERVICE_HELD "LIS:HELD"

#define ACCEPT_HDR                      \
	"Accept: "                          \
	"application/pidf+xml,application/" \
	"held+xml;q=0.5"

extern int lost_geoloc_type;
extern int lost_geoloc_order;
extern int lost_geoloc_3d;
extern int lost_verbose;
extern int held_resp_time;
extern int held_exact_type;
extern int held_post_req;
extern str held_loc_type;

static str mtheld = str_init("application/held+xml;charset=utf-8");
static str mtlost = str_init("application/lost+xml;charset=utf-8");
static str accept_hdr = str_init(ACCEPT_HDR);

char uri_element[] = "uri";
char name_element[] = "displayName";
char errors_element[] = "errors";

/*
 * lost_held_type(type, exact, lgth)
 * verifies module params and returns valid HELD loaction type
 * allocated in private memory
 */
char *lost_held_type(char *type, int *exact, int *lgth)
{
	char *ret = NULL;
	char *tmp = NULL;
	int len = 0;

	ret = (char *)pkg_malloc(1);
	if(ret == NULL)
		goto err;

	memset(ret, 0, 1);
	*lgth = 0;

	if(strstr(type, HELD_TYPE_ANY)) {
		len = strlen(ret) + strlen(HELD_TYPE_ANY) + 1;
		tmp = pkg_realloc(ret, len);
		if(tmp == NULL)
			goto err;
		ret = tmp;
		strcat(ret, HELD_TYPE_ANY);
		*exact = 0;
	} else {
		if(strstr(type, HELD_TYPE_CIV)) {
			len = strlen(ret) + strlen(HELD_TYPE_CIV) + 1;
			tmp = pkg_realloc(ret, len);
			if(tmp == NULL)
				goto err;
			ret = tmp;
			strcat(ret, HELD_TYPE_CIV);
		}
		if(strstr(type, HELD_TYPE_GEO)) {
			if(strlen(ret) > 1) {
				len = strlen(ret) + strlen(HELD_TYPE_SEP) + 1;
				tmp = pkg_realloc(ret, len);
				if(tmp == NULL)
					goto err;
				ret = tmp;
				strcat(ret, HELD_TYPE_SEP);
			}
			len = strlen(ret) + strlen(HELD_TYPE_GEO) + 1;
			tmp = pkg_realloc(ret, len);
			if(tmp == NULL)
				goto err;
			ret = tmp;
			strcat(ret, HELD_TYPE_GEO);
		}
		if(strstr(type, HELD_TYPE_URI)) {
			if(strlen(ret) > 1) {
				len = strlen(ret) + strlen(HELD_TYPE_SEP) + 1;
				tmp = pkg_realloc(ret, len);
				if(tmp == NULL)
					goto err;
				ret = tmp;
				strcat(ret, HELD_TYPE_SEP);
			}
			len = strlen(ret) + strlen(HELD_TYPE_URI) + 1;
			tmp = pkg_realloc(ret, len);
			if(tmp == NULL)
				goto err;
			ret = tmp;
			strcat(ret, HELD_TYPE_URI);
		}
	}

	*lgth = strlen(ret);
	return ret;

err:
	PKG_MEM_ERROR;
	/* clean up */
	if(ret != NULL) {
		pkg_free(ret);
		ret = NULL;
	}
	*lgth = 0;
	return NULL;
}

/*
 * lost_ctx_free(ctx)
 * releases everything a prepare step may have left in the context
 */
void lost_ctx_free(lost_ctx_t *ctx)
{
	if(ctx == NULL)
		return;

	lost_http_free(&ctx->req);
	lost_free_string(&ctx->req.body);
	lost_free_loc(&ctx->loc);
	lost_free_string(&ctx->urnbuf);
}

/*
 * lost_held_query_prepare(msg, ctx, con, id)
 * assembles a HELD locationRequest into ctx->req
 */
int lost_held_query_prepare(
		struct sip_msg *_m, lost_ctx_t *ctx, str *_con, str *_id)
{
	p_lost_held_t held = NULL;

	str url = STR_NULL;
	str did = STR_NULL;
	str que = STR_NULL;
	str con = STR_NULL;
	str host = STR_NULL;
	str name = STR_NULL;
	str idhdr = STR_NULL;

	static str rtype = STR_STATIC_INIT(HELD_DEFAULT_TYPE);
	static str sheld = STR_STATIC_INIT(NAPTR_LIS_SERVICE_HELD);

	lost_conn_t *conn = NULL;

	char istr[NI_MAXHOST];
	char *ipstr = NULL;

	int len = 0;
	int flag = 0;
	int naptr = 0;

	/* module parameter */
	if(held_loc_type.len > 0) {
		rtype.s = held_loc_type.s;
		rtype.len = held_loc_type.len;
	}
	/* connection from parameter */
	if(_con != NULL && _con->len > 0) {
		con = *_con;
		conn = lost_conn_get(&con);
		if(conn == NULL) {
			LM_ERR("connection: [%.*s] does not exist\n", con.len, con.s);
			goto err;
		}
	}
	/* id from parameter */
	if(_id != NULL) {
		if(_id->len == 0) {
			LM_ERR("no device id found\n");
			goto err;
		}
		/* script parameters are not NUL-terminated, but lost_parse_host()
		 * below treats the id as a C string */
		if(pkg_nt_str_dup(&idhdr, _id) < 0) {
			PKG_MEM_ERROR;
			goto err;
		}
		did = idhdr;
	} else {

		LM_DBG("parsing P-A-I header\n");

		/* id from P-A-I header */
		idhdr.s = lost_get_pai_header(_m, &idhdr.len);
		if(idhdr.len == 0) {
			LM_WARN("P-A-I header not found, trying From header ...\n");

			LM_DBG("parsing From header\n");

			lost_free_string(&idhdr);
			/* id from From header */
			idhdr.s = lost_get_from_header(_m, &idhdr.len);
			if(idhdr.len == 0) {
				LM_ERR("no device id found\n");
				goto err;
			}
		}
		did.s = idhdr.s;
		did.len = idhdr.len;
	}
	LM_INFO("### HELD id [%.*s]\n", did.len, did.s);
	/* assemble locationRequest */
	held = lost_new_held(did, rtype, held_resp_time, held_exact_type);
	if(held == NULL) {
		LM_ERR("held object allocation failed\n");
		goto err;
	}
	que.s = lost_held_location_request(held, &que.len);
	lost_free_held(&held); /* clean up */
	if(que.len == 0) {
		LM_ERR("held request document error\n");
		goto err;
	}

	LM_DBG("held location request: [%s]\n", que.s);

	ctx->req.kind = LOST_REQ_HELD;
	ctx->req.body = que;
	ctx->req.ctype = mtheld;
	ctx->req.hdrs = accept_hdr;

	/* send locationRequest to location server - HTTP POST */
	if(conn != NULL) {

		LM_DBG("using connection [%.*s]\n", con.len, con.s);

		ctx->req.conn = conn;
	} else {
		/* we have no connection ... do a NAPTR lookup */
		if(lost_parse_host(did.s, &host, &flag) > 0) {

			LM_DBG("no conn. trying NAPTR lookup [%.*s]\n", host.len, host.s);

			/* remove '[' and ']' from string (IPv6) */
			if(flag == AF_INET6) {
				host.s++;
				host.len = host.len - 2;
			}
			/* is it a name or ip ... check nameinfo (reverse lookup) */
			len = 0;
			ipstr = lost_copy_string(host, &len);
			if(ipstr != NULL) {
				name.s = &(istr[0]);
				name.len = NI_MAXHOST;
				if(lost_get_nameinfo(ipstr, &name, flag) > 0) {

					LM_DBG("ip [%s] to name [%.*s]\n", ipstr, name.len, name.s);

					/* change ip string to name */
					host.s = name.s;
					host.len = name.len;
				} else {

					/* keep string */
					LM_DBG("no nameinfo for [%s]\n", ipstr);
				}
				pkg_free(ipstr); /* clean up */
				ipstr = NULL;
			} else {
				LM_ERR("could not copy host info\n");
			}
			url.s = &(ctx->urlbuf[0]);
			url.len = MAX_URI_SIZE;
			if((naptr = lost_naptr_lookup(host, &sheld, &url)) == 0) {
				LM_ERR("NAPTR failed on [%.*s]\n", host.len, host.s);
				goto err;
			}
		} else {
			LM_ERR("failed to get location service for [%.*s]\n", did.len,
					did.s);
			goto err;
		}

		LM_DBG("NAPTR lookup returned [%.*s]\n", url.len, url.s);

		ctx->req.url = url;
	}

	/* clean up */
	lost_free_string(&idhdr);

	return 0;

err:
	lost_free_held(&held);
	lost_free_string(&idhdr);
	lost_free_string(&que);

	return LOST_CLIENT_ERROR;
}

/*
 * lost_held_query_finish(msg, ctx, code)
 * parses a HELD locationResponse and writes the output variables
 */
int lost_held_query_finish(struct sip_msg *_m, lost_ctx_t *ctx, int _code)
{
	pv_value_t pvpidf;
	pv_value_t pvurl;
	pv_value_t pverr;

	xmlDocPtr doc = NULL;
	xmlNodePtr root = NULL;
	xmlNodePtr cur_node = NULL;

	str geo = STR_NULL;	 /* return value geolocation uri */
	str egeo = STR_NULL; /* entire value geolocation uri */
	str res = STR_NULL;	 /* return value pidf */
	str err = STR_NULL;	 /* return value error */
	str pidfurl = STR_NULL;

	lost_http_req_t req;

	char *heldreq = NULL;

	int len = 0;
	int curl = 0;
	int presence = 0;
	int res_error = 0;

	/* the request body is no longer needed */
	lost_free_string(&ctx->req.body);

	res = ctx->req.res;
	ctx->req.res = (str)STR_NULL;

	/* only HTTP 2xx responses are accepted */
	if(_code >= 300 || _code < 100)
		goto err;

	/* read and parse the returned xml */
	doc = xmlReadMemory(res.s, res.len, 0, NULL,
			XML_PARSE_NOBLANKS | XML_PARSE_NONET | XML_PARSE_NOCDATA);
	if(doc == NULL) {
		LM_WARN("invalid xml document: [%.*s]\n", res.len, res.s);
		doc = xmlReadMemory(res.s, res.len, 0, NULL,
				XML_PARSE_NOBLANKS | XML_PARSE_NONET | XML_PARSE_NOCDATA
						| XML_PARSE_RECOVER);
		if(doc == NULL) {
			LM_ERR("xml document recovery failed on: [%.*s]\n", res.len, res.s);
			goto err;
		}

		LM_DBG("xml document recovered\n");
	}
	root = xmlDocGetRootElement(doc);
	if(root == NULL) {
		LM_ERR("empty xml document\n");
		goto err;
	}
	/* check the root element ... shall be locationResponse, or error */
	if(xmlStrcmp(root->name, (const xmlChar *)"locationResponse") == 0) {

		LM_DBG("HELD location response [%.*s]\n", res.len, res.s);

		for(cur_node = root->children; cur_node; cur_node = cur_node->next) {
			if(cur_node->type == XML_ELEMENT_NODE) {
				if(xmlStrcmp(cur_node->name, (const xmlChar *)"locationUriSet")
						== 0) {

					LM_DBG("*** node '%s' found\n", cur_node->name);

					lost_free_string(&egeo);
					/* get the locationUri element */
					egeo.s = lost_get_content(
							root, (char *)HELD_TYPE_URI, &egeo.len);
					if(egeo.len == 0) {
						LM_WARN("%s element not found\n", HELD_TYPE_URI);
						lost_free_string(&egeo);
					} else {
						geo.len = egeo.len;
						geo.s = lost_trim_content(egeo.s, &geo.len);
					}
				}
				if(xmlStrcmp(cur_node->name, (const xmlChar *)"presence")
						== 0) {

					LM_DBG("*** node '%s' found\n", cur_node->name);

					/* response contains presence node */
					presence = 1;
				}
			}
		}
		/* if we do not have a presence node but a location URI */
		/* dereference pidf.lo at location server via HTTP GET */
		if((presence == 0) && (geo.s != NULL && geo.len > 0)) {
			LM_INFO("presence node not found in HELD response, trying URI "
					"...\n");

			/* NOTE: this secondary leg is always synchronous, even when the
			 * locationRequest above ran async.  Scripts that care should
			 * dereference the returned URI themselves with an async
			 * lost_held_dereference() call. */
			memset(&req, 0, sizeof req);
			req.kind = LOST_REQ_HELD;
			req.url = geo;
			req.ctype = mtheld;
			req.hdrs = accept_hdr;

			/* RFC 6753 allows the location URI to be dereferenced with a
			 * bare GET; "post_request" switches to a HELD POST instead */
			if(held_post_req != 0) {
				len = 0;
				heldreq = lost_held_post_request(&len, 0, NULL);
				if(heldreq == NULL) {
					LM_ERR("could not create POST request\n");
					goto err;
				}

				LM_DBG("held POST request: [%.*s]\n", len, heldreq);

				req.body.s = heldreq;
				req.body.len = len;
			}

			curl = lost_http_query(_m, &req);
			pidfurl = req.res;

			if(heldreq != NULL) {
				pkg_free(heldreq); /* clean up */
				heldreq = NULL;
			}

			/* only HTTP 2xx responses are accepted */
			if(curl >= 300 || curl < 100)
				goto err;

			if(pidfurl.len == 0) {
				LM_WARN("HELD location request failed [%.*s]\n", geo.len,
						geo.s);
			} else {

				LM_DBG("HELD location response [%.*s]\n", pidfurl.len,
						pidfurl.s);

				/* the dereferenced PIDF-LO replaces the locationUriSet
				 * response we no longer need */
				lost_free_string(&res);
				res = pidfurl;
				pidfurl = (str)STR_NULL;
			}
		}
		/* error received */
	} else if(xmlStrcmp(root->name, (const xmlChar *)"error") == 0) {

		LM_DBG("HELD error response [%.*s]\n", res.len, res.s);

		/* get the error property */
		err.s = lost_get_property(root, (char *)"code", &err.len);
		if(err.len == 0) {
			LM_ERR("error - property not found: [%.*s]\n", res.len, res.s);
			goto err;
		}
		LM_WARN("locationRequest error response: [%.*s]\n", err.len, err.s);
	} else {
		LM_ERR("root element is not valid: [%.*s]\n", res.len, res.s);
		goto err;
	}

	/* clean up */
	xmlFreeDoc(doc);
	doc = NULL;

	/* set writeable pvars */
	pvpidf.rs = res;
	pvpidf.flags = PV_VAL_STR;
	if(pv_set_value(_m, ctx->pv1, 0, &pvpidf) != 0)
		LM_ERR("failed to set pidf pvar\n");
	lost_free_string(&res); /* clean up */

	pvurl.rs = geo;
	pvurl.flags = PV_VAL_STR;
	if(pv_set_value(_m, ctx->pv2, 0, &pvurl) != 0)
		LM_ERR("failed to set url pvar\n");
	lost_free_string(&egeo); /* clean up */

	/* return error code in case of response error */
	if(err.len > 0) {
		res_error = 1;
	}
	pverr.rs = err;
	pverr.flags = PV_VAL_STR;
	if(pv_set_value(_m, ctx->pv3, 0, &pverr) != 0)
		LM_ERR("failed to set err pvar\n");
	lost_free_string(&err); /* clean up */

	return (res_error > 0) ? LOST_SERVER_ERROR : LOST_SUCCESS;

err:
	/* clean up pointer */
	lost_free_string(&pidfurl);
	/* clean up xml */
	if(doc != NULL) {
		xmlFreeDoc(doc);
	}
	/* clean up string */
	lost_free_string(&res);
	lost_free_string(&egeo);
	lost_free_string(&err);
	if(heldreq != NULL) {
		pkg_free(heldreq);
	}

	return LOST_CLIENT_ERROR;
}

/*
 * lost_held_deref_prepare(msg, ctx, url, rtime, rtype)
 * assembles a HELD locationRequest (POST) against a location URI
 */
int lost_held_deref_prepare(
		struct sip_msg *_m, lost_ctx_t *ctx, str *_url, str *_rtime, str *_rtype)
{
	str url = STR_NULL;
	str rtm = STR_NULL;
	str rtp = STR_NULL;

	char *heldreq = NULL;
	char *rtype = NULL;

	long rtime = 0;

	int itime = 0;
	int len = 0;
	int exact = 0;

	/* dereference url from parameter */
	url = *_url;
	if(url.len == 0) {
		LM_ERR("no dereference url found\n");
		goto err;
	}

	/* response time from parameter: either a number of milliseconds or one
	 * of the "emergencyRouting"/"emergencyDispatch" tokens (RFC 6155 6.2) */
	if(_rtime != NULL) {
		rtm = *_rtime;
		trim(&rtm);

		if(rtm.len == 0) {
			/* default: rtime = 0 */
			LM_WARN("no response time found\n");
		} else if(str2sint(&rtm, &itime) == 0) {
			if(itime > 0) {
				/* responseTime: milliseconds */
				rtime = itime;
			}
		} else if(rtm.len >= (int)strlen(HELD_ED)
				  && strncasecmp(rtm.s, HELD_ED, strlen(HELD_ED)) == 0) {
			/* responseTime: emergencyDispatch */
			rtime = -1;
		} else if(rtm.len >= (int)strlen(HELD_ER)
				  && strncasecmp(rtm.s, HELD_ER, strlen(HELD_ER)) == 0) {
			/* responseTime: emergencyRouting */
			rtime = 0;
		} else {
			LM_WARN("unrecognised response time [%.*s], using "
					"emergencyRouting\n",
					rtm.len, rtm.s);
		}
	}

	/* response type from parameter */
	if(_rtype != NULL) {
		if(_rtype->len == 0) {
			LM_WARN("no response type found\n");
		} else {
			/* lost_held_type() scans the value with strstr(), so it needs a
			 * NUL-terminated copy of the script parameter */
			if(pkg_nt_str_dup(&rtp, _rtype) < 0) {
				PKG_MEM_ERROR;
				goto err;
			}

			len = 0;
			/* response type string sanity check */
			rtype = lost_held_type(rtp.s, &exact, &len);
			/* default value will be used if nothing was returned */
			if(rtype == NULL) {
				LM_WARN("cannot normalize [%.*s]\n", rtp.len, rtp.s);
			}
		}
	}

	/* get the HELD request body */
	heldreq = lost_held_post_request(&len, rtime, rtype);

	/* clean up */
	if(rtype != NULL) {
		pkg_free(rtype);
		rtype = NULL;
	}

	if(heldreq == NULL) {
		LM_ERR("could not create POST request\n");
		goto err;
	}

	LM_DBG("POST request: [%.*s]\n", len, heldreq);

	ctx->req.kind = LOST_REQ_HELD;
	ctx->req.url = url;
	ctx->req.body.s = heldreq;
	ctx->req.body.len = len;
	ctx->req.ctype = mtheld;
	ctx->req.hdrs = accept_hdr;

	lost_free_string(&rtp);

	return 0;

err:
	lost_free_string(&rtp);
	if(heldreq != NULL) {
		pkg_free(heldreq);
	}
	if(rtype != NULL) {
		pkg_free(rtype);
	}

	return LOST_CLIENT_ERROR;
}

/*
 * lost_held_deref_finish(msg, ctx, code)
 * parses the dereferenced PIDF-LO and writes the output variables
 */
int lost_held_deref_finish(struct sip_msg *_m, lost_ctx_t *ctx, int _code)
{
	pv_value_t pvpidf;
	pv_value_t pverr;

	xmlDocPtr doc = NULL;
	xmlNodePtr root = NULL;

	str res = STR_NULL; /* return value location response */
	str err = STR_NULL; /* return value error */

	int ret = LOST_SUCCESS;

	/* the request body is no longer needed */
	lost_free_string(&ctx->req.body);

	res = ctx->req.res;
	ctx->req.res = (str)STR_NULL;

	/* only HTTP 2xx responses are accepted */
	if(_code >= 300 || _code < 100)
		goto err;

	if(res.s != NULL && res.len > 0) {

		LM_DBG("LbR pidf-lo: [%.*s]\n", res.len, res.s);

	} else {
		LM_ERR("dereferencing location failed\n");
		goto err;
	}

	/* read and parse the returned xml */
	doc = xmlReadMemory(res.s, res.len, 0, NULL,
			XML_PARSE_NOBLANKS | XML_PARSE_NONET | XML_PARSE_NOCDATA);
	if(doc == NULL) {
		LM_WARN("invalid xml document: [%.*s]\n", res.len, res.s);
		doc = xmlReadMemory(res.s, res.len, 0, NULL,
				XML_PARSE_NOBLANKS | XML_PARSE_NONET | XML_PARSE_NOCDATA
						| XML_PARSE_RECOVER);
		if(doc == NULL) {
			LM_ERR("xml document recovery failed on: [%.*s]\n", res.len, res.s);
			goto err;
		}

		LM_DBG("xml document recovered\n");
	}
	root = xmlDocGetRootElement(doc);
	if(root == NULL) {
		LM_ERR("empty xml document\n");
		goto err;
	}

	/* check root element ... shall be presence|locationResponse, or error */
	if((!xmlStrcmp(root->name, (const xmlChar *)"presence"))
			|| (!xmlStrcmp(root->name, (const xmlChar *)"locationResponse"))) {

		LM_DBG("HELD location response [%.*s]\n", res.len, res.s);

		/* check content and set response code
		 * + 0 nothing found: return 200
		 * + 1 reference found: return 201
		 * + 2 value found: return 202
		 * + 3 value and reference found: return 203
		 */
		ret += lost_check_HeldResponse(root);
		/* error received */
	} else if(xmlStrcmp(root->name, (const xmlChar *)"error") == 0) {

		LM_DBG("HELD error response [%.*s]\n", res.len, res.s);

		/* get the error property */
		err.s = lost_get_property(root, (char *)"code", &err.len);
		if(err.len == 0) {
			LM_ERR("error - property not found: [%.*s]\n", res.len, res.s);
			goto err;
		}
		LM_WARN("locationRequest error response: [%.*s]\n", err.len, err.s);
	} else {
		LM_ERR("root element is not valid: [%.*s]\n", res.len, res.s);
		goto err;
	}

	/* clean up */
	xmlFreeDoc(doc);
	doc = NULL;

	/* set writeable pvars */
	pvpidf.rs = res;
	pvpidf.flags = PV_VAL_STR;
	if(pv_set_value(_m, ctx->pv1, 0, &pvpidf) != 0)
		LM_ERR("failed to set pidf pvar\n");
	lost_free_string(&res); /* clean up */

	/* return error code in case of response error */
	if(err.len > 0) {
		ret = LOST_SERVER_ERROR;
	}
	pverr.rs = err;
	pverr.flags = PV_VAL_STR;
	if(pv_set_value(_m, ctx->pv2, 0, &pverr) != 0)
		LM_ERR("failed to set err pvar\n");
	lost_free_string(&err); /* clean up */

	return ret;

err:
	/* clean up xml */
	if(doc != NULL) {
		xmlFreeDoc(doc);
	}
	/* clean up string */
	lost_free_string(&res);
	lost_free_string(&err);

	return LOST_CLIENT_ERROR;
}

/*
 * lost_function(msg, con, pidf, uri, name, err, pidf, urn)
 * assembles and runs LOST findService request, parses results
 */
int lost_query_prepare(
		struct sip_msg *_m, lost_ctx_t *ctx, str *_con, str *_pidf, str *_urn)
{
	p_lost_loc_t loc = NULL;
	p_lost_geolist_t geolist = NULL;

	str url = STR_NULL;
	str urn = STR_NULL;
	str req = STR_NULL;
	str con = STR_NULL;
	str ret = STR_NULL;
	str pidf = STR_NULL;
	str losturl = STR_NULL;
	str urnbuf = STR_NULL;
	str derefurl = STR_NULL;

	static str shttp = STR_STATIC_INIT(NAPTR_LOST_SERVICE_HTTP);
	static str shttps = STR_STATIC_INIT(NAPTR_LOST_SERVICE_HTTPS);

	lost_conn_t *conn = NULL;
	lost_http_req_t req_http;

	struct msg_start *fl;

	char ustr[MAX_URI_SIZE];
	char *geoval = NULL;
	char *heldreq = NULL;

	int geotype = 0;
	int curl = 0;
	int len = 0;
	int naptr = 0;
	int geoitems = 0;

	/* connection from parameter */
	if(_con != NULL && _con->len > 0) {
		con = *_con;
		conn = lost_conn_get(&con);
		if(conn == NULL) {
			LM_WARN("connection: [%.*s] does not exist\n", con.len, con.s);
			/* check if NAPTR lookup works with connection parameter */
			losturl.s = &(ustr[0]);
			losturl.len = MAX_URI_SIZE;
			if((naptr = lost_naptr_lookup(con, &shttps, &losturl)) == 0) {
				naptr = lost_naptr_lookup(con, &shttp, &losturl);
			}
			if(naptr == 0) {
				LM_ERR("NAPTR failed on [%.*s]\n", con.len, con.s);
				goto err;
			}
			url = losturl;
		}
	}
	/* urn from parameter */
	if(_urn != NULL && _urn->len > 0) {
		/* is_urn() inspects the value as a C string */
		if(pkg_nt_str_dup(&urnbuf, _urn) < 0) {
			PKG_MEM_ERROR;
			goto err;
		}
		urn = urnbuf;
	}
	/* urn from request line */
	if(urn.len == 0) {

		LM_DBG("no service urn parameter, trying request line ...\n");

		fl = &(_m->first_line);
		if(pkg_nt_str_dup(&urnbuf, &fl->u.request.uri) < 0) {
			PKG_MEM_ERROR;
			goto err;
		}
		urn = urnbuf;
	}
	/* check urn scheme */
	if(is_urn(urn.s) > 0) {
		LM_INFO("### LOST urn\t[%.*s]\n", urn.len, urn.s);
	} else {
		LM_ERR("service urn not found\n");
		goto err;
	}
	/* pidf from parameter */
	if(_pidf != NULL && _pidf->len > 0) {
		pidf = *_pidf;

		LM_DBG("parsing pidf parameter ...\n");
		LM_DBG("pidf: [%.*s]\n", pidf.len, pidf.s);

		/* parse the pidf and get loc object */
		loc = lost_parse_pidf(pidf, urn);
	}
	/* neither valid pidf parameter nor loc ... check geolocation header */
	if(loc == NULL) {

		/* parse Geolocation header */

		LM_DBG("parsing geolocation header ...\n");

		geolist = lost_get_geolocation_header(_m, &geoitems);

		if(geoitems == 0) {
			LM_ERR("geolocation header not found\n");
			goto err;
		}

		LM_DBG("number of location URIs: %d\n", geoitems);

		if(lost_geoloc_order == 0) {

			LM_DBG("reversing location URI sequence\n");

			lost_reverse_geoheader_list(&geolist);
		}
		switch(lost_geoloc_type) {
			case ANY: /* type: 0 */
				geoval = lost_get_geoheader_value(geolist, ANY, &geotype);

				LM_DBG("geolocation header field (any): %s\n", geoval);

				break;
			case CID: /* type: 1 */
				geoval = lost_get_geoheader_value(geolist, CID, &geotype);

				LM_DBG("geolocation header field (LbV): %s\n", geoval);

				break;
			case HTTP: /* type: 2 */
				geoval = lost_get_geoheader_value(geolist, HTTP, &geotype);
				/* fallback to https */
				if(geoval == NULL) {
					LM_WARN("no valid http URL ... trying https\n");
					geoval = lost_get_geoheader_value(geolist, HTTPS, &geotype);
				}

				LM_DBG("geolocation header field (LbR): %s\n", geoval);

				break;
			case HTTPS: /* type: 3 */
				/* prefer https */
				geoval = lost_get_geoheader_value(geolist, HTTPS, &geotype);
				/* fallback to http */
				if(geoval == NULL) {
					LM_WARN("no valid https URL ... trying http\n");
					geoval = lost_get_geoheader_value(geolist, HTTP, &geotype);
				}

				LM_DBG("geolocation header field (LbR): %s\n", geoval);

				break;
			default:
				LM_WARN("unknown module parameter value\n");
				geoval = lost_get_geoheader_value(geolist, UNKNOWN, &geotype);

				LM_DBG("geolocation header field (any): %s\n", geoval);

				break;
		}
		if(geoval == NULL) {
			LM_ERR("invalid geolocation header\n");
			goto err;
		}
		LM_INFO("### LOST loc\t[%s]\n", geoval);
		/* clean up */
		pidf.s = NULL;
		pidf.len = 0;
		/* use location by value */
		if(geotype == CID) {
			/* content indirection: the PIDF-LO travels as a body part
			 * referenced by Content-ID (RFC 4483, RFC 6442) */
			pidf.s = lost_get_pidf_by_cid(_m, geoval, &pidf.len);
			if(pidf.s != NULL && pidf.len > 0) {

				LM_DBG("LbV pidf-lo: [%.*s]\n", pidf.len, pidf.s);

			} else {
				LM_WARN("no multipart body found\n");
			}
		}
		/* use location by reference */
		if((geotype == HTTPS) || (geotype == HTTP)) {
			derefurl.s = geoval;
			derefurl.len = strlen(geoval);

			memset(&req_http, 0, sizeof req_http);
			req_http.kind = LOST_REQ_HELD;
			req_http.url = derefurl;
			req_http.ctype = mtheld;
			req_http.hdrs = accept_hdr;

			/* dereference pidf-lo at the location server; a bare GET is
			 * enough (RFC 6753), "post_request" forces a HELD POST */
			if(held_post_req != 0) {
				len = 0;
				heldreq = lost_held_post_request(&len, 0, NULL);
				if(heldreq == NULL) {
					LM_ERR("could not create POST request\n");
					goto err;
				}

				LM_DBG("POST request: [%.*s]\n", len, heldreq);

				req_http.body.s = heldreq;
				req_http.body.len = len;
			}

			curl = lost_http_query(_m, &req_http);
			ret = req_http.res;

			if(heldreq != NULL) {
				pkg_free(heldreq); /* clean up */
				heldreq = NULL;
			}

			/* only HTTP 2xx responses are accepted */
			if(curl >= 300 || curl < 100) {
				/* clean up */
				lost_free_string(&ret);
				goto err;
			}

			pidf = ret;
			if(pidf.s != NULL && pidf.len > 0) {

				LM_DBG("LbR pidf-lo: [%.*s]\n", pidf.len, pidf.s);

			} else {
				LM_WARN("dereferencing location failed\n");
			}
		}
		if(pidf.s == NULL && pidf.len == 0) {
			LM_ERR("location object not found\n");
			goto err;
		}
		/* parse the pidf and get loc object.  Note that in the LbR case pidf
		 * still points into ret, so parse before releasing the buffer. */
		loc = lost_parse_pidf(pidf, urn);

		/* clean up */
		lost_free_geoheader_list(&geolist);
		lost_free_string(&ret);
		pidf = STR_NULL;
	}
	/* pidf parsing failed ... return */
	if(loc == NULL) {
		LM_ERR("parsing pidf failed\n");
		goto err;
	}
	/* assemble findService request */
	req.s = lost_find_service_request(loc, NULL, &req.len);

	if(req.s == NULL && req.len == 0) {
		LM_ERR("lost request failed\n");
		goto err;
	}

	LM_DBG("findService request: [%.*s]\n", req.len, req.s);

	/* send findService request to mapping server - HTTP POST */
	ctx->req.kind = LOST_REQ_LOST;
	ctx->req.conn = conn;
	ctx->req.body = req;
	ctx->req.ctype = mtlost;

	/* url only when there is no named connection; it lives in the NAPTR
	 * buffer, which is local to this function, so copy it into the context */
	if(conn == NULL) {
		if(url.len == 0 || url.len >= MAX_URI_SIZE) {
			LM_ERR("no usable mapping server url\n");
			goto err;
		}
		memcpy(ctx->urlbuf, url.s, url.len);
		ctx->req.url.s = &(ctx->urlbuf[0]);
		ctx->req.url.len = url.len;
	}

	/* the parsed location and the urn are needed again should the mapping
	 * server answer with a redirect */
	ctx->loc = loc;
	ctx->urnbuf = urnbuf;

	return 0;

err:
	lost_free_geoheader_list(&geolist);
	lost_free_loc(&loc);
	lost_free_string(&ret);
	lost_free_string(&req);
	lost_free_string(&urnbuf);
	if(heldreq != NULL) {
		pkg_free(heldreq);
	}

	return LOST_CLIENT_ERROR;
}

/*
 * lost_query_finish(msg, ctx, code)
 * parses a findServiceResponse, follows redirects and writes the outputs
 */
int lost_query_finish(struct sip_msg *_m, lost_ctx_t *ctx, int _code)
{
	pv_value_t pvname;
	pv_value_t pvuri;
	pv_value_t pverr;

	p_lost_fsr_t fsrdata = NULL;

	str name = STR_NULL; /* return value displayName */
	str uri = STR_NULL;	 /* return value uri */
	str err = STR_NULL;	 /* return value error */

	str tmp = STR_NULL;
	str url = STR_NULL;
	str ret = STR_NULL;
	str src = STR_NULL;
	str rereq = STR_NULL;
	str oldurl = STR_NULL;

	static str shttp = STR_STATIC_INIT(NAPTR_LOST_SERVICE_HTTP);
	static str shttps = STR_STATIC_INIT(NAPTR_LOST_SERVICE_HTTPS);

	lost_http_req_t req_http;

	char ustr[MAX_URI_SIZE];

	int redirect = 0;
	int curl = 0;
	int naptr = 0;
	int res_error = 0;

	/* the request body is no longer needed */
	lost_free_string(&ctx->req.body);

	ret = ctx->req.res;
	ctx->req.res = (str)STR_NULL;

	/* only HTTP 2xx responses are accepted */
	if(_code >= 300 || _code < 100)
		goto err;

	if(ret.len == 0) {
		LM_ERR("findService request failed\n");
		goto err;
	}

	LM_DBG("findService response: [%.*s]\n", ret.len, ret.s);

	/* at least parse one response */
	redirect = 1;
	while(redirect) {
		fsrdata = lost_parse_findServiceResponse(ret);
		if(fsrdata == NULL) {
			LM_ERR("findService response parsing failed\n");
			goto err;
		}
		if(lost_verbose == 1) {
			lost_print_findServiceResponse(fsrdata);
		}
		switch(fsrdata->category) {
			case RESPONSE:
				if(fsrdata->uri != NULL) {
					/* get the first sips uri element ... */
					if(lost_search_response_list(&fsrdata->uri, &tmp.s, SIPS_S)
							> 0) {
						tmp.len = strlen(tmp.s);
						/* or the first sip uri element ... */
					} else if(lost_search_response_list(
									  &fsrdata->uri, &tmp.s, SIP_S)
							  > 0) {
						tmp.len = strlen(tmp.s);
						/* or return error if nothing found */
					} else {
						LM_ERR("sip/sips uri not found: [%.*s]\n", ret.len,
								ret.s);
						goto err;
					}
					/* copy uri string */
					if(pkg_str_dup(&uri, &tmp) < 0) {
						LM_ERR("could not copy: [%.*s]\n", tmp.len, tmp.s);
						goto err;
					}
				} else {
					LM_ERR("uri element not found: [%.*s]\n", ret.len, ret.s);
					goto err;
				}
				if(fsrdata->mapping != NULL) {
					/* get the displayName element */
					if((tmp.s = fsrdata->mapping->name->text) != NULL) {
						tmp.len = strlen(fsrdata->mapping->name->text);
						if(pkg_str_dup(&name, &tmp) < 0) {
							LM_ERR("could not copy: [%.*s]\n", tmp.len, tmp.s);
							goto err;
						}
					}
				} else {
					LM_ERR("name not found: [%.*s]\n", ret.len, ret.s);
					goto err;
				}
				/* we are done */
				redirect = 0;
				break;
			case ERROR:
				/* get the errors element */
				if(fsrdata->errors != NULL) {
					if((tmp.s = fsrdata->errors->issue->type) != NULL) {
						tmp.len = strlen(fsrdata->errors->issue->type);
						if(pkg_str_dup(&err, &tmp) < 0) {
							LM_ERR("could not copy: [%.*s]\n", tmp.len, tmp.s);
							goto err;
						}
					}
					/* clean up */
					tmp.s = NULL;
					tmp.len = 0;
				} else {
					LM_ERR("errors not found: [%.*s]\n", ret.len, ret.s);
					goto err;
				}
				/* we are done */
				redirect = 0;
				break;
			case REDIRECT:
				/* get the target element */
				if(fsrdata->redirect != NULL) {
					if((tmp.s = fsrdata->redirect->target) != NULL) {
						tmp.len = strlen(fsrdata->redirect->target);
						url.s = &(ustr[0]);
						url.len = MAX_URI_SIZE;
						/* check loop ... current response */
						if(oldurl.s != NULL && oldurl.len > 0) {
							if(str_strcasecmp(&tmp, &oldurl) == 0) {
								LM_ERR("loop detected: "
									   "[%.*s]<-->[%.*s]\n",
										oldurl.len, oldurl.s, tmp.len, tmp.s);
								goto err;
							}
						}
						/* add redirecting source to path list */
						if((src.s = fsrdata->redirect->source) != NULL) {
							src.len = strlen(fsrdata->redirect->source);
							if(lost_append_response_list(&fsrdata->path, src)
									== 0) {
								LM_ERR("could not append server to path "
									   "elememt\n");
								goto err;
							}
						}
						/* clean up */
						src.s = NULL;
						src.len = 0;
						/* check loop ... path elements */
						char *via = NULL;
						if(lost_search_response_list(
								   &fsrdata->path, &via, tmp.s)
								> 0) {
							LM_ERR("loop detected: "
								   "[%s]<-->[%.*s]\n",
									via, tmp.len, tmp.s);
							goto err;
						}
						/* remember the redirect target */
						if(pkg_str_dup(&oldurl, &tmp) < 0) {
							LM_ERR("could not copy: [%.*s]\n", tmp.len, tmp.s);
							goto err;
						}
						/* get url string via NAPTR */
						naptr = lost_naptr_lookup(tmp, &shttps, &url);
						if(naptr == 0) {
							/* fallback to http */
							naptr = lost_naptr_lookup(tmp, &shttp, &url);
						}
						/* nothing found ... return */
						if(naptr == 0) {
							LM_ERR("NAPTR failed on [%.*s]\n", tmp.len, tmp.s);
							goto err;
						}
						/* clean up */
						tmp.s = NULL;
						tmp.len = 0;

						/* assemble new findService request including path element */
						rereq.s = lost_find_service_request(
								ctx->loc, fsrdata->path, &rereq.len);
						/* clean up */
						lost_free_findServiceResponse(&fsrdata);
						lost_free_string(&ret);

						LM_DBG("findService request: [%.*s]\n", rereq.len,
								rereq.s);

						/* NOTE: redirects are always followed synchronously,
						 * even when the initial request ran async.  With the
						 * default recursion=1 the mapping server resolves the
						 * chain itself and this path is not taken. */
						memset(&req_http, 0, sizeof req_http);
						req_http.kind = LOST_REQ_LOST;
						req_http.url = url;
						req_http.body = rereq;
						req_http.ctype = mtlost;

						curl = lost_http_query(_m, &req_http);
						ret = req_http.res;

						/*clean up */
						lost_free_string(&rereq);

						/* only HTTP 2xx responses are accepted */
						if(curl >= 300 || curl < 100) {
							goto err;
						}
						/* reset url string */
						url.s = NULL;
						url.len = 0;
						/* once more ... we got a redirect */
						redirect = 1;
					}
				} else {
					LM_ERR("redirect element not found: [%.*s]\n", ret.len,
							ret.s);
					goto err;
				}
				break;
			case OTHER:
			default:
				LM_ERR("pidf is not valid: [%.*s]\n", ret.len, ret.s);
				goto err;
				break;
		}
	}

	/* clean up */
	lost_free_findServiceResponse(&fsrdata);
	lost_free_string(&ret);
	lost_free_string(&oldurl);

	/* set writable pvars */
	pvuri.rs = uri;
	pvuri.flags = PV_VAL_STR;
	if(pv_set_value(_m, ctx->pv1, 0, &pvuri) < 0)
		LM_ERR("cannot set uri pvar\n");
	lost_free_string(&uri); /* clean up */

	pvname.rs = name;
	pvname.flags = PV_VAL_STR;
	if(pv_set_value(_m, ctx->pv2, 0, &pvname) < 0)
		LM_ERR("cannot set name pvar\n");
	lost_free_string(&name); /* clean up */

	/* return error code in case of response error */
	if(err.len > 0) {
		res_error = 1;
	}
	pverr.rs = err;
	pverr.flags = PV_VAL_STR;
	if(pv_set_value(_m, ctx->pv3, 0, &pverr) < 0)
		LM_ERR("cannot set error pvar\n");
	lost_free_string(&err); /* clean up */

	return (res_error > 0) ? LOST_SERVER_ERROR : LOST_SUCCESS;

err:
	/* clean up */
	lost_free_findServiceResponse(&fsrdata);
	/* clean up string */
	lost_free_string(&oldurl);
	lost_free_string(&ret);
	lost_free_string(&rereq);
	lost_free_string(&name);
	lost_free_string(&uri);
	lost_free_string(&err);

	return LOST_CLIENT_ERROR;
}

/*
 * Synchronous entry points: prepare, transfer, finish.
 * The async counterparts live in lost_async.c.
 */

int lost_held_function(struct sip_msg *_m, str *_con, pv_spec_t *_pidf,
		pv_spec_t *_url, pv_spec_t *_err, str *_id)
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
		return rc;
	}

	rc = lost_http_query(_m, &ctx.req);
	rc = lost_held_query_finish(_m, &ctx, rc);

	lost_ctx_free(&ctx);
	return rc;
}

int lost_held_dereference(struct sip_msg *_m, str *_url, pv_spec_t *_pidf,
		pv_spec_t *_err, str *_rtime, str *_rtype)
{
	lost_ctx_t ctx;
	int rc;

	memset(&ctx, 0, sizeof ctx);
	ctx.pv1 = _pidf;
	ctx.pv2 = _err;

	rc = lost_held_deref_prepare(_m, &ctx, _url, _rtime, _rtype);
	if(rc != 0) {
		lost_ctx_free(&ctx);
		return rc;
	}

	rc = lost_http_query(_m, &ctx.req);
	rc = lost_held_deref_finish(_m, &ctx, rc);

	lost_ctx_free(&ctx);
	return rc;
}

int lost_function(struct sip_msg *_m, str *_con, pv_spec_t *_uri,
		pv_spec_t *_name, pv_spec_t *_err, str *_pidf, str *_urn)
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
		return rc;
	}

	rc = lost_http_query(_m, &ctx.req);
	rc = lost_query_finish(_m, &ctx, rc);

	lost_ctx_free(&ctx);
	return rc;
}
