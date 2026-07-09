/* Observers — see observer.h. Extracted from main.c. The callback fires on intersection/mutation/resize, none
 * of which exist headless — but the callback IS page code, so register it as a scheduler FLOW (the attacker
 * session + orphan-driving fire it), and return the OPAQUE object so any property/method access is the honest
 * unknown rather than a fixed-shape stub the page's JS wouldn't expect. */
#include "observer.h"
#include "opaque.h"   /* g_opaque */

extern JSValue js_add_listener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv);   /* scheduler-side: register the callback as a driven flow */

JSValue js_observer_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    (void)nt;
    if (argc >= 1 && JS_IsFunction(ctx, argv[0])) { JSValue r = js_add_listener(ctx, JS_UNDEFINED, 1, argv); JS_FreeValue(ctx, r); }
    return js_concolic(ctx, "{observer}", JS_UNDEFINED);   /* .observe/.disconnect/.takeRecords + any read -> opaque (no fixed-shape stub) */
}
