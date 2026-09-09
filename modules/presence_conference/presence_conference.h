/*
 * presence_conference module - Conference event package (RFC 4575)
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

#ifndef _PRES_CONFERENCE_H_
#define _PRES_CONFERENCE_H_

/* RFC 4575: SIP event package name and content type */
#define CONF_EVENT_NAME       "conference"
#define CONF_EVENT_NAME_LEN   (sizeof(CONF_EVENT_NAME) - 1)

#define CONF_CONTENT_TYPE     "application/conference-info+xml"
#define CONF_CONTENT_TYPE_LEN (sizeof(CONF_CONTENT_TYPE) - 1)

/* RFC 4575 conference-info XML namespace */
#define CONF_INFO_NS          "urn:ietf:params:xml:ns:conference-info"

/* 11-character monotonic-version placeholder (a 32-bit signed int has at most
 * 10 digits plus a sign). The per-watcher version is patched into each NOTIFY
 * body by conf_patch_version(), mirroring presence_reginfo. */
#define CONF_VERSION_PLACEHOLDER "00000000000"
#define MAX_INT_LEN 11

#endif
