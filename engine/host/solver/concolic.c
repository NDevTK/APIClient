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
    int cmp_op;         /* for a COMPARISON RESULT: OPCMP_EQ/NE — `src <op> cmp_tok` (else OPCMP_NONE) */
    char *cmp_tok;      /* the concrete side of the comparison */
} Concolic;

/* THE PER-FLOW PATH CONSTRAINT. One map, keyed by SOURCE IDENTITY, carrying every fact this flow has learned
   about the unknown input it read — which is the whole of what a DART/SAGE-lineage constraint is here, because
   concrete execution grounds all shared and interprocedural state and only the INPUT variables are ever
   symbolic. Two facts, because two things narrow a domain:
     val   — an EQ gate PINNED the source to a concrete value on its true arm, so later reads compute the REAL
              @H value (`/api/admin`, never `/api/{state}.role`). CONCRETIZE-ON-PIN.
     truth — a PREDICATE over this source was already decided in this flow. `if (cfg.admin)` is not an equality
              and pins nothing, yet taking its true arm still says the value is truthy FOR THIS FLOW — and a
              bundle branches on the same flag over and over. Without this every one of those branches forked
              again, so N tests of one flag cost 2^N flows and the frontier is exponential in a quantity that
              carries no information: the sibling arms are CONTRADICTED, they explore nothing, and each still
              drags a COW delta. Deciding them is feasible-refinement, the thing that makes forced multi-path
              execution tractable, and it is sound-only — it prunes a branch on the SAME predicate the flow has
              already fixed, never one whose domain still permits both outcomes.
   Both are per-flow and travel together, which is why they are ONE entry rather than two maps that a fork,
   a suspend and a resume would each have to remember to carry. */
typedef struct { char *key; char *val; signed char truth; } Cons;
static Cons *g_pins = NULL; static int g_pins_n = 0, g_pins_cap = 0;
static Cons *cons_entry(const char *key) {
    for (int i = 0; i < g_pins_n; i++) if (!strcmp(g_pins[i].key, key)) return &g_pins[i];
    if (g_pins_n >= g_pins_cap) { g_pins_cap = g_pins_cap ? g_pins_cap * 2 : 8; g_pins = realloc(g_pins, (size_t)g_pins_cap * sizeof(Cons)); CHECK(g_pins, "concolic: OOM path constraint"); }
    g_pins[g_pins_n].key = strdup(key); CHECK(g_pins[g_pins_n].key, "concolic: OOM constraint key");
    g_pins[g_pins_n].val = NULL; g_pins[g_pins_n].truth = -1;
    return &g_pins[g_pins_n++];
}
void concolic_pin(const char *src, const char *val) {
    Cons *c = cons_entry(src);
    free(c->val); c->val = strdup(val); CHECK(c->val, "concolic: OOM pin value");
}
void concolic_constrain_branch(const char *key, int truth) {
    cons_entry(key)->truth = (signed char)(truth ? 1 : 0);
}
int concolic_branch_decided(const char *key) {
    for (int i = 0; i < g_pins_n; i++) if (!strcmp(g_pins[i].key, key)) return g_pins[i].truth;
    return -1;
}
void concolic_clear_pins(void) { for (int i = 0; i < g_pins_n; i++) { free(g_pins[i].key); free(g_pins[i].val); } g_pins_n = 0; }

/* Per-flow constraint state is swappable so interleaved flows keep their OWN narrowing: suspend snapshots the
   live map (deep copy), resume replaces the live map with a snapshot. A blob is one flow's constraint parked
   while another runs — and the blob a FORK takes is the sibling's whole starting knowledge. */
