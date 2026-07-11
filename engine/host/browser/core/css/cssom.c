/* CSSOM — see cssom.h. Extracted from main.c and made a faithful, PER-FLOW CSSStyleDeclaration.
 *
 * DESIGN (what a browser + solver engineer agree on):
 *  - PER-FLOW via the ONE delta primitive: el.style is backed by the element's `style` ATTRIBUTE, which the
 *    DOM COW delta (dom_cow.c) already captures per flow. So `el.style.color='red'` is a per-flow style-attr
 *    mutation — a flow sees its OWN CSS writes and they ISOLATE across flows exactly like every DOM/heap write.
 *    No parallel CSS store, no fresh-object stub (whose writes were lost), no divergence between el.style.x and
 *    getAttribute('style'): the attribute is the single source of truth.
 *  - NATIVE object, not a JS Proxy: CSSStyleDeclaration is a real IDL interface (Blink core/css/cssom), so it
 *    is a native exotic class here too — `get_own_property` serves live CSS properties (falling through to a
 *    real prototype for getPropertyValue/setProperty/toString), `define_own_property` writes them back. A Proxy
 *    with a JS target would leak its internals via Object.keys and only trap get/set; the exotic object matches
 *    the browser and composes with the prototype.
 *  - el.style unset property -> "" (real CSSStyleDeclaration semantics); getComputedStyle unset -> OPAQUE (the
 *    cascade/layout value is genuinely unknown headless, so a gate on it FORKS). Inline-set props are concrete
 *    + per-flow in BOTH. matchMedia -> opaque (no viewport headless: .matches forks, .addEventListener drives). */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "core/css/cssom.h"
#include "solver/concolic.h"      /* g_concolic, JS_IsConcolic/Example/ShapeC */
#include "core/dom/dom_element.h" /* g_el_class_id — unwrap the element argument */
#include "solver/dom_cow.h"     /* dom_attr_capture — a style write joins the per-flow COW delta */
#include "solver/attr_shadow.h" /* attr_shadow_set — an opaque CSS value keeps its taint on the style attr */
#include "solver/solve.h"       /* solve_add — el.style.x = tainted is a CSS-context @S sink */
#include "check.h"       /* CHECK — an OOM must crash at the origin, never degrade el.style to a fake opaque */
#include <lexbor/dom/dom.h>

extern char *g_candidate;   /* a candidate replay flow writes the concrete payload, not the shadow taint */
extern JSValue js_add_listener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv);   /* MediaQueryList change listener -> driven flow */

/* The DEFINED default viewport the headless engine models (a real browser has one even with no window): a
   1280x720 landscape desktop, light theme, fine pointer, no reduced motion. matchMedia evaluates against THIS
   so `.matches` is a CONCRETE answer, never an opaque shrug — the browser's behavior without a device is
   spec-defined. (A minimal evaluator over the common media features; an unrecognized feature is conservatively
   non-matching. Alternate-viewport exploration is a separate solver concern, never a fake-opaque fork here.) */
#define VIEWPORT_W 1280
#define VIEWPORT_H 720
static int media_matches(const char *q) {
    if (!q) return 0;
    const char *p;
    if (strstr(q, "prefers-color-scheme: dark")) return 0;
    if (strstr(q, "prefers-color-scheme: light")) return 1;
    if (strstr(q, "prefers-reduced-motion: reduce")) return 0;
    if (strstr(q, "prefers-reduced-motion")) return 1;                 /* no-preference query */
    if (strstr(q, "pointer: coarse")) return 0;
    if (strstr(q, "pointer: fine") || strstr(q, "any-pointer: fine")) return 1;
    if (strstr(q, "hover: none")) return 0;
    if (strstr(q, "hover: hover") || strstr(q, "any-hover: hover")) return 1;
    if (strstr(q, "orientation: portrait")) return 0;
    if (strstr(q, "orientation: landscape")) return 1;
    if ((p = strstr(q, "max-width:")))  return VIEWPORT_W <= atoi(p + 10);
    if ((p = strstr(q, "min-width:")))  return VIEWPORT_W >= atoi(p + 10);
    if ((p = strstr(q, "max-height:"))) return VIEWPORT_H <= atoi(p + 11);
    if ((p = strstr(q, "min-height:"))) return VIEWPORT_H >= atoi(p + 11);
    return 0;   /* unrecognized feature: conservatively non-matching (concrete, never opaque) */
}

typedef struct { lxb_dom_element_t *el; int computed; } StyleDecl;
static JSClassID g_style_class_id;

