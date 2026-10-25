/* The abstract operations every Intl service shares. See intl_service.h. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/agent_state.h"
#include "core/idl_slots.h"
#include "core/intl/intl_environment.h"
#include "core/intl/intl_service.h"
#include "core/timing/thread_work.h"
#include "solver/concolic.h"
#include <lexbor/core/base.h>   /* lexbor_malloc: the allocator the engine gives its refusal edge */

/* The key an instance's resolved record hangs under, and the record's member holding its unknown operands. */
static JSValue g_slots_key = JS_UNDEFINED;
static JSAtom g_atom_slots = JS_ATOM_NULL;
static JSAtom g_atom_length = JS_ATOM_NULL;
#define DEPS_MEMBER "deps"

void intl_service_init(JSContext *ctx)
{
    g_slots_key = JS_NewSymbol(ctx, "intlSlots", false);
    CHECK(!JS_IsException(g_slots_key), "the Intl slot record key could not be allocated");
    g_atom_slots = JS_ValueToAtom(ctx, g_slots_key);
    g_atom_length = JS_NewAtom(ctx, "length");
    CHECK(g_atom_slots != JS_ATOM_NULL && g_atom_length != JS_ATOM_NULL, "the Intl record keys could not be interned");
    agent_state_value("intl", &g_slots_key, "the Intl services' slot record key");
    agent_state_atom("intl", &g_atom_slots, "the Intl slot record key, interned");
    agent_state_atom("intl", &g_atom_length, "LengthOfArrayLike's `length`, interned");
}

void intl_service_free(JSRuntime *rt)
{
    JS_FreeValueRT(rt, g_slots_key);
    JS_FreeAtomRT(rt, g_atom_slots);
    JS_FreeAtomRT(rt, g_atom_length);
    agent_state_reached("intl");
}

/* ---- the state --------------------------------------------------------------------------------------------- */

void intl_state_start(IntlState *s)
{
    s->requested = s->options = s->read = s->deps = s->obj = JS_UNDEFINED;
    s->slots = s->result = s->held = s->list = s->opt = s->raw = JS_UNDEFINED;
    s->k = s->len = 0;
    s->phase = 0;
    s->started = 1;
}

void intl_state_visit(JSContext *ctx, IntlState *s, JSStepVisit *v)
{
    if (!s->started) return;
    v->val(ctx, &s->requested);
    v->val(ctx, &s->options);
    v->val(ctx, &s->read);
    v->val(ctx, &s->deps);
    v->val(ctx, &s->obj);
    v->val(ctx, &s->slots);
    v->val(ctx, &s->result);
    v->val(ctx, &s->held);
    v->val(ctx, &s->list);
    v->val(ctx, &s->opt);
    v->val(ctx, &s->raw);
}

JSValue intl_state_fini(IntlState *s, bool take_result)
{
    JSValue r;

    if (!take_result) return JS_UNDEFINED;
    DCHECK(s->started, "an Intl machine's result was taken before its prologue ran");
    r = s->result;
    s->result = JS_UNDEFINED;
    return r;
}

/* An unknown operand computed with: kept, so what the instance answers is derived from it. */
static void keep_dep(JSContext *ctx, IntlState *s, JSValueConst v)
{
    int64_t n = 0;

    if (JS_IsUndefined(s->deps)) {
        s->deps = JS_NewArray(ctx);
        CHECK(!JS_IsException(s->deps), "an Intl operand list could not be allocated");
    }
    JS_GetLength(ctx, s->deps, &n);
    CHECK(n < UINT32_MAX && JS_DefinePropertyValueUint32(ctx, s->deps, (uint32_t)n, JS_DupValue(ctx, v),
                                                         JS_PROP_C_W_E) >= 0,
          "an Intl operand list could not grow");
}

/* The example of an unknown, as a String, when it holds one whose ToString runs no page code: the value a read
   of the operand computes with. JS_UNDEFINED when it holds none. */
static JSValue example_string(JSContext *ctx, JSValueConst v)
{
    ConcolicExState st = concolic_example_state(v);

    if (st != CONCOLIC_EX_HELD && st != CONCOLIC_EX_DETERMINED) return JS_UNDEFINED;
    return concolic_example_string(ctx, v);
}

static void record_in(JSContext *ctx, JSValue *rec, const char *prop, JSValueConst value)
{
    if (JS_IsUndefined(*rec)) {
        *rec = idl_slots_new(ctx);
        CHECK(!JS_IsException(*rec), "an Intl options record could not be allocated");
    }
    CHECK(JS_DefinePropertyValueStr(ctx, *rec, prop, JS_DupValue(ctx, value), JS_PROP_C_W_E) >= 0,
          "an Intl options record could not record a value");
}

static void record_read(JSContext *ctx, IntlState *s, const char *prop, JSValueConst value)
{
    record_in(ctx, &s->read, prop, value);
}

JSValue intl_read(JSContext *ctx, IntlState *s, const char *prop)
{
    return JS_IsUndefined(s->read) ? JS_UNDEFINED : JS_GetPropertyStr(ctx, s->read, prop);
}

void intl_set_read(JSContext *ctx, IntlState *s, const char *prop, JSValue v)
{
    record_read(ctx, s, prop, v);
    JS_FreeValue(ctx, v);
}

void intl_set_opt(JSContext *ctx, IntlState *s, const char *key, JSValue v)
{
    record_in(ctx, &s->opt, key, v);
    JS_FreeValue(ctx, v);
}

/* ---- §9.2.1 CanonicalizeLocaleList ------------------------------------------------------------------------- */

enum { LCL_START = 0, LCL_LENGTH, LCL_TOLENGTH, LCL_HAS, LCL_GET, LCL_TOSTRING };

