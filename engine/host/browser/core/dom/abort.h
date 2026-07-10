/* AbortSignal — Blink core/dom/AbortSignal, defined by its Web IDL (see abort.c). The static factories
 * AbortSignal.timeout(ms) / .any([...]) / .abort(reason) each return an AbortSignal. */
#ifndef ENGINE_HOST_BROWSER_ABORT_H
#define ENGINE_HOST_BROWSER_ABORT_H
#include "quickjs.h"
JSValue js_abortsignal_make(JSContext *ctx, JSValueConst t, int c, JSValueConst *v);
JSValue js_abortcontroller_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv);   /* new AbortController() -> {signal, abort} */
#endif
