/* ECMA-402 §8 The Intl Object. See intl.h. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/agent_state.h"
#include "core/intl/collator.h"
#include "core/intl/date_time_format.h"
#include "core/intl/intl.h"
#include "core/intl/intl_service.h"
#include "core/intl/number_format.h"
#include "core/realm.h"
#include "solver/concolic.h"

static int g_gcl_stepid = -1, g_svo_stepid = -1, g_slo_stepid[INTL_SVC_N] = { -1, -1, -1 };
static int g_ready;

static const char *const SERVICE_NAMES[INTL_SVC_N] = { "Collator", "NumberFormat", "DateTimeFormat" };

typedef struct {
    IntlState s;
} IntlFnState;

static void fn_visit(JSContext *ctx, void *st, JSStepVisit *v) { intl_state_visit(ctx, &((IntlFnState *)st)->s, v); }
static JSValue fn_fini(JSContext *ctx, void *st, bool take_result)
{
    (void)ctx;
    return intl_state_fini(&((IntlFnState *)st)->s, take_result);
}

/* ---- §8.3.1 Intl.getCanonicalLocales ( locales ) ------------------------------------------------------- */

#define GCL_STAGES(X) X(GCL_LIST, "ECMA-402 §8.3.1 step 1 (CanonicalizeLocaleList)")
enum { GCL_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const GCL_STEPS[] = { GCL_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int gcl_step(JSContext *ctx, void *st, JSValue cb, JSValue **out_cb, int *out_argc)
{
    IntlState *s = &((IntlFnState *)st)->s;
    int r;