/* ! ToString ( 𝔽 ( k ) ) as a property key. */
static JSAtom index_atom(JSContext *ctx, int64_t k)
{
    char buf[24];

    if (k <= 0x7fffffff) return JS_NewAtomUInt32(ctx, (uint32_t)k);
    snprintf(buf, sizeof buf, "%lld", (long long)k);
    return JS_NewAtom(ctx, buf);
}

static void throw_bad_tag(JSContext *ctx, JSValueConst tag)
{
    const char *t = JS_ToCString(ctx, tag);

    JS_ThrowRangeError(ctx, "Incorrect locale information provided: %s", t ? t : "");
    if (t) JS_FreeCString(ctx, t);
}

/* Steps 7.c.iv-vii for one tag: well-formed, canonicalized by the host, appended once. */
static int add_tag(JSContext *ctx, IntlState *s, JSValueConst tag)
{
    size_t n = 0;
    const char *t = JS_ToCStringLen(ctx, &n, tag);
    IntlHostValue arg;
    JSValue canon;
    int64_t len = 0, i;
    bool ok;

    CHECK(t != NULL, "a locale tag's bytes could not be read");
    ok = intl_is_well_formed_language_tag(t, n);
    if (!ok) {
        JS_FreeCString(ctx, t);
        throw_bad_tag(ctx, tag);
        return -1;
    }
    arg = (IntlHostValue){ .kind = INTL_VALUE_STRING, .str = t, .len = n };
    canon = intl_ask(ctx, INTL_OP_LOCALE_CANONICALIZE, JS_UNDEFINED, NULL, false, JS_UNDEFINED, NULL, &arg, 1);
    JS_FreeCString(ctx, t);
    if (JS_IsException(canon)) return -1;
    DCHECK(JS_IsString(canon), "the Intl host canonicalized a locale to something that is not a String");
    JS_GetLength(ctx, s->requested, &len);
    for (i = 0; i < len; i++) {
        JSValue e = JS_GetPropertyInt64(ctx, s->requested, i);
        bool same = JS_IsStrictEqual(ctx, e, canon);

        JS_FreeValue(ctx, e);
        if (same) { JS_FreeValue(ctx, canon); return 0; }
    }
    CHECK(len < UINT32_MAX && JS_DefinePropertyValueUint32(ctx, s->requested, (uint32_t)len, canon,
                                                           JS_PROP_C_W_E) >= 0,
          "requestedLocales could not grow");
    return 0;
}

/* A tag the page handed as unknown input: its example, kept as an operand, or the collapse ToString owes it. */
static int add_unknown_tag(JSContext *ctx, IntlState *s, JSValueConst v, JSValue in, JSValue **out_cb,
                           int *out_argc)
{
    JSValue ex = example_string(ctx, v), str;
    int r;

    if (!JS_IsUndefined(ex)) {
        JS_FreeValue(ctx, in);
        keep_dep(ctx, s, v);
        r = add_tag(ctx, s, ex);
        JS_FreeValue(ctx, ex);
        return r;
    }
    r = step_tostring_run(ctx, &s->hdr, v, in, &str, out_cb, out_argc);
    DCHECK(r != 0, "ToString of unknown input with no example produced a String");
    return r;
}

int intl_canonicalize_locale_list(JSContext *ctx, IntlState *s, JSValueConst locales, JSValue in,
                                  JSValue **out_cb, int *out_argc)
{
    JSStepHdr *h = &s->hdr;
    JSAtom a;
    int r, has;
    double d;

    if (s->phase == LCL_START) {
        s->requested = JS_NewArray(ctx);
        CHECK(!JS_IsException(s->requested), "requestedLocales could not be allocated");
        if (JS_IsUndefined(locales)) { JS_FreeValue(ctx, in); return 0; }        /* step 1 */
        /* Step 3: a String, or unknown input, which stands for a primitive in the page. Intl.Locale is not built,
           so no object carries [[InitializedLocale]]. */
        if (JS_IsString(locales) || concolic_is(locales)) {
            s->list = JS_NewArray(ctx);
            CHECK(!JS_IsException(s->list), "localesObj could not be allocated");
            CHECK(JS_DefinePropertyValueUint32(ctx, s->list, 0, JS_DupValue(ctx, locales), JS_PROP_C_W_E) >= 0,
                  "localesObj could not hold its one element");
        } else {
            s->list = JS_ToObject(ctx, locales);                                 /* step 4 */
            if (JS_IsException(s->list)) { s->list = JS_UNDEFINED; JS_FreeValue(ctx, in); return -1; }
        }
        s->phase = LCL_LENGTH;
        in = JS_UNDEFINED;
    }
    if (s->phase == LCL_LENGTH) {                                                /* step 5: LengthOfArrayLike */
        r = step_getprop_run(ctx, h, s->list, g_atom_length, in, &s->held, out_cb, out_argc);
        if (r) return r;
        in = JS_UNDEFINED;
        s->phase = LCL_TOLENGTH;
    }
    if (s->phase == LCL_TOLENGTH) {
        r = step_todouble_run(ctx, h, s->held, in, &d, out_cb, out_argc);
        if (r) return r;
        in = JS_UNDEFINED;
        JS_FreeValue(ctx, s->held);
        s->held = JS_UNDEFINED;
        /* §7.1.22 ToLength */
        d = isnan(d) || d <= 0 ? 0 : d >= 9007199254740991.0 ? 9007199254740991.0 : floor(d);
        s->len = (int64_t)d;
        s->k = 0;
        s->phase = LCL_HAS;
    }
    while (s->k < s->len) {
        if (s->phase == LCL_HAS) {                                               /* step 7.b */
            a = index_atom(ctx, s->k);
            r = step_hasproperty_run(ctx, h, s->list, a, in, &has, out_cb, out_argc);
            JS_FreeAtom(ctx, a);
            if (r) return r;
            in = JS_UNDEFINED;
            if (!has) { s->k++; return JS_STEP_YIELD; }
            s->phase = LCL_GET;
        }
        if (s->phase == LCL_GET) {                                               /* step 7.c.i */
            a = index_atom(ctx, s->k);
            r = step_getprop_run(ctx, h, s->list, a, in, &s->held, out_cb, out_argc);
            JS_FreeAtom(ctx, a);
            if (r) return r;
            in = JS_UNDEFINED;
            if (!JS_IsString(s->held) && !JS_IsObject(s->held)) {                /* step 7.c.ii */
                JS_ThrowTypeError(ctx, "a locale is neither a String nor an Object");
                return -1;
            }
            s->phase = LCL_TOSTRING;
        }
        DCHECK(s->phase == LCL_TOSTRING, "CanonicalizeLocaleList resumed at a phase it does not have");
        if (concolic_is(s->held)) {
            r = add_unknown_tag(ctx, s, s->held, in, out_cb, out_argc);
        } else if (JS_IsString(s->held)) {
            JS_FreeValue(ctx, in);
            r = add_tag(ctx, s, s->held);
        } else {
            JSValue str;

            r = step_tostring_run(ctx, h, s->held, in, &str, out_cb, out_argc);  /* step 7.c.iv */
            if (r) return r;
            r = add_tag(ctx, s, str);
            JS_FreeValue(ctx, str);
        }
        if (r) return r;
        in = JS_UNDEFINED;
        JS_FreeValue(ctx, s->held);
        s->held = JS_UNDEFINED;
        s->k++;
        s->phase = LCL_HAS;
        return JS_STEP_YIELD;
    }
    JS_FreeValue(ctx, in);
    JS_FreeValue(ctx, s->list);
    s->list = JS_UNDEFINED;
    s->phase = 0;
    return 0;
}

