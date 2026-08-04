/* WEB IDL'S INDEXED PROPERTY GETTER — what `list[0]`, `nodes[i]` and `attrs[0]` all are.
 *
 * WHY IT IS A MECHANISM AND NOT A PROPERTY SNAPSHOT. Four interfaces in the DOM declare one — DOMTokenList,
 * NodeList, HTMLCollection, NamedNodeMap — and every one of them is a VIEW over state that changes underneath
 * it. Writing `0`, `1`, `2` onto the object as real properties would answer today's question and go stale the
 * moment the class attribute or the child list changed, which is worse than the absence: a page reading
 * `list[0]` after `list.add('x')` would get the old answer with nothing to indicate it was wrong.
 *
 * SO THE LOOKUP IS THE ALGORITHM. Web IDL §3.9 says [[GetOwnProperty]] on such an object first consults the
 * OWN properties, then, for an array-index property name, the supported property indices. quickjs's exotic
 * class hook is that seam exactly — it runs only after the ordinary own-property lookup misses, which is the
 * ordering the spec states and not something this file has to arrange.
 *
 * ONE MECHANISM, FOUR BACKINGS. What differs between the interfaces is only how many indices they support and
 * what is at one, so that is what an interface declares; everything else — the index parse, the descriptor,
 * the key enumeration, `in`, and @@iterator — is written once here. Four copies would be four chances to
 * disagree about what `list[-1]` or `list['01']` means, and the answer to both is "not an index".
 *
 * ITERATION IS THE SPEC'S OWN ANSWER: §3.7.10 gives an interface with an indexed getter
 * %Array.prototype.values% as its @@iterator. Not a lookalike — the actual function, which works because the
 * object is array-like by construction. `for (const c of el.classList)` is ordinary code that had nothing. */
#include <string.h>
#include <stdlib.h>

#include "check.h"
#include "quickjs.h"
#include <stdio.h>
#include "core/idl_indexed.h"

static JSClassID g_class;
static int       g_ready;

/* Web IDL "array index property name": a canonical decimal in [0, 2^32-1). `"01"` and `"-1"` are NOT indices,
   and neither is `"1.0"` — the string must be the one ToString would produce, or two spellings of one index
   would be two different properties. */
static bool idx_of(const char *s, uint32_t *out)
{
    uint64_t v = 0;
    size_t i;

    if (!s || !*s) return false;
    if (s[0] == '0' && s[1]) return false;   /* no leading zero, and "0" itself is fine */
    for (i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (uint64_t)(s[i] - '0');
        if (v >= 0xFFFFFFFFu) return false;
    }
    *out = (uint32_t)v;
    return true;
}

static const IdlIndexedDecl *decl_of(JSValueConst obj)
{
    return JS_GetOpaque(obj, g_class);
}

static int idl_indexed_get_own(JSContext *ctx, JSPropertyDescriptor *desc, JSValueConst obj, JSAtom prop)
{
    const IdlIndexedDecl *d = decl_of(obj);
    const char *s;
    uint32_t i;
    JSValue v;
    bool is_index;

    if (!d) return 0;
    s = JS_AtomToCString(ctx, prop);
    if (!s) return -1;
    is_index = idx_of(s, &i);
    if (is_index) {
        v = (i < d->length(ctx, obj)) ? d->item(ctx, obj, i) : JS_UNDEFINED;
    } else if (d->named) {
        v = d->named(ctx, obj, s);
    } else {
        v = JS_UNDEFINED;
    }
    JS_FreeCString(ctx, s);
    if (JS_IsUndefined(v)) return 0;   /* not a supported index or name: the ordinary lookup continues */
    if (!desc) { JS_FreeValue(ctx, v); return 1; }
    /* A VALUE, never an accessor. The engine's C read path asserts against an exotic accessor because a getter
       there would be page code reached from inside a property read; an indexed getter has none by definition. */
    desc->flags = JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE;
    desc->value = v;
    desc->getter = JS_UNDEFINED;
    desc->setter = JS_UNDEFINED;
    return 1;
}

/* §3.9's SUPPORTED PROPERTY INDICES enumeration — what Object.keys, spread and for-in see. The indices come first and in
   order, which is what makes `Object.keys(list)` read like an array's. */
