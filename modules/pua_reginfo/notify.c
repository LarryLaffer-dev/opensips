/*
 * pua_reginfo module - Presence-User-Agent Handling of reg events
 *
 * Copyright (C) 2011, 2023 Carsten Bock, carsten@ng-voice.com
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

#include "notify.h"
#include "../../parser/parse_from.h"
#include "../../parser/parse_content.h"
#include "../../parser/parse_uri.h"
#include "../usrloc/usrloc.h"
#include <libxml/parser.h>
#include "usrloc_cb.h"
#include "pua_reginfo.h"

/*<?xml version="1.0"?>
<reginfo xmlns="urn:ietf:params:xml:ns:reginfo" version="0" state="full">
.<registration aor="sip:carsten@ng-voice.com" id="0xb33fa860" state="active">
..<contact id="0xb33fa994" state="active" event="registered" expires="3600">
...<uri>sip:carsten@10.157.87.36:43582;transport=udp</uri>
...<unknown-param name="+g.3gpp.cs-voice"></unknown-param>
...<unknown-param name="+g.3gpp.icsi-ref">urn0X0.0041FB74E7B54P-1022urn-70X0P+03gpp-application.ims.iari.gsma-vs</unknown-param>
...<unknown-param name="audio"></unknown-param>
...<unknown-param name="+g.oma.sip-im.large-message"></unknown-param>
...<unknown-param name="+g.3gpp.smsip"></unknown-param>
...<unknown-param name="language">en,fr</unknown-param>
...<unknown-param name="+g.oma.sip-im"></unknown-param>
...<unknown-param name="expires">600000</unknown-param>
..</contact>
.</registration>
</reginfo> */

#define STATE_ACTIVE 1
#define STATE_TERMINATED 0
#define STATE_UNKNOWN -1

#define EVENT_UNKNOWN -1
#define EVENT_REGISTERED 0
#define EVENT_UNREGISTERED 1
#define EVENT_TERMINATED 2
#define EVENT_CREATED 3
#define EVENT_REFRESHED 4
#define EVENT_EXPIRED 5

#define RESULT_ERROR -1
#define RESULT_CONTACTS_FOUND 1
#define RESULT_NO_CONTACTS 2

int process_contact(udomain_t *domain, urecord_t **ul_record, str aor,
		str callid, int cseq, int expires, int event, str contact_uri)
{
	static str no_ua = str_init("n/a");
	static ucontact_info_t ci;
	ucontact_t *ul_contact;
	int ret;

	struct ct_match cmatch;
	memset(&cmatch, 0, sizeof(struct ct_match));
	cmatch.mode = CT_MATCH_NONE;

	pua_reginfo_update_self_op(1);
	if(*ul_record == NULL) {
		switch(event) {
			case EVENT_REGISTERED:
			case EVENT_CREATED:
			case EVENT_REFRESHED:
				/* In case, no record exists and new one should be created,
				 * create a new entry for this user in the usrloc-DB */
				if(ul.insert_urecord(domain, &aor, ul_record, 0) < 0) {
					LM_ERR("failed to insert new user-record\n");
					ret = RESULT_ERROR;
					goto done;
				}
				break;
			default:
				/* No entry in usrloc and the contact is expired, deleted, unregistered, whatever:
				 * We do not need to do anything. */
				ret = RESULT_NO_CONTACTS;
				goto done;
		}
	}

	/* Make sure, no crap is in the structure: */
	memset(&ci, 0, sizeof(ucontact_info_t));
	/* Get callid of the message */
	ci.callid = &callid;
	/* Get CSeq number of the message */
	ci.cseq = cseq;
	ci.sock = NULL;
	/* additional info from message */
	ci.user_agent = &no_ua;
	ci.last_modified = time(0);

	/* set expire time */
	ci.expires = time(0) + expires;

	/* Now we start looking for the contact: */
	if(((*ul_record)->contacts == 0)
			|| (ul.get_ucontact(*ul_record, &contact_uri, &callid, cseq + 1, &cmatch, &ul_contact)
					!= 0)) {
		if(ul.insert_ucontact(*ul_record, &contact_uri, &ci, &cmatch, 0, &ul_contact) < 0) {
			LM_ERR("failed to insert new contact\n");
			ret = RESULT_ERROR;
			goto done;
		}
	} else {
		if(ul.update_ucontact(*ul_record, ul_contact, &ci, &cmatch, 0) < 0) {
			LM_ERR("failed to update contact\n");
			ret = RESULT_ERROR;
			goto done;
		}
	}
	ul_contact = (*ul_record)->contacts;
	while(ul_contact) {
		if(VALID_CONTACT(ul_contact, time(0)))
			return RESULT_CONTACTS_FOUND;
		ul_contact = ul_contact->next;
	}

	ret = RESULT_NO_CONTACTS;
done:
	pua_reginfo_update_self_op(0);
	return ret;
}