int intl_coerce_options(JSContext *ctx, IntlState *s, JSValueConst options)
{
    if (JS_IsUndefined(options)) {
        s->options = idl_slots_new(ctx);   /* OrdinaryObjectCreate(null) */
        CHECK(!JS_IsException(s->options), "an options object could not be allocated");
        return 0;
    }
    s->options = JS_ToObject(ctx, options);
    if (JS_IsException(s->options)) { s->options = JS_UNDEFINED; return -1; }
    return 0;
}

/* ---- §9.2.11 GetOption and its number and boolean-or-string forms ------------------------------------------ */

enum { OPT_GET = 0, OPT_CONVERT };

static int read_member_of(JSContext *ctx, IntlState *s, JSValueConst obj, const char *prop, JSValue in,
                          JSValue **out_cb, int *out_argc)
{
    JSAtom a = JS_NewAtom(ctx, prop);
    int r;

    CHECK(a != JS_ATOM_NULL, "an option name could not be interned");
    r = step_getprop_run(ctx, &s->hdr, obj, a, in, &s->held, out_cb, out_argc);
    JS_FreeAtom(ctx, a);
    return r;
}

static int read_member(JSContext *ctx, IntlState *s, const char *prop, JSValue in, JSValue **out_cb,
                       int *out_argc)
{
    return read_member_of(ctx, s, s->options, prop, in, out_cb, out_argc);
}

static int count_values(const char *const *values)
{
    int n = 0;

    while (values && values[n]) n++;
    return n;
}

static int value_index(JSContext *ctx, const char *const *values, JSValueConst str)
{
    const char *c = JS_ToCString(ctx, str);
    int i, found = -1;

    CHECK(c != NULL, "an option value's bytes could not be read");
    for (i = 0; values[i]; i++)
        if (strcmp(values[i], c) == 0) { found = i; break; }
    JS_FreeCString(ctx, c);
    return found;
}

static void throw_bad_value(JSContext *ctx, const char *prop, JSValueConst v)
{
    const char *c = JS_ToCString(ctx, v);

    JS_ThrowRangeError(ctx, "Value %s out of range for Intl options property %s", c ? c : "", prop);
    if (c) JS_FreeCString(ctx, c);
}

static JSValue default_of(JSContext *ctx, const IntlOption *o)
{
    if (!o->dflt) return JS_UNDEFINED;
    if (o->type == INTL_OPT_BOOLEAN) return JS_NewBool(ctx, strcmp(o->dflt, "true") == 0);
    return JS_NewString(ctx, o->dflt);
}

/* GetOption's conversion over unknown input. A list of values is the spec's partition, RangeError included, so it
   forks; a boolean forks through ToBoolean; a string with no list computes with the example. */
static int convert_unknown(JSContext *ctx, IntlState *s, const IntlOption *o, JSValue in, JSValue *pout,
                           JSValue **out_cb, int *out_argc)
{
    int n = count_values(o->values), arm, r, b;
    JSValue ex;

    DCHECK(o->fork_op != NULL, "an Intl option read over unknown input names no spec step to fork under");
    if (o->type == INTL_OPT_BOOLEAN) {
        JS_FreeValue(ctx, in);
        r = step_tobool_run(ctx, &s->hdr, s->held, o->fork_op, &b);
        if (r) return r;
        *pout = JS_NewBool(ctx, b);
        return 0;
    }
    if (n > 0) {
        int real = JS_OUTCOME_REAL_UNSTATED;

        JS_FreeValue(ctx, in);
        ex = example_string(ctx, s->held);
        if (!JS_IsUndefined(ex)) {
            int i = value_index(ctx, o->values, ex);

            real = i >= 0 ? i : n;
            JS_FreeValue(ctx, ex);
        }
        r = step_fork_run(ctx, &s->hdr, s->held, o->fork_op, n + 1, real, &arm);
        if (r) return r;
        if (arm == n) {
            JS_ThrowRangeError(ctx, "Value out of range for Intl options property %s", o->prop);
            return -1;
        }
        *pout = JS_NewString(ctx, o->values[arm]);
        return 0;
    }
    ex = example_string(ctx, s->held);
    if (JS_IsUndefined(ex)) {
        JSValue str;

        r = step_tostring_run(ctx, &s->hdr, s->held, in, &str, out_cb, out_argc);
        DCHECK(r != 0, "ToString of unknown input with no example produced a String");
        return r;
    }
    JS_FreeValue(ctx, in);
    keep_dep(ctx, s, s->held);
    *pout = ex;
    return 0;
}

