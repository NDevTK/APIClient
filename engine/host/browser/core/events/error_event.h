/* ErrorEvent — HTML "Interface ErrorEvent". See error_event.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_ERROR_EVENT_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_ERROR_EVENT_H
#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"

void error_event_init(JSContext *ctx);              /* the slot key + the IDL declarations (agent init) */
void error_event_install_proto(JSContext *ctx);     /* §3.7: one prototype per REALM */
void error_event_install(JSContext *ctx, JSValueConst global);
void error_event_free(JSContext *ctx);
/* `ErrorEvent.prototype` for this realm. OWNED: the caller frees. */
JSValue error_event_proto(JSContext *ctx);

/* AN ErrorEvent THE ENGINE FIRES, carrying the error information. TRUSTED — the engine fired it, not the page.
 * Every JSValue argument is BORROWED; the result is a new owned ErrorEvent.
 *
 * THE TYPE AND THE CANCELABILITY BELONG TO THE FIRE AND NOT TO THE INTERFACE, which is why they are arguments.
 * Two algorithms dispatch this interface and they disagree on both: HTML §8.1.4.6's report-an-exception step 6.2
 * fires `error` "with the cancelable attribute initialized to true", because an `onerror` returning true is how
 * a page says it HANDLED the exception; HTML §7.2.6.8's abort-a-NavigateEvent step 6 fires `navigateerror` with
 * DOM's plain fire-an-event and no flags at all, because nothing branches on its result. Hardcoding the first
 * caller's pair here is what made the second one unable to reach this constructor. It does not BUBBLE in either
 * case, and it is never composed. */
JSValue error_event_new(JSContext *ctx, const char *type, bool cancelable, JSValueConst message,
                        JSValueConst filename, uint32_t lineno, uint32_t colno, JSValueConst error);

#endif