xmlNodePtr xmlGetNodeByName(xmlNodePtr parent, const char *name)
{
	xmlNodePtr cur = parent;
	xmlNodePtr match = NULL;
	while(cur) {
		if(xmlStrcasecmp(cur->name, (unsigned char *)name) == 0)
			return cur;
		match = xmlGetNodeByName(cur->children, name);
		if(match)
			return match;
		cur = cur->next;
	}
	return NULL;
}

char *xmlGetAttrContentByName(xmlNodePtr node, const char *name)
{
	xmlAttrPtr attr = node->properties;
	while(attr) {
		if(xmlStrcasecmp(attr->name, (unsigned char *)name) == 0)
			return (char *)xmlNodeGetContent(attr->children);
		attr = attr->next;
	}
	return NULL;
}

int reginfo_parse_state(char *s)
{
	if(s == NULL) {
		return STATE_UNKNOWN;
	}
	switch(strlen(s)) {
		case 6:
			if(strncmp(s, "active", 6) == 0)
				return STATE_ACTIVE;
			break;
		case 10:
			if(strncmp(s, "terminated", 10) == 0)
				return STATE_TERMINATED;
			break;
		default:
			LM_ERR("Unknown State %s\n", s);
			return STATE_UNKNOWN;
	}
	LM_ERR("Unknown State %s\n", s);
	return STATE_UNKNOWN;
}

int reginfo_parse_event(char *s)
{
	if(s == NULL) {
		return EVENT_UNKNOWN;
	}
	switch(strlen(s)) {
		case 7:
			if(strncmp(s, "created", 7) == 0)
				return EVENT_CREATED;
			if(strncmp(s, "expired", 7) == 0)
				return EVENT_EXPIRED;
			break;
		case 9:
			if(strncmp(s, "refreshed", 9) == 0)
				return EVENT_CREATED;
			break;
		case 10:
			if(strncmp(s, "registered", 10) == 0)
				return EVENT_REGISTERED;
			if(strncmp(s, "terminated", 10) == 0)
				return EVENT_TERMINATED;
			break;
		case 12:
			if(strncmp(s, "unregistered", 12) == 0)
				return EVENT_UNREGISTERED;
			break;
		default:
			LM_ERR("Unknown Event %s\n", s);
			return EVENT_UNKNOWN;
	}
	LM_ERR("Unknown Event %s\n", s);
	return EVENT_UNKNOWN;
}

