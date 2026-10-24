/* ECMA-402 §11 DateTimeFormat Objects. See date_time_format.h. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/agent_state.h"
#include "core/idl_slots.h"
#include "core/intl/date_time_format.h"
#include "core/intl/intl_environment.h"
#include "core/intl/intl_service.h"
#include "solver/concolic.h"

static JSClassID g_class;
static int g_ctor_stepid = -1, g_format_stepid = -1, g_parts_stepid = -1;

/* §11.1.2's Table 16, in table order, with each row's values. fractionalSecondDigits is a number option. */
typedef struct { const char *prop; const char *values[7]; const char *fork_op; } Component;
#define ROW(p) "ECMA-402 §11.1.2 step 24 (GetOption " p ")"
static const Component COMPONENTS[] = {
    { "weekday", { "narrow", "short", "long", NULL }, ROW("weekday") },
    { "era", { "narrow", "short", "long", NULL }, ROW("era") },
    { "year", { "2-digit", "numeric", NULL }, ROW("year") },
    { "month", { "2-digit", "numeric", "narrow", "short", "long", NULL }, ROW("month") },
    { "day", { "2-digit", "numeric", NULL }, ROW("day") },
    { "dayPeriod", { "narrow", "short", "long", NULL }, ROW("dayPeriod") },
    { "hour", { "2-digit", "numeric", NULL }, ROW("hour") },
    { "minute", { "2-digit", "numeric", NULL }, ROW("minute") },
    { "second", { "2-digit", "numeric", NULL }, ROW("second") },
    { "fractionalSecondDigits", { NULL }, ROW("fractionalSecondDigits") },
    { "timeZoneName", { "short", "long", "shortOffset", "longOffset", "shortGeneric", "longGeneric", NULL },
      ROW("timeZoneName") },
};
#undef ROW
#define N_COMPONENTS ((int)(sizeof COMPONENTS / sizeof COMPONENTS[0]))

/* What the host formats with: every slot but the locale, which travels as the request's locale. */
static const char *const FORMAT_SLOTS[] = {
    "calendar", "numberingSystem", "timeZone", "hourCycle", "weekday", "era", "year", "month", "day", "dayPeriod",
    "hour", "minute", "second", "fractionalSecondDigits", "timeZoneName", "dateStyle", "timeStyle", NULL
};

/* ---- §11.1.1 Intl.DateTimeFormat ( [ locales [ , options ] ] ), through §11.1.2 CreateDateTimeFormat ------- */

#define DT_STAGES(X)                                                                                          \
    X(DT_CREATE, "ECMA-402 §11.1.2 step 1 (OrdinaryCreateFromConstructor)")                                   \
    X(DT_LOCALES, "ECMA-402 §9.2.8 ResolveOptions step 1 (CanonicalizeLocaleList)")                           \
    X(DT_MATCHER, "ECMA-402 §9.2.8 step 4 (GetOption localeMatcher)")                                         \
    X(DT_CA, "ECMA-402 §9.2.8 step 6 (the calendar descriptor)")                                             \
    X(DT_NU, "ECMA-402 §9.2.8 step 6 (the numberingSystem descriptor)")                                      \
    X(DT_HOUR12, "ECMA-402 §9.2.8 step 6 (the hour12 descriptor)")                                           \
    X(DT_HC, "ECMA-402 §9.2.8 step 6 (the hourCycle descriptor)")                                            \
    X(DT_TIME_ZONE, "ECMA-402 §11.1.2 step 17 (GetOption timeZone)")                                          \
    X(DT_COMPONENT, "ECMA-402 §11.1.2 step 24 (the next row of Table 16)")                                    \
    X(DT_FORMAT_MATCHER, "ECMA-402 §11.1.2 step 25 (GetOption formatMatcher)")                                \
    X(DT_DATE_STYLE, "ECMA-402 §11.1.2 step 26 (GetOption dateStyle)")                                        \
    X(DT_TIME_STYLE, "ECMA-402 §11.1.2 step 28 (GetOption timeStyle)")
