/* AbortController / AbortSignal — DOM §3.2. See abort.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_ABORT_H
#define ENGINE_HOST_BROWSER_CORE_DOM_ABORT_H
#include <stdbool.h>

#include "quickjs.h"

void abort_init(JSContext *ctx);                                 /* the private slot key (install time) */
void abort_install(JSContext *ctx, JSValueConst global);         /* AbortController + AbortSignal */
void abort_free(JSContext *ctx);                                 /* release the slot key this component owns */

/* A FRESH, UNABORTED SIGNAL, and the STATE half of "signal abort" on one.
 *
 * Streams §5.4 gives every WritableStreamDefaultController an AbortController whose signal the page reads as
 * `controller.signal`, and §5.2's abort() signals it. That is the DOM's AbortSignal and not a second one — a
 * page passes `controller.signal` straight to `fetch`, so anything else would be an object that looks like one.
 *
 * The abort is SPLIT the way §3.2 splits it: this sets the flag and the reason and answers whether the caller
 * must now FIRE `abort` at the signal, and the fire is a §2.9 dispatch that runs the page's listeners, so it
 * belongs to a caller that can park. `reason` is CONSUMED. */
JSValue abort_signal_new(JSContext *ctx);
bool    abort_signal_state(JSContext *ctx, JSValueConst sig, JSValue reason);

#endif
