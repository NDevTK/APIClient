/* IntersectionObserver — see intersection_observer.h. The SHAPE is generated from canonical IntersectionObserver
 * IDL (idl.gen.h in this folder); the component supplies BEHAVIOR: the ctor registers the callback as a driven
 * flow (it never fires from a real intersection headless), and observe/unobserve/disconnect/takeRecords are
 * DELIBERATE noops (declared so the strict binding does not DCHECK them). root/rootMargin/thresholds/... read
 * as the concolic unknown — spec-typed, value-forks. */
#include "core/intersection_observer/intersection_observer.h"
#include "core/intersection_observer/idl.gen.h"   /* IntersectionObserver_IDL — generated member shape */
#include "bindings/idl.h"
#include "opaque.h"   /* js_noop */

extern JSValue js_add_listener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv);   /* register the callback as a driven flow */

/* Deliberate noops (the callback is orphan-driven, not fired by a real intersection) — declared so strict
   binding treats them as intentional, not an unbuilt gap; every other spec operation would DCHECK-on-call. */
static const IdlImpl INTERSECTION_OBSERVER_IMPLS[] = {
    { "observe",     NULL, NULL, js_noop, -1 },
    { "unobserve",   NULL, NULL, js_noop, -1 },
    { "disconnect",  NULL, NULL, js_noop, -1 },
    { "takeRecords", NULL, NULL, js_noop, -1 },
};

JSValue js_intersection_observer_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    (void)nt;
    if (argc >= 1 && JS_IsFunction(ctx, argv[0])) { JSValue r = js_add_listener(ctx, JS_UNDEFINED, 1, argv); JS_FreeValue(ctx, r); }
    JSValue o = JS_NewObject(ctx);
    idl_bind(ctx, o, "IntersectionObserver", IntersectionObserver_IDL, IntersectionObserver_IDL_N,
             INTERSECTION_OBSERVER_IMPLS, (int)(sizeof INTERSECTION_OBSERVER_IMPLS / sizeof INTERSECTION_OBSERVER_IMPLS[0]),
             /*install_attrs*/1, /*strict*/1);
    return o;
}
