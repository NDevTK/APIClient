/* Web IDL binding driver — see idl.h. A declarative IDL member table becomes a native object: operations are
 * functions, opaque attributes are getters returning the concolic unknown. A headless-unknown readonly
 * attribute still EXISTS and has its type (the IDL declares it), but its VALUE forks — spec-locked shape,
 * honest-unknown value. The interface reads as its IDL, never hand-assembled property-by-property. */
#include <stdio.h>
#include <string.h>
#include "bindings/idl.h"
#include "opaque.h"   /* js_concolic (opaque attr value) + js_noop (a spec-present, unmodelled operation) */
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

/* ── GENERATED-SHAPE binding: install a spec member list, wiring each member's behavior from the component ── */
static const IdlImpl *idl_find_impl(const IdlImpl *impls, int n, const char *name) {
    for (int i = 0; i < n; i++) if (strcmp(impls[i].name, name) == 0) return &impls[i];
    return NULL;
}
void idl_bind(JSContext *ctx, JSValueConst target, const IdlGenMember *shape, int shape_n, const IdlImpl *impls, int impl_n, int install_attrs) {
    for (int i = 0; i < shape_n; i++) {
        const IdlGenMember *m = &shape[i];
        if (m->kind == IDL_GEN_ATTR && !install_attrs) continue;   /* ops-only pass (attrs installed separately) */
        const IdlImpl *im = idl_find_impl(impls, impl_n, m->name);
        JSAtom a = JS_NewAtom(ctx, m->name);
        if (m->kind == IDL_GEN_OP) {
            JSCFunction *fn = (im && im->op) ? im->op : js_noop;   /* a spec operation we haven't modelled is a present noop, never missing */
            JS_DefinePropertyValue(ctx, (JSValue)target, a, JS_NewCFunction(ctx, fn, m->name, m->arg), JS_PROP_C_W_E);
        } else if (im && (im->get || im->set)) {   /* modelled attribute: a real getter and/or setter */
            JSValue getter = JS_UNDEFINED, setter = JS_UNDEFINED;
            if (im->magic >= 0) {
                if (im->get) getter = JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)im->get, m->name, 0, JS_CFUNC_getter_magic, im->magic);
                if (im->set) setter = JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)im->set, m->name, 1, JS_CFUNC_setter_magic, im->magic);
            } else {
                if (im->get) getter = JS_NewCFunction2(ctx, im->get, m->name, 0, JS_CFUNC_getter, 0);
                if (im->set) setter = JS_NewCFunction2(ctx, im->set, m->name, 1, JS_CFUNC_setter, 0);
            }
            JS_DefinePropertyGetSet(ctx, (JSValue)target, a, getter, setter, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        } else if (m->readonly) {   /* unmodelled READONLY attribute: the concolic unknown (EXISTS + typed, VALUE forks) */
            char shp[80]; snprintf(shp, sizeof shp, "{%s}", m->name);
            JS_DefinePropertyValue(ctx, (JSValue)target, a, js_concolic(ctx, shp, JS_UNDEFINED), JS_PROP_ENUMERABLE);
        } else {   /* unmodelled WRITABLE attribute: a plain settable property (the page may assign; not an attacker source) */
            JS_DefinePropertyValue(ctx, (JSValue)target, a, JS_UNDEFINED, JS_PROP_C_W_E);
        }
        JS_FreeAtom(ctx, a);
    }
}

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
