/* ECMA-402 §10 Collator Objects. See collator.h. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/agent_state.h"
#include "core/idl_slots.h"
#include "core/intl/collator.h"
#include "core/intl/intl_service.h"
#include "solver/concolic.h"

static JSClassID g_class;
static int g_ctor_stepid = -1, g_compare_stepid = -1;

/* §10.3.2's Table, in its order: the slots resolvedOptions reports. */
static const char *const RESOLVED[] = {
    "locale", "usage", "sensitivity", "ignorePunctuation", "collation", "numeric", "caseFirst", NULL
};

/* ---- §10.1.1 Intl.Collator ( [ locales [ , options ] ] ) -------------------------------------------------- */

#define CO_STAGES(X)                                                                                          \
    X(CO_CREATE, "ECMA-402 §10.1.1 step 3 (OrdinaryCreateFromConstructor)")                                   \
    X(CO_LOCALES, "ECMA-402 §10.1.1 step 5 (CanonicalizeLocaleList)")                                         \
    X(CO_USAGE, "ECMA-402 §10.1.1 step 7 (GetOption usage)")                                                  \
    X(CO_MATCHER, "ECMA-402 §9.2.8 ResolveOptions step 4 (GetOption localeMatcher)")                          \
    X(CO_COLLATION, "ECMA-402 §9.2.8 step 6 (the collation descriptor)")                                     \
    X(CO_NUMERIC, "ECMA-402 §9.2.8 step 6 (the numeric descriptor)")                                         \
    X(CO_CASE_FIRST, "ECMA-402 §9.2.8 step 6 (the caseFirst descriptor)")                                    \
    X(CO_SENSITIVITY, "ECMA-402 §10.1.1 step 20 (GetOption sensitivity)")                                     \
    X(CO_PUNCTUATION, "ECMA-402 §10.1.1 step 22 (GetOption ignorePunctuation)")
