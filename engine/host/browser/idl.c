/* Web IDL binding driver — see idl.h. A declarative IDL member table becomes a native object: operations are
 * functions, opaque attributes are getters returning the concolic unknown. A headless-unknown readonly
 * attribute still EXISTS and has its type (the IDL declares it), but its VALUE forks — spec-locked shape,
 * honest-unknown value. The interface reads as its IDL, never hand-assembled property-by-property. */
#include <stdio.h>
#include "idl.h"
#include "opaque.h"   /* js_concolic — an opaque attribute reads as a name-tagged concolic (forks + provenance) */

JSValue idl_instance(JSContext *ctx, const IDLMember *members, int n) {
    JSValue o = JS_NewObject(ctx);
    for (int i = 0; i < n; i++) {
        const IDLMember *m = &members[i];
        if (m->kind == IDL_METHOD) {
            JS_SetPropertyStr(ctx, o, m->name, JS_NewCFunction(ctx, m->fn, m->name, m->length));
        } else {   /* IDL_ATTR_OPAQUE — value unknown headless: a concolic tagged by the ATTRIBUTE NAME (aborted
                      -> {aborted}), so it forks AND names its provenance, never a generic {} or {idlAttr}. */
            char shape[80]; snprintf(shape, sizeof shape, "{%s}", m->name);
            JS_SetPropertyStr(ctx, o, m->name, js_concolic(ctx, shape, JS_UNDEFINED));
        }
    }
    return o;
}
