/* THE ELEMENT INTERFACE — Blink core/dom, over the real Lexbor tree.
 *
 * IDENTITY IS THE INVARIANT. One JS object per Lexbor element, kept in a map on this component. A page compares
 * nodes by identity constantly (`el === document.body`, a Set of visited nodes, a WeakMap keyed by node), and a
 * fresh wrapper per lookup makes every one of those silently false — the page then re-walks, re-binds and
 * re-inserts, and this engine reports a surface built out of that confusion instead of the page's.
 *
 * READS are pure Lexbor and run no page code, so they are ordinary C. WRITES go through the solver's
 * chokepoints (dom_cow_set_attribute) because a DOM write is per-flow TIME-TRAVEL state: two forked arms write
 * the same attribute differently and each reads back its own. Never raw Lexbor, which would make the write
 * global and the flows visible to each other.
 *
 * setAttribute also carries TAINT. Lexbor stores bytes, so an attacker value written into an attribute and read
 * back would come out a plain string with its provenance gone; attr_shadow keeps the (element,name) -> concolic
 * association, so `el.setAttribute("data-x", location.hash)` followed later by a sink read is still solved. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "solver/attr_shadow.h"
#include "solver/concolic.h"
#include "solver/dom_cow.h"
#include "solver/solve.h"
#include "core/dom/element.h"

static JSClassID g_element_class;

typedef struct { lxb_dom_element_t *el; JSValue obj; } ElemEntry;
static ElemEntry *g_wraps;
static int        g_wrap_n, g_wrap_cap;

static lxb_dom_element_t *elem_of(JSValueConst v)
{
    return JS_GetOpaque(v, g_element_class);
}

static JSValue js_el_get_attribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    lxb_dom_element_t *el = elem_of(this_val);
    const char *name;
    JSValue r = JS_NULL;
    int si;

    if (!el || argc < 1) return JS_NULL;
    name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    /* The TAINT SHADOW answers first: an attacker value written here came back out of Lexbor as plain bytes
       with its provenance gone, and a sink reading it would look clean. */
    si = attr_shadow_find(el, name);
    if (si >= 0) {
        r = JS_DupValue(ctx, attr_shadow_opaque(si));   /* borrowed — dup to hand it out */
    } else {
        size_t vl = 0;
        const lxb_char_t *v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)name, strlen(name), &vl);
        if (v) r = JS_NewStringLen(ctx, (const char *)v, vl);
    }
    JS_FreeCString(ctx, name);
    return r;
}

static JSValue js_el_set_attribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    lxb_dom_element_t *el = elem_of(this_val);
    const char *name, *val;

    if (!el || argc < 2) return JS_UNDEFINED;
    name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    /* A concolic value has no bytes to store. Record it in the shadow so the read gives the SAME concolic back,
       and write its shape into the tree so a serialization of the document still shows something. */
    if (concolic_is(argv[1]))
        attr_shadow_set(ctx, el, name, argv[1]);
    else
        attr_shadow_set(ctx, el, name, JS_UNDEFINED);   /* a concrete write clears any earlier taint */
    val = JS_ToCString(ctx, argv[1]);
    if (val) {
        dom_cow_set_attribute(el, name, val, strlen(val));   /* chokepoint: capture-then-mutate, per flow */
        JS_FreeCString(ctx, val);
    }
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}

static JSValue js_el_get_tag(JSContext *ctx, JSValueConst this_val)
{
    lxb_dom_element_t *el = elem_of(this_val);
    size_t n = 0;
    const lxb_char_t *t;
    if (!el) return JS_UNDEFINED;
    t = lxb_dom_element_qualified_name(el, &n);
    return t ? JS_NewStringLen(ctx, (const char *)t, n) : JS_UNDEFINED;
}

/* innerHTML= is an HTML-CONTEXT SINK: the assigned string is parsed as markup into this element, so an attacker
   value reaching it is a breakout solved against the real parse context. The solver owns that decision. */
static JSValue js_el_set_inner_html(JSContext *ctx, JSValueConst this_val, JSValueConst val)
{
    lxb_dom_element_t *el = elem_of(this_val);
    if (!el) return JS_UNDEFINED;
    solve_html_sink(ctx, val);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_element_proto[] = {
    JS_CFUNC_DEF("getAttribute", 1, js_el_get_attribute),
    JS_CFUNC_DEF("setAttribute", 2, js_el_set_attribute),
    JS_CGETSET_DEF("tagName", js_el_get_tag, NULL),
    JS_CGETSET_DEF("innerHTML", NULL, js_el_set_inner_html),
};

static void element_finalizer(JSRuntime *rt, JSValue val) { (void)rt; (void)val; }

JSValue element_wrap(JSContext *ctx, lxb_dom_element_t *el)
{
    JSValue obj;
    int i;

    if (!el)
        return JS_NULL;
    for (i = 0; i < g_wrap_n; i++)
        if (g_wraps[i].el == el)
            return JS_DupValue(ctx, g_wraps[i].obj);

    obj = JS_NewObjectClass(ctx, g_element_class);
    if (JS_IsException(obj))
        return obj;
    JS_SetOpaque(obj, el);
    JS_SetPropertyFunctionList(ctx, obj, js_element_proto,
                               (int)(sizeof(js_element_proto) / sizeof(js_element_proto[0])));

    if (g_wrap_n == g_wrap_cap) {
        int c = g_wrap_cap ? g_wrap_cap * 2 : 16;
        ElemEntry *a = realloc(g_wraps, sizeof(*a) * (size_t)c);
        CHECK(a != NULL, "the element wrapper table allocation failed: a dropped wrapper breaks node identity, "
                         "and every `el === other` the page makes after it is silently false");
        g_wraps = a; g_wrap_cap = c;
    }
    g_wraps[g_wrap_n].el = el;
    g_wraps[g_wrap_n].obj = JS_DupValue(ctx, obj);
    g_wrap_n++;
    return obj;
}

void element_init(JSContext *ctx)
{
    JSClassDef def = { "Element", .finalizer = element_finalizer };
    JS_NewClassID(JS_GetRuntime(ctx), &g_element_class);
    JS_NewClass(JS_GetRuntime(ctx), g_element_class, &def);
}

void element_free(JSContext *ctx)
{
    int i;
    for (i = 0; i < g_wrap_n; i++)
        JS_FreeValue(ctx, g_wraps[i].obj);
    free(g_wraps);
    g_wraps = NULL; g_wrap_n = g_wrap_cap = 0;
}