enum { CO_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const CO_STEPS[] = { CO_STAGES(JS_STEP_STAGE_LABEL) NULL };

static const char *const USAGES[] = { "sort", "search", NULL };
static const char *const MATCHERS[] = { "lookup", "best fit", NULL };
static const char *const CASE_FIRSTS[] = { "upper", "lower", "false", NULL };
static const char *const SENSITIVITIES[] = { "base", "accent", "case", "variant", NULL };

static const IntlOption OPT_USAGE = { "usage", INTL_OPT_STRING, USAGES, "sort",
                                      "ECMA-402 §10.1.1 step 7 (GetOption usage)" };
static const IntlOption OPT_MATCHER = { "localeMatcher", INTL_OPT_STRING, MATCHERS, "best fit",
                                        "ECMA-402 §9.2.8 step 4 (GetOption localeMatcher), Collator" };
static const IntlOption OPT_COLLATION = { "collation", INTL_OPT_STRING, NULL, NULL,
                                          "ECMA-402 §9.2.8 step 6 (GetOption collation)" };
static const IntlOption OPT_NUMERIC = { "numeric", INTL_OPT_BOOLEAN, NULL, NULL,
                                        "ECMA-402 §9.2.8 step 6 (GetOption numeric)" };
static const IntlOption OPT_CASE_FIRST = { "caseFirst", INTL_OPT_STRING, CASE_FIRSTS, NULL,
                                           "ECMA-402 §9.2.8 step 6 (GetOption caseFirst), Collator" };

typedef struct {
    IntlState s;
} CollatorCtor;

static void ctor_visit(JSContext *ctx, void *st, JSStepVisit *v) { intl_state_visit(ctx, &((CollatorCtor *)st)->s, v); }
static JSValue ctor_fini(JSContext *ctx, void *st, bool take_result)
{
    (void)ctx;
    return intl_state_fini(&((CollatorCtor *)st)->s, take_result);
}

/* A string member of the host's resolution, as the C string an option default is stated with. Owned. */
static const char *resolution_cstr(JSContext *ctx, JSValueConst r, const char *name)
{
    JSValue v = JS_GetPropertyStr(ctx, r, name);
    const char *c;

    DCHECKF(JS_IsString(v), "the Intl host's collator resolution states no %s", name);
    c = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    CHECKF(c != NULL, "the Intl host's collator resolution states no %s", name);
    return c;
}

static int ctor_step(JSContext *ctx, void *st, JSValue cb, JSValue **out_cb, int *out_argc)
{
    IntlState *s = &((CollatorCtor *)st)->s;
    JSStepHdr *h = &s->hdr;
    JSValue v;
    int r;

    if (!s->started) intl_state_start(s);
    STEP_DISPATCH(CO_STAGES, h->stage, h->def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(CO_CREATE);
    /* Step 1: an undefined NewTarget is the active function object. */
    r = intl_create_from_constructor(ctx, s, JS_IsUndefined(h->this_val) ? h->func_obj : h->this_val, g_class, cb,
                                     out_cb, out_argc);
    if (r) return r < 0 ? JS_STEP_ABRUPT : r;
    STEP_GOTO(h->stage, CO_LOCALES, INTL_CURSORS(s));
    return JS_STEP_YIELD;

    STEP_ARM(CO_LOCALES);
    r = intl_canonicalize_locale_list(ctx, s, step_arg(h, 0), cb, out_cb, out_argc);
    if (r) return r < 0 ? JS_STEP_ABRUPT : r;
    if (intl_coerce_options(ctx, s, step_arg(h, 1)) < 0) return JS_STEP_ABRUPT;       /* step 6 */
    STEP_GOTO(h->stage, CO_USAGE, INTL_CURSORS(s));
    return JS_STEP_YIELD;

    STEP_ARM(CO_USAGE);
    r = intl_get_option(ctx, s, &OPT_USAGE, cb, &v, out_cb, out_argc);
    if (r) return r < 0 ? JS_STEP_ABRUPT : r;
    JS_FreeValue(ctx, v);
    /* Step 11: ResolveOptions over CreateArrayFromList(requestedLocales) canonicalizes the canonical list again
       and takes the options object as it is, neither of which is observable. */
    STEP_GOTO(h->stage, CO_MATCHER, INTL_CURSORS(s));
    return JS_STEP_YIELD;

    STEP_ARM(CO_MATCHER);
    r = intl_get_option(ctx, s, &OPT_MATCHER, cb, &v, out_cb, out_argc);
    if (r) return r < 0 ? JS_STEP_ABRUPT : r;
    intl_set_opt(ctx, s, "localeMatcher", v);
    STEP_GOTO(h->stage, CO_COLLATION, INTL_CURSORS(s));
    return JS_STEP_YIELD;

    STEP_ARM(CO_COLLATION);
    r = intl_resolution_option(ctx, s, &OPT_COLLATION, "co", cb, out_cb, out_argc);
    if (r) return r < 0 ? JS_STEP_ABRUPT : r;
    STEP_GOTO(h->stage, CO_NUMERIC, INTL_CURSORS(s));
    return JS_STEP_YIELD;

    STEP_ARM(CO_NUMERIC);
    r = intl_resolution_option(ctx, s, &OPT_NUMERIC, "kn", cb, out_cb, out_argc);
    if (r) return r < 0 ? JS_STEP_ABRUPT : r;
    STEP_GOTO(h->stage, CO_CASE_FIRST, INTL_CURSORS(s));
    return JS_STEP_YIELD;

    STEP_ARM(CO_CASE_FIRST);
    r = intl_resolution_option(ctx, s, &OPT_CASE_FIRST, "kf", cb, out_cb, out_argc);
    if (r) return r < 0 ? JS_STEP_ABRUPT : r;
    {
        static const char *const KEYS[] = { "usage", "localeMatcher", "co", "kn", "kf", NULL };

        intl_set_opt(ctx, s, "usage", intl_read(ctx, s, "usage"));
        /* §9.2.7 ResolveLocale over the collator's data, which is [[SortLocaleData]] or [[SearchLocaleData]] by
           usage (steps 9-10), with the default locale last (§9.2.7 step 3). */
        s->list = intl_ask(ctx, INTL_OP_COLLATOR_RESOLVE, s->requested, NULL, true, s->opt, KEYS, NULL, 0);
        if (JS_IsException(s->list)) { s->list = JS_UNDEFINED; return JS_STEP_ABRUPT; }
    }
    s->slots = idl_slots_new(ctx);
    CHECK(!JS_IsException(s->slots), "a Collator's slots could not be allocated");
    JS_SetPropertyStr(ctx, s->slots, "locale", JS_GetPropertyStr(ctx, s->list, "locale"));       /* step 13 */
    JS_SetPropertyStr(ctx, s->slots, "usage", intl_read(ctx, s, "usage"));                     /* step 8 */
    JS_SetPropertyStr(ctx, s->slots, "collation", JS_GetPropertyStr(ctx, s->list, "collation")); /* step 15 */
    JS_SetPropertyStr(ctx, s->slots, "numeric", JS_GetPropertyStr(ctx, s->list, "numeric"));     /* step 16 */
    JS_SetPropertyStr(ctx, s->slots, "caseFirst", JS_GetPropertyStr(ctx, s->list, "caseFirst")); /* step 17 */
    STEP_GOTO(h->stage, CO_SENSITIVITY, INTL_CURSORS(s));
    return JS_STEP_YIELD;

    STEP_ARM(CO_SENSITIVITY);
    {
        /* Step 19: "variant" for sort, the locale data's [[sensitivity]] for search. */
        const char *dflt = resolution_cstr(ctx, s->list, "sensitivity");
        IntlOption o = { "sensitivity", INTL_OPT_STRING, SENSITIVITIES, dflt,
                         "ECMA-402 §10.1.1 step 20 (GetOption sensitivity)" };

        r = intl_get_option(ctx, s, &o, cb, &v, out_cb, out_argc);
        JS_FreeCString(ctx, dflt);
    }
    if (r) return r < 0 ? JS_STEP_ABRUPT : r;
    JS_SetPropertyStr(ctx, s->slots, "sensitivity", v);
    STEP_GOTO(h->stage, CO_PUNCTUATION, INTL_CURSORS(s));
    return JS_STEP_YIELD;

    STEP_ARM(CO_PUNCTUATION);
    {
        JSValue d = JS_GetPropertyStr(ctx, s->list, "ignorePunctuation");
        IntlOption o = { "ignorePunctuation", INTL_OPT_BOOLEAN, NULL, JS_ToBool(ctx, d) ? "true" : "false",
                         "ECMA-402 §10.1.1 step 22 (GetOption ignorePunctuation)" };

        JS_FreeValue(ctx, d);
        r = intl_get_option(ctx, s, &o, cb, &v, out_cb, out_argc);
    }
    if (r) return r < 0 ? JS_STEP_ABRUPT : r;
    JS_SetPropertyStr(ctx, s->slots, "ignorePunctuation", v);
    intl_slots_take_deps(ctx, s, s->slots);
    intl_slots_attach(ctx, s->obj, s->slots);
    s->slots = JS_UNDEFINED;
    s->result = s->obj;                                                                  /* step 23 */
    s->obj = JS_UNDEFINED;
    return JS_STEP_DONE;
}

static const JSTrampStepDef CTOR_DEF = {
    sizeof(CollatorCtor), ctor_step, ctor_fini, 0, .visit = ctor_visit,
    .algorithm = "ECMA-402 §10.1.1 Intl.Collator ( [ locales [ , options ] ] )", .steps = CO_STEPS
};

/* ---- §10.3.3 get Intl.Collator.prototype.compare, and §10.3.3.1 CompareStrings ----------------------------- */

#define CC_STAGES(X)                                                                                          \
    X(CC_X, "ECMA-402 §10.3.3 step 3.a.i (ToString ( x ))")                                                   \
    X(CC_Y, "ECMA-402 §10.3.3 step 3.a.ii (ToString ( y ))")
enum { CC_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const CC_STEPS[] = { CC_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSStepHdr hdr;
    JSValue x, y;     /* the two Strings, or the examples of unknown operands */
    JSValue result;
    uint8_t started;
} CompareState;

static void cc_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    CompareState *s = st;

    if (!s->started) return;
    v->val(ctx, &s->x);
    v->val(ctx, &s->y);
    v->val(ctx, &s->result);
}

static JSValue cc_fini(JSContext *ctx, void *st, bool take_result)
{
    CompareState *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;

    (void)ctx;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

/* ToString of an operand: a String as itself, an unknown's example, or the request; 0 once *out is a String. */
static int operand_string(JSContext *ctx, JSStepHdr *h, JSValueConst v, JSValue in, JSValue *out, JSValue **out_cb,
                          int *out_argc)
{
    if (concolic_is(v)) {
        ConcolicExState st = concolic_example_state(v);

        if (st == CONCOLIC_EX_HELD || st == CONCOLIC_EX_DETERMINED) {
            JSValue ex = concolic_example_string(ctx, v);

            if (!JS_IsUndefined(ex)) { JS_FreeValue(ctx, in); *out = ex; return 0; }
        }
    } else if (JS_IsString(v)) {
        JS_FreeValue(ctx, in);
        *out = JS_DupValue(ctx, v);
        return 0;
    }
    return step_tostring_run(ctx, h, v, in, out, out_cb, out_argc);
}

static int cc_step(JSContext *ctx, void *st, JSValue cb, JSValue **out_cb, int *out_argc)
{
    CompareState *s = st;
    JSStepHdr *h = &s->hdr;
    JSValueConst collator = JS_StepClosureData(h, 0);
    int r;

    if (!s->started) { s->x = s->y = s->result = JS_UNDEFINED; s->started = 1; }
    STEP_DISPATCH(CC_STAGES, h->stage, h->def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(CC_X);
    r = operand_string(ctx, h, step_arg(h, 0), cb, &s->x, out_cb, out_argc);
    if (r) return r < 0 ? JS_STEP_ABRUPT : r;
    STEP_GOTO(h->stage, CC_Y, &h->str_phase, NULL);
    return JS_STEP_YIELD;

    STEP_ARM(CC_Y);
    r = operand_string(ctx, h, step_arg(h, 1), cb, &s->y, out_cb, out_argc);
    if (r) return r < 0 ? JS_STEP_ABRUPT : r;
    {
        static const char *const KEYS[] = { "usage", "sensitivity", "ignorePunctuation", "collation", "numeric",
                                            "caseFirst", NULL };
        JSValue slots = intl_slots_of(ctx, collator, g_class), loc = intl_slot(ctx, slots, "locale"), real;
        const char *l = JS_ToCString(ctx, loc), *xs = NULL, *ys = NULL;
        IntlHostValue args[2];
        JSValueConst inputs[2] = { step_arg(h, 0), step_arg(h, 1) };
        bool both;

        DCHECK(JS_IsObject(slots) && l != NULL, "a bound compare function's collator carries no resolved slots");
        both = intl_host_value_of(ctx, s->x, &args[0], &xs) && intl_host_value_of(ctx, s->y, &args[1], &ys);
        DCHECK(both, "CompareStrings' two operands are not both Strings after ToString");
        real = intl_ask(ctx, INTL_OP_COLLATOR_COMPARE, JS_UNDEFINED, l, false, slots, KEYS, args, 2);  /* 3.a.iii */
        JS_FreeCString(ctx, xs);
        JS_FreeCString(ctx, ys);
        JS_FreeCString(ctx, l);
        JS_FreeValue(ctx, loc);
        if (JS_IsException(real)) { JS_FreeValue(ctx, slots); return JS_STEP_ABRUPT; }
        s->result = intl_derive(ctx, "Intl.Collator.prototype.compare", slots, inputs, 2, real);
        JS_FreeValue(ctx, slots);
    }
    return JS_STEP_DONE;
}

static const JSTrampStepDef COMPARE_DEF = {
    sizeof(CompareState), cc_step, cc_fini, 0, .visit = cc_visit,
    .algorithm = "ECMA-402 §10.3.3.1 CompareStrings ( collator, x, y ), from a bound compare function", .steps = CC_STEPS
};

static JSValue throw_not_collator(JSContext *ctx, const char *member)
{
    return JS_ThrowTypeError(ctx, "Intl.Collator.prototype.%s called on an object that is not an Intl.Collator", member);
}

static JSValue js_collator_compare(JSContext *ctx, JSValueConst this_val)
{
    JSValue slots = intl_slots_of(ctx, this_val, g_class), f;

    if (JS_IsUndefined(slots)) return throw_not_collator(ctx, "compare");           /* step 2 */
    f = JS_GetPropertyStr(ctx, slots, "boundCompare");
    if (JS_IsUndefined(f)) {                                                         /* step 3 */
        f = JS_NewStepClosure2(ctx, g_compare_stepid, "", 2, 1, &this_val);
        CHECK(!JS_IsException(f), "a bound compare function could not be allocated");
        JS_SetPropertyStr(ctx, slots, "boundCompare", JS_DupValue(ctx, f));
    }
    JS_FreeValue(ctx, slots);
    return f;
}

/* ---- §10.3.2 Intl.Collator.prototype.resolvedOptions ( ) -------------------------------------------------- */

static JSValue js_collator_resolved_options(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue slots = intl_slots_of(ctx, this_val, g_class), o;
    int i;

    (void)argc; (void)argv;
    if (JS_IsUndefined(slots)) return throw_not_collator(ctx, "resolvedOptions");
    o = JS_NewObject(ctx);
    CHECK(!JS_IsException(o), "resolvedOptions' object could not be allocated");
    for (i = 0; RESOLVED[i]; i++) {
        JSValue v = intl_slot(ctx, slots, RESOLVED[i]);

        if (JS_IsUndefined(v)) continue;
        v = intl_derive(ctx, "Intl.Collator.prototype.resolvedOptions", slots, NULL, 0, v);
        JS_DefinePropertyValueStr(ctx, o, RESOLVED[i], v, JS_PROP_C_W_E);
    }
    JS_FreeValue(ctx, slots);
    return o;
}

static const JSCFunctionListEntry PROTO_FUNCS[] = {
    JS_CFUNC_DEF("resolvedOptions", 0, js_collator_resolved_options),
    JS_CGETSET_DEF("compare", js_collator_compare, NULL),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Intl.Collator", JS_PROP_CONFIGURABLE),
};

JSValue collator_install(JSContext *ctx)
{
    JSValue proto = JS_NewObject(ctx), ctor;

    CHECK(!JS_IsException(proto), "%Intl.Collator.prototype% could not be allocated");
    JS_SetPropertyFunctionList(ctx, proto, PROTO_FUNCS, (int)(sizeof PROTO_FUNCS / sizeof PROTO_FUNCS[0]));
    ctor = JS_NewCFunction2(ctx, NULL, "Collator", 0, JS_CFUNC_step_ctor, g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "%Intl.Collator% could not be allocated");
    /* §10.2.1 and §10.3.1: prototype { writable: false, enumerable: false, configurable: false }, and the
       prototype's constructor. */
    JS_SetConstructor(ctx, ctor, proto);
    intl_install_supported_locales_of(ctx, ctor, INTL_SVC_COLLATOR);
    JS_SetClassProto(ctx, g_class, proto);
    return ctor;
}

void collator_init(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSClassDef d = { "Intl.Collator", NULL, NULL, NULL, NULL };

    JS_NewClassID(rt, &g_class);
    JS_NewClass(rt, g_class, &d);
    g_ctor_stepid = JS_RegisterStepDef(rt, &CTOR_DEF);
    g_compare_stepid = JS_RegisterStepDef(rt, &COMPARE_DEF);
    agent_state_class("intl", &g_class, "§10's Intl.Collator class");
    agent_state_id("intl", &g_ctor_stepid, "§10.1.1's constructor machine");
    agent_state_id("intl", &g_compare_stepid, "§10.3.3's bound compare machine");
}

void collator_free(JSRuntime *rt)
{
    (void)rt;   /* its class and machines are ids the row's undo puts back */
    agent_state_reached("intl");
}