typedef struct { Cons *pins; int n; } PinBlob;
void *concolic_pins_suspend(void) {
    PinBlob *b = malloc(sizeof *b); CHECK(b, "concolic: OOM constraint blob");
    b->n = g_pins_n;
    b->pins = g_pins_n ? malloc((size_t)g_pins_n * sizeof(Cons)) : NULL;
    if (g_pins_n) CHECK(b->pins, "concolic: OOM constraint blob copy");
    for (int i = 0; i < g_pins_n; i++) {
        b->pins[i].key = strdup(g_pins[i].key); CHECK(b->pins[i].key, "concolic: OOM constraint blob key");
        b->pins[i].val = g_pins[i].val ? strdup(g_pins[i].val) : NULL;
        b->pins[i].truth = g_pins[i].truth;
    }
    return b;
}
void concolic_pins_resume(void *blob) {
    concolic_clear_pins();                 /* free the live map before overwriting it */
    PinBlob *b = blob;
    for (int i = 0; i < b->n; i++) {
        Cons *c = cons_entry(b->pins[i].key);
        c->val = b->pins[i].val ? strdup(b->pins[i].val) : NULL;
        c->truth = b->pins[i].truth;
    }
}
void concolic_pins_blob_free(void *blob) {
    PinBlob *b = blob; if (!b) return;
    for (int i = 0; i < b->n; i++) { free(b->pins[i].key); free(b->pins[i].val); }
    free(b->pins); free(b);
}
static const char *pin_of(const char *src) {
    for (int i = 0; i < g_pins_n; i++) if (!strcmp(g_pins[i].key, src)) return g_pins[i].val;
    return NULL;
}

static JSClassID g_concolic_class = 0;   /* runtime-allocated; 0 until concolic_init */

static void concolic_finalizer(JSRuntime *rt, JSValueConst val) {
    Concolic *c = JS_GetOpaque(val, g_concolic_class);
    if (!c) return;
    free(c->shape);
    free(c->src);
    free(c->cmp_tok);
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
    const char *pv = pin_of(shape);                          /* an EQ gate pinned this source -> the real value */
    if (pv) return JS_NewString(ctx, pv);
    return concolic_new(ctx, shape, shape, JS_UNDEFINED);    /* src = the field path (precise @S identity) */
}

/* Mint a COMPARISON-RESULT concolic bool carrying `src <op> tok`, so `if (x === 'admin')` forks (instead of a
   concrete false) and the taken arm can pin/negate. */
JSValue concolic_new_cmp(JSContext *ctx, const char *src, int op, const char *tok) {
    JSValue r = concolic_new(ctx, "{cmp}", src, JS_UNDEFINED);
    Concolic *c = JS_GetOpaque(r, g_concolic_class);
    if (c) { c->cmp_op = op; c->cmp_tok = tok ? strdup(tok) : NULL; }
    return r;
}
int concolic_cmp(JSValueConst v, const char **psrc, const char **ptok) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    if (!c || c->cmp_op == OPCMP_NONE) return OPCMP_NONE;
    if (psrc) *psrc = c->src; if (ptok) *ptok = c->cmp_tok;
    return c->cmp_op;
}

/* JSConcolicCmpHook for == / === : a concolic operand -> a concolic bool carrying {src, op, tok}. Matches the
   slow-eq stack effect (both operands freed, result in sp[-2]). is_neq flips EQ->NE. */
int concolic_cmp_hook(JSContext *ctx, JSValue *sp, int is_neq) {
    JSValue a = sp[-2], b = sp[-1];
    int ca = concolic_is(a), cb = concolic_is(b);
    if (!ca && !cb) return 0;
    JSValueConst opq = ca ? a : b, other = ca ? b : a;
    const char *src = concolic_src_c(opq);
    char *tok = NULL;
    if (!concolic_is(other)) { const char *s = JS_ToCString(ctx, other); if (s) { tok = strdup(s); JS_FreeCString(ctx, s); } }
    JSValue res = concolic_new_cmp(ctx, src, is_neq ? OPCMP_NE : OPCMP_EQ, tok);
    free(tok);
    JS_FreeValue(ctx, a); JS_FreeValue(ctx, b);
    sp[-2] = res;
    return 1;
}
/* CALLING an unknown yields an unknown. `document.cookie.indexOf("role=admin")` reads a field off a concolic —
   which concolic_exotic_get already answers with another concolic — and then CALLS it. A concolic that is not
   callable throws "not a function" there, and the throw takes the whole program with it: every statement after
   the first method call on an unknown string is unreachable, which silently truncated exploration at exactly
   the checks that gate a session. The result is concolic because that is what it IS: nothing here knows what
   indexOf would return over a cookie jar this engine was never given, so the comparison after it FORKS instead
   of collapsing. The ARGUMENTS still ran — they are the page's own expressions and their effects have already
   happened by the time this is reached. */
