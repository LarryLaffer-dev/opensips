---
title: "lost Module"
description: "The *lost* module is a client for the two HTTP-based protocols an Emergency-CSCF needs in order to route an emergency call: HELD and LoST."
---

## Admin Guide


### Overview


The *lost* module is a client for the two HTTP-based protocols an
Emergency-CSCF needs in order to route an emergency call:


- *HELD* (HTTP Enabled Location Delivery, RFC 6155) to ask a Location
Information Server (LIS) where the caller is, and to dereference a
location URI (RFC 6753).
- *LoST* (Location-to-Service Translation, RFC 5222) to ask an Emergency
Call Routing Function (ECRF) which PSAP serves that location for a given
service URN (RFC 5031).


The caller location is taken from the PIDF-LO carried by the request:
either by value from a *multipart/related* body part referenced by a
*Geolocation: &lt;cid:...&gt;* header, which is what a 3GPP UE sends (see
TS 24.229), or by reference from a *Geolocation: &lt;https://...&gt;*
header, or passed directly as a function parameter.


All HTTP work is delegated to the *rest_client* module, so TLS,
connection reuse and timeouts behave exactly as they do there.


### Asynchronous Operation


Every exported function is available both as a regular function and as an
async one. Since an emergency call needs two serial round trips to external
servers, running them synchronously blocks a SIP worker for the entire
duration of the exchange. Prefer the async flavour:


```opensips title="Asynchronous location retrieval and mapping"
...
route[EMERGENCY] {
	async(lost_held_query("lis", $var(pidf), $var(locuri), $var(err)),
		LOC_DONE);
}

route[LOC_DONE] {
	async(lost_query("ecrf", $var(psap), $var(psapname), $var(err),
		$var(pidf)), MAP_DONE);
}

route[MAP_DONE] {
	$ru = $var(psap);
	t_relay();
}
...
```


Only the first HTTP exchange of a function call is asynchronous. Two
secondary exchanges are always performed synchronously, because both are
avoidable in practice:


