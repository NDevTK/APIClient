/* XMLHttpRequest — the XMLHttpRequest Standard §3. See xml_http_request.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_XHR_XML_HTTP_REQUEST_H
#define ENGINE_HOST_BROWSER_CORE_XHR_XML_HTTP_REQUEST_H

#include "quickjs.h"

/* §3, §5's neighbours and §5 itself, declared once for the AGENT: the three classes, the member pool entries
   and the per-realm prototype entry. ProgressEvent is declared from here for the reason fetch_init declares
   Headers/Response/Request: §5 is part of this standard and every event this component fires is one, so a host
   that installed XMLHttpRequest without it would have a component whose events had no interface. */
void xhr_init(JSContext *ctx);
/* §3's, §3's upload's and §5's PROTOTYPES FOR ONE REALM. */
void xhr_install_protos(JSContext *ctx);
/* The four interface objects: XMLHttpRequestEventTarget, XMLHttpRequestUpload, XMLHttpRequest, ProgressEvent. */
void xhr_install(JSContext *ctx, JSValueConst global);
void xhr_free(JSRuntime *rt);

#endif