static JSValue concolic_call(JSContext *ctx, JSValueConst func_obj, JSValueConst this_val,
                             int argc, JSValueConst *argv, int flags)
{
    Concolic *c = JS_GetOpaque(func_obj, g_concolic_class);
    char shape[224];
    (void)this_val; (void)argc; (void)argv; (void)flags;
    DCHECK(c != NULL, "the concolic call handler ran on something that is not a concolic value");
    snprintf(shape, sizeof shape, "%s()", c->shape ? c->shape : "{}");
    return concolic_new(ctx, shape, shape, JS_UNDEFINED);
}

/* ORDERING over an unknown is unknown. < <= > >= coerce with ToPrimitive, which a concolic cannot satisfy —
   it is an object whose coercion answers with another concolic — so the operator threw TypeError and took the
   whole program with it: `document.cookie.indexOf("role=admin") >= 0` explored NEITHER arm, losing both the
   session path and the anonymous one. The result is a concolic BOOL, so the branch forks exactly as an equality
   gate does; it carries no {op,tok} constraint because an ordering does not PIN a value the way `=== 'admin'`
   does — it narrows a domain, and the arm that is taken says which way. */
int concolic_rel_hook(JSContext *ctx, JSValue *sp, int op) {
    JSValue a = sp[-2], b = sp[-1];
    int ca = concolic_is(a), cb = concolic_is(b);
    if (!ca && !cb) return 0;
    (void)op;
    {
        const char *src = concolic_src_c(ca ? a : b);
        JSValue res = concolic_new(ctx, "{cmp}", src ? src : "{cmp}", JS_UNDEFINED);
        JS_FreeValue(ctx, a); JS_FreeValue(ctx, b);
        sp[-2] = res;
    }
    return 1;
}

/* `typeof` an unknown is UNKNOWN. A concolic is a real object of a host class, and that class is callable so
   `document.cookie.indexOf(...)` works — which made typeof report "function" and a bundle testing
   `typeof x === "function"` take an arm decided by the solver's representation rather than the value. The
   answer is a concolic STRING carrying the same source, so the comparison after it forks like every other gate
   over an unknown. */
JSValue concolic_typeof_hook(JSContext *ctx, JSValueConst v) {
    const char *src;
    char shape[192];
    if (!concolic_is(v)) return JS_UNINITIALIZED;
    src = concolic_src_c(v);
    snprintf(shape, sizeof shape, "typeof %s", src ? src : "{}");
    return concolic_new(ctx, shape, shape, JS_UNDEFINED);
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
        JSClassDef def = { "Concolic", .finalizer = concolic_finalizer, .gc_mark = concolic_gc_mark,
                           .call = concolic_call, .exotic = &g_concolic_exotic };
        int r = JS_NewClass(rt, g_concolic_class, &def);
        CHECK(r == 0, "concolic: JS_NewClass failed — cannot register the solver's value type");
    }
}

void concolic_free(JSContext *ctx) { (void)ctx; /* class lives with the runtime; per-value state freed by the finalizer */ }

JSValue concolic_new(JSContext *ctx, const char *shape, const char *src, JSValue example) {
    /* A CANDIDATE RUN substitutes one source with a breakout. The check lived only in the field-read path, so a
       source installed as a plain property value — location.hash, document.cookie — was minted once at install
       and never passed through it: its candidate could not be delivered and the sink never fired. Minting is
       the one place every source goes through, whichever way it is reached. */
    if (g_cand_src && src && !strcmp(src, g_cand_src))
        return JS_NewString(ctx, g_cand_payload ? g_cand_payload : "");
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
