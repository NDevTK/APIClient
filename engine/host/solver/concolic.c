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

/* @S CANDIDATE injection: during a verification re-run, the attacker source identified by g_cand_src returns
   the concrete breakout payload instead of a concolic, so the REAL code builds the exploit and it fires. */
static char *g_cand_src = NULL, *g_cand_payload = NULL;
void concolic_set_candidate(const char *src, const char *payload) {
    free(g_cand_src); free(g_cand_payload);
    g_cand_src = src ? strdup(src) : NULL;
    g_cand_payload = payload ? strdup(payload) : NULL;
}

/* Exotic [[Get]]: reading ANY field of a concolic value yields a DERIVED concolic — unknown injected/attacker
   state is unknown per-field, carrying the FIELD-PATH identity ("{state}.admin"), which doubles as the source
   identity for @S injection. This is what lets a gated `if (state.admin)` fork AND lets an @S candidate inject
   at a precise source. */
static JSValue concolic_exotic_get(JSContext *ctx, JSValueConst obj, JSAtom atom, JSValueConst receiver) {
    (void)receiver;
    Concolic *c = JS_GetOpaque(obj, g_concolic_class);
    if (!c) return JS_UNDEFINED;
    const char *field = JS_AtomToCString(ctx, atom);
    char shape[192];
    snprintf(shape, sizeof shape, "%s.%s", c->shape ? c->shape : "{}", field ? field : "?");
    if (field) JS_FreeCString(ctx, field);
    if (g_cand_src && !strcmp(shape, g_cand_src))            /* candidate run: this source -> the concrete breakout */
        return JS_NewString(ctx, g_cand_payload ? g_cand_payload : "");
    return concolic_new(ctx, shape, shape, JS_UNDEFINED);    /* src = the field path (precise @S identity) */
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

static char *cstr_dup(JSContext *ctx, JSValueConst v) {   /* concrete operand -> its string form (heap copy) */
    const char *s = JS_ToCString(ctx, v);
    char *r = strdup(s ? s : "");
    if (s) JS_FreeCString(ctx, s);
    return r;
}

/* JSConcolicAddHook: `a + b` where a or b is concolic -> a DERIVED concolic. shape = display(a)++display(b)
   (a concrete operand contributes its string, a concolic its shape); example = the concrete concat when BOTH
   sides have a concrete value/example, else absent (unknown input stays unknown). Matches js_add_slow's stack
   effect: both operands freed, result in sp[-2]. */
int concolic_add_hook(JSContext *ctx, JSValue *sp) {
    JSValue a = sp[-2], b = sp[-1];
    int ca = concolic_is(a), cb = concolic_is(b);
    if (!ca && !cb) return 0;

    char *sha = ca ? strdup(concolic_shape_c(a) ? concolic_shape_c(a) : "{}") : cstr_dup(ctx, a);
    char *shb = cb ? strdup(concolic_shape_c(b) ? concolic_shape_c(b) : "{}") : cstr_dup(ctx, b);
    CHECK(sha && shb, "concolic +: OOM shape");
    size_t ln = strlen(sha) + strlen(shb) + 1;
    char *shape = malloc(ln); CHECK(shape, "concolic +: OOM shape concat");
    snprintf(shape, ln, "%s%s", sha, shb);
    const char *src = ca ? concolic_src_c(a) : concolic_src_c(b);

    JSValue exa = ca ? concolic_example(ctx, a) : JS_DupValue(ctx, a);
    JSValue exb = cb ? concolic_example(ctx, b) : JS_DupValue(ctx, b);
    JSValue example = JS_UNDEFINED;
    if (!JS_IsUndefined(exa) && !JS_IsUndefined(exb)) {
        const char *pa = JS_ToCString(ctx, exa), *pb = JS_ToCString(ctx, exb);
        if (pa && pb) { size_t l = strlen(pa) + strlen(pb) + 1; char *e = malloc(l); if (e) { snprintf(e, l, "%s%s", pa, pb); example = JS_NewString(ctx, e); free(e); } }
        if (pa) JS_FreeCString(ctx, pa); if (pb) JS_FreeCString(ctx, pb);
    }
    JS_FreeValue(ctx, exa); JS_FreeValue(ctx, exb);

    JSValue result = concolic_new(ctx, shape, src, example);   /* consumes example */
    free(sha); free(shb); free(shape);
    JS_FreeValue(ctx, a); JS_FreeValue(ctx, b);
    sp[-2] = result;
    return 1;
}