- the dereference of a *locationURI* when a HELD *locationResponse*
contains no *presence* element - dereference the returned URI yourself with
an async [lost_held_dereference()](#lost_held_dereferenceurl-pidf-err-rtime-rtype)
instead;
- following a LoST redirect, which does not happen with the default
[recursion](#recursion-integer) setting, since the mapping server then
resolves the chain itself.


### Return Codes


All functions return a positive value on success and a negative one on
failure, so they can be tested directly in a condition. The magnitude
carries the detail and is available in *$rc*:


- *200* - success. [lost_held_dereference()](#lost_held_dereferenceurl-pidf-err-rtime-rtype)
refines this to *201* when the response held a location reference, *202* a
location value, and *203* both.
- *-400* - the exchange failed locally: bad parameters, no location found,
transport error, or a non-2xx HTTP status.
- *-500* - the LIS or ECRF answered with a protocol error response. The
error code or type is written to the *err* output variable.


Note that the output variables are only written when the exchange
completed, so a script must check the return code before using them.


### Dependencies


#### OpenSIPS Modules


The following modules must be loaded before this module:


- *rest_client* - performs all HTTP transfers.
- *tls_mgm* - only when a connection uses the *tls_dom* property.


#### External Libraries or Applications


The following libraries or applications must be installed before
running OpenSIPS with this module loaded:


- *libxml2*.


### Exported Parameters


#### connection (string)


Defines a named LIS or ECRF server, which the exported functions then
reference by name. The format is:


```
name=>url=<url>[;timeout=<seconds>][;tls_dom=<domain>]
```


- *url* (mandatory) - the HTTP(S) endpoint.
- *timeout* - maximum duration of a transfer, in seconds. Defaults to the
*rest_client* *curl_timeout* setting.
- *tls_dom* - a *tls_mgm* client domain, for server verification and
client certificates (mutual TLS).


The parameter may be set multiple times. Names must be unique.


```opensips title="Set connection parameter"
...
modparam("lost", "connection", "lis=>url=https://lis.example.org/held;timeout=3;tls_dom=ecscf")
modparam("lost", "connection", "ecrf=>url=https://ecrf.example.org/lost;timeout=3;tls_dom=ecscf")
...
```


#### location_type (string)


The *locationType* requested from the LIS. Any combination of "civic",
"geodetic", "locationURI" and "any", separated by spaces.


*Default value is "geodetic locationURI".*


```opensips title="Set location_type parameter"
...
modparam("lost", "location_type", "civic geodetic")
...
```


#### exact_type (integer)


When set to 1, the *locationRequest* asks for an exact match of
[location_type](#location_type-string) (*exact="true"*), meaning the LIS
must return every requested type or fail.


*Default value is 0.*


```opensips title="Set exact_type parameter"
...
modparam("lost", "exact_type", 1)
...
```


#### response_time (integer)


The *responseTime* of a *locationRequest*, in milliseconds. A value of 0
requests *emergencyRouting*, i.e. the fastest answer the LIS can give.


*Default value is 0.*


```opensips title="Set response_time parameter"
...
modparam("lost", "response_time", 500)
...
```


#### post_request (integer)


When set to 1, a location URI is dereferenced with a HELD
*locationRequest* POST instead of a plain GET.


*Default value is 0.*


```opensips title="Set post_request parameter"
...
modparam("lost", "post_request", 1)
...
```


#### geoheader_type (integer)


Which kind of *Geolocation* header value to use:


- *0* - any (first one found)
- *1* - *cid:* only, i.e. location by value from the message body
- *2* - HTTP(S) URI only, i.e. location by reference


*Default value is 0.*


```opensips title="Set geoheader_type parameter"
...
modparam("lost", "geoheader_type", 1)
...
```


#### geoheader_order (integer)


Which *Geolocation* header to prefer when the request carries more than
one: 0 for the last, 1 for the first.


*Default value is 0.*


```opensips title="Set geoheader_order parameter"
...
modparam("lost", "geoheader_order", 1)
...
```


#### geoheader_incl_alt (integer)


When set to 1, altitude is kept in the location sent to the mapping
server. Most ECRFs only map latitude and longitude.


*Default value is 0.*


```opensips title="Set geoheader_incl_alt parameter"
...
modparam("lost", "geoheader_incl_alt", 1)
...
```


#### location_profile (integer)


Which location profile to send in a *findService* request when the PIDF-LO
holds both a civic and a geodetic location (RFC 5222 §8.3):


- *0* - both, geodetic preferred
- *1* - both, civic preferred
- *2* - geodetic only
- *3* - civic only


*Default value is 0.*


```opensips title="Set location_profile parameter"
...
modparam("lost", "location_profile", 2)
...
```


#### recursion (integer)


Sets *recursive="true"* on a *findService* request, asking the mapping
server to resolve any referral itself. With 0, the module follows redirects
on its own, synchronously.


*Default value is 1.*


```opensips title="Set recursion parameter"
...
modparam("lost", "recursion", 0)
...
```


#### verbose (integer)


When set to 1, the parsed contents of every *findServiceResponse* are
written to the log.


*Default value is 0.*


```opensips title="Set verbose parameter"
...
modparam("lost", "verbose", 1)
...
```


### Exported Functions


#### lost_query(con, uri, name, err[, pidf[, urn]])


Sends a LoST *findService* request to a mapping server and returns the URI
of the serving PSAP.


Parameters:


- *con* (string) - name of a [connection](#connection-string). If no such
connection exists, the value is treated as a domain and resolved via a
*LoST:https* or *LoST:http* NAPTR lookup.
- *uri* (var) - output: the PSAP URI.
- *name* (var) - output: the PSAP display name.
- *err* (var) - output: the LoST error type, if the mapping server
returned an error response.
- *pidf* (string, optional) - the PIDF-LO to map. When absent, it is taken
from the *Geolocation* header, as controlled by
[geoheader_type](#geoheader_type-integer).
- *urn* (string, optional) - the service URN to map. Defaults to the
Request URI.


See [Return Codes](#return-codes).


This function can be used from any route. An async variant is available.


```opensips title="lost_query usage"
...
if (!lost_query("ecrf", $var(psap), $var(name), $var(err))) {
	xlog("L_ERR", "LoST failed: $var(err)\n");
	send_reply(503, "Service Unavailable");
	exit;
}
$ru = $var(psap);
...
```


#### lost_held_query(con, pidf, url, err[, id])


Sends a HELD *locationRequest* to a LIS and returns the caller location.


Parameters:


- *con* (string) - name of a [connection](#connection-string). When it
does not name a connection, the LIS is discovered with a *LIS:HELD* NAPTR
lookup on the device identity.
- *pidf* (var) - output: the returned PIDF-LO.
- *url* (var) - output: the returned location URI, if the response carries
a *locationUriSet*.
- *err* (var) - output: the HELD error code, if the LIS returned an error
response.
- *id* (string, optional) - the device identity to ask about. Defaults to
*P-Asserted-Identity*, then *From*.


See [Return Codes](#return-codes).


This function can be used from any route. An async variant is available.


```opensips title="lost_held_query usage"
...
if (!lost_held_query("lis", $var(pidf), $var(locuri), $var(err))) {
	xlog("L_ERR", "HELD failed: $var(err)\n");
}
...
```


#### lost_held_dereference(url, pidf, err[, rtime[, rtype]])


Dereferences a location URI (RFC 6753) and returns the PIDF-LO found
there.


Parameters:


- *url* (string) - the location URI to dereference.
- *pidf* (var) - output: the returned PIDF-LO.
- *err* (var) - output: the HELD error code, if the server returned an
error response.
- *rtime* (string, optional) - the *responseTime*: a number of
milliseconds, or one of "emergencyRouting" and "emergencyDispatch".
Defaults to "emergencyRouting".
- *rtype* (string, optional) - the *locationType* to request, overriding
[location_type](#location_type-string).


See [Return Codes](#return-codes).


This function can be used from any route. An async variant is available.


```opensips title="lost_held_dereference usage"
...
if (!lost_held_dereference($var(locuri), $var(pidf), $var(err),
		"emergencyRouting", "geodetic")) {
	xlog("L_ERR", "dereference failed: $var(err)\n");
}
...
```


### Exported Statistics


#### held_requests


Number of HELD requests sent to a LIS.


#### held_failures


Number of HELD requests that failed at the transport level or returned a
non-2xx status.


#### lost_requests


Number of LoST requests sent to a mapping server.


#### lost_failures


Number of LoST requests that failed at the transport level or returned a
non-2xx status.


### Credits


This module was ported from Kamailio, where it was written and is
maintained by Wolfgang Kampichler (DEC112, FREQUENTIS AG).
<!-- CONTRIBUTORS -->

### License

All documentation files (i.e. .md extension) are licensed under the Creative Common License 4.0
