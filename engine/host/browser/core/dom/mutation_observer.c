/* MutationObserver — see mutation_observer.h. Generated shape (idl.gen.h) merged with behavior: callback -> driven flow;
 * observe/unobserve/disconnect/takeRecords are deliberate noops (declared so strict binding does not DCHECK). */
#include "core/dom/mutation_observer.h"
#include "core/dom/idl.gen.h"   /* MutationObserver_IDL */
#include "bindings/idl.h"
#include "opaque.h"   /* js_noop */

extern JSValue js_add_listener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv);

static const IdlImpl MUTATION_OBSERVER_IMPLS[] = {
    { "observe", NULL, NULL, js_noop, -1 }, { "unobserve", NULL, NULL, js_noop, -1 },
    { "disconnect", NULL, NULL, js_noop, -1 }, { "takeRecords", NULL, NULL, js_noop, -1 },
};

JSValue js_mutation_observer_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    (void)nt;
    if (argc >= 1 && JS_IsFunction(ctx, argv[0])) { JSValue r = js_add_listener(ctx, JS_UNDEFINED, 1, argv); JS_FreeValue(ctx, r); }
    JSValue o = JS_NewObject(ctx);
    idl_bind(ctx, o, "MutationObserver", MutationObserver_IDL, MutationObserver_IDL_N, MUTATION_OBSERVER_IMPLS,
             (int)(sizeof MUTATION_OBSERVER_IMPLS / sizeof MUTATION_OBSERVER_IMPLS[0]), /*attrs*/1, /*strict*/1);
    return o;
}
