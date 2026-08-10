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

/* HTML §8.1.4.6 step 5.2's event: `error`, CANCELABLE, carrying the error information. TRUSTED — the engine
   fired it, not the page. Every argument is BORROWED; the result is a new owned ErrorEvent. */
JSValue error_event_new(JSContext *ctx, JSValueConst message, JSValueConst filename,
                        uint32_t lineno, uint32_t colno, JSValueConst error);

#endif
