/* ECMA-402 §16 NumberFormat Objects. See number_format.h. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/agent_state.h"
#include "core/idl_slots.h"
#include "core/intl/intl_service.h"
#include "core/intl/number_format.h"
#include "solver/concolic.h"

static JSClassID g_class;
static int g_ctor_stepid = -1, g_format_stepid = -1, g_parts_stepid = -1;

/* §16.3.2's Table 28, in its order. */
static const char *const RESOLVED[] = {
    "locale", "numberingSystem", "style", "currency", "currencyDisplay", "currencySign", "unit", "unitDisplay",
    "minimumIntegerDigits", "minimumFractionDigits", "maximumFractionDigits", "minimumSignificantDigits",
    "maximumSignificantDigits", "useGrouping", "notation", "compactDisplay", "signDisplay", "roundingIncrement",
    "roundingMode", "roundingPriority", "trailingZeroDisplay", NULL
};
/* What the host formats with: Table 28 but the locale, which travels as the request's locale, and the
   [[RoundingType]] the table does not report. */
static const char *const FORMAT_SLOTS[] = {
    "numberingSystem", "style", "currency", "currencyDisplay", "currencySign", "unit", "unitDisplay",
    "minimumIntegerDigits", "minimumFractionDigits", "maximumFractionDigits", "minimumSignificantDigits",
    "maximumSignificantDigits", "useGrouping", "notation", "compactDisplay", "signDisplay", "roundingIncrement",
    "roundingMode", "roundingPriority", "trailingZeroDisplay", "roundingType", NULL
};

/* ---- §16.1.1 Intl.NumberFormat ( [ locales [ , options ] ] ) --------------------------------------------- */

#define NF_STAGES(X)                                                                                          \
    X(NF_CREATE, "ECMA-402 §16.1.1 step 2 (OrdinaryCreateFromConstructor)")                                   \
    X(NF_LOCALES, "ECMA-402 §9.2.8 ResolveOptions step 1 (CanonicalizeLocaleList)")                           \
    X(NF_MATCHER, "ECMA-402 §9.2.8 step 4 (GetOption localeMatcher)")                                         \
    X(NF_NU, "ECMA-402 §9.2.8 step 6 (the numberingSystem descriptor)")                                      \
    X(NF_STYLE, "ECMA-402 §16.1.3 step 1 (GetOption style)")                                                  \
    X(NF_CURRENCY, "ECMA-402 §16.1.3 step 3 (GetOption currency)")                                            \
    X(NF_CURRENCY_DISPLAY, "ECMA-402 §16.1.3 step 6 (GetOption currencyDisplay)")                             \
    X(NF_CURRENCY_SIGN, "ECMA-402 §16.1.3 step 7 (GetOption currencySign)")                                   \
    X(NF_UNIT, "ECMA-402 §16.1.3 step 8 (GetOption unit)")                                                    \
    X(NF_UNIT_DISPLAY, "ECMA-402 §16.1.3 step 11 (GetOption unitDisplay)")                                    \
    X(NF_NOTATION, "ECMA-402 §16.1.1 step 11 (GetOption notation)")                                           \
    X(NF_MIN_INT, "ECMA-402 §16.1.2 step 1 (GetNumberOption minimumIntegerDigits)")                           \
    X(NF_MIN_FD, "ECMA-402 §16.1.2 step 2 (Get minimumFractionDigits)")                                       \
    X(NF_MAX_FD, "ECMA-402 §16.1.2 step 3 (Get maximumFractionDigits)")                                       \
    X(NF_MIN_SD, "ECMA-402 §16.1.2 step 4 (Get minimumSignificantDigits)")                                    \
    X(NF_MAX_SD, "ECMA-402 §16.1.2 step 5 (Get maximumSignificantDigits)")                                    \
    X(NF_INCREMENT, "ECMA-402 §16.1.2 step 6 (GetNumberOption roundingIncrement)")                            \
    X(NF_ROUNDING_MODE, "ECMA-402 §16.1.2 step 8 (GetOption roundingMode)")                                   \
    X(NF_PRIORITY, "ECMA-402 §16.1.2 step 9 (GetOption roundingPriority)")                                    \
    X(NF_TRAILING, "ECMA-402 §16.1.2 step 10 (GetOption trailingZeroDisplay)")                                \
    X(NF_MIN_SD_NUM, "ECMA-402 §16.1.2 step 21.a.i (DefaultNumberOption minimumSignificantDigits)")           \
    X(NF_MAX_SD_NUM, "ECMA-402 §16.1.2 step 21.a.ii (DefaultNumberOption maximumSignificantDigits)")          \
    X(NF_MIN_FD_NUM, "ECMA-402 §16.1.2 step 22.a.i (DefaultNumberOption minimumFractionDigits)")              \
    X(NF_MAX_FD_NUM, "ECMA-402 §16.1.2 step 22.a.ii (DefaultNumberOption maximumFractionDigits)")             \
    X(NF_COMPACT, "ECMA-402 §16.1.1 step 17 (GetOption compactDisplay)")                                      \
    X(NF_GROUPING, "ECMA-402 §16.1.1 step 21 (GetBooleanOrStringNumberFormatOption useGrouping)")             \
    X(NF_SIGN, "ECMA-402 §16.1.1 step 25 (GetOption signDisplay)")