int intl_get_option(JSContext *ctx, IntlState *s, const IntlOption *o, JSValue in, JSValue *pout,
                    JSValue **out_cb, int *out_argc)
{
    JSValue v = JS_UNDEFINED;
    int r;

    if (s->phase == OPT_GET) {
        r = read_member(ctx, s, o->prop, in, out_cb, out_argc);                  /* step 1 */
        if (r) return r;
        in = JS_UNDEFINED;
        s->phase = OPT_CONVERT;
    }
    if (JS_IsUndefined(s->held)) {                                               /* step 2 */
        JS_FreeValue(ctx, in);
        v = default_of(ctx, o);
    } else if (concolic_is(s->held)) {
        r = convert_unknown(ctx, s, o, in, &v, out_cb, out_argc);
        if (r) return r;
    } else if (o->type == INTL_OPT_BOOLEAN) {                                    /* step 3 */
        JS_FreeValue(ctx, in);
        v = JS_NewBool(ctx, JS_ToBool(ctx, s->held));
    } else {                                                                     /* step 4 */
        if (JS_IsString(s->held)) {
            JS_FreeValue(ctx, in);
            v = JS_DupValue(ctx, s->held);
        } else {
            r = step_tostring_run(ctx, &s->hdr, s->held, in, &v, out_cb, out_argc);
            if (r) return r;
        }
        if (o->values && value_index(ctx, o->values, v) < 0) {                   /* step 5 */
            throw_bad_value(ctx, o->prop, v);
            JS_FreeValue(ctx, v);
            return -1;
        }
    }
    JS_FreeValue(ctx, s->held);
    s->held = JS_UNDEFINED;
    s->phase = OPT_GET;
    record_read(ctx, s, o->prop, v);
    *pout = v;
    return 0;
}

int intl_get_number_option(JSContext *ctx, IntlState *s, const char *prop, double min, double max,
                           double fallback, JSValue in, double *pout, JSValue **out_cb, int *out_argc)
{
    double d;
    int r;

    if (s->phase == OPT_GET) {
        r = read_member(ctx, s, prop, in, out_cb, out_argc);
        if (r) return r;
        in = JS_UNDEFINED;
        s->phase = OPT_CONVERT;
    }
    if (JS_IsUndefined(s->held)) {                                               /* §9.2.13 step 1 */
        JS_FreeValue(ctx, in);
        d = fallback;
    } else {
        ConcolicExState st = concolic_is(s->held) ? concolic_example_state(s->held) : CONCOLIC_EX_NONE;

        if (st == CONCOLIC_EX_HELD || st == CONCOLIC_EX_DETERMINED) {
            JSValue ex = concolic_example(ctx, s->held);

            JS_FreeValue(ctx, in);
            keep_dep(ctx, s, s->held);
            r = JS_ToFloat64(ctx, &d, ex);                                       /* a primitive: runs no code */
            JS_FreeValue(ctx, ex);
            if (r < 0) return -1;
        } else {
            r = step_todouble_run(ctx, &s->hdr, s->held, in, &d, out_cb, out_argc);   /* step 2 */
            if (r) return r;
        }
        if (!isfinite(d) || d < min || d > max) {                                /* step 3 */
            JS_ThrowRangeError(ctx, "%s value is out of range", prop);
            return -1;
        }
        d = floor(d);                                                            /* step 4 */
    }
    JS_FreeValue(ctx, s->held);
    s->held = JS_UNDEFINED;
    s->phase = OPT_GET;
    {
        JSValue rv = isnan(d) ? JS_UNDEFINED : JS_NewFloat64(ctx, d);

        record_read(ctx, s, prop, rv);
        JS_FreeValue(ctx, rv);
    }
    *pout = d;
    return 0;
}

int intl_create_from_constructor(JSContext *ctx, IntlState *s, JSValueConst new_target, JSClassID cls, JSValue in,
                                 JSValue **out_cb, int *out_argc)
{
    JSValue proto;
    int r;

    r = read_member_of(ctx, s, new_target, "prototype", in, out_cb, out_argc);   /* §10.1.14 step 2 */
    if (r) return r;
    /* Step 3's realm of the constructor is this realm: every Intl constructor a page can reach is its own. */
    proto = JS_IsObject(s->held) ? JS_DupValue(ctx, s->held) : JS_GetClassProto(ctx, cls);
    JS_FreeValue(ctx, s->held);
    s->held = JS_UNDEFINED;
    s->obj = JS_NewObjectProtoClass(ctx, proto, cls);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(s->obj), "an Intl instance could not be allocated");
    return 0;
}

