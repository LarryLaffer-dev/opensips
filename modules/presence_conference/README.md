---
title: "Presence_Conference Module"
description: "Serves the SIP conference event package (RFC 4575) for an IMS conference focus, sourcing the participant roster live from a FreeSWITCH MRF over ESL."
---

## Admin Guide


### Overview


This module serves the SIP conference event package defined in RFC 4575. It
answers `SUBSCRIBE` requests carrying `Event: conference` and sends `NOTIFY`
bodies of type `application/conference-info+xml` describing the live
participant roster of a conference.

It is meant to run on an IMS MMTel-AS acting as the conference focus
(RFC 4579 section 4.4, 3GPP TS 24.147). The media mixing itself is done by a
FreeSWITCH MRF; this module reads the roster from the mixer over the Event
Socket Library, through the [freeswitch](../freeswitch/README.md) module.


### In-dialog subscriptions


A VoLTE UE does not open a separate dialog to watch a conference. It
subscribes *in-dialog*, reusing the focus INVITE dialog as RFC 4575 section 3
and RFC 5057 allow: the `SUBSCRIBE` carries the To-tag the MRF minted when it
answered the conference INVITE, and loose-routes back through the MMTel-AS
record-route.

That is why this module exists rather than a notifier plugged into the
[presence](../presence/README.md) engine. The presence engine only serves
subscriptions on dialogs it created itself; it cannot adopt a dialog owned by
the MRF, and answers 481 to these SUBSCRIBEs.

Instead, the notifier is built directly on the dialog the MMTel-AS already
tracks for the conference. `conference_subscribe()` accepts the in-dialog
SUBSCRIBE and emits the roster NOTIFY through the
[dialog](../dialog/README.md) module's in-dialog request support, which
supplies the route set, remote target and CSeq from the stored dialog.
`conference_notify()` rebuilds the roster and fans a fresh NOTIFY out to every
watcher when the mixer membership changes.

Watcher state is held in shared memory, keyed by conference id. Each entry
references the focus dialog and is dropped when that dialog is destroyed, by
BYE or by timeout. No database is involved.


### Privacy


Participants whose caller identity is restricted (CLIR, TS 24.147 and
TS 24.607) are anonymised in the roster: the entity becomes
`sip:anonymous@anonymous.invalid` and no display name is emitted.

Detection is heuristic, based on the caller identity FreeSWITCH renders for the
leg, because the mixer roster is the only view of the participant this module
has.


### Dependencies


#### OpenSIPS Modules


The following modules must be loaded before this module:


- *dialog*.
- *tm*.
- *freeswitch*.


#### External Libraries or Applications


- *libxml2*.


### Exported Parameters


#### mrf_esl_url (string)


ESL URL of the FreeSWITCH MRF whose mixer holds the conferences, in the form
`fs://[[user]:password@]host[:port]`.

The module runs `conference xml_list` against it and picks the conference whose
name matches the id in the focus URI, comparing only the part before any `@`.
That makes it tolerant of FreeSWITCH naming the conference `<id>`,
`<id>@default` or `<id>@<domain>`.

If left unset, every roster is served empty.


*Default value is empty.*


```opensips title="Set mrf_esl_url parameter"
...
modparam("presence_conference", "mrf_esl_url", "fs://:ClueCon@mrf-1:8021")
...
```


#### conf_user_prefix (string)


The user-part prefix that marks a URI as a conference focus, as in
`sip:conf=<id>@conf-factory.example.org`. The conference id is the user part
with this prefix removed.


*Default value is "conf=".*


```opensips title="Set conf_user_prefix parameter"
...
modparam("presence_conference", "conf_user_prefix", "conference-")
...
```


#### default_expires (integer)


Subscription lifetime, in seconds, used when the SUBSCRIBE carries no
`Expires` header.


*Default value is 3600.*


```opensips title="Set default_expires parameter"
...
modparam("presence_conference", "default_expires", 1800)
...
```


### Exported Functions


#### conference_subscribe(presentity)


Accepts an in-dialog conference event package SUBSCRIBE on the focus dialog and
sends the initial or refresh roster NOTIFY.

*presentity* is the conference focus URI, normally the SUBSCRIBE Request-URI.

The script must have run `loose_route()` and `t_newtran()` before calling this,
the latter so that UDP retransmissions are absorbed by the transaction rather
than producing a second NOTIFY.

An `Expires: 0` refresh sends a terminating NOTIFY and removes the watcher.

This function can be used from REQUEST_ROUTE.


```opensips title="conference_subscribe() usage"
...
	if (is_method("SUBSCRIBE") && $hdr(Event) == "conference") {
		t_newtran();
		conference_subscribe($ru);
		exit;
	}
...
```


#### conference_notify(presentity)


Rebuilds the roster for the given focus presentity URI from the MRF mixer and
sends a NOTIFY to every active watcher of that conference.

Intended to be driven by the FreeSWITCH `conference::maintenance` event, which
the [freeswitch_scripting](../freeswitch_scripting/README.md) module surfaces
as `E_FREESWITCH`.

This function can be used from any route.


```opensips title="conference_notify() usage"
...
event_route[E_FREESWITCH] {
	if ($param(name) == "CUSTOM conference::maintenance")
		conference_notify("sip:conf=" + $json(body/Conference-Name) +
			"@conf-factory.example.org");
}
...
```