int process_body(str notify_body, udomain_t *domain)
{
	xmlDocPtr doc = NULL;
	xmlNodePtr doc_root = NULL, registrations = NULL, contacts = NULL,
			   uris = NULL, params = NULL;
	char uri[MAX_URI_SIZE];
	str aor_key = {0, 0};
	str aor = {0, 0};
	str callid = {0, 0};
	str contact = {0, 0};
	str contact_uri = {0, 0};
	str contact_params = {0, 0};
	str param = {0, 0};
	int state = STATE_UNKNOWN, event, expires, result, final_result = RESULT_ERROR;
	char *expires_char = NULL, *cseq_char = NULL,*state_attr = NULL, *event_attr = NULL, *param_name = NULL;
	int cseq = 0;
	int len;
	urecord_t *ul_record = NULL;
	ucontact_t *ul_contact = NULL;
	struct sip_uri parsed_aor;

	/* Temporary */
	int mem_only = 1;

	uri[0] = '\0';
	doc = xmlParseMemory(notify_body.s, notify_body.len);
	if(doc == NULL) {
		LM_ERR("Error while parsing the xml body message, Body is:\n%.*s\n",
				notify_body.len, notify_body.s);
		return -1;
	}
	doc_root = xmlGetNodeByName(doc->children, "reginfo");
	if(doc_root == NULL) {
		LM_ERR("while extracting the reginfo node\n");
		goto error;
	}
	registrations = doc_root->children;
	while(registrations) {
		/* Only process registration sub-items */
		if(xmlStrcasecmp(registrations->name, BAD_CAST "registration") != 0)
			goto next_registration;
		state_attr = xmlGetAttrContentByName(registrations, "state");
		if(state_attr) {
			state = reginfo_parse_state(state_attr);
			xmlFree(state_attr);
			state_attr = NULL;
		}
		if(state == STATE_UNKNOWN) {
			LM_ERR("No state for this contact!\n");
			goto next_registration;
		}
		aor.s = xmlGetAttrContentByName(registrations, "aor");
		if(aor.s == NULL) {
			LM_ERR("No AOR for this contact!\n");
			goto next_registration;
		}
		aor.len = strlen(aor.s);
		LM_DBG("AOR %.*s has state \"%d\"\n", aor.len, aor.s, state);

		/* Get username part of the AOR, search for @ in the AOR. */
		if(parse_uri(aor.s, aor.len, &parsed_aor) < 0) {
			LM_ERR("failed to parse Address of Record (%.*s)\n", aor.len,
					aor.s);
			goto next_registration;
		}

		if(reginfo_use_domain) {
			aor_key.s = uri;
			aor_key.len = strlen(aor_key.s);
		} else {
			aor_key = parsed_aor.user;
		}
		/* Now let's lock that domain for this AOR: */
		ul.lock_udomain(domain, &aor_key);
		/* and retrieve the user-record for this user: */
		result = ul.get_urecord(domain, &aor_key, &ul_record);
		if(result < 0) {
			ul.unlock_udomain(domain, &aor_key);
			LM_ERR("failed to query usrloc (AOR %.*s)\n", aor_key.len,
					aor_key.s);
			goto next_registration;
		}
		/* If no contacts found, then set the ul_record to NULL */
		if(result != 0)
			ul_record = NULL;

		/* If the state is terminated, we just can delete all bindings */
		if(state == STATE_TERMINATED) {
			if(ul_record) {
				ul_contact = ul_record->contacts;
				while(ul_contact) {
					if(mem_only) {
						ul_contact->flags |= FL_MEM;
					} else {
						ul_contact->flags &= ~FL_MEM;
					}
					ul_contact = ul_contact->next;
				}

				if(ul.delete_urecord(domain, &aor_key, ul_record, 0) < 0) {
					LM_ERR("failed to remove record from usrloc\n");
				}

				/* Record deleted, and should not be used anymore */
				ul_record = NULL;


				/* If already a registration with contacts was found, then keep that result.
				   otherwise the result is now "No contacts found" */
				if(final_result != RESULT_CONTACTS_FOUND)
					final_result = RESULT_NO_CONTACTS;
			}
			/* Otherwise, process the content */
		} else {
			/* Now lets process the Contact's from this Registration: */
			contacts = registrations->children;
			while(contacts) {
				if(xmlStrcasecmp(contacts->name, BAD_CAST "contact") != 0)
					goto next_contact;
				callid.len = 0;
				callid.s = xmlGetAttrContentByName(contacts, "callid");
				// If no CallID present, try the "id"-field instead:
				if(callid.s == NULL)
					callid.s = xmlGetAttrContentByName(contacts, "id");
				callid.len = strlen(callid.s);
				LM_DBG("Got Call-ID \"%.*s\"\n", callid.len, callid.s);

				event_attr = xmlGetAttrContentByName(contacts, "event");
				if(event_attr == NULL) {
					LM_ERR("No event for this contact!\n");
					goto next_contact;
				}
				event = reginfo_parse_event(event_attr);
				xmlFree(event_attr);
				event_attr = NULL;
				if(event == EVENT_UNKNOWN) {
					LM_ERR("No event for this contact!\n");
					goto next_contact;
				}
				expires_char = xmlGetAttrContentByName(contacts, "expires");
				if(expires_char == NULL) {
					LM_ERR("No expires for this contact!\n");
					goto next_contact;
				}
				expires = atoi(expires_char);
				xmlFree(expires_char);
				expires_char = NULL;				
				if(expires < 0) {
					LM_ERR("No valid expires for this contact!\n");
					goto next_contact;
				}
				LM_DBG("%.*s: Event \"%d\", expires %d\n", callid.len, callid.s,
						event, expires);

				cseq_char = xmlGetAttrContentByName(contacts, "cseq");
				if(cseq_char == NULL) {
					LM_DBG("No cseq for this contact!\n");
				} else {
					cseq = atoi(cseq_char);
					if(cseq < 0) {
						LM_WARN("No valid cseq for this contact!\n");
					}
				}
				xmlFree(cseq_char);
				cseq_char = NULL;

				// <unknown-param name="+g.oma.sip-im"></unknown-param>
				// <unknown-param name="language">en,fr</unknown-param>
				/* 1) Iterate through the params, to get the full content-length */
				params = contacts->children;
				contact_params.len = 0;
				len = 0;
				while(params) {
					if(xmlStrcasecmp(params->name, BAD_CAST "unknown-param")
							!= 0)
						goto next_param;
					param_name = xmlGetAttrContentByName(params, "name");
					len += 1 /* ; */ + strlen(param_name);
					param.s = (char *)xmlNodeGetContent(params);
					param.len = strlen(param.s);
					if(param.len > 0)
						len += 1 /* = */ + param.len;
					xmlFree(param_name);
					param_name = NULL;
					xmlFree(param.s);
					param.s = NULL;
					param.len = 0;

				next_param:
					params = params->next;
				}
				LM_DBG("Calculated length for params: %i\n", len);

				if(len > 0) {
					len += 1;
					if(contact_params.s)
						pkg_free(contact_params.s);
					contact_params.s = pkg_malloc(len);
					params = contacts->children;
					contact_params.len = 0;
					while(params) {
						if(xmlStrcasecmp(params->name, BAD_CAST "unknown-param")
								!= 0)
							goto next_param2;

						param_name = xmlGetAttrContentByName(params, "name");
						contact_params.len += snprintf(
								contact_params.s + contact_params.len,
								len - contact_params.len, ";%s", param_name);
						param.s = (char *)xmlNodeGetContent(params);
						param.len = strlen(param.s);
						if(param.len > 0)
							contact_params.len += snprintf(
									contact_params.s + contact_params.len,
									len - contact_params.len, "=%.*s",
									param.len, param.s);

						LM_DBG("Contact params are: %.*s\n", contact_params.len,
								contact_params.s);

						xmlFree(param_name);
						param_name = NULL;
						xmlFree(param.s);
						param.s = NULL;
						param.len = 0;

					next_param2:
						params = params->next;
					}
					LM_DBG("Contact params are: %.*s\n", contact_params.len,
							contact_params.s);
				}

				/* Now lets process the URI's from this Contact: */
				uris = contacts->children;
				while(uris) {
					if(xmlStrcasecmp(uris->name, BAD_CAST "uri") != 0)
						goto next_uri;
					contact_uri.s = (char *)xmlNodeGetContent(uris);
					if(contact_uri.s == NULL) {
						LM_ERR("No URI for this contact!\n");
						goto next_registration;
					}
					contact_uri.len = strlen(contact_uri.s);
					if(contact_params.len > 0) {
						if(contact.s)
							pkg_free(contact.s);
						contact.s = (char *)pkg_malloc(
								contact_uri.len + contact_params.len + 1);
						contact.len = snprintf(contact.s,
								contact_uri.len + contact_params.len + 1,
								"%.*s%.*s", contact_uri.len, contact_uri.s,
								contact_params.len, contact_params.s);
						LM_DBG("Contact: %.*s\n", contact.len, contact.s);

						/* Add to Usrloc: */
						result = process_contact(domain, &ul_record, aor_key,
								callid, cseq, expires, event, contact);
					} else {
						LM_DBG("Contact: %.*s\n", contact_uri.len,
								contact_uri.s);

						/* Add to Usrloc: */
						result = process_contact(domain, &ul_record, aor_key,
								callid, cseq, expires, event, contact_uri);
					}

					/* Process the result */
					if(final_result != RESULT_CONTACTS_FOUND)
						final_result = result;

					xmlFree(contact_uri.s);
					contact_uri.s = NULL;
					contact_uri.len = 0;						
				next_uri:
				
					uris = uris->next;
				}
			next_contact:
				if(callid.s) {
					xmlFree(callid.s);
					callid.s = NULL;
					callid.len = 0;
				}			
				contacts = contacts->next;
			}
		}
	next_registration:
		if(ul_record)
			ul.release_urecord(ul_record, 0);
		/* Unlock the domain for this AOR: */
		if(aor_key.len > 0)
			ul.unlock_udomain(domain, &aor_key);
		if(callid.s) {
			xmlFree(callid.s);
			callid.s = NULL;
			callid.len = 0;
		}
		if(aor.s) {
			xmlFree(aor.s);
			aor.s = NULL;
			aor.len = 0;
		}
		registrations = registrations->next;
	}
error:
	if(contact.s)
		pkg_free(contact.s);
	if(contact_params.s)
		pkg_free(contact_params.s);
	/* Free the XML-Document */

	if(doc)
		xmlFreeDoc(doc);
	return final_result;
}

int reginfo_handle_notify(struct sip_msg *msg, char *domain, char *s2)
{
	str body;
	int result = 1;

	/* If not done yet, parse the whole message now: */
	if(parse_headers(msg, HDR_EOH_F, 0) == -1) {
		LM_ERR("Error parsing headers\n");
		return -1;
	}
	if(get_content_length(msg) == 0) {
		LM_DBG("Content length = 0\n");
		/* No Body? Then there is no published information available, which is ok. */
		return 1;
	} else {
		if (get_body(msg, &body) != 0) {
			LM_ERR("cannot extract body from msg\n");
			return -1;
		}
	}

	LM_DBG("Body is %.*s\n", body.len, body.s);

	result = process_body(body, (udomain_t *)domain);

	return result;
}