/* The `type` Unicode locale nonterminal: (alphanum{3,8}) ("-" alphanum{3,8})*. */
static bool is_unicode_type(const char *v, size_t n)
{
    size_t i = 0;

    while (i < n) {
        size_t j = i;

        while (j < n && v[j] != '-') {
            char c = v[j];

            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
            j++;
        }
        if (j - i < 3 || j - i > 8) return false;
        if (j == n) return true;
        i = j + 1;
        if (i == n) return false;
    }
    return false;
}

int intl_resolution_option(JSContext *ctx, IntlState *s, const IntlOption *o, const char *key, JSValue in,
                           JSValue **out_cb, int *out_argc)
{
    JSValue v, str;
    int r = intl_get_option(ctx, s, o, in, &v, out_cb, out_argc);

    if (r) return r;
    if (JS_IsUndefined(v)) { record_in(ctx, &s->opt, key, v); return 0; }
    str = JS_ToString(ctx, v);                                                   /* step 6.d.i: ! ToString */
    JS_FreeValue(ctx, v);
    CHECK(!JS_IsException(str), "a resolution option's ToString of a primitive threw");
    {
        size_t n = 0;
        const char *c = JS_ToCStringLen(ctx, &n, str);
        bool ok = c && is_unicode_type(c, n);

        if (c) JS_FreeCString(ctx, c);
        if (!ok) {                                                               /* step 6.d.ii */
            throw_bad_value(ctx, o->prop, str);
            JS_FreeValue(ctx, str);
            return -1;
        }
    }
    record_in(ctx, &s->opt, key, str);
    JS_FreeValue(ctx, str);
    return 0;
}

int intl_get_raw(JSContext *ctx, IntlState *s, const char *prop, JSValue in, JSValue **out_cb, int *out_argc)
{
    int r = read_member(ctx, s, prop, in, out_cb, out_argc);

    if (r) return r;
    record_in(ctx, &s->raw, prop, s->held);
    JS_FreeValue(ctx, s->held);
    s->held = JS_UNDEFINED;
    return 0;
}

int intl_default_number_option(JSContext *ctx, IntlState *s, const char *prop, double min, double max,
                               double fallback, JSValue in, double *pout, JSValue **out_cb, int *out_argc)
{
    JSValue v = JS_IsUndefined(s->raw) ? JS_UNDEFINED : JS_GetPropertyStr(ctx, s->raw, prop);
    double d;
    int r = 0;

    if (JS_IsUndefined(v)) {                                                     /* step 1 */
        JS_FreeValue(ctx, in);
        d = fallback;
    } else {
        ConcolicExState st = concolic_is(v) ? concolic_example_state(v) : CONCOLIC_EX_NONE;

        if (st == CONCOLIC_EX_HELD || st == CONCOLIC_EX_DETERMINED) {
            JSValue ex = concolic_example(ctx, v);

            JS_FreeValue(ctx, in);
            keep_dep(ctx, s, v);
            r = JS_ToFloat64(ctx, &d, ex);
            JS_FreeValue(ctx, ex);
        } else {
            r = step_todouble_run(ctx, &s->hdr, v, in, &d, out_cb, out_argc);  /* step 2 */
        }
        JS_FreeValue(ctx, v);
        if (r) return r;
        if (!isfinite(d) || d < min || d > max) {                                /* step 3 */
            JS_ThrowRangeError(ctx, "%s value is out of range", prop);
            return -1;
        }
        d = floor(d);                                                            /* step 4 */
    }
    {
        JSValue rv = isnan(d) ? JS_UNDEFINED : JS_NewFloat64(ctx, d);

        record_read(ctx, s, prop, rv);
        JS_FreeValue(ctx, rv);
    }
    *pout = d;
    return 0;
}

int intl_get_bool_or_string_option(JSContext *ctx, IntlState *s, const char *prop, const char *const *values,
                                   const char *fork_op, JSValue fallback, JSValue in, JSValue *pout,
                                   JSValue **out_cb, int *out_argc)
{
    JSValue v = JS_UNDEFINED;
    int r, b;

    if (s->phase == OPT_GET) {
        r = read_member(ctx, s, prop, in, out_cb, out_argc);                     /* step 1 */
        if (r) return r;
        in = JS_UNDEFINED;
        s->phase = OPT_CONVERT;
    }
    if (JS_IsUndefined(s->held)) {                                               /* step 2 */
        JS_FreeValue(ctx, in);
        v = JS_DupValue(ctx, fallback);
    } else if (JS_IsBool(s->held) && JS_ToBool(ctx, s->held)) {                  /* step 3 */
        JS_FreeValue(ctx, in);
        v = JS_TRUE;
    } else if (concolic_is(s->held)) {
        /* Steps 4-6 over unknown input: false, or one of the string values, or a RangeError. */
        int n = count_values(values), arm, real = JS_OUTCOME_REAL_UNSTATED;
        JSValue ex = example_string(ctx, s->held);

        JS_FreeValue(ctx, in);
        if (!JS_IsUndefined(ex)) {
            int i = value_index(ctx, values, ex);

            real = JS_ToBool(ctx, ex) ? (i >= 0 ? i + 1 : n + 1) : 0;
            JS_FreeValue(ctx, ex);
        }
        r = step_fork_run(ctx, &s->hdr, s->held, fork_op, n + 2, real, &arm);
        if (r) return r;
        if (arm == n + 1) {
            JS_ThrowRangeError(ctx, "Value out of range for Intl options property %s", prop);
            return -1;
        }
        v = arm == 0 ? JS_FALSE : JS_NewString(ctx, values[arm - 1]);
    } else if (!JS_ToBool(ctx, s->held)) {                                       /* step 4 */
        JS_FreeValue(ctx, in);
        v = JS_FALSE;
    } else {
        if (JS_IsString(s->held)) {
            JS_FreeValue(ctx, in);
            v = JS_DupValue(ctx, s->held);
        } else {
            r = step_tostring_run(ctx, &s->hdr, s->held, in, &v, out_cb, out_argc);   /* step 5 */
            if (r) return r;
        }
        if (value_index(ctx, values, v) < 0) {                                   /* step 6 */
            throw_bad_value(ctx, prop, v);
            JS_FreeValue(ctx, v);
            return -1;
        }
    }
    JS_FreeValue(ctx, s->held);
    s->held = JS_UNDEFINED;
    s->phase = OPT_GET;
    record_read(ctx, s, prop, v);
    *pout = v;
    return 0;
}