enum { DT_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const DT_STEPS[] = { DT_STAGES(JS_STEP_STAGE_LABEL) NULL };

static const char *const MATCHERS[] = { "lookup", "best fit", NULL };
static const char *const HOUR_CYCLES[] = { "h11", "h12", "h23", "h24", NULL };
static const char *const FORMAT_MATCHERS[] = { "basic", "best fit", NULL };
static const char *const STYLES[] = { "full", "long", "medium", "short", NULL };

static const IntlOption OPT_MATCHER = { "localeMatcher", INTL_OPT_STRING, MATCHERS, "best fit",
                                        "ECMA-402 §9.2.8 step 4 (GetOption localeMatcher), DateTimeFormat" };
static const IntlOption OPT_CA = { "calendar", INTL_OPT_STRING, NULL, NULL,
                                   "ECMA-402 §9.2.8 step 6 (GetOption calendar)" };
static const IntlOption OPT_NU = { "numberingSystem", INTL_OPT_STRING, NULL, NULL,
                                   "ECMA-402 §9.2.8 step 6 (GetOption numberingSystem), DateTimeFormat" };
static const IntlOption OPT_HOUR12 = { "hour12", INTL_OPT_BOOLEAN, NULL, NULL,
                                       "ECMA-402 §9.2.8 step 6 (GetOption hour12)" };
static const IntlOption OPT_HC = { "hourCycle", INTL_OPT_STRING, HOUR_CYCLES, NULL,
                                   "ECMA-402 §9.2.8 step 6 (GetOption hourCycle)" };
static const IntlOption OPT_TIME_ZONE = { "timeZone", INTL_OPT_STRING, NULL, INTL_SYSTEM_TIME_ZONE,
                                          "ECMA-402 §11.1.2 step 17 (GetOption timeZone)" };
static const IntlOption OPT_FORMAT_MATCHER = { "formatMatcher", INTL_OPT_STRING, FORMAT_MATCHERS, "best fit",
                                               "ECMA-402 §11.1.2 step 25 (GetOption formatMatcher)" };
static const IntlOption OPT_DATE_STYLE = { "dateStyle", INTL_OPT_STRING, STYLES, NULL,
                                           "ECMA-402 §11.1.2 step 26 (GetOption dateStyle)" };
static const IntlOption OPT_TIME_STYLE = { "timeStyle", INTL_OPT_STRING, STYLES, NULL,
                                           "ECMA-402 §11.1.2 step 28 (GetOption timeStyle)" };

typedef struct {
    IntlState s;
} DateTimeFormatCtor;

static void ctor_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    intl_state_visit(ctx, &((DateTimeFormatCtor *)st)->s, v);
}

static JSValue ctor_fini(JSContext *ctx, void *st, bool take_result)
{
    (void)ctx;
    return intl_state_fini(&((DateTimeFormatCtor *)st)->s, take_result);
}

static const char *read_cstr(JSContext *ctx, IntlState *s, const char *prop, JSValue *holder)
{
    *holder = intl_read(ctx, s, prop);
    return JS_IsString(*holder) ? JS_ToCString(ctx, *holder) : NULL;
}

/* §11.1.2 steps 18-19's offset form: an ECMA-262 UTCOffset with at most one MinuteSecond, as §11.1.3
   FormatOffsetTimeZoneIdentifier's "±HH:MM". 1 written, 0 not an offset string, -1 an offset string with
   seconds (step 18.c's RangeError). */
static int offset_time_zone(const char *tz, char out[8])
{
    size_t n = strlen(tz), i = 3;
    int hh, mm = 0, parts = 0;

    if (n < 3 || (tz[0] != '+' && tz[0] != '-') || tz[1] < '0' || tz[1] > '9' || tz[2] < '0' || tz[2] > '9')
        return 0;
    hh = (tz[1] - '0') * 10 + (tz[2] - '0');
    if (hh > 23) return 0;
    while (i < n) {
        bool colon = tz[i] == ':';

        if (colon) i++;
        if (i + 2 > n || tz[i] < '0' || tz[i] > '5' || tz[i + 1] < '0' || tz[i + 1] > '9') return 0;
        if (parts == 0) mm = (tz[i] - '0') * 10 + (tz[i + 1] - '0');
        parts++;
        i += 2;
        if (parts == 2) {
            if (i < n && (tz[i] == '.' || tz[i] == ',')) {
                size_t f = ++i;

                while (i < n && tz[i] >= '0' && tz[i] <= '9') i++;
                if (i == f || i - f > 9) return 0;
            }
            if (i != n) return 0;
            return -1;
        }
    }
    if (hh == 0 && mm == 0 && tz[0] == '-') { snprintf(out, 8, "+00:00"); return 1; }   /* §11.1.3 step 1 */
    snprintf(out, 8, "%c%02d:%02d", tz[0], hh, mm);
    return 1;
}

