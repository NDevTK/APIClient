/* XMLHttpRequest — the XMLHttpRequest Standard §3. See xml_http_request.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_XHR_XML_HTTP_REQUEST_H
#define ENGINE_HOST_BROWSER_CORE_XHR_XML_HTTP_REQUEST_H

#include "quickjs.h"

/* §3, §5's neighbours and §5 itself, declared once for the AGENT: the three classes, the member pool entries
   and the per-realm prototype entry. ProgressEvent is declared from here for the reason fetch_init declares
   Headers/Response/Request: §5 is part of this standard and every event this component fires is one, so a host
   that installed XMLHttpRequest without it would have a component whose events had no interface. */
void xhr_init(JSContext *ctx);
/* XHR §3's three prototypes, their Web IDL §3.7.1 interface objects and the §3.8 property references for
   `XMLHttpRequestEventTarget`, `XMLHttpRequestUpload` and `XMLHttpRequest` — for ONE realm, declared into
   core/realm.h's list. ONE entry because Web IDL §3.8 `define the global property references` is given
   "target" and "a realm realm" and its step 1 population is "every interface that is exposed in realm": no
   Document appears in it. XHR §3 declares all three `[Exposed=(Window,DedicatedWorker,SharedWorker)]`, so a
   worker realm is owed all three — and while the interface objects were installed from core/platform.c's
   per-document column, such a realm reaches no platform_document_install and received none of them.
   §5's prototype and interface object are NOT this entry's: progress_event.c declares an intrinsic of its own
   and now places `ProgressEvent` from it, so the per-document half that used to call progress_event_install
   at its end is gone rather than moved. */
void xhr_install_realm(JSContext *ctx);
void xhr_free(JSRuntime *rt);

#endif