static int idl_indexed_own_names(JSContext *ctx, JSPropertyEnum **ptab, uint32_t *plen, JSValueConst obj)
{
    const IdlIndexedDecl *d = decl_of(obj);
    JSPropertyEnum *tab;
    uint32_t n, i;

    if (!d) { *ptab = NULL; *plen = 0; return 0; }
    n = d->length(ctx, obj);
    tab = n ? js_malloc(ctx, sizeof(*tab) * n) : NULL;
    if (n && !tab) return -1;
    for (i = 0; i < n; i++) {
        char buf[16];
        snprintf(buf, sizeof buf, "%u", i);
        tab[i].is_enumerable = 1;
        tab[i].atom = JS_NewAtom(ctx, buf);
        if (tab[i].atom == JS_ATOM_NULL) {
            while (i--) JS_FreeAtom(ctx, tab[i].atom);
            js_free(ctx, tab);
            return -1;
        }
    }
    *ptab = tab;
    *plen = n;
    return 0;
}

static int idl_indexed_has(JSContext *ctx, JSValueConst obj, JSAtom prop)
{
    return idl_indexed_get_own(ctx, NULL, obj, prop);
}

static JSClassExoticMethods g_exotic = {
    .get_own_property = idl_indexed_get_own,
    .get_own_property_names = idl_indexed_own_names,
    .has_property = idl_indexed_has,
    /* The lookup above is an index parse and a read of the component's own state. There is no accessor in a
       Web IDL indexed property getter by construction, which is what lets the engine's accessor walk run it
       from C instead of routing it onto the trampoline. */
    .get_own_property_no_user_code = true,
};

static JSClassDef g_class_def = {
    "IndexedProperties", NULL, NULL, NULL, &g_exotic
};

void idl_indexed_init(JSContext *ctx)
{
    DCHECK(!g_ready, "idl_indexed_init ran twice — one instance is one document");
    g_class = 0;
    JS_NewClassID(JS_GetRuntime(ctx), &g_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_class, &g_class_def) == 0,
          "the indexed-property class could not be registered");
    g_ready = 1;
}

void idl_indexed_free(JSContext *ctx)
{
    (void)ctx;
    g_ready = 0;
}

JSValue idl_indexed_new(JSContext *ctx, JSValueConst proto, const IdlIndexedDecl *decl)
{
    JSValue obj;

    DCHECK(g_ready, "an indexed-property object was built before its class was registered");
    DCHECK(decl && decl->length && decl->item, "an indexed interface must answer both length and item");
    obj = JS_NewObjectProtoClass(ctx, proto, g_class);
    if (JS_IsException(obj)) return obj;
    /* BORROWED, and that is why it is a static per-interface constant: the object outlives no runtime and the
       decl must outlive every object. */
    JS_SetOpaque(obj, (void *)(uintptr_t)decl);
    return obj;
}

void idl_indexed_install_iterable(JSContext *ctx, JSValueConst proto)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue arr = JS_GetPropertyStr(ctx, global, "Array");
    JSValue ap = JS_GetPropertyStr(ctx, arr, "prototype");
    JSValue values = JS_GetPropertyStr(ctx, ap, "values");

    DCHECK(JS_IsFunction(ctx, values), "Array.prototype.values is missing — §3.7.10 names it as the @@iterator "
                                       "an interface with an indexed getter is given, so there is nothing to "
                                       "install in its place");
    /* THE ACTUAL FUNCTION, not a lookalike: §3.7.10 states %Array.prototype.values%, and it works because the
       object is array-like by construction. A private copy would be a second array iterator to keep in step. */
    {
        JSValue sym_ctor = JS_GetPropertyStr(ctx, global, "Symbol");
        JSValue it = JS_GetPropertyStr(ctx, sym_ctor, "iterator");
        JSAtom a = JS_ValueToAtom(ctx, it);
        CHECK(a != JS_ATOM_NULL, "@@iterator could not be reached");
        JS_DefinePropertyValue(ctx, (JSValue)proto, a, JS_DupValue(ctx, values),
                               JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
        JS_FreeAtom(ctx, a);
        JS_FreeValue(ctx, it);
        JS_FreeValue(ctx, sym_ctor);
    }
    /* The value-iterator members the same clause gives it, and for the same reason: they are the array ones. */
    {
        static const char *const NAMES[] = { "values", "keys", "entries", "forEach" };
        unsigned k;
        for (k = 0; k < sizeof(NAMES) / sizeof(NAMES[0]); k++) {
            JSValue f = JS_GetPropertyStr(ctx, ap, NAMES[k]);
            DCHECK(JS_IsFunction(ctx, f), "an Array.prototype iterator member named by §3.7.10 is missing");
            JS_SetPropertyStr(ctx, (JSValue)proto, NAMES[k], f);
        }
    }
    JS_FreeValue(ctx, values);
    JS_FreeValue(ctx, ap);
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, global);
}
