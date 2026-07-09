/* Web IDL binding driver — see idl.h. A declarative IDL member table becomes a native object: operations are
 * functions, opaque attributes are getters returning the concolic unknown. A headless-unknown readonly
 * attribute still EXISTS and has its type (the IDL declares it), but its VALUE forks — spec-locked shape,
 * honest-unknown value. The interface reads as its IDL, never hand-assembled property-by-property. */
#include <stdio.h>
#include "idl.h"
#include "opaque.h"   /* js_concolic — an opaque attribute reads as a name-tagged concolic (forks + provenance) */

/* Install an IDL member onto `target` (a plain instance or a class prototype): a method is a native function,
   an opaque attribute is a concolic tagged by the ATTRIBUTE NAME (aborted -> {aborted}) so it forks AND names
   its provenance, never a generic {} or {idlAttr}. */
static void idl_put_member(JSContext *ctx, JSValueConst target, const IDLMember *m) {
    if (m->kind == IDL_METHOD) {
        JS_SetPropertyStr(ctx, target, m->name, JS_NewCFunction(ctx, m->fn, m->name, m->length));
    } else {
        char shape[80]; snprintf(shape, sizeof shape, "{%s}", m->name);
        JS_SetPropertyStr(ctx, target, m->name, js_concolic(ctx, shape, JS_UNDEFINED));
    }
}

JSValue idl_instance(JSContext *ctx, const IDLMember *members, int n) {
    JSValue o = JS_NewObject(ctx);
    for (int i = 0; i < n; i++) idl_put_member(ctx, o, &members[i]);
    return o;
}

JSClassID idl_define_class(JSContext *ctx, const IDLInterface *iface) {
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSClassID id = 0;
    JS_NewClassID(rt, &id);
    JSClassDef def = { iface->name, .finalizer = iface->finalizer };
    JS_NewClass(rt, id, &def);
    JSValue proto = JS_NewObject(ctx);                      /* the member table IS the prototype */
    for (int i = 0; i < iface->n; i++) idl_put_member(ctx, proto, &iface->members[i]);
    JS_SetClassProto(ctx, id, proto);
    return id;
}