/* camelCase -> kebab (backgroundColor -> background-color); custom props (--x) / already-kebab pass through. */
static void css_kebab(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 2 < cap; i++) {
        char c = in[i];
        if (c >= 'A' && c <= 'Z') { out[o++] = '-'; if (o + 1 < cap) out[o++] = (char)(c - 'A' + 'a'); }
        else out[o++] = c;
    }
    out[o] = 0;
}
/* Read property `name` (kebab) from a `k: v; k2: v2` style string into out; returns 1 if found. */
static int css_get(const char *style, const char *name, char *out, size_t cap) {
    if (!style) return 0;
    size_t nl = strlen(name);
    for (const char *p = style; *p; ) {
        while (*p == ' ' || *p == ';') p++;
        if (!*p) break;
        const char *colon = strchr(p, ':'); if (!colon) break;
        const char *semi = strchr(colon, ';'); if (!semi) semi = colon + strlen(colon);
        const char *ke = colon; while (ke > p && ke[-1] == ' ') ke--;
        if ((size_t)(ke - p) == nl && !strncmp(p, name, nl)) {
            const char *vs = colon + 1; while (*vs == ' ') vs++;
            const char *ve = semi; while (ve > vs && ve[-1] == ' ') ve--;
            size_t vl = (size_t)(ve - vs); if (vl >= cap) vl = cap - 1;
            memcpy(out, vs, vl); out[vl] = 0; return 1;
        }
        p = (*semi) ? semi + 1 : semi;
    }
    return 0;
}
/* Return a NEW style string (malloc) with `name`(kebab)=`val` upserted (existing occurrence dropped). */
static char *css_upsert(const char *style, const char *name, const char *val) {
    size_t cap = (style ? strlen(style) : 0) + strlen(name) + strlen(val) + 8;
    char *out = malloc(cap); CHECK(out, "css_upsert: style buffer alloc — a dropped style write corrupts the per-flow delta"); size_t o = 0;
    size_t nl = strlen(name);
    if (style) for (const char *p = style; *p; ) {
        while (*p == ' ' || *p == ';') p++;
        if (!*p) break;
        const char *colon = strchr(p, ':'); if (!colon) break;
        const char *semi = strchr(colon, ';'); if (!semi) semi = colon + strlen(colon);
        const char *ke = colon; while (ke > p && ke[-1] == ' ') ke--;
        int match = ((size_t)(ke - p) == nl && !strncmp(p, name, nl));
        if (!match) { size_t seg = (size_t)(semi - p); memcpy(out + o, p, seg); o += seg; out[o++] = ';'; out[o++] = ' '; }
        p = (*semi) ? semi + 1 : semi;
    }
    o += (size_t)snprintf(out + o, cap - o, "%s: %s", name, val);
    return out;
}

/* The whole inline `style` attribute string (COW-captured -> per flow). Caller owns nothing (borrowed). */
static const char *style_attr(StyleDecl *s, size_t *len) {
    if (!s || !s->el) { *len = 0; return NULL; }
    return (const char *)lxb_dom_element_get_attribute(s->el, (const lxb_char_t *)"style", 5, len);
}
/* A property NAME that is a method/special on the prototype, not a CSS property. */
static int style_is_member(const char *k) {
    return !strcmp(k, "getPropertyValue") || !strcmp(k, "setProperty") || !strcmp(k, "removeProperty")
        || !strcmp(k, "item") || !strcmp(k, "length") || !strcmp(k, "constructor") || !strcmp(k, "toString")
        || (k[0] == 'c' && !strcmp(k, "cssText"));   /* cssText handled explicitly below, not as a CSS prop */
}

/* Write one CSS property (or cssText wholesale) into the element's `style` attribute — the ONE per-flow
   mutation path (capture baseline -> taint shadow -> @S CSS sink -> upsert). Shared by define_own + setProperty. */
static void style_write(JSContext *ctx, StyleDecl *s, const char *key, JSValueConst value) {
    if (!s || !s->el || s->computed) return;   /* getComputedStyle is read-only */
    int is_css_text = !strcmp(key, "cssText");
    int is_opq = JS_IsConcolic(value);
    dom_attr_capture(s->el, "style");                                       /* pre-write baseline -> per-flow COW delta */
    if (!g_candidate) attr_shadow_set(ctx, s->el, "style", is_opq ? value : JS_UNDEFINED);   /* opaque CSS value -> taint the style attr */
    solve_add(ctx, "style", "css", value);                                  /* el.style.x = tainted -> CSS-context @S sink */
    JSValue exv = is_opq ? JS_ConcolicExample(ctx, value) : JS_UNDEFINED;
    int ex_str = is_opq && !JS_IsUndefined(exv);
    const char *v = ex_str ? JS_ToCString(ctx, exv) : (is_opq ? JS_ConcolicShapeC(value) : JS_ToCString(ctx, value));
    if (is_css_text) {
        if (v) lxb_dom_element_set_attribute(s->el, (const lxb_char_t *)"style", 5, (const lxb_char_t *)v, strlen(v));
    } else {
        char kebab[128]; css_kebab(key, kebab, sizeof kebab);
        size_t vl = 0; const char *cur = style_attr(s, &vl);
        char *ns = css_upsert(cur, kebab, v ? v : "");
        if (ns) { lxb_dom_element_set_attribute(s->el, (const lxb_char_t *)"style", 5, (const lxb_char_t *)ns, strlen(ns)); free(ns); }
    }
    if (v && (ex_str || !is_opq)) JS_FreeCString(ctx, v);
    JS_FreeValue(ctx, exv);
}