enum { NF_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const NF_STEPS[] = { NF_STAGES(JS_STEP_STAGE_LABEL) NULL };

static const char *const MATCHERS[] = { "lookup", "best fit", NULL };
static const char *const STYLES[] = { "decimal", "percent", "currency", "unit", NULL };
static const char *const CURRENCY_DISPLAYS[] = { "code", "symbol", "narrowSymbol", "name", NULL };
static const char *const CURRENCY_SIGNS[] = { "standard", "accounting", NULL };
static const char *const UNIT_DISPLAYS[] = { "short", "narrow", "long", NULL };
static const char *const NOTATIONS[] = { "standard", "scientific", "engineering", "compact", NULL };
static const char *const ROUNDING_MODES[] = { "ceil", "floor", "expand", "trunc", "halfCeil", "halfFloor",
                                              "halfExpand", "halfTrunc", "halfEven", NULL };
static const char *const PRIORITIES[] = { "auto", "morePrecision", "lessPrecision", NULL };
static const char *const TRAILING[] = { "auto", "stripIfInteger", NULL };
static const char *const COMPACT_DISPLAYS[] = { "short", "long", NULL };
static const char *const GROUPINGS[] = { "min2", "auto", "always", "true", "false", NULL };
static const char *const SIGNS[] = { "auto", "never", "always", "exceptZero", "negative", NULL };

#define OPT(name, prop, vals, dflt, stage) \
    static const IntlOption name = { prop, INTL_OPT_STRING, vals, dflt, "ECMA-402 " stage }
OPT(OPT_MATCHER, "localeMatcher", MATCHERS, "best fit", "§9.2.8 step 4 (GetOption localeMatcher), NumberFormat");
OPT(OPT_NU, "numberingSystem", NULL, NULL, "§9.2.8 step 6 (GetOption numberingSystem), NumberFormat");
OPT(OPT_STYLE, "style", STYLES, "decimal", "§16.1.3 step 1 (GetOption style)");
OPT(OPT_CURRENCY, "currency", NULL, NULL, "§16.1.3 step 3 (GetOption currency)");
OPT(OPT_CURRENCY_DISPLAY, "currencyDisplay", CURRENCY_DISPLAYS, "symbol", "§16.1.3 step 6 (GetOption currencyDisplay)");
OPT(OPT_CURRENCY_SIGN, "currencySign", CURRENCY_SIGNS, "standard", "§16.1.3 step 7 (GetOption currencySign)");
OPT(OPT_UNIT, "unit", NULL, NULL, "§16.1.3 step 8 (GetOption unit)");
OPT(OPT_UNIT_DISPLAY, "unitDisplay", UNIT_DISPLAYS, "short", "§16.1.3 step 11 (GetOption unitDisplay)");
OPT(OPT_NOTATION, "notation", NOTATIONS, "standard", "§16.1.1 step 11 (GetOption notation)");
OPT(OPT_ROUNDING_MODE, "roundingMode", ROUNDING_MODES, "halfExpand", "§16.1.2 step 8 (GetOption roundingMode)");
OPT(OPT_PRIORITY, "roundingPriority", PRIORITIES, "auto", "§16.1.2 step 9 (GetOption roundingPriority)");
OPT(OPT_TRAILING, "trailingZeroDisplay", TRAILING, "auto", "§16.1.2 step 10 (GetOption trailingZeroDisplay)");
OPT(OPT_COMPACT, "compactDisplay", COMPACT_DISPLAYS, "short", "§16.1.1 step 17 (GetOption compactDisplay)");
OPT(OPT_SIGN, "signDisplay", SIGNS, "auto", "§16.1.1 step 25 (GetOption signDisplay)");
#undef OPT

typedef struct {
    IntlState s;
} NumberFormatCtor;

static void ctor_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    intl_state_visit(ctx, &((NumberFormatCtor *)st)->s, v);
}

static JSValue ctor_fini(JSContext *ctx, void *st, bool take_result)
{
    (void)ctx;
    return intl_state_fini(&((NumberFormatCtor *)st)->s, take_result);
}

static bool read_is(JSContext *ctx, IntlState *s, const char *prop, const char *value)
{
    JSValue v = intl_read(ctx, s, prop);
    const char *c = JS_IsString(v) ? JS_ToCString(ctx, v) : NULL;
    bool eq = c && strcmp(c, value) == 0;

    if (c) JS_FreeCString(ctx, c);
    JS_FreeValue(ctx, v);
    return eq;
}

static bool read_is_slot(JSContext *ctx, JSValueConst slots, const char *name, const char *value)
{
    JSValue v = JS_GetPropertyStr(ctx, slots, name);
    const char *c = JS_IsString(v) ? JS_ToCString(ctx, v) : NULL;
    bool eq = c && strcmp(c, value) == 0;

    if (c) JS_FreeCString(ctx, c);
    JS_FreeValue(ctx, v);
    return eq;
}

static bool raw_present(JSContext *ctx, IntlState *s, const char *prop)
{
    JSValue v = JS_IsUndefined(s->raw) ? JS_UNDEFINED : JS_GetPropertyStr(ctx, s->raw, prop);
    bool present = !JS_IsUndefined(v);

    JS_FreeValue(ctx, v);
    return present;
}

static double slot_num(JSContext *ctx, JSValueConst slots, const char *name)
{
    JSValue v = JS_GetPropertyStr(ctx, slots, name);
    double d = NAN;

    if (JS_IsNumber(v)) JS_ToFloat64(ctx, &d, v);
    JS_FreeValue(ctx, v);
    return d;
}

static void set_num(JSContext *ctx, JSValueConst slots, const char *name, double d)
{
    JS_SetPropertyStr(ctx, slots, name, JS_NewFloat64(ctx, d));
}

/* §16.1.2's needSD and needFD, from steps 17-20 over what steps 1-10 read. */
static void digit_needs(JSContext *ctx, IntlState *s, bool *need_sd, bool *need_fd, bool *has_sd, bool *has_fd)
{
    *has_sd = raw_present(ctx, s, "minimumSignificantDigits") || raw_present(ctx, s, "maximumSignificantDigits");
    *has_fd = raw_present(ctx, s, "minimumFractionDigits") || raw_present(ctx, s, "maximumFractionDigits");
    if (read_is(ctx, s, "roundingPriority", "auto")) {
        *need_sd = *has_sd;
        *need_fd = !*need_sd && !(!*has_fd && read_is(ctx, s, "notation", "compact"));
    } else {
        *need_sd = *need_fd = true;
    }
}