/* ---- the host question ------------------------------------------------------------------------------------- */

bool intl_host_value_of(JSContext *ctx, JSValueConst v, IntlHostValue *out, const char **owned)
{
    double d;

    *owned = NULL;
    memset(out, 0, sizeof *out);
    if (JS_IsString(v)) {
        size_t n = 0;

        *owned = JS_ToCStringLen(ctx, &n, v);
        CHECK(*owned != NULL, "an Intl host argument's bytes could not be read");
        *out = (IntlHostValue){ .kind = INTL_VALUE_STRING, .str = *owned, .len = n };
        return true;
    }
    if (JS_IsBool(v)) {
        *out = (IntlHostValue){ .kind = INTL_VALUE_BOOLEAN, .boolean = JS_ToBool(ctx, v) };
        return true;
    }
    if (JS_IsNull(v)) {
        out->kind = INTL_VALUE_NULL;
        return true;
    }
    if (JS_IsNumber(v)) {
        JS_ToFloat64(ctx, &d, v);
        *out = (IntlHostValue){ .kind = INTL_VALUE_NUMBER, .num = d };
        return true;
    }
    return false;
}

JSValue intl_ask(JSContext *ctx, IntlHostOp op, JSValueConst locales, const char *one_locale, bool with_default,
                 JSValueConst record, const char *const *keys, const IntlHostValue *args, int nargs)
{
    const char **tags, **owned;
    IntlHostField *fields;
    int64_t nl = 0, i;
    int nt = 0, nf = 0, nk = 0, no = 0;
    IntlHostRequest req;
    ThreadWorkSpan span;
    uint64_t bytes = 0;
    char *answer;
    JSValue parsed, v;

    DCHECK(nargs <= 2, "an Intl host question takes at most two arguments");
    if (!JS_IsUndefined(locales)) JS_GetLength(ctx, locales, &nl);
    while (keys && keys[nk]) nk++;
    tags = lexbor_malloc(sizeof(char *) * (size_t)(nl + 2));
    owned = lexbor_malloc(sizeof(char *) * (size_t)(nl + nk + 1));
    fields = lexbor_malloc(sizeof(IntlHostField) * (size_t)(nk + 1));
    CHECK(tags != NULL && owned != NULL && fields != NULL, "an Intl host question could not be allocated");
    for (i = 0; i < nl; i++) {
        JSValue e = JS_GetPropertyInt64(ctx, locales, i);

        DCHECK(JS_IsString(e), "requestedLocales holds something that is not a canonicalized tag");
        tags[nt] = JS_ToCString(ctx, e);
        CHECK(tags[nt] != NULL, "a locale tag's bytes could not be read");
        owned[no++] = tags[nt];
        bytes += strlen(tags[nt++]);
        JS_FreeValue(ctx, e);
    }
    if (one_locale) tags[nt++] = one_locale;
    if (with_default) tags[nt++] = INTL_DEFAULT_LOCALE;
    for (i = 0; i < nk; i++) {
        JSValue fv = JS_IsUndefined(record) ? JS_UNDEFINED : JS_GetPropertyStr(ctx, record, keys[i]);
        const char *o = NULL;

        if (intl_host_value_of(ctx, fv, &fields[nf].value, &o)) {
            fields[nf].key = keys[i];
            bytes += fields[nf].value.len + strlen(keys[i]);
            nf++;
        } else {
            DCHECK(JS_IsUndefined(fv), "an Intl record member is not a String, Number, Boolean or null");
        }
        if (o) owned[no++] = o;
        JS_FreeValue(ctx, fv);
    }
    for (i = 0; i < nargs; i++) bytes += args[i].len + sizeof(double);
    memset(&req, 0, sizeof req);
    req.op = op;
    req.locales = tags;
    req.nlocales = nt;
    req.fields = fields;
    req.nfields = nf;
    req.nargs = nargs;
    for (i = 0; i < nargs; i++) req.args[i] = args[i];
    thread_work_begin(&span);
    answer = intl_host_call(&req);
    thread_work_end(&span, THREAD_WORK_INTL, bytes);
    thread_work_charge(THREAD_WORK_INTL, bytes);
    for (i = 0; i < no; i++) JS_FreeCString(ctx, owned[i]);
    free(owned);
    free(tags);
    free(fields);
    parsed = JS_ParseJSON(ctx, answer, strlen(answer), "<intl host>");
    CHECKF(JS_IsObject(parsed), "the Intl host's answer to %s is not a JSON object: %s", intl_host_op_name(op),
           answer);
    free(answer);
    v = JS_GetPropertyStr(ctx, parsed, "ok");
    if (!JS_IsUndefined(v)) { JS_FreeValue(ctx, parsed); return v; }
    {
        JSValue name = JS_GetPropertyStr(ctx, parsed, "throw"), msg = JS_GetPropertyStr(ctx, parsed, "message");
        const char *n = JS_ToCString(ctx, name), *m = JS_ToCString(ctx, msg);

        CHECK(n && m && JS_IsString(name) && JS_IsString(msg),
              "the Intl host answered neither `ok` nor a `throw` with a `message`");
        if (strcmp(n, "TypeError") == 0) {
            JS_ThrowTypeError(ctx, "%s", m);
        } else {
            CHECKF(strcmp(n, "RangeError") == 0, "the Intl host threw a %s, which no ECMA-402 step it answers throws",
                   n);
            JS_ThrowRangeError(ctx, "%s", m);
        }
        JS_FreeCString(ctx, n);
        JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, name);
        JS_FreeValue(ctx, msg);
    }
    JS_FreeValue(ctx, parsed);
    return JS_EXCEPTION;
}