/* Live value of a CSS property (or cssText) for reads — from the per-flow style attribute. */
static JSValue style_read(JSContext *ctx, StyleDecl *s, const char *key) {
    size_t vl = 0; const char *style = style_attr(s, &vl);
    if (!strcmp(key, "cssText")) return JS_NewStringLen(ctx, style ? style : "", style ? vl : 0);
    char kebab[128], val[512]; css_kebab(key, kebab, sizeof kebab);
    if (style && css_get(style, kebab, val, sizeof val)) return JS_NewString(ctx, val);   /* inline-set: concrete, per-flow */
    return (s && s->computed) ? js_concolic(ctx, "{computedStyle}", JS_UNDEFINED)          /* computed + unset: cascade unknown -> source-tagged concolic (forks), not bare {} */
                              : JS_NewString(ctx, "");                                    /* el.style + unset: "" (spec) */
}

/* EXOTIC get_own_property: serve live CSS properties + cssText as own; methods fall through to the prototype
   (return false). Symbol keys and members are not own (proto/default handles them). */
static int style_get_own(JSContext *ctx, JSPropertyDescriptor *desc, JSValueConst obj, JSAtom prop) {
    JSValue av = JS_AtomToValue(ctx, prop);
    int is_str = JS_IsString(av); JS_FreeValue(ctx, av);
    if (!is_str) return 0;                                  /* Symbol.* -> prototype/default */
    const char *key = JS_AtomToCString(ctx, prop); if (!key) return 0;
    if (style_is_member(key) && strcmp(key, "cssText")) { JS_FreeCString(ctx, key); return 0; }   /* method -> prototype */
    StyleDecl *s = JS_GetOpaque(obj, g_style_class_id);
    JSValue v = style_read(ctx, s, key);
    JS_FreeCString(ctx, key);
    if (desc) { desc->flags = JS_PROP_C_W_E; desc->value = v; desc->getter = JS_UNDEFINED; desc->setter = JS_UNDEFINED; }
    else JS_FreeValue(ctx, v);
    return 1;
}
/* EXOTIC define_own_property: an assignment `el.style.color='red'` / `el.style.cssText='...'` writes the
   per-flow style attribute. */
static int style_define_own(JSContext *ctx, JSValueConst this_obj, JSAtom prop, JSValueConst val,
                            JSValueConst getter, JSValueConst setter, int flags) {
    (void)getter; (void)setter; (void)flags;
    JSValue av = JS_AtomToValue(ctx, prop);
    int is_str = JS_IsString(av); JS_FreeValue(ctx, av);
    if (!is_str) return 1;                                  /* ignore Symbol defines */
    const char *key = JS_AtomToCString(ctx, prop); if (!key) return 1;
    if (!(style_is_member(key) && strcmp(key, "cssText")))  /* a CSS property or cssText -> write it */
        style_write(ctx, JS_GetOpaque(this_obj, g_style_class_id), key, val);
    JS_FreeCString(ctx, key);
    return 1;
}
static const JSClassExoticMethods style_exotic = { .get_own_property = style_get_own, .define_own_property = style_define_own };
static void style_finalizer(JSRuntime *rt, JSValue val) { StyleDecl *s = JS_GetOpaque(val, g_style_class_id); (void)rt; if (s) free(s); }

/* Prototype methods (getPropertyValue/setProperty/removeProperty/toString) — the real CSSStyleDeclaration API,
   each operating on `this`'s per-flow style attribute. */