/* §16.1.1 steps 13-15: the fraction digit defaults by style and notation, CurrencyDigits from the host. */
static int fraction_defaults(JSContext *ctx, IntlState *s, double *min_fd, double *max_fd)
{
    if (read_is(ctx, s, "style", "currency") && read_is(ctx, s, "notation", "standard")) {
        JSValue cur = JS_GetPropertyStr(ctx, s->slots, "currency"), d;
        const char *c = JS_ToCString(ctx, cur);
        IntlHostValue arg = { .kind = INTL_VALUE_STRING, .str = c, .len = c ? strlen(c) : 0 };

        CHECK(c != NULL, "a currency NumberFormat's [[Currency]] could not be read");
        d = intl_ask(ctx, INTL_OP_CURRENCY_DIGITS, JS_UNDEFINED, NULL, false, JS_UNDEFINED, NULL, &arg, 1);
        JS_FreeCString(ctx, c);
        JS_FreeValue(ctx, cur);
        if (JS_IsException(d)) return -1;
        JS_ToFloat64(ctx, min_fd, d);
        JS_FreeValue(ctx, d);
        *max_fd = *min_fd;
    } else if (read_is(ctx, s, "style", "percent")) {
        *min_fd = *max_fd = 0;
    } else {
        *min_fd = 0;
        *max_fd = 3;
    }
    return 0;
}

/* §16.1.2 steps 12-28, after the conversions steps 21-22 performed: the rounding slots. */
static int digit_slots(JSContext *ctx, IntlState *s)
{
    JSValueConst sl = s->slots;
    bool need_sd, need_fd, has_sd, has_fd;
    double min_fd_default, max_fd_default, inc = slot_num(ctx, sl, "roundingIncrement");
    double rmin_fd = NAN, rmax_fd = NAN;
    JSValue v;

    if (fraction_defaults(ctx, s, &min_fd_default, &max_fd_default) < 0) return -1;
    if (inc != 1) max_fd_default = min_fd_default;                                       /* step 12 */
    digit_needs(ctx, s, &need_sd, &need_fd, &has_sd, &has_fd);
    if (need_sd && !has_sd) {                                                            /* step 21.b */
        set_num(ctx, sl, "minimumSignificantDigits", 1);
        set_num(ctx, sl, "maximumSignificantDigits", 21);
    }
    if (need_fd) {                                                                       /* step 22 */
        if (has_fd) {
            v = intl_read(ctx, s, "minimumFractionDigits");
            if (JS_IsNumber(v)) JS_ToFloat64(ctx, &rmin_fd, v);
            JS_FreeValue(ctx, v);
            v = intl_read(ctx, s, "maximumFractionDigits");
            if (JS_IsNumber(v)) JS_ToFloat64(ctx, &rmax_fd, v);
            JS_FreeValue(ctx, v);
            if (isnan(rmin_fd)) rmin_fd = fmin(min_fd_default, rmax_fd);
            else if (isnan(rmax_fd)) rmax_fd = fmax(max_fd_default, rmin_fd);
            else if (rmin_fd > rmax_fd) {
                JS_ThrowRangeError(ctx, "minimumFractionDigits is greater than maximumFractionDigits");
                return -1;
            }
        } else {
            rmin_fd = min_fd_default;
            rmax_fd = max_fd_default;
        }
        set_num(ctx, sl, "minimumFractionDigits", rmin_fd);
        set_num(ctx, sl, "maximumFractionDigits", rmax_fd);
    }
    if (!need_sd && !need_fd) {                                                          /* steps 23-27 */
        set_num(ctx, sl, "minimumFractionDigits", 0);
        set_num(ctx, sl, "maximumFractionDigits", 0);
        set_num(ctx, sl, "minimumSignificantDigits", 1);
        set_num(ctx, sl, "maximumSignificantDigits", 2);
        JS_SetPropertyStr(ctx, sl, "roundingType", JS_NewString(ctx, "morePrecision"));
        JS_SetPropertyStr(ctx, sl, "roundingPriority", JS_NewString(ctx, "morePrecision"));
    } else if (read_is(ctx, s, "roundingPriority", "morePrecision") ||
               read_is(ctx, s, "roundingPriority", "lessPrecision")) {
        JS_SetPropertyStr(ctx, sl, "roundingType", intl_read(ctx, s, "roundingPriority"));
        JS_SetPropertyStr(ctx, sl, "roundingPriority", intl_read(ctx, s, "roundingPriority"));
    } else {
        JS_SetPropertyStr(ctx, sl, "roundingType", JS_NewString(ctx, has_sd ? "significantDigits" : "fractionDigits"));
        JS_SetPropertyStr(ctx, sl, "roundingPriority", JS_NewString(ctx, "auto"));
    }
    if (inc != 1) {                                                                      /* step 28 */
        if (!read_is_slot(ctx, sl, "roundingType", "fractionDigits")) {
            JS_ThrowTypeError(ctx, "roundingIncrement is only valid with fraction digits rounding");
            return -1;
        }
        if (slot_num(ctx, sl, "maximumFractionDigits") != slot_num(ctx, sl, "minimumFractionDigits")) {
            JS_ThrowRangeError(ctx, "roundingIncrement needs maximumFractionDigits equal to minimumFractionDigits");
            return -1;
        }
    }
    return 0;
}

static const double INCREMENTS[] = { 1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 2500, 5000 };

/* The first of §16.1.2's conversions (steps 21-22) this instance needs, or the step after them. */
static int after_trailing(JSContext *ctx, IntlState *s)
{
    bool need_sd, need_fd, has_sd, has_fd;

    digit_needs(ctx, s, &need_sd, &need_fd, &has_sd, &has_fd);
    if (need_sd && has_sd) return NF_MIN_SD_NUM;
    if (need_fd && has_fd) return NF_MIN_FD_NUM;
    return NF_COMPACT;
}

