/* The concolic value type — see concolic.h. A host component built on upstream quickjs's PUBLIC class API, so
   the qjs fork carries no value-type delta. */
#include "solver/concolic.h"
#include "check.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* The per-value state hung off the JSObject via JS_SetOpaque. */
typedef struct {
    char *shape;        /* @H/@S display form */
    char *src;          /* source identity (constraint correlation key) */
    JSValue example;    /* concrete example, or JS_UNDEFINED */
} Concolic;

static JSClassID g_concolic_class = 0;   /* runtime-allocated; 0 until concolic_init */

static void concolic_finalizer(JSRuntime *rt, JSValueConst val) {
    Concolic *c = JS_GetOpaque(val, g_concolic_class);
    if (!c) return;
    free(c->shape);
    free(c->src);
    JS_FreeValueRT(rt, c->example);
    free(c);
}

static void concolic_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func) {
    Concolic *c = JS_GetOpaque(val, g_concolic_class);
    if (c && !JS_IsUndefined(c->example)) JS_MarkValue(rt, c->example, mark_func);
}

/* Exotic [[Get]]: reading ANY field of a concolic value yields a DERIVED concolic — unknown injected/attacker
   state is unknown per-field, carrying the parent's provenance ("{state}.admin"). This is what lets a gated
   `if (state.admin)` fork and surface the logged-in endpoint while logged out. */
static JSValue concolic_exotic_get(JSContext *ctx, JSValueConst obj, JSAtom atom, JSValueConst receiver) {
    (void)receiver;
    Concolic *c = JS_GetOpaque(obj, g_concolic_class);
    if (!c) return JS_UNDEFINED;
    const char *field = JS_AtomToCString(ctx, atom);
    char shape[192];
    snprintf(shape, sizeof shape, "%s.%s", c->shape ? c->shape : "{}", field ? field : "?");
    JSValue r = concolic_new(ctx, shape, c->src, JS_UNDEFINED);   /* derived value, same source root */
    if (field) JS_FreeCString(ctx, field);
    return r;
}
/* `x in concolic` / property existence: a concolic collection "has" any key (so a membership gate still runs). */
static int concolic_exotic_has(JSContext *ctx, JSValueConst obj, JSAtom atom) {
    (void)ctx; (void)atom;
    return JS_GetOpaque(obj, g_concolic_class) != NULL;
}
static JSClassExoticMethods g_concolic_exotic = {
    .get_property = concolic_exotic_get,
    .has_property = concolic_exotic_has,
};

void concolic_init(JSContext *ctx) {
    JSRuntime *rt = JS_GetRuntime(ctx);
    if (g_concolic_class == 0) {
        JS_NewClassID(rt, &g_concolic_class);
        DCHECK(g_concolic_class != 0, "concolic: class id allocation returned 0 — runtime class table exhausted");
    }
    if (!JS_IsRegisteredClass(rt, g_concolic_class)) {
        JSClassDef def = { "Concolic", .finalizer = concolic_finalizer, .gc_mark = concolic_gc_mark, .exotic = &g_concolic_exotic };
        int r = JS_NewClass(rt, g_concolic_class, &def);
        CHECK(r == 0, "concolic: JS_NewClass failed — cannot register the solver's value type");
    }
}

void concolic_free(JSContext *ctx) { (void)ctx; /* class lives with the runtime; per-value state freed by the finalizer */ }

JSValue concolic_new(JSContext *ctx, const char *shape, const char *src, JSValue example) {
    DCHECK(g_concolic_class != 0, "concolic_new before concolic_init — the class is unregistered");
    JSValue obj = JS_NewObjectClass(ctx, g_concolic_class);
    if (JS_IsException(obj)) { JS_FreeValue(ctx, example); return obj; }
    Concolic *c = calloc(1, sizeof *c);
    CHECK(c, "concolic_new: OOM allocating value state — a dropped concolic corrupts the flow's domain");
    c->shape = strdup(shape ? shape : "{}");
    c->src = src ? strdup(src) : NULL;
    c->example = example;   /* consume */
    JS_SetOpaque(obj, c);
    return obj;
}

int concolic_is(JSValueConst v) {
    return g_concolic_class != 0 && JS_GetOpaque(v, g_concolic_class) != NULL;
}

const char *concolic_shape_c(JSValueConst v) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    return c ? c->shape : NULL;
}

const char *concolic_src_c(JSValueConst v) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    return c ? c->src : NULL;
}

JSValue concolic_example(JSContext *ctx, JSValueConst v) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    if (!c || JS_IsUndefined(c->example)) return JS_UNDEFINED;
    return JS_DupValue(ctx, c->example);
}

void concolic_set_example(JSContext *ctx, JSValueConst v, JSValue example) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    if (!c) { JS_FreeValue(ctx, example); return; }
    JS_FreeValue(ctx, c->example);
    c->example = example;   /* consume */
}