/* The time zone the option named, as §11.1.2 steps 18-19 resolve it, into the slots. -1 having thrown. */
static int resolve_time_zone(JSContext *ctx, IntlState *s, JSValue tz)
{
    const char *c = JS_ToCString(ctx, tz);
    char off[8];
    int r;

    CHECK(c != NULL, "a timeZone option's bytes could not be read");
    r = offset_time_zone(c, off);
    if (r != 0) {
        JS_FreeCString(ctx, c);
        if (r < 0) { JS_ThrowRangeError(ctx, "Invalid time zone specified"); return -1; }
        JS_SetPropertyStr(ctx, s->slots, "timeZone", JS_NewString(ctx, off));
        return 0;
    }
    {
        IntlHostValue arg = { .kind = INTL_VALUE_STRING, .str = c, .len = strlen(c) };
        JSValue p = intl_ask(ctx, INTL_OP_TIME_ZONE_IDENTIFIER, JS_UNDEFINED, NULL, false, JS_UNDEFINED, NULL, &arg, 1);

        JS_FreeCString(ctx, c);
        if (JS_IsException(p)) return -1;                                                /* step 19.b */
        JS_SetPropertyStr(ctx, s->slots, "timeZone", p);                                 /* step 20 */
    }
    return 0;
}

/* Steps 30-33: the best format, from the host's data, its components and whether it has an hour. */
static int best_format(JSContext *ctx, IntlState *s)
{
    static const char *const KEYS[] = {
        "calendar", "numberingSystem", "hourCycle", "weekday", "era", "year", "month", "day", "dayPeriod", "hour",
        "minute", "second", "fractionalSecondDigits", "timeZoneName", "formatMatcher", "dateStyle", "timeStyle", NULL
    };
    JSValue fmt = idl_slots_new(ctx), best, ds = intl_read(ctx, s, "dateStyle"), ts = intl_read(ctx, s, "timeStyle");
    JSValue loc = JS_GetPropertyStr(ctx, s->slots, "locale"), hc;
    bool styled = !JS_IsUndefined(ds) || !JS_IsUndefined(ts), explicit_components = false, need_defaults = true;
    const char *l = JS_ToCString(ctx, loc);
    int i;

    CHECK(!JS_IsException(fmt) && l != NULL, "a DateTimeFormat's format record could not be allocated");
    JS_SetPropertyStr(ctx, fmt, "calendar", JS_GetPropertyStr(ctx, s->slots, "calendar"));
    JS_SetPropertyStr(ctx, fmt, "numberingSystem", JS_GetPropertyStr(ctx, s->slots, "numberingSystem"));
    hc = intl_read(ctx, s, "[[hc]]");
    JS_SetPropertyStr(ctx, fmt, "hourCycle", hc);                                        /* step 22 */
    for (i = 0; i < N_COMPONENTS; i++) {
        JSValue v = intl_read(ctx, s, COMPONENTS[i].prop);

        if (!JS_IsUndefined(v)) {
            explicit_components = true;
            /* Steps 31.c-e, required any: every row but era and timeZoneName decides needDefaults. */
            if (strcmp(COMPONENTS[i].prop, "era") != 0 && strcmp(COMPONENTS[i].prop, "timeZoneName") != 0)
                need_defaults = false;
        }
        JS_SetPropertyStr(ctx, fmt, COMPONENTS[i].prop, v);
    }
    JS_SetPropertyStr(ctx, fmt, "formatMatcher", intl_read(ctx, s, "formatMatcher"));
    if (styled) {
        if (explicit_components) {                                                       /* step 30.a */
            JS_ThrowTypeError(ctx, "Can't set option dateStyle or timeStyle with explicit date or time components");
            goto fail;
        }
        JS_SetPropertyStr(ctx, fmt, "dateStyle", JS_DupValue(ctx, ds));
        JS_SetPropertyStr(ctx, fmt, "timeStyle", JS_DupValue(ctx, ts));
    } else if (need_defaults) {                                                          /* step 31.f, defaults date */
        JS_SetPropertyStr(ctx, fmt, "year", JS_NewString(ctx, "numeric"));
        JS_SetPropertyStr(ctx, fmt, "month", JS_NewString(ctx, "numeric"));
        JS_SetPropertyStr(ctx, fmt, "day", JS_NewString(ctx, "numeric"));
    }
    best = intl_ask(ctx, INTL_OP_DATE_PATTERN, JS_UNDEFINED, l, false, fmt, KEYS, NULL, 0);
    if (JS_IsException(best)) goto fail;
    if (!styled)                                                                         /* resolvedOptions' rows */
        for (i = 0; i < N_COMPONENTS; i++)
            JS_SetPropertyStr(ctx, s->slots, COMPONENTS[i].prop, JS_GetPropertyStr(ctx, best, COMPONENTS[i].prop));
    {
        JSValue has = JS_GetPropertyStr(ctx, best, "hasHour");

        DCHECK(JS_IsBool(has), "the Intl host's best format states no hasHour");
        if (JS_ToBool(ctx, has)) JS_SetPropertyStr(ctx, s->slots, "hourCycle", intl_read(ctx, s, "[[hc]]"));
        JS_FreeValue(ctx, has);                                                          /* step 33 */
    }
    JS_FreeValue(ctx, best);
    JS_SetPropertyStr(ctx, s->slots, "dateStyle", ds);
    JS_SetPropertyStr(ctx, s->slots, "timeStyle", ts);
    JS_FreeValue(ctx, fmt);
    JS_FreeCString(ctx, l);
    JS_FreeValue(ctx, loc);
    return 0;
fail:
    JS_FreeValue(ctx, ds);
    JS_FreeValue(ctx, ts);
    JS_FreeValue(ctx, fmt);
    JS_FreeCString(ctx, l);
    JS_FreeValue(ctx, loc);
    return -1;
}

