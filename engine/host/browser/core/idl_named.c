/* Web IDL §3.9.7 Abstract operations: the named property visibility algorithm, steps 4-5, "Let prototype be
 * O.[[GetPrototypeOf]](). While prototype is not null: If prototype is not a named properties object, and
 * prototype has an own property named P, then return false." One walk for every interface that asks it
 * (Storage, and the indexed decls with a named getter), so `length` on a collection holding an element of
 * that id and `localStorage.length` after setItem('length', …) are answered alike.
 * A named properties object belongs to a [Global] interface and is exotic, so this walk meets one only as a link
 * it refuses. Every link it accepts answers from its own slots, so a caller may ask this before step 1. */
#include "check.h"
#include "quickjs.h"
#include "core/idl_named.h"

bool idl_named_hidden_by_prototype(JSContext *ctx, JSValueConst obj, JSAtom prop)
{
    int r = JS_PrototypeChainHasOwnProperty(ctx, obj, prop);

    if (r < 0)
        DFAIL("a legacy platform object's prototype chain holds a link answering by its own internal methods (a "
              "Proxy, an exotic class, a computed prototype), whose [[GetOwnProperty]] Web IDL §3.9.7's "
              "visibility algorithm asks only after step 1: ask that link on the keyed entry's GP_GETOWNPROP");
    return r == 1;
}