/* ---- slots and derivations --------------------------------------------------------------------------------- */

JSValue intl_slots_of(JSContext *ctx, JSValueConst obj, JSClassID cls)
{
    JSValue slots = JS_UNDEFINED;

    if (!JS_IsObject(obj) || JS_GetClassID(obj) != cls) return JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &slots, obj, g_atom_slots) <= 0) return JS_UNDEFINED;
    return slots;
}

void intl_slots_attach(JSContext *ctx, JSValueConst obj, JSValue slots)
{
    CHECK(JS_DefinePropertyValue(ctx, obj, g_atom_slots, slots, 0) >= 0, "an Intl instance's slots could not attach");
}

JSValue intl_slot(JSContext *ctx, JSValueConst slots, const char *name)
{
    return JS_GetPropertyStr(ctx, slots, name);
}

void intl_slots_take_deps(JSContext *ctx, IntlState *s, JSValueConst slots)
{
    if (JS_IsUndefined(s->deps)) return;
    CHECK(JS_DefinePropertyValueStr(ctx, slots, DEPS_MEMBER, s->deps, JS_PROP_C_W_E) >= 0,
          "an Intl instance's operands could not be recorded");
    s->deps = JS_UNDEFINED;
}

JSValue intl_derive(JSContext *ctx, const char *op, JSValueConst slots, const JSValueConst *inputs, int ninputs,
                    JSValue real)
{
    JSValue deps = JS_GetPropertyStr(ctx, slots, DEPS_MEMBER), d;
    JSValueConst *ops;
    JSValue *held;
    int64_t nd = 0, i;
    int n = 0;

    if (!JS_IsUndefined(deps)) JS_GetLength(ctx, deps, &nd);
    ops = lexbor_malloc(sizeof(JSValueConst) * (size_t)(nd + ninputs + 1));
    held = lexbor_malloc(sizeof(JSValue) * (size_t)(nd + 1));
    CHECK(ops != NULL && held != NULL, "an Intl derivation's operands could not be allocated");
    for (i = 0; i < nd; i++) ops[n++] = held[i] = JS_GetPropertyInt64(ctx, deps, i);
    for (i = 0; i < ninputs; i++) ops[n++] = inputs[i];
    d = n ? concolic_new_derived(ctx, op, ops, n, JS_DupValue(ctx, real)) : JS_UNINITIALIZED;
    for (i = 0; i < nd; i++) JS_FreeValue(ctx, held[i]);
    free(ops);
    free(held);
    JS_FreeValue(ctx, deps);
    if (JS_VALUE_GET_TAG(d) == JS_TAG_UNINITIALIZED) return real;
    JS_FreeValue(ctx, real);
    return d;
}

/* ---- §6.2.1 IsWellFormedLanguageTag, over the lowercased unicode_locale_id grammar ------------------------- */

typedef struct { const char *s; size_t n, i; } Tags;   /* `-`-separated subtags of a tag */

static bool next_subtag(Tags *t, const char **p, size_t *len)
{
    size_t j;

    if (t->i > t->n) return false;
    j = t->i;
    while (j < t->n && t->s[j] != '-') j++;
    *p = t->s + t->i;
    *len = j - t->i;
    t->i = j + 1;
    return true;
}

static bool all_of(const char *p, size_t n, int (*pred)(int))
{
    size_t i;

    for (i = 0; i < n; i++)
        if (!pred((unsigned char)p[i])) return false;
    return true;
}

static int is_lower_alpha(int c) { return c >= 'a' && c <= 'z'; }
static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int is_alnum(int c) { return is_lower_alpha(c) || is_digit(c); }

static bool is_language(const char *p, size_t n)
{
    return ((n >= 2 && n <= 3) || (n >= 5 && n <= 8)) && all_of(p, n, is_lower_alpha);
}
static bool is_script(const char *p, size_t n) { return n == 4 && all_of(p, n, is_lower_alpha); }
static bool is_region(const char *p, size_t n)
{
    return (n == 2 && all_of(p, n, is_lower_alpha)) || (n == 3 && all_of(p, n, is_digit));
}
static bool is_variant(const char *p, size_t n)
{
    return all_of(p, n, is_alnum) && ((n >= 5 && n <= 8) || (n == 4 && is_digit((unsigned char)p[0])));
}

/* unicode_language_id after its language subtag: script, region, variants (unique). The cursor is left on the
   first subtag that is none of them. */
static bool language_tail(Tags *t)
{
    const char *p, *vars[32];
    size_t n, vn[32], nv = 0, k, save = t->i;

    if (next_subtag(t, &p, &n) && is_script(p, n)) save = t->i; else t->i = save;
    if (next_subtag(t, &p, &n) && is_region(p, n)) save = t->i; else t->i = save;
    while (next_subtag(t, &p, &n) && is_variant(p, n)) {
        for (k = 0; k < nv; k++)
            if (vn[k] == n && memcmp(vars[k], p, n) == 0) return false;   /* step 5: duplicate variants */
        if (nv < 32) { vars[nv] = p; vn[nv] = n; nv++; }
        save = t->i;
    }
    t->i = save;
    return true;
}