static int ctor_step(JSContext *ctx, void *st, JSValue cb, JSValue **out_cb, int *out_argc)
{
    IntlState *s = &((DateTimeFormatCtor *)st)->s;
    JSStepHdr *h = &s->hdr;
    JSValue v;
    int r;

    if (!s->started) intl_state_start(s);
    STEP_DISPATCH(DT_STAGES, h->stage, h->def->algorithm, JS_STEP_ABRUPT);

#define NEXT(to_) do { STEP_GOTO(h->stage, (to_), INTL_CURSORS(s)); return JS_STEP_YIELD; } while (0)
#define RUN(call) do { r = (call); if (r) return r < 0 ? JS_STEP_ABRUPT : r; } while (0)

    STEP_ARM(DT_CREATE);
    /* §11.1.1 step 1: an undefined NewTarget is the active function object. */
    RUN(intl_create_from_constructor(ctx, s, JS_IsUndefined(h->this_val) ? h->func_obj : h->this_val, g_class, cb,
                                     out_cb, out_argc));
    s->slots = idl_slots_new(ctx);
    CHECK(!JS_IsException(s->slots), "a DateTimeFormat's slots could not be allocated");
    NEXT(DT_LOCALES);

    STEP_ARM(DT_LOCALES);
    RUN(intl_canonicalize_locale_list(ctx, s, step_arg(h, 0), cb, out_cb, out_argc));
    if (intl_coerce_options(ctx, s, step_arg(h, 1)) < 0) return JS_STEP_ABRUPT;     /* §9.2.8 step 3 */
    NEXT(DT_MATCHER);

    STEP_ARM(DT_MATCHER);
    RUN(intl_get_option(ctx, s, &OPT_MATCHER, cb, &v, out_cb, out_argc));
    intl_set_opt(ctx, s, "localeMatcher", v);
    NEXT(DT_CA);

    STEP_ARM(DT_CA);
    RUN(intl_resolution_option(ctx, s, &OPT_CA, "ca", cb, out_cb, out_argc));
    NEXT(DT_NU);

    STEP_ARM(DT_NU);
    RUN(intl_resolution_option(ctx, s, &OPT_NU, "nu", cb, out_cb, out_argc));
    NEXT(DT_HOUR12);

    STEP_ARM(DT_HOUR12);
    RUN(intl_resolution_option(ctx, s, &OPT_HOUR12, "hour12", cb, out_cb, out_argc));
    NEXT(DT_HC);

    STEP_ARM(DT_HC);
    RUN(intl_resolution_option(ctx, s, &OPT_HC, "hc", cb, out_cb, out_argc));
    {
        static const char *const KEYS[] = { "localeMatcher", "ca", "nu", "hc", NULL };
        JSValue hour12 = JS_GetPropertyStr(ctx, s->opt, "hour12"), res, hc;
        const char *h12 = JS_IsString(hour12) ? JS_ToCString(ctx, hour12) : NULL;

        /* Step 3's modifyResolutionOptions: [[hour12]] leaves the record, and a stated one makes [[hc]] null. */
        if (h12) intl_set_opt(ctx, s, "hc", JS_NULL);
        res = intl_ask(ctx, INTL_OP_DATE_RESOLVE, s->requested, NULL, true, s->opt, KEYS, NULL, 0);
        if (JS_IsException(res)) { if (h12) JS_FreeCString(ctx, h12); JS_FreeValue(ctx, hour12); return JS_STEP_ABRUPT; }
        JS_SetPropertyStr(ctx, s->slots, "locale", JS_GetPropertyStr(ctx, res, "locale"));        /* step 8 */
        JS_SetPropertyStr(ctx, s->slots, "calendar", JS_GetPropertyStr(ctx, res, "calendar"));    /* step 10 */
        JS_SetPropertyStr(ctx, s->slots, "numberingSystem", JS_GetPropertyStr(ctx, res, "numberingSystem"));
        /* Steps 13-16: the hour cycle hour12 or the locale states. */
        if (h12) hc = JS_GetPropertyStr(ctx, res, strcmp(h12, "true") == 0 ? "hourCycle12" : "hourCycle24");
        else {
            hc = JS_GetPropertyStr(ctx, res, "hc");
            if (JS_IsNull(hc)) hc = JS_GetPropertyStr(ctx, res, "hourCycle");
        }
        intl_set_read(ctx, s, "[[hc]]", hc);
        if (h12) JS_FreeCString(ctx, h12);
        JS_FreeValue(ctx, hour12);
        JS_FreeValue(ctx, res);
    }
    NEXT(DT_TIME_ZONE);

    STEP_ARM(DT_TIME_ZONE);
    RUN(intl_get_option(ctx, s, &OPT_TIME_ZONE, cb, &v, out_cb, out_argc));
    r = resolve_time_zone(ctx, s, v);
    JS_FreeValue(ctx, v);
    if (r < 0) return JS_STEP_ABRUPT;
    s->k = 0;
    NEXT(DT_COMPONENT);

    STEP_ARM(DT_COMPONENT);
    {
        const Component *c = &COMPONENTS[s->k];

        if (strcmp(c->prop, "fractionalSecondDigits") == 0) {
            double d;

            RUN(intl_get_number_option(ctx, s, c->prop, 1, 3, NAN, cb, &d, out_cb, out_argc));
        } else {
            IntlOption o = { c->prop, INTL_OPT_STRING, c->values, NULL, c->fork_op };

            RUN(intl_get_option(ctx, s, &o, cb, &v, out_cb, out_argc));
            JS_FreeValue(ctx, v);
        }
        s->k++;
        if (s->k < N_COMPONENTS) return JS_STEP_YIELD;
    }
    NEXT(DT_FORMAT_MATCHER);

    STEP_ARM(DT_FORMAT_MATCHER);
    RUN(intl_get_option(ctx, s, &OPT_FORMAT_MATCHER, cb, &v, out_cb, out_argc));
    JS_FreeValue(ctx, v);
    NEXT(DT_DATE_STYLE);

    STEP_ARM(DT_DATE_STYLE);
    RUN(intl_get_option(ctx, s, &OPT_DATE_STYLE, cb, &v, out_cb, out_argc));
    JS_FreeValue(ctx, v);
    NEXT(DT_TIME_STYLE);

    STEP_ARM(DT_TIME_STYLE);
    RUN(intl_get_option(ctx, s, &OPT_TIME_STYLE, cb, &v, out_cb, out_argc));
    JS_FreeValue(ctx, v);
    if (best_format(ctx, s) < 0) return JS_STEP_ABRUPT;
    intl_slots_take_deps(ctx, s, s->slots);
    intl_slots_attach(ctx, s->obj, s->slots);
    s->slots = JS_UNDEFINED;
    s->result = s->obj;
    s->obj = JS_UNDEFINED;
    return JS_STEP_DONE;
#undef NEXT
#undef RUN
}