static int after_sd(JSContext *ctx, IntlState *s)
{
    bool need_sd, need_fd, has_sd, has_fd;

    digit_needs(ctx, s, &need_sd, &need_fd, &has_sd, &has_fd);
    return need_fd && has_fd ? NF_MIN_FD_NUM : NF_COMPACT;
}

/* A string GetOption whose answer is a slot. */
static int option_slot(JSContext *ctx, IntlState *s, const IntlOption *o, JSValue cb, JSValue **out_cb, int *out_argc)
{
    JSValue v;
    int r = intl_get_option(ctx, s, o, cb, &v, out_cb, out_argc);

    if (r) return r;
    JS_SetPropertyStr(ctx, s->slots, o->prop, v);
    return 0;
}

static int ctor_step(JSContext *ctx, void *st, JSValue cb, JSValue **out_cb, int *out_argc)
{
    IntlState *s = &((NumberFormatCtor *)st)->s;
    JSStepHdr *h = &s->hdr;
    JSValue v;
    double d;
    int r, to;

    if (!s->started) intl_state_start(s);
    STEP_DISPATCH(NF_STAGES, h->stage, h->def->algorithm, JS_STEP_ABRUPT);

#define NEXT(to_) do { STEP_GOTO(h->stage, (to_), INTL_CURSORS(s)); return JS_STEP_YIELD; } while (0)
#define RUN(call) do { r = (call); if (r) return r < 0 ? JS_STEP_ABRUPT : r; } while (0)

    STEP_ARM(NF_CREATE);
    RUN(intl_create_from_constructor(ctx, s, JS_IsUndefined(h->this_val) ? h->func_obj : h->this_val, g_class, cb,
                                     out_cb, out_argc));
    s->slots = idl_slots_new(ctx);
    CHECK(!JS_IsException(s->slots), "a NumberFormat's slots could not be allocated");
    NEXT(NF_LOCALES);

    STEP_ARM(NF_LOCALES);
    RUN(intl_canonicalize_locale_list(ctx, s, step_arg(h, 0), cb, out_cb, out_argc));
    if (intl_coerce_options(ctx, s, step_arg(h, 1)) < 0) return JS_STEP_ABRUPT;     /* §9.2.8 step 3 */
    NEXT(NF_MATCHER);

    STEP_ARM(NF_MATCHER);
    RUN(intl_get_option(ctx, s, &OPT_MATCHER, cb, &v, out_cb, out_argc));
    intl_set_opt(ctx, s, "localeMatcher", v);
    NEXT(NF_NU);

    STEP_ARM(NF_NU);
    RUN(intl_resolution_option(ctx, s, &OPT_NU, "nu", cb, out_cb, out_argc));
    {
        static const char *const KEYS[] = { "localeMatcher", "nu", NULL };
        JSValue res = intl_ask(ctx, INTL_OP_NUMBER_RESOLVE, s->requested, NULL, true, s->opt, KEYS, NULL, 0);

        if (JS_IsException(res)) return JS_STEP_ABRUPT;
        JS_SetPropertyStr(ctx, s->slots, "locale", JS_GetPropertyStr(ctx, res, "locale"));      /* step 6 */
        JS_SetPropertyStr(ctx, s->slots, "numberingSystem", JS_GetPropertyStr(ctx, res, "numberingSystem"));
        JS_FreeValue(ctx, res);
    }
    NEXT(NF_STYLE);

    STEP_ARM(NF_STYLE);
    RUN(option_slot(ctx, s, &OPT_STYLE, cb, out_cb, out_argc));
    NEXT(NF_CURRENCY);

    STEP_ARM(NF_CURRENCY);
    RUN(intl_get_option(ctx, s, &OPT_CURRENCY, cb, &v, out_cb, out_argc));
    if (JS_IsUndefined(v)) {                                                             /* §16.1.3 step 4 */
        if (read_is(ctx, s, "style", "currency")) {
            JS_ThrowTypeError(ctx, "Currency code is required with currency style.");
            return JS_STEP_ABRUPT;
        }
    } else {
        size_t n = 0;
        const char *c = JS_ToCStringLen(ctx, &n, v);
        bool ok = c && intl_is_well_formed_currency_code(c, n);

        if (c) JS_FreeCString(ctx, c);
        if (!ok) {                                                                       /* step 5 */
            JS_FreeValue(ctx, v);
            JS_ThrowRangeError(ctx, "Invalid currency code");
            return JS_STEP_ABRUPT;
        }
    }
    JS_FreeValue(ctx, v);
    NEXT(NF_CURRENCY_DISPLAY);

    STEP_ARM(NF_CURRENCY_DISPLAY);
    RUN(intl_get_option(ctx, s, &OPT_CURRENCY_DISPLAY, cb, &v, out_cb, out_argc));
    JS_FreeValue(ctx, v);
    NEXT(NF_CURRENCY_SIGN);

    STEP_ARM(NF_CURRENCY_SIGN);
    RUN(intl_get_option(ctx, s, &OPT_CURRENCY_SIGN, cb, &v, out_cb, out_argc));
    JS_FreeValue(ctx, v);
    NEXT(NF_UNIT);

    STEP_ARM(NF_UNIT);
    RUN(intl_get_option(ctx, s, &OPT_UNIT, cb, &v, out_cb, out_argc));
    if (JS_IsUndefined(v)) {                                                             /* step 9 */
        if (read_is(ctx, s, "style", "unit")) {
            JS_ThrowTypeError(ctx, "Unit is required with unit style.");
            return JS_STEP_ABRUPT;
        }
    } else {
        size_t n = 0;
        const char *c = JS_ToCStringLen(ctx, &n, v);
        bool ok = c && intl_is_well_formed_unit_identifier(c, n);

        if (c) JS_FreeCString(ctx, c);
        if (!ok) {                                                                       /* step 10 */
            JS_FreeValue(ctx, v);
            JS_ThrowRangeError(ctx, "Invalid unit argument");
            return JS_STEP_ABRUPT;
        }
    }
    JS_FreeValue(ctx, v);
    NEXT(NF_UNIT_DISPLAY);

    STEP_ARM(NF_UNIT_DISPLAY);
    RUN(intl_get_option(ctx, s, &OPT_UNIT_DISPLAY, cb, &v, out_cb, out_argc));
    JS_FreeValue(ctx, v);
    if (read_is(ctx, s, "style", "currency")) {                                          /* step 12 */
        JSValue cur = intl_read(ctx, s, "currency"), up;
        const char *c = JS_ToCString(ctx, cur);
        char buf[4];
        int i;

        CHECK(c != NULL && strlen(c) == 3, "a well-formed currency code is not three letters");
        for (i = 0; i < 3; i++) buf[i] = (char)(c[i] >= 'a' && c[i] <= 'z' ? c[i] - 'a' + 'A' : c[i]);
        buf[3] = '\0';
        up = JS_NewString(ctx, buf);
        JS_FreeCString(ctx, c);
        JS_FreeValue(ctx, cur);
        JS_SetPropertyStr(ctx, s->slots, "currency", up);
        JS_SetPropertyStr(ctx, s->slots, "currencyDisplay", intl_read(ctx, s, "currencyDisplay"));
        JS_SetPropertyStr(ctx, s->slots, "currencySign", intl_read(ctx, s, "currencySign"));
    }
    if (read_is(ctx, s, "style", "unit")) {                                              /* step 13 */
        JS_SetPropertyStr(ctx, s->slots, "unit", intl_read(ctx, s, "unit"));
        JS_SetPropertyStr(ctx, s->slots, "unitDisplay", intl_read(ctx, s, "unitDisplay"));
    }
    NEXT(NF_NOTATION);

    STEP_ARM(NF_NOTATION);
    RUN(option_slot(ctx, s, &OPT_NOTATION, cb, out_cb, out_argc));
    NEXT(NF_MIN_INT);

    STEP_ARM(NF_MIN_INT);
    RUN(intl_get_number_option(ctx, s, "minimumIntegerDigits", 1, 21, 1, cb, &d, out_cb, out_argc));
    set_num(ctx, s->slots, "minimumIntegerDigits", d);
    NEXT(NF_MIN_FD);

    STEP_ARM(NF_MIN_FD);
    RUN(intl_get_raw(ctx, s, "minimumFractionDigits", cb, out_cb, out_argc));
    NEXT(NF_MAX_FD);

    STEP_ARM(NF_MAX_FD);
    RUN(intl_get_raw(ctx, s, "maximumFractionDigits", cb, out_cb, out_argc));
    NEXT(NF_MIN_SD);

    STEP_ARM(NF_MIN_SD);
    RUN(intl_get_raw(ctx, s, "minimumSignificantDigits", cb, out_cb, out_argc));
    NEXT(NF_MAX_SD);

    STEP_ARM(NF_MAX_SD);
    RUN(intl_get_raw(ctx, s, "maximumSignificantDigits", cb, out_cb, out_argc));
    NEXT(NF_INCREMENT);

    STEP_ARM(NF_INCREMENT);
    RUN(intl_get_number_option(ctx, s, "roundingIncrement", 1, 5000, 1, cb, &d, out_cb, out_argc));
    {
        size_t i;
        bool listed = false;

        for (i = 0; i < sizeof INCREMENTS / sizeof INCREMENTS[0]; i++) listed = listed || INCREMENTS[i] == d;
        if (!listed) {                                                                   /* step 7 */
            JS_ThrowRangeError(ctx, "roundingIncrement value is out of range");
            return JS_STEP_ABRUPT;
        }
    }
    set_num(ctx, s->slots, "roundingIncrement", d);
    NEXT(NF_ROUNDING_MODE);

    STEP_ARM(NF_ROUNDING_MODE);
    RUN(option_slot(ctx, s, &OPT_ROUNDING_MODE, cb, out_cb, out_argc));
    NEXT(NF_PRIORITY);

    STEP_ARM(NF_PRIORITY);
    RUN(intl_get_option(ctx, s, &OPT_PRIORITY, cb, &v, out_cb, out_argc));
    JS_FreeValue(ctx, v);
    NEXT(NF_TRAILING);

    STEP_ARM(NF_TRAILING);
    RUN(option_slot(ctx, s, &OPT_TRAILING, cb, out_cb, out_argc));
    to = after_trailing(ctx, s);
    if (to == NF_COMPACT && digit_slots(ctx, s) < 0) return JS_STEP_ABRUPT;
    NEXT(to);

    STEP_ARM(NF_MIN_SD_NUM);
    RUN(intl_default_number_option(ctx, s, "minimumSignificantDigits", 1, 21, 1, cb, &d, out_cb, out_argc));
    set_num(ctx, s->slots, "minimumSignificantDigits", d);
    NEXT(NF_MAX_SD_NUM);

    STEP_ARM(NF_MAX_SD_NUM);
    RUN(intl_default_number_option(ctx, s, "maximumSignificantDigits",
                                   slot_num(ctx, s->slots, "minimumSignificantDigits"), 21, 21, cb, &d, out_cb,
                                   out_argc));
    set_num(ctx, s->slots, "maximumSignificantDigits", d);
    to = after_sd(ctx, s);
    if (to == NF_COMPACT && digit_slots(ctx, s) < 0) return JS_STEP_ABRUPT;
    NEXT(to);

    STEP_ARM(NF_MIN_FD_NUM);
    RUN(intl_default_number_option(ctx, s, "minimumFractionDigits", 0, 100, NAN, cb, &d, out_cb, out_argc));
    NEXT(NF_MAX_FD_NUM);

    STEP_ARM(NF_MAX_FD_NUM);
    RUN(intl_default_number_option(ctx, s, "maximumFractionDigits", 0, 100, NAN, cb, &d, out_cb, out_argc));
    if (digit_slots(ctx, s) < 0) return JS_STEP_ABRUPT;
    NEXT(NF_COMPACT);

    STEP_ARM(NF_COMPACT);
    RUN(intl_get_option(ctx, s, &OPT_COMPACT, cb, &v, out_cb, out_argc));
    if (read_is(ctx, s, "notation", "compact")) JS_SetPropertyStr(ctx, s->slots, "compactDisplay", v);   /* 19 */
    else JS_FreeValue(ctx, v);
    NEXT(NF_GROUPING);

    STEP_ARM(NF_GROUPING);
    {
        JSValue dflt = JS_NewString(ctx, read_is(ctx, s, "notation", "compact") ? "min2" : "auto");   /* 18-19 */

        r = intl_get_bool_or_string_option(ctx, s, "useGrouping", GROUPINGS,
                                           "ECMA-402 §16.1.1 step 21 (GetBooleanOrStringNumberFormatOption "
                                           "useGrouping)", dflt, cb, &v, out_cb, out_argc);
        if (r) { JS_FreeValue(ctx, dflt); return r < 0 ? JS_STEP_ABRUPT : r; }
        if (JS_IsString(v) && (read_is(ctx, s, "useGrouping", "true") || read_is(ctx, s, "useGrouping", "false"))) {
            JS_FreeValue(ctx, v);                                                        /* step 22 */
            v = JS_DupValue(ctx, dflt);
        }
        JS_FreeValue(ctx, dflt);
        if (JS_IsBool(v) && JS_ToBool(ctx, v)) v = JS_NewString(ctx, "always");          /* step 23 */
        JS_SetPropertyStr(ctx, s->slots, "useGrouping", v);
    }
    NEXT(NF_SIGN);

    STEP_ARM(NF_SIGN);
    RUN(option_slot(ctx, s, &OPT_SIGN, cb, out_cb, out_argc));
    intl_slots_take_deps(ctx, s, s->slots);
    intl_slots_attach(ctx, s->obj, s->slots);
    s->slots = JS_UNDEFINED;
    s->result = s->obj;                                                                  /* step 28 */
    s->obj = JS_UNDEFINED;
    return JS_STEP_DONE;
#undef NEXT
#undef RUN
}