bool intl_is_well_formed_language_tag(const char *tag, size_t len)
{
    char low[256], seen[36] = { 0 };
    Tags t;
    const char *p;
    size_t n, i;

    if (len == 0 || len >= sizeof low) return false;
    for (i = 0; i < len; i++) {                                                  /* step 1 */
        unsigned char c = (unsigned char)tag[i];

        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        if (!is_alnum(c) && c != '-') return false;
        low[i] = (char)c;
    }
    low[len] = '\0';
    if (low[0] == '-' || low[len - 1] == '-' || strstr(low, "--")) return false;
    t = (Tags){ low, len, 0 };
    /* Steps 2-3: unicode_language_id in its BCP 47 conformant form: a language subtag first, never "root". */
    if (!next_subtag(&t, &p, &n) || !is_language(p, n)) return false;
    if (!language_tail(&t)) return false;
    while (next_subtag(&t, &p, &n)) {                                            /* extensions */
        char single = p[0];
        int slot;

        if (n != 1) return false;
        if (single == 'x') {                                                     /* pu_extensions */
            size_t count = 0;

            while (next_subtag(&t, &p, &n)) {
                if (n < 1 || n > 8) return false;
                count++;
            }
            return count > 0;
        }
        slot = is_digit((unsigned char)single) ? single - '0' : single - 'a' + 10;
        if (seen[slot]) return false;                                            /* step 7: duplicate singletons */
        seen[slot] = 1;
        if (single == 'u') {
            size_t save = t.i, count = 0;

            /* attributes (3-8), then keywords: a key (alphanum alpha) and its type subtags (3-8) */
            while (next_subtag(&t, &p, &n)) {
                if (!(n >= 3 && n <= 8) && !(n == 2 && is_lower_alpha((unsigned char)p[1]))) { t.i = save; break; }
                save = t.i;
                count++;
            }
            t.i = save;
            if (count == 0) return false;
        } else if (single == 't') {
            size_t save = t.i, count = 0;

            if (next_subtag(&t, &p, &n) && is_language(p, n)) {                  /* tlang */
                if (!language_tail(&t)) return false;                            /* step 9: tlang variants */
                count++;
                save = t.i;
            } else {
                t.i = save;
            }
            while (next_subtag(&t, &p, &n)) {
                if (n == 2 && is_lower_alpha((unsigned char)p[0]) && is_digit((unsigned char)p[1])) {
                    size_t vals = 0;

                    save = t.i;
                    while (next_subtag(&t, &p, &n) && n >= 3 && n <= 8 && all_of(p, n, is_alnum)) {
                        vals++;
                        save = t.i;
                    }
                    t.i = save;
                    if (vals == 0) return false;
                    count++;
                } else {
                    t.i = save;
                    break;
                }
            }
            t.i = save;
            if (count == 0) return false;
        } else {
            size_t save = t.i, count = 0;

            while (next_subtag(&t, &p, &n) && n >= 2 && n <= 8) { count++; save = t.i; }
            t.i = save;
            if (count == 0) return false;
        }
    }
    return true;
}

/* ---- §6.3 and §6.6: currency codes and units --------------------------------------------------------------- */

bool intl_is_well_formed_currency_code(const char *c, size_t len)
{
    size_t i;

    if (len != 3) return false;
    for (i = 0; i < 3; i++)
        if (!((c[i] >= 'a' && c[i] <= 'z') || (c[i] >= 'A' && c[i] <= 'Z'))) return false;
    return true;
}

/* ECMA-402 §6.6.2's Table 2, in its own order, which is lexicographic. */
static const char *const SANCTIONED_UNITS[] = {
    "acre", "bit", "byte", "celsius", "centimeter", "day", "degree", "fahrenheit", "fluid-ounce", "foot",
    "gallon", "gigabit", "gigabyte", "gram", "hectare", "hour", "inch", "kilobit", "kilobyte", "kilogram",
    "kilometer", "liter", "megabit", "megabyte", "meter", "microsecond", "mile", "mile-scandinavian", "milliliter",
    "millimeter", "millisecond", "minute", "month", "nanosecond", "ounce", "percent", "petabyte", "pound", "second",
    "stone", "terabit", "terabyte", "week", "yard", "year", NULL
};

static bool is_sanctioned(const char *u, size_t len)
{
    int i;

    for (i = 0; SANCTIONED_UNITS[i]; i++)
        if (strlen(SANCTIONED_UNITS[i]) == len && memcmp(SANCTIONED_UNITS[i], u, len) == 0) return true;
    return false;
}

bool intl_is_well_formed_unit_identifier(const char *u, size_t len)
{
    const char *per;
    size_t i, at = 0, count = 0;

    if (is_sanctioned(u, len)) return true;                                     /* step 1 */
    for (i = 0; i + 5 <= len; i++)                                               /* step 2: "-per-" once */
        if (memcmp(u + i, "-per-", 5) == 0) { count++; at = i; }
    if (count != 1) return false;
    per = u + at;
    return is_sanctioned(u, at) && is_sanctioned(per + 5, len - at - 5);        /* steps 3-6 */
}

JSValue intl_available_canonical_units(JSContext *ctx)
{
    JSValue a = JS_NewArray(ctx);
    int i;

    CHECK(!JS_IsException(a), "AvailableCanonicalUnits' list could not be allocated");
    for (i = 0; SANCTIONED_UNITS[i]; i++)
        CHECK(JS_DefinePropertyValueUint32(ctx, a, (uint32_t)i, JS_NewString(ctx, SANCTIONED_UNITS[i]),
                                           JS_PROP_C_W_E) >= 0, "AvailableCanonicalUnits' list could not grow");
    return a;
}