static const JSTrampStepDef CTOR_DEF = {
    sizeof(DateTimeFormatCtor), ctor_step, ctor_fini, 0, .visit = ctor_visit,
    .algorithm = "ECMA-402 §11.1.1 Intl.DateTimeFormat ( [ locales [ , options ] ] )", .steps = DT_STEPS
};

/* ---- §11.3.3 get Intl.DateTimeFormat.prototype.format, and §11.3.6 formatToParts -------------------------- */

#define DTF_STAGES(X) X(DTF_DATE, "ECMA-402 §11.3.3 step 4.a.i (ToNumber ( date ))")
enum { DTF_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const DTF_STEPS[] = { DTF_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSStepHdr hdr;
    JSValue result;
    uint8_t started;
} FormatState;

static void ff_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    FormatState *s = st;

    if (s->started) v->val(ctx, &s->result);
}

static JSValue ff_fini(JSContext *ctx, void *st, bool take_result)
{
    FormatState *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;

    (void)ctx;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

static int dtf_format(JSContext *ctx, FormatState *s, JSValueConst dtf, IntlHostOp op, const char *algorithm,
                      JSValue cb, JSValue **out_cb, int *out_argc)
{
    JSStepHdr *h = &s->hdr;
    JSValueConst date = step_arg(h, 0);
    JSValue slots, loc, real;
    IntlHostValue x;
    const char *l;
    double t;
    int r;

    if (!s->started) { s->result = JS_UNDEFINED; s->started = 1; }
    STEP_DISPATCH(DTF_STAGES, h->stage, h->def->algorithm, JS_STEP_ABRUPT);
    STEP_ARM(DTF_DATE);
    slots = intl_slots_of(ctx, dtf, g_class);
    if (JS_IsUndefined(slots)) {
        JS_FreeValue(ctx, cb);
        JS_ThrowTypeError(ctx, "%s called on an object that is not an Intl.DateTimeFormat", algorithm);
        return JS_STEP_ABRUPT;
    }
    if (JS_IsUndefined(date)) {
        JS_FreeValue(ctx, cb);
        t = JS_DateNow();                                                                /* ! Call(%Date.now%) */
    } else {
        ConcolicExState st = concolic_is(date) ? concolic_example_state(date) : CONCOLIC_EX_NONE;

        if (st == CONCOLIC_EX_HELD || st == CONCOLIC_EX_DETERMINED) {
            JSValue ex = concolic_example(ctx, date);

            JS_FreeValue(ctx, cb);
            r = JS_ToFloat64(ctx, &t, ex);                                               /* a primitive */
            JS_FreeValue(ctx, ex);
            if (r < 0) { JS_FreeValue(ctx, slots); return JS_STEP_ABRUPT; }
        } else {
            r = step_todouble_run(ctx, h, date, cb, &t, out_cb, out_argc);
            if (r) { JS_FreeValue(ctx, slots); return r < 0 ? JS_STEP_ABRUPT : r; }
        }
    }
    /* §11.5.5 PartitionDateTimePattern steps 1-2: TimeClip, and NaN is a RangeError. */
    if (!isfinite(t) || fabs(t) > 8.64e15) {
        JS_FreeValue(ctx, slots);
        JS_ThrowRangeError(ctx, "Invalid time value");
        return JS_STEP_ABRUPT;
    }
    t = trunc(t) + 0.0;
    x = (IntlHostValue){ .kind = INTL_VALUE_NUMBER, .num = t };
    loc = intl_slot(ctx, slots, "locale");
    l = JS_ToCString(ctx, loc);
    CHECK(l != NULL, "a DateTimeFormat's resolved locale could not be read");
    real = intl_ask(ctx, op, JS_UNDEFINED, l, false, slots, FORMAT_SLOTS, &x, 1);
    JS_FreeCString(ctx, l);
    JS_FreeValue(ctx, loc);
    if (JS_IsException(real)) { JS_FreeValue(ctx, slots); return JS_STEP_ABRUPT; }
    s->result = intl_derive(ctx, algorithm, slots, &date, JS_IsUndefined(date) ? 0 : 1, real);
    JS_FreeValue(ctx, slots);
    return JS_STEP_DONE;
}

static int ff_step(JSContext *ctx, void *st, JSValue cb, JSValue **out_cb, int *out_argc)
{
    FormatState *s = st;

    return dtf_format(ctx, s, JS_StepClosureData(&s->hdr, 0), INTL_OP_DATE_FORMAT,
                      "Intl.DateTimeFormat.prototype.format", cb, out_cb, out_argc);
}

static int fp_step(JSContext *ctx, void *st, JSValue cb, JSValue **out_cb, int *out_argc)
{
    FormatState *s = st;

    return dtf_format(ctx, s, s->hdr.this_val, INTL_OP_DATE_TO_PARTS, "Intl.DateTimeFormat.prototype.formatToParts",
                      cb, out_cb, out_argc);
}

static const JSTrampStepDef FORMAT_DEF = {
    sizeof(FormatState), ff_step, ff_fini, 0, .visit = ff_visit,
    .algorithm = "ECMA-402 §11.3.3 a bound DateTimeFormat format function", .steps = DTF_STEPS
};
static const JSTrampStepDef PARTS_DEF = {
    sizeof(FormatState), fp_step, ff_fini, 0, .visit = ff_visit,
    .algorithm = "ECMA-402 §11.3.6 Intl.DateTimeFormat.prototype.formatToParts ( date )", .steps = DTF_STEPS
};

static JSValue js_dtf_format(JSContext *ctx, JSValueConst this_val)
{
    JSValue slots = intl_slots_of(ctx, this_val, g_class), f;

    if (JS_IsUndefined(slots))                                                           /* step 3 */
        return JS_ThrowTypeError(ctx, "Intl.DateTimeFormat.prototype.format called on an object that is not an "
                                      "Intl.DateTimeFormat");
    f = JS_GetPropertyStr(ctx, slots, "boundFormat");
    if (JS_IsUndefined(f)) {                                                             /* step 4 */
        f = JS_NewStepClosure2(ctx, g_format_stepid, "", 1, 1, &this_val);
        CHECK(!JS_IsException(f), "a bound format function could not be allocated");
        JS_SetPropertyStr(ctx, slots, "boundFormat", JS_DupValue(ctx, f));
    }
    JS_FreeValue(ctx, slots);
    return f;
}

/* ---- §11.3.2 Intl.DateTimeFormat.prototype.resolvedOptions ( ) ------------------------------------------- */

static void resolved(JSContext *ctx, JSValueConst o, JSValueConst slots, const char *prop, JSValue v)
{
    if (JS_IsUndefined(v)) return;
    v = intl_derive(ctx, "Intl.DateTimeFormat.prototype.resolvedOptions", slots, NULL, 0, v);
    JS_DefinePropertyValueStr(ctx, o, prop, v, JS_PROP_C_W_E);
}

static JSValue js_dtf_resolved_options(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    static const char *const LEADING[] = { "locale", "calendar", "numberingSystem", "timeZone", "hourCycle", NULL };
    JSValue slots = intl_slots_of(ctx, this_val, g_class), o, hc, ds, ts;
    int i;

    (void)argc; (void)argv;
    if (JS_IsUndefined(slots))
        return JS_ThrowTypeError(ctx, "Intl.DateTimeFormat.prototype.resolvedOptions called on an object that is "
                                      "not an Intl.DateTimeFormat");
    o = JS_NewObject(ctx);
    CHECK(!JS_IsException(o), "resolvedOptions' object could not be allocated");
    for (i = 0; LEADING[i]; i++) resolved(ctx, o, slots, LEADING[i], intl_slot(ctx, slots, LEADING[i]));
    hc = intl_slot(ctx, slots, "hourCycle");
    if (JS_IsString(hc)) {                                                               /* Table 15's hour12 */
        const char *c = JS_ToCString(ctx, hc);

        resolved(ctx, o, slots, "hour12", JS_NewBool(ctx, c && (strcmp(c, "h11") == 0 || strcmp(c, "h12") == 0)));
        if (c) JS_FreeCString(ctx, c);
    }
    JS_FreeValue(ctx, hc);
    ds = intl_slot(ctx, slots, "dateStyle");
    ts = intl_slot(ctx, slots, "timeStyle");
    if (JS_IsUndefined(ds) && JS_IsUndefined(ts))
        for (i = 0; i < N_COMPONENTS; i++)
            resolved(ctx, o, slots, COMPONENTS[i].prop, intl_slot(ctx, slots, COMPONENTS[i].prop));
    resolved(ctx, o, slots, "dateStyle", ds);
    resolved(ctx, o, slots, "timeStyle", ts);
    JS_FreeValue(ctx, slots);
    return o;
}

static const JSCFunctionListEntry PROTO_FUNCS[] = {
    JS_CFUNC_DEF("resolvedOptions", 0, js_dtf_resolved_options),
    JS_CGETSET_DEF("format", js_dtf_format, NULL),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Intl.DateTimeFormat", JS_PROP_CONFIGURABLE),
};

JSValue date_time_format_install(JSContext *ctx)
{
    JSValue proto = JS_NewObject(ctx), ctor, parts;

    CHECK(!JS_IsException(proto), "%Intl.DateTimeFormat.prototype% could not be allocated");
    JS_SetPropertyFunctionList(ctx, proto, PROTO_FUNCS, (int)(sizeof PROTO_FUNCS / sizeof PROTO_FUNCS[0]));
    parts = JS_NewCFunction2(ctx, NULL, "formatToParts", 1, JS_CFUNC_step, g_parts_stepid);
    CHECK(!JS_IsException(parts), "formatToParts could not be allocated");
    JS_DefinePropertyValueStr(ctx, proto, "formatToParts", parts, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    ctor = JS_NewCFunction2(ctx, NULL, "DateTimeFormat", 0, JS_CFUNC_step_ctor, g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "%Intl.DateTimeFormat% could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    intl_install_supported_locales_of(ctx, ctor, INTL_SVC_DATE_TIME_FORMAT);
    JS_SetClassProto(ctx, g_class, proto);
    return ctor;
}

void date_time_format_init(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSClassDef d = { "Intl.DateTimeFormat", NULL, NULL, NULL, NULL };

    JS_NewClassID(rt, &g_class);
    JS_NewClass(rt, g_class, &d);
    g_ctor_stepid = JS_RegisterStepDef(rt, &CTOR_DEF);
    g_format_stepid = JS_RegisterStepDef(rt, &FORMAT_DEF);
    g_parts_stepid = JS_RegisterStepDef(rt, &PARTS_DEF);
    agent_state_class("intl", &g_class, "§11's Intl.DateTimeFormat class");
    agent_state_id("intl", &g_ctor_stepid, "§11.1.1's constructor machine");
    agent_state_id("intl", &g_format_stepid, "§11.3.3's bound format machine");
    agent_state_id("intl", &g_parts_stepid, "§11.3.6's formatToParts machine");
}

void date_time_format_free(JSRuntime *rt)
{
    (void)rt;   /* its class and machines are ids the row's undo puts back */
    agent_state_reached("intl");
}