static const JSTrampStepDef CTOR_DEF = {
    sizeof(NumberFormatCtor), ctor_step, ctor_fini, 0, .visit = ctor_visit,
    .algorithm = "ECMA-402 §16.1.1 Intl.NumberFormat ( [ locales [ , options ] ] )", .steps = NF_STEPS
};

/* ---- §16.5.15 ToIntlMathematicalValue ( value ) ----------------------------------------------------------- */

/* One StrWhiteSpaceChar of ECMAScript §7.1.4.1 ToNumber Applied to the String Type at `p`, as its UTF-8 length,
   or 0. */
static size_t str_white_space(const unsigned char *p, size_t n)
{
    unsigned int c;

    if (n >= 1 && (p[0] == 9 || p[0] == 10 || p[0] == 11 || p[0] == 12 || p[0] == 13 || p[0] == 32)) return 1;
    if (n >= 2 && p[0] == 0xC2 && p[1] == 0xA0) return 2;                                 /* U+00A0 */
    if (n < 3 || (p[0] & 0xF0) != 0xE0) return 0;
    c = ((unsigned)(p[0] & 0x0F) << 12) | ((unsigned)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    if (c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F ||
        c == 0x3000 || c == 0xFEFF)
        return 3;
    return 0;
}

/* A non-decimal integer literal's digits in `base`, as the exact decimal string; malloc'd. */
static char *radix_to_decimal(const char *p, size_t n, int base)
{
    unsigned char *dec = calloc(n * 4 + 2, 1);   /* decimal digits, least significant first */
    size_t len = 1, i, k;
    char *out;

    CHECK(dec != NULL, "a non-decimal literal's digits could not be allocated");
    for (i = 0; i < n; i++) {
        int d = p[i] >= '0' && p[i] <= '9' ? p[i] - '0' : (p[i] | 0x20) - 'a' + 10;
        unsigned carry = (unsigned)d;

        for (k = 0; k < len; k++) {
            unsigned v = dec[k] * (unsigned)base + carry;

            dec[k] = (unsigned char)(v % 10);
            carry = v / 10;
        }
        while (carry) { dec[len++] = (unsigned char)(carry % 10); carry /= 10; }
    }
    while (len > 1 && dec[len - 1] == 0) len--;
    out = malloc(len + 1);
    CHECK(out != NULL, "a non-decimal literal's decimal could not be allocated");
    for (k = 0; k < len; k++) out[k] = (char)('0' + dec[len - 1 - k]);
    out[len] = '\0';
    free(dec);
    return out;
}

/* The Intl mathematical value of a String: the StringNumericLiteral of ECMAScript §7.1.4.1 ToNumber Applied to the
   String Type, read as StringIntlMV, then ECMA-402 §16.5.15 steps 7-9's rounding tests. A decimal is kept as its exact string (*owned, malloc'd); every other
   value is a Number. */
static void string_intl_mv(const char *s, size_t n, IntlHostValue *out, char **owned)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t a = 0, b = n, w, i, digits = 0;
    bool neg = false, nonzero = false;
    char *body;
    double d;

    *owned = NULL;
    while (a < b && (w = str_white_space(p + a, b - a))) a += w;
    while (b > a) {
        size_t back = 0;

        for (w = 1; w <= 3 && w <= b - a; w++)
            if (str_white_space(p + b - w, w) == w) { back = w; break; }
        if (!back) break;
        b -= back;
    }
    *out = (IntlHostValue){ .kind = INTL_VALUE_NUMBER, .num = 0 };
    if (a == b) return;                                                                  /* StrWhiteSpace: 0 */
    if (b - a > 2 && s[a] == '0' && strchr("xXoObB", s[a + 1])) {                        /* NonDecimalIntegerLiteral */
        int base = (s[a + 1] | 0x20) == 'x' ? 16 : (s[a + 1] | 0x20) == 'o' ? 8 : 2;

        for (i = a + 2; i < b; i++) {
            int c = s[i] | 0x20, dv = s[i] >= '0' && s[i] <= '9' ? s[i] - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 99;

            if (dv >= base) { out->num = NAN; return; }
        }
        body = radix_to_decimal(s + a + 2, b - a - 2, base);
    } else {
        i = a;
        if (s[i] == '+' || s[i] == '-') { neg = s[i] == '-'; i++; }
        if (b - i == 8 && memcmp(s + i, "Infinity", 8) == 0) { out->num = neg ? -INFINITY : INFINITY; return; }
        {
            size_t j = i;

            while (j < b && s[j] >= '0' && s[j] <= '9') { nonzero = nonzero || s[j] != '0'; j++; digits++; }
            if (j < b && s[j] == '.') {
                j++;
                while (j < b && s[j] >= '0' && s[j] <= '9') { nonzero = nonzero || s[j] != '0'; j++; digits++; }
            }
            if (digits == 0) { out->num = NAN; return; }
            if (j < b && (s[j] == 'e' || s[j] == 'E')) {
                size_t e = ++j;

                if (j < b && (s[j] == '+' || s[j] == '-')) j++;
                e = j;
                while (j < b && s[j] >= '0' && s[j] <= '9') j++;
                if (j == e) { out->num = NAN; return; }
            }
            if (j != b) { out->num = NAN; return; }
        }
        body = strndup(s + a, b - a);
        CHECK(body != NULL, "a decimal literal could not be copied");
    }
    d = strtod(body, NULL);
    if (isinf(d)) { out->num = d; free(body); return; }                                  /* step 9.b-c */
    if (d == 0) { out->num = neg && nonzero ? -0.0 : 0; free(body); return; }            /* step 9.d-e */
    *owned = body;
    *out = (IntlHostValue){ .kind = INTL_VALUE_STRING, .str = body, .len = strlen(body) };
}