static JSValue m_getPropertyValue(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    StyleDecl *s = JS_GetOpaque(t, g_style_class_id);
    if (c < 1) return JS_NewString(ctx, "");
    const char *k = JS_ToCString(ctx, v[0]); if (!k) return JS_NewString(ctx, "");
    JSValue r = style_read(ctx, s, k); JS_FreeCString(ctx, k); return r;
}
static JSValue m_setProperty(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    StyleDecl *s = JS_GetOpaque(t, g_style_class_id);
    if (c < 2) return JS_UNDEFINED;
    const char *k = JS_ToCString(ctx, v[0]); if (!k) return JS_UNDEFINED;
    style_write(ctx, s, k, v[1]); JS_FreeCString(ctx, k); return JS_UNDEFINED;
}
static JSValue m_removeProperty(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    StyleDecl *s = JS_GetOpaque(t, g_style_class_id);
    if (c >= 1) { const char *k = JS_ToCString(ctx, v[0]); if (k) { JSValue empty = JS_NewString(ctx, ""); style_write(ctx, s, k, empty); JS_FreeValue(ctx, empty); JS_FreeCString(ctx, k); } }
    return JS_NewString(ctx, "");
}
static JSValue m_toString(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    (void)c; (void)v; return style_read(ctx, JS_GetOpaque(t, g_style_class_id), "cssText");
}

void cssom_init(JSContext *ctx) {
    JSRuntime *rt = JS_GetRuntime(ctx);
    JS_NewClassID(rt, &g_style_class_id);
    JSClassDef def = { "CSSStyleDeclaration", .finalizer = style_finalizer, .exotic = (JSClassExoticMethods *)&style_exotic };
    JS_NewClass(rt, g_style_class_id, &def);
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, proto, "getPropertyValue", JS_NewCFunction(ctx, m_getPropertyValue, "getPropertyValue", 1));
    JS_SetPropertyStr(ctx, proto, "setProperty", JS_NewCFunction(ctx, m_setProperty, "setProperty", 2));
    JS_SetPropertyStr(ctx, proto, "removeProperty", JS_NewCFunction(ctx, m_removeProperty, "removeProperty", 1));
    JS_SetPropertyStr(ctx, proto, "toString", JS_NewCFunction(ctx, m_toString, "toString", 0));
    JS_SetClassProto(ctx, g_style_class_id, proto);
}

static JSValue style_wrap(JSContext *ctx, lxb_dom_element_t *el, int computed) {
    JSValue o = JS_NewObjectClass(ctx, g_style_class_id);
    DCHECK(!JS_IsException(o), "CSSStyleDeclaration class not initialised (cssom_init must run in qjs_init)");
    StyleDecl *s = malloc(sizeof *s);
    CHECK(s, "CSSStyleDeclaration slot alloc — a fake-opaque el.style would silently drop every per-flow style write");
    s->el = el; s->computed = computed;
    JS_SetOpaque(o, s);
    return o;
}

/* el.style — a live, per-flow inline CSSStyleDeclaration (called from dom_element.c's style getter). */
JSValue js_el_inline_style(JSContext *ctx, JSValueConst el_obj) {
    lxb_dom_element_t *el = JS_GetOpaque(el_obj, g_el_class_id);
    return el ? style_wrap(ctx, el, 0) : js_concolic(ctx, "{style}", JS_UNDEFINED);
}
JSValue js_get_computed_style(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    (void)t;
    lxb_dom_element_t *el = (c >= 1) ? JS_GetOpaque(v[0], g_el_class_id) : NULL;
    return el ? style_wrap(ctx, el, 1) : js_concolic(ctx, "{computedStyle}", JS_UNDEFINED);   /* reflects inline writes; unset -> source-tagged concolic */
}
/* matchMedia(query) -> a MediaQueryList whose .matches is CONCOLIC: opaque so a `if (mq.matches)` branch still
   FORKS (explore the alternate-viewport world — more logic, you don't know which arm ships an endpoint),
   carrying the DEFAULT-viewport answer as its concrete EXAMPLE. Plus the query string + change-listener methods. */
JSValue js_match_media(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    (void)t;
    const char *q = (c >= 1) ? JS_ToCString(ctx, v[0]) : NULL;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "matches", js_concolic(ctx, "{matches}", JS_NewBool(ctx, media_matches(q))));   /* fork + default-viewport example */
    JS_SetPropertyStr(ctx, o, "media", JS_NewString(ctx, q ? q : ""));
    JS_SetPropertyStr(ctx, o, "onchange", JS_NULL);
    JS_SetPropertyStr(ctx, o, "addEventListener", JS_NewCFunction(ctx, js_add_listener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, o, "removeEventListener", JS_NewCFunction(ctx, js_noop, "removeEventListener", 2));
    JS_SetPropertyStr(ctx, o, "addListener", JS_NewCFunction(ctx, js_add_listener, "addListener", 1));
    JS_SetPropertyStr(ctx, o, "removeListener", JS_NewCFunction(ctx, js_noop, "removeListener", 1));
    if (q) JS_FreeCString(ctx, q);
    return o;
}