    if (!s->started) intl_state_start(s);
    STEP_DISPATCH(GCL_STAGES, s->hdr.stage, s->hdr.def->algorithm, JS_STEP_ABRUPT);
    STEP_ARM(GCL_LIST);
    r = intl_canonicalize_locale_list(ctx, s, step_arg(&s->hdr, 0), cb, out_cb, out_argc);
    if (r) return r < 0 ? JS_STEP_ABRUPT : r;
    s->result = s->requested;                                   /* step 2: CreateArrayFromList */
    s->requested = JS_UNDEFINED;
    return JS_STEP_DONE;
}

static const JSTrampStepDef GCL_DEF = {
    sizeof(IntlFnState), gcl_step, fn_fini, 0, .visit = fn_visit,
    .algorithm = "ECMA-402 §8.3.1 Intl.getCanonicalLocales ( locales )", .steps = GCL_STEPS
};

/* ---- §8.3.2 Intl.supportedValuesOf ( key ) ------------------------------------------------------------- */

#define SVO_STAGES(X) X(SVO_KEY, "ECMA-402 §8.3.2 step 1 (ToString ( key ))")
enum { SVO_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const SVO_STEPS[] = { SVO_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* Steps 2-8's keys, in the order the algorithm tests them. */
static const char *const SVO_KEYS[] = { "calendar", "collation", "currency", "numberingSystem", "timeZone", "unit",
                                        NULL };

static int svo_list(JSContext *ctx, IntlState *s, const char *key)
{
    IntlHostValue arg;

    if (strcmp(key, "unit") == 0) {                             /* step 7: AvailableCanonicalUnits */
        s->result = intl_available_canonical_units(ctx);
        return 0;
    }
    arg = (IntlHostValue){ .kind = INTL_VALUE_STRING, .str = key, .len = strlen(key) };
    s->result = intl_ask(ctx, INTL_OP_SUPPORTED_VALUES, JS_UNDEFINED, NULL, false, JS_UNDEFINED, NULL, &arg, 1);
    return JS_IsException(s->result) ? (s->result = JS_UNDEFINED, -1) : 0;
}

static int svo_step(JSContext *ctx, void *st, JSValue cb, JSValue **out_cb, int *out_argc)
{
    IntlState *s = &((IntlFnState *)st)->s;
    JSValueConst key = step_arg(&s->hdr, 0);
    JSValue str;
    const char *c;
    int r, i, arm;

    if (!s->started) intl_state_start(s);
    STEP_DISPATCH(SVO_STAGES, s->hdr.stage, s->hdr.def->algorithm, JS_STEP_ABRUPT);
    STEP_ARM(SVO_KEY);
    if (concolic_is(key)) {
        /* Over unknown input the outcome is one of steps 2-7's lists or step 8's RangeError: the spec's own
           partition, so the machine forks over it. */
        ConcolicExState es = concolic_example_state(key);
        JSValue ex = es == CONCOLIC_EX_HELD || es == CONCOLIC_EX_DETERMINED ? concolic_example_string(ctx, key)
                                                                            : JS_UNDEFINED;
        int real = JS_OUTCOME_REAL_UNSTATED;

        JS_FreeValue(ctx, cb);
        if (!JS_IsUndefined(ex)) {
            c = JS_ToCString(ctx, ex);
            real = 6;
            for (i = 0; c && SVO_KEYS[i]; i++)
                if (strcmp(SVO_KEYS[i], c) == 0) real = i;
            if (c) JS_FreeCString(ctx, c);
            JS_FreeValue(ctx, ex);
        }
        r = step_fork_run(ctx, &s->hdr, key, "ECMA-402 §8.3.2 steps 2-8 (which key)", 7, real, &arm);
        if (r) return r;
        if (arm == 6) {
            JS_ThrowRangeError(ctx, "Invalid key for Intl.supportedValuesOf");
            return JS_STEP_ABRUPT;
        }
        return svo_list(ctx, s, SVO_KEYS[arm]) < 0 ? JS_STEP_ABRUPT : JS_STEP_DONE;
    }
    if (JS_IsString(key)) {
        JS_FreeValue(ctx, cb);
        str = JS_DupValue(ctx, key);
    } else {
        r = step_tostring_run(ctx, &s->hdr, key, cb, &str, out_cb, out_argc);
        if (r) return r < 0 ? JS_STEP_ABRUPT : r;
    }
    c = JS_ToCString(ctx, str);
    JS_FreeValue(ctx, str);
    CHECK(c != NULL, "Intl.supportedValuesOf's key could not be read");
    for (i = 0; SVO_KEYS[i]; i++)
        if (strcmp(SVO_KEYS[i], c) == 0) break;
    JS_FreeCString(ctx, c);
    if (!SVO_KEYS[i]) {                                         /* step 8 */
        JS_ThrowRangeError(ctx, "Invalid key for Intl.supportedValuesOf");
        return JS_STEP_ABRUPT;
    }
    return svo_list(ctx, s, SVO_KEYS[i]) < 0 ? JS_STEP_ABRUPT : JS_STEP_DONE;   /* step 9: CreateArrayFromList */
}

static const JSTrampStepDef SVO_DEF = {
    sizeof(IntlFnState), svo_step, fn_fini, 0, .visit = fn_visit,
    .algorithm = "ECMA-402 §8.3.2 Intl.supportedValuesOf ( key )", .steps = SVO_STEPS
};

/* ---- supportedLocalesOf: §10.2.2, §11.2.2 and §16.2.2, over §9.2.9 FilterLocales ------------------------ */

#define SLO_STAGES(X)                                                                                \
    X(SLO_LIST, "ECMA-402 §10.2.2 step 2 (CanonicalizeLocaleList)")                                  \
    X(SLO_MATCHER, "ECMA-402 §9.2.9 FilterLocales step 2 (GetOption localeMatcher)")
enum { SLO_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const SLO_STEPS[] = { SLO_STAGES(JS_STEP_STAGE_LABEL) NULL };

static const char *const MATCHERS[] = { "lookup", "best fit", NULL };
static const IntlOption SLO_MATCHER_OPT = { "localeMatcher", INTL_OPT_STRING, MATCHERS, "best fit",
                                            "ECMA-402 §9.2.9 step 2 (GetOption localeMatcher)" };

static int slo_step(JSContext *ctx, void *st, JSValue cb, JSValue **out_cb, int *out_argc)
{
    IntlState *s = &((IntlFnState *)st)->s;
    JSValue v;
    int r;

    if (!s->started) intl_state_start(s);
    STEP_DISPATCH(SLO_STAGES, s->hdr.stage, s->hdr.def->algorithm, JS_STEP_ABRUPT);
    STEP_ARM(SLO_LIST);
    r = intl_canonicalize_locale_list(ctx, s, step_arg(&s->hdr, 0), cb, out_cb, out_argc);
    if (r) return r < 0 ? JS_STEP_ABRUPT : r;
    cb = JS_UNDEFINED;
    if (intl_coerce_options(ctx, s, step_arg(&s->hdr, 1)) < 0) return JS_STEP_ABRUPT;   /* §9.2.9 step 1 */
    STEP_GOTO(s->hdr.stage, SLO_MATCHER, INTL_CURSORS(s));
    return JS_STEP_YIELD;
    STEP_ARM(SLO_MATCHER);
    r = intl_get_option(ctx, s, &SLO_MATCHER_OPT, cb, &v, out_cb, out_argc);
    if (r) return r < 0 ? JS_STEP_ABRUPT : r;
    JS_FreeValue(ctx, v);
    intl_set_read(ctx, s, "service", JS_NewString(ctx, SERVICE_NAMES[s->hdr.arg]));
    {
        static const char *const KEYS[] = { "service", "localeMatcher", NULL };

        s->result = intl_ask(ctx, INTL_OP_LOCALE_SUPPORTED, s->requested, NULL, false, s->read, KEYS, NULL, 0);
    }
    if (JS_IsException(s->result)) { s->result = JS_UNDEFINED; return JS_STEP_ABRUPT; }
    return JS_STEP_DONE;
}

static const JSTrampStepDef SLO_DEFS[INTL_SVC_N] = {
    { sizeof(IntlFnState), slo_step, fn_fini, INTL_SVC_COLLATOR, .visit = fn_visit,
      .algorithm = "ECMA-402 §10.2.2 Intl.Collator.supportedLocalesOf ( locales [ , options ] )", .steps = SLO_STEPS },
    { sizeof(IntlFnState), slo_step, fn_fini, INTL_SVC_NUMBER_FORMAT, .visit = fn_visit,
      .algorithm = "ECMA-402 §16.2.2 Intl.NumberFormat.supportedLocalesOf ( locales [ , options ] )",
      .steps = SLO_STEPS },
    { sizeof(IntlFnState), slo_step, fn_fini, INTL_SVC_DATE_TIME_FORMAT, .visit = fn_visit,
      .algorithm = "ECMA-402 §11.2.2 Intl.DateTimeFormat.supportedLocalesOf ( locales [ , options ] )",
      .steps = SLO_STEPS },
};

void intl_install_supported_locales_of(JSContext *ctx, JSValueConst ctor, IntlService svc)
{
    JSValue f = JS_NewCFunction2(ctx, NULL, "supportedLocalesOf", 1, JS_CFUNC_step, g_slo_stepid[svc]);

    CHECK(!JS_IsException(f), "a supportedLocalesOf function could not be allocated");
    JS_DefinePropertyValueStr(ctx, ctor, "supportedLocalesOf", f, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
}

/* ---- §8 the namespace, per realm ---------------------------------------------------------------------- */

static void intl_install_realm(JSContext *ctx)
{
    JSValue ns = JS_NewObject(ctx), global, f;
    JSAtom tag = JS_WellKnownSymbolAtom(JS_WKS_TO_STRING_TAG);

    CHECK(!JS_IsException(ns), "%Intl% could not be allocated");
    /* §8.1.1 Intl [ %Symbol.toStringTag% ]: "Intl", { [[Writable]]: false, [[Enumerable]]: false,
       [[Configurable]]: true }. */
    JS_DefinePropertyValue(ctx, ns, tag, JS_NewString(ctx, "Intl"), JS_PROP_CONFIGURABLE);
    /* ECMA-402 §8.2's constructors and ECMA-402 §8.3's functions carry ECMAScript §18's attributes: writable and
       configurable, not enumerable. */
    JS_DefinePropertyValueStr(ctx, ns, "Collator", collator_install(ctx), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, ns, "DateTimeFormat", date_time_format_install(ctx),
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, ns, "NumberFormat", number_format_install(ctx),
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    f = JS_NewCFunction2(ctx, NULL, "getCanonicalLocales", 1, JS_CFUNC_step, g_gcl_stepid);
    CHECK(!JS_IsException(f), "Intl.getCanonicalLocales could not be allocated");
    JS_DefinePropertyValueStr(ctx, ns, "getCanonicalLocales", f, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    f = JS_NewCFunction2(ctx, NULL, "supportedValuesOf", 1, JS_CFUNC_step, g_svo_stepid);
    CHECK(!JS_IsException(f), "Intl.supportedValuesOf could not be allocated");
    JS_DefinePropertyValueStr(ctx, ns, "supportedValuesOf", f, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    /* ECMA-402 §8 The Intl Object: it "is the initial value of the "Intl" property of the global object", with
       ECMAScript §19.4 Other Properties of the Global Object's attributes. */
    global = JS_GetGlobalObject(ctx);
    JS_DefinePropertyValueStr(ctx, global, "Intl", ns, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_FreeValue(ctx, global);
}

void intl_init(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    int i;

    DCHECK(!g_ready, "intl_init ran twice — the Intl services are declared once per agent");
    intl_service_init(ctx);
    g_gcl_stepid = JS_RegisterStepDef(rt, &GCL_DEF);
    g_svo_stepid = JS_RegisterStepDef(rt, &SVO_DEF);
    agent_state_id("intl", &g_gcl_stepid, "§8.3.1 getCanonicalLocales' machine");
    agent_state_id("intl", &g_svo_stepid, "§8.3.2 supportedValuesOf's machine");
    for (i = 0; i < INTL_SVC_N; i++) {
        g_slo_stepid[i] = JS_RegisterStepDef(rt, &SLO_DEFS[i]);
        agent_state_id("intl", &g_slo_stepid[i], "a service's supportedLocalesOf machine");
    }
    collator_init(ctx);
    number_format_init(ctx);
    date_time_format_init(ctx);
    g_ready = 1;
    agent_state_flag("intl", &g_ready, "the Intl declaration latch");
    realm_declare_intrinsic(intl_install_realm);
}

void intl_free(JSRuntime *rt)
{
    DCHECK(g_ready, "the Intl services were released in an agent that never declared them");
    collator_free(rt);
    number_format_free(rt);
    date_time_format_free(rt);
    intl_service_free(rt);
    /* Every handle the row declared, from this file and the four it reaches, is put back here. */
    agent_state_undo("intl");
}