/* §16.5.15 over the primitive steps 1 produced; *owned holds a decimal's bytes. -1 when ToNumber threw. */
static int intl_mv_of(JSContext *ctx, JSValueConst prim, IntlHostValue *out, char **owned)
{
    *owned = NULL;
    if (JS_IsBigInt(prim)) {                                                             /* step 2 */
        const char *c = JS_ToCString(ctx, prim);

        CHECK(c != NULL, "a BigInt's decimal could not be read");
        *owned = strdup(c);
        JS_FreeCString(ctx, c);
        *out = (IntlHostValue){ .kind = INTL_VALUE_STRING, .str = *owned, .len = strlen(*owned) };
        return 0;
    }
    if (JS_IsString(prim)) {                                                             /* step 3 */
        size_t n = 0;
        const char *c = JS_ToCStringLen(ctx, &n, prim);

        CHECK(c != NULL, "a String operand's bytes could not be read");
        string_intl_mv(c, n, out, owned);
        JS_FreeCString(ctx, c);
        return 0;
    }
    {
        double d;

        if (JS_ToFloat64(ctx, &d, prim) < 0) return -1;                                  /* step 4.a */
        *out = (IntlHostValue){ .kind = INTL_VALUE_NUMBER, .num = d };                   /* a Number is exact */
        return 0;
    }
}

