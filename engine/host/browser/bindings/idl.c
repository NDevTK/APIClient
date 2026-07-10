/* Web IDL binding driver — see idl.h. A declarative IDL member table becomes a native object: operations are
 * functions, opaque attributes are getters returning the concolic unknown. A headless-unknown readonly
 * attribute still EXISTS and has its type (the IDL declares it), but its VALUE forks — spec-locked shape,
 * honest-unknown value. The interface reads as its IDL, never hand-assembled property-by-property. */
#include <stdio.h>
#include <string.h>
#include "bindings/idl.h"
#include "solver/opaque.h"   /* js_concolic (opaque attr value) + js_noop (a spec-present, unmodelled operation) */
#include "check.h"    /* DCHECK — this narrow driver is where EVERY interface is born; a silent failure here corrupts all */

/* Install an IDL member onto `target` (a plain instance or a class prototype): a method is a native function,
   an opaque attribute is a concolic tagged by the ATTRIBUTE NAME (aborted -> {aborted}) so it forks AND names
   its provenance. A readonly IDL attribute is NON-WRITABLE/NON-CONFIGURABLE (the page cannot `signal.aborted=1`
   or `delete` it), enumerable like a real reflected attribute. */
static void idl_put_member(JSContext *ctx, JSValueConst target, const IDLMember *m) {
    JSAtom a = JS_NewAtom(ctx, m->name);
    if (m->kind == IDL_METHOD) {
        JS_DefinePropertyValue(ctx, target, a, JS_NewCFunction(ctx, m->fn, m->name, m->length), JS_PROP_C_W_E);
    } else {
        char shape[80]; snprintf(shape, sizeof shape, "{%s}", m->name);
        JS_DefinePropertyValue(ctx, target, a, js_concolic(ctx, shape, JS_UNDEFINED), JS_PROP_ENUMERABLE);   /* readonly: no WRITABLE/CONFIGURABLE */
    }
    JS_FreeAtom(ctx, a);
}

JSValue idl_instance(JSContext *ctx, const IDLMember *members, int n) {
    JSValue o = JS_NewObject(ctx);
    DCHECK(!JS_IsException(o), "idl_instance: object alloc failed");
    for (int i = 0; i < n; i++) idl_put_member(ctx, o, &members[i]);
    return o;
}

/* (The runtime shape-driver idl_bind was REMOVED — installing concolic/noop stubs for unmodelled members is the
   banned-stub anti-pattern; bindings are now GENERATED as C by engine/idlgen.mjs. Only the native-class helper
   below remains, until Blob/Response/TrustedTypes/Intl/Notification are codegen'd and this file is deleted.) */

JSClassID idl_define_class(JSContext *ctx, const IDLInterface *iface) {
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSClassID id = 0;
    JS_NewClassID(rt, &id);
    DCHECK(id != 0, "idl_define_class: JS_NewClassID returned 0");
    JSClassDef def = { iface->name, .finalizer = iface->finalizer };
    JS_NewClass(rt, id, &def);
    JSValue proto = JS_NewObject(ctx);                      /* the member table IS the prototype */
    DCHECK(!JS_IsException(proto), "idl_define_class: prototype alloc failed");
    for (int i = 0; i < iface->n; i++) idl_put_member(ctx, proto, &iface->members[i]);
    JS_SetClassProto(ctx, id, proto);
    return id;
}