/* ---- §16.3.3 get Intl.NumberFormat.prototype.format, and §16.3.6 formatToParts ------------------------------ */

#define NFF_STAGES(X) X(NFF_VALUE, "ECMA-402 §16.5.15 step 1 (ToPrimitive ( value, number ))")
enum { NFF_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const NFF_STEPS[] = { NFF_STAGES(JS_STEP_STAGE_LABEL) NULL };

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

/* FormatNumeric or FormatNumericToParts of `value` by the NumberFormat `nf`, into s->result. */
static int nf_format(JSContext *ctx, FormatState *s, JSValueConst nf, IntlHostOp op, const char *algorithm, JSValue cb,
                     JSValue **out_cb, int *out_argc)
{
    JSStepHdr *h = &s->hdr;
    JSValueConst value = step_arg(h, 0);
    JSValue prim, slots, loc, real;
    IntlHostValue x;
    char *owned;
    const char *l;
    int r;

    if (!s->started) { s->result = JS_UNDEFINED; s->started = 1; }
    STEP_DISPATCH(NFF_STAGES, h->stage, h->def->algorithm, JS_STEP_ABRUPT);
    STEP_ARM(NFF_VALUE);
    slots = intl_slots_of(ctx, nf, g_class);
    if (JS_IsUndefined(slots)) {
        JS_FreeValue(ctx, cb);
        JS_ThrowTypeError(ctx, "%s called on an object that is not an Intl.NumberFormat", algorithm);
        return JS_STEP_ABRUPT;
    }
    {
        ConcolicExState st = concolic_is(value) ? concolic_example_state(value) : CONCOLIC_EX_NONE;

        if (st == CONCOLIC_EX_HELD || st == CONCOLIC_EX_DETERMINED) {
            JS_FreeValue(ctx, cb);
            prim = concolic_example(ctx, value);         /* the example is a primitive: step 1 runs no code */
        } else {
            r = step_toprimitive_number_run(ctx, h, value, cb, &prim, out_cb, out_argc);
            if (r) { JS_FreeValue(ctx, slots); return r < 0 ? JS_STEP_ABRUPT : r; }
        }
    }
    r = intl_mv_of(ctx, prim, &x, &owned);
    JS_FreeValue(ctx, prim);
    if (r < 0) { JS_FreeValue(ctx, slots); return JS_STEP_ABRUPT; }
    loc = intl_slot(ctx, slots, "locale");
    l = JS_ToCString(ctx, loc);
    CHECK(l != NULL, "a NumberFormat's resolved locale could not be read");
    real = intl_ask(ctx, op, JS_UNDEFINED, l, false, slots, FORMAT_SLOTS, &x, 1);
    JS_FreeCString(ctx, l);
    JS_FreeValue(ctx, loc);
    free(owned);
    if (JS_IsException(real)) { JS_FreeValue(ctx, slots); return JS_STEP_ABRUPT; }
    s->result = intl_derive(ctx, algorithm, slots, &value, 1, real);
    JS_FreeValue(ctx, slots);
    return JS_STEP_DONE;
}

static int ff_step(JSContext *ctx, void *st, JSValue cb, JSValue **out_cb, int *out_argc)
{
    FormatState *s = st;

    return nf_format(ctx, s, JS_StepClosureData(&s->hdr, 0), INTL_OP_NUMBER_FORMAT,
                     "Intl.NumberFormat.prototype.format", cb, out_cb, out_argc);
}

static int fp_step(JSContext *ctx, void *st, JSValue cb, JSValue **out_cb, int *out_argc)
{
    FormatState *s = st;

    return nf_format(ctx, s, s->hdr.this_val, INTL_OP_NUMBER_TO_PARTS, "Intl.NumberFormat.prototype.formatToParts",
                     cb, out_cb, out_argc);
}

static const JSTrampStepDef FORMAT_DEF = {
    sizeof(FormatState), ff_step, ff_fini, 0, .visit = ff_visit,
    .algorithm = "ECMA-402 §16.3.3 a bound NumberFormat format function", .steps = NFF_STEPS
};
static const JSTrampStepDef PARTS_DEF = {
    sizeof(FormatState), fp_step, ff_fini, 0, .visit = ff_visit,
    .algorithm = "ECMA-402 §16.3.6 Intl.NumberFormat.prototype.formatToParts ( value )", .steps = NFF_STEPS
};

static JSValue js_nf_format(JSContext *ctx, JSValueConst this_val)
{
    JSValue slots = intl_slots_of(ctx, this_val, g_class), f;

    if (JS_IsUndefined(slots))                                                           /* step 3 */
        return JS_ThrowTypeError(ctx, "Intl.NumberFormat.prototype.format called on an object that is not an "
                                      "Intl.NumberFormat");
    f = JS_GetPropertyStr(ctx, slots, "boundFormat");
    if (JS_IsUndefined(f)) {                                                             /* step 4 */
        f = JS_NewStepClosure2(ctx, g_format_stepid, "", 1, 1, &this_val);
        CHECK(!JS_IsException(f), "a bound format function could not be allocated");
        JS_SetPropertyStr(ctx, slots, "boundFormat", JS_DupValue(ctx, f));
    }
    JS_FreeValue(ctx, slots);
    return f;
}

/* ---- §16.3.2 Intl.NumberFormat.prototype.resolvedOptions ( ) --------------------------------------------- */

static JSValue js_nf_resolved_options(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue slots = intl_slots_of(ctx, this_val, g_class), o;
    int i;

    (void)argc; (void)argv;
    if (JS_IsUndefined(slots))
        return JS_ThrowTypeError(ctx, "Intl.NumberFormat.prototype.resolvedOptions called on an object that is not "
                                      "an Intl.NumberFormat");
    o = JS_NewObject(ctx);
    CHECK(!JS_IsException(o), "resolvedOptions' object could not be allocated");
    for (i = 0; RESOLVED[i]; i++) {
        JSValue v = intl_slot(ctx, slots, RESOLVED[i]);

        if (JS_IsUndefined(v)) continue;
        v = intl_derive(ctx, "Intl.NumberFormat.prototype.resolvedOptions", slots, NULL, 0, v);
        JS_DefinePropertyValueStr(ctx, o, RESOLVED[i], v, JS_PROP_C_W_E);
    }
    JS_FreeValue(ctx, slots);
    return o;
}

static const JSCFunctionListEntry PROTO_FUNCS[] = {
    JS_CFUNC_DEF("resolvedOptions", 0, js_nf_resolved_options),
    JS_CGETSET_DEF("format", js_nf_format, NULL),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Intl.NumberFormat", JS_PROP_CONFIGURABLE),
};

JSValue number_format_install(JSContext *ctx)
{
    JSValue proto = JS_NewObject(ctx), ctor, parts;

    CHECK(!JS_IsException(proto), "%Intl.NumberFormat.prototype% could not be allocated");
    JS_SetPropertyFunctionList(ctx, proto, PROTO_FUNCS, (int)(sizeof PROTO_FUNCS / sizeof PROTO_FUNCS[0]));
    parts = JS_NewCFunction2(ctx, NULL, "formatToParts", 1, JS_CFUNC_step, g_parts_stepid);
    CHECK(!JS_IsException(parts), "formatToParts could not be allocated");
    JS_DefinePropertyValueStr(ctx, proto, "formatToParts", parts, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    ctor = JS_NewCFunction2(ctx, NULL, "NumberFormat", 0, JS_CFUNC_step_ctor, g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "%Intl.NumberFormat% could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    intl_install_supported_locales_of(ctx, ctor, INTL_SVC_NUMBER_FORMAT);
    JS_SetClassProto(ctx, g_class, proto);
    return ctor;
}

void number_format_init(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSClassDef d = { "Intl.NumberFormat", NULL, NULL, NULL, NULL };

    JS_NewClassID(rt, &g_class);
    JS_NewClass(rt, g_class, &d);
    g_ctor_stepid = JS_RegisterStepDef(rt, &CTOR_DEF);
    g_format_stepid = JS_RegisterStepDef(rt, &FORMAT_DEF);
    g_parts_stepid = JS_RegisterStepDef(rt, &PARTS_DEF);
    agent_state_class("intl", &g_class, "§16's Intl.NumberFormat class");
    agent_state_id("intl", &g_ctor_stepid, "§16.1.1's constructor machine");
    agent_state_id("intl", &g_format_stepid, "§16.3.3's bound format machine");
    agent_state_id("intl", &g_parts_stepid, "§16.3.6's formatToParts machine");
}

void number_format_free(JSRuntime *rt)
{
    (void)rt;   /* its class and machines are ids the row's undo puts back */
    agent_state_reached("intl");
}
