/* Date */

static int64_t math_mod(int64_t a, int64_t b) {
    /* return positive modulo */
    int64_t m = a % b;
    return m + (m < 0) * b;
}

static int64_t floor_div_int64(int64_t a, int64_t b) {
    /* integer division rounding toward -Infinity */
    int64_t m = a % b;
    return (a - (m + (m < 0) * b)) / b;
}

static JSValue js_Date_parse(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv);

/* A Date's [[DateValue]] when it is unknown input (§21.4.4.27 setTime over one), or JS_UNINITIALIZED for a time
   value this engine holds. Asked of a value already known to be a Date. Borrowed. */
static JSValueConst js_date_value_unknown(JSValueConst this_val)
{
    JSObject *p = JS_VALUE_GET_OBJ(this_val);

    DCHECK(p->class_id == JS_CLASS_DATE, "an unknown [[DateValue]] was asked of an object that is not a Date");
    return js_value_is_concolic(p->u.object_data) ? p->u.object_data : JS_UNINITIALIZED;
}

/* thisTimeValue: the [[DateValue]] of a Date, or a TypeError. For an unknown [[DateValue]] it is the example time
   value (NaN where there is none), which only the example world's computations may read; a caller that answers
   the page asks js_date_value_unknown first. */
static __exception int JS_ThisTimeValue(JSContext *ctx, double *valp,
                                        JSValueConst this_val)
{
    if (JS_VALUE_GET_TAG(this_val) == JS_TAG_OBJECT) {
        JSObject *p = JS_VALUE_GET_OBJ(this_val);
        if (p->class_id == JS_CLASS_DATE && JS_IsNumber(p->u.object_data))
            return JS_ToFloat64(ctx, valp, p->u.object_data);
        if (p->class_id == JS_CLASS_DATE && js_value_is_concolic(p->u.object_data)) {
            JSValue ex = js_concolic_operand(ctx, p->u.object_data);

            *valp = JS_IsNumber(ex) ? (JS_VALUE_GET_TAG(ex) == JS_TAG_INT ? (double)JS_VALUE_GET_INT(ex)
                                                                           : JS_VALUE_GET_FLOAT64(ex)) : NAN;
            JS_FreeValue(ctx, ex);
            return 0;
        }
    }
    JS_ThrowTypeError(ctx, "not a Date object");
    return -1;
}

static JSValue JS_SetThisTimeValue(JSContext *ctx, JSValueConst this_val, double v)
{
    if (JS_VALUE_GET_TAG(this_val) == JS_TAG_OBJECT) {
        JSObject *p = JS_VALUE_GET_OBJ(this_val);
        if (p->class_id == JS_CLASS_DATE) {
            /* forced-exec: [[DateValue]] is the object's own state and this writes it straight through the
               union, so `d.setHours(0)` on a baseline Date moved it for every sibling. */
            cow_capture_obj(ctx, p);
            JS_FreeValue(ctx, p->u.object_data);
            p->u.object_data = js_float64(v);
            return js_dup(p->u.object_data);
        }
    }
    return JS_ThrowTypeError(ctx, "not a Date object");
}

static int64_t days_from_year(int64_t y) {
    return 365 * (y - 1970) + floor_div_int64(y - 1969, 4) -
        floor_div_int64(y - 1901, 100) + floor_div_int64(y - 1601, 400);
}

static int64_t days_in_year(int64_t y) {
    return 365 + !(y % 4) - !(y % 100) + !(y % 400);
}

/* return the year, update days */
static int64_t year_from_days(int64_t *days) {
    int64_t y, d1, nd, d = *days;
    y = floor_div_int64(d * 10000, 3652425) + 1970;
    /* the initial approximation is very good, so only a few
       iterations are necessary */
    for(;;) {
        d1 = d - days_from_year(y);
        if (d1 < 0) {
            y--;
            d1 += days_in_year(y);
        } else {
            nd = days_in_year(y);
            if (d1 < nd)
                break;
            d1 -= nd;
            y++;
        }
    }
    *days = d1;
    return y;
}

static int const month_days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
static char const month_names[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
static char const day_names[] = "SunMonTueWedThuFriSat";

static __exception int get_date_fields(JSContext *ctx, JSValueConst obj,
                                       double fields[minimum_length(9)],
                                       int is_local, int force)
{
    double dval;
    int64_t d, days, wd, y, i, md, h, m, s, ms, tz = 0;

    if (JS_ThisTimeValue(ctx, &dval, obj))
        return -1;

    if (isnan(dval)) {
        if (!force)
            return false; /* NaN */
        d = 0;        /* initialize all fields to 0 */
    } else {
        d = dval;     /* assuming -8.64e15 <= dval <= -8.64e15 */
        if (is_local) {
            tz = -getTimezoneOffset(d);
            d += tz * 60000;
        }
    }

    /* result is >= 0, we can use % */
    h = math_mod(d, 86400000);
    days = (d - h) / 86400000;
    ms = h % 1000;
    h = (h - ms) / 1000;
    s = h % 60;
    h = (h - s) / 60;
    m = h % 60;
    h = (h - m) / 60;
    wd = math_mod(days + 4, 7); /* week day */
    y = year_from_days(&days);

    for(i = 0; i < 11; i++) {
        md = month_days[i];
        if (i == 1)
            md += days_in_year(y) - 365;
        if (days < md)
            break;
        days -= md;
    }
    fields[0] = y;
    fields[1] = i;
    fields[2] = days + 1;
    fields[3] = h;
    fields[4] = m;
    fields[5] = s;
    fields[6] = ms;
    fields[7] = wd;
    fields[8] = tz;
    return true;
}

static double time_clip(double t) {
    if (t >= -8.64e15 && t <= 8.64e15)
        return trunc(t) + 0.0;  /* convert -0 to +0 */
    else
        return NAN;
}

/* The spec mandates the use of 'double' and it specifies the order
   of the operations */
static double set_date_fields(double fields[minimum_length(7)], int is_local) {
    double y, m, dt, ym, mn, day, h, s, milli, time, tv;
    int yi, mi, i;
    int64_t days;
    volatile double temp;  /* enforce evaluation order */
    double d = NAN;

    JS_X87_FPCW_SAVE_AND_ADJUST(fpcw);
    /* emulate 21.4.1.15 MakeDay ( year, month, date ) */
    y = fields[0];
    m = fields[1];
    dt = fields[2];
    ym = y + floor(m / 12);
    mn = fmod(m, 12);
    if (mn < 0)
        mn += 12;
    if (ym < -271821 || ym > 275760)
        goto out;

    yi = ym;
    mi = mn;
    days = days_from_year(yi);
    for(i = 0; i < mi; i++) {
        days += month_days[i];
        if (i == 1)
            days += days_in_year(yi) - 365;
    }
    day = days + dt - 1;

    /* emulate 21.4.1.14 MakeTime ( hour, min, sec, ms ) */
    h = fields[3];
    m = fields[4];
    s = fields[5];
    milli = fields[6];
    /* Use a volatile intermediary variable to ensure order of evaluation
     * as specified in ECMA. This fixes a test262 error on
     * test262/test/built-ins/Date/UTC/fp-evaluation-order.js.
     * Without the volatile qualifier, the compile can generate code
     * that performs the computation in a different order or with instructions
     * that produce a different result such as FMA (float multiply and add).
     */
    time = h * 3600000;
    time += (temp = m * 60000);
    time += (temp = s * 1000);
    time += milli;

    /* emulate 21.4.1.16 MakeDate ( day, time ) */
    tv = (temp = day * 86400000) + time;   /* prevent generation of FMA */
    if (!isfinite(tv))
        goto out;

    /* adjust for local time and clip */
    if (is_local) {
        int64_t ti = tv < INT64_MIN ? INT64_MIN : tv >= 0x1p63 ? INT64_MAX : (int64_t)tv;
        tv += getTimezoneOffset(ti) * 60000;
    }
    d = time_clip(tv);
out:
    JS_X87_FPCW_RESTORE(fpcw);
    return d;
}

static JSValue get_date_field(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv, int magic)
{
    // get_date_field(obj, n, is_local)
    double fields[9];
    int res, n, is_local;

    is_local = magic & 0x0F;
    n = (magic >> 4) & 0x0F;
    res = get_date_fields(ctx, this_val, fields, is_local, 0);
    if (res < 0)
        return JS_EXCEPTION;
    if (!JS_IsUninitialized(js_date_value_unknown(this_val))) {
        /* §21.4.4's field getters over an unknown time value: each field is derived from it under its own
           getter's name, with the example time value's field as its example. */
        static const char *const getters[2][8] = {
            { "21.4.4.14 Date.prototype.getUTCFullYear", "21.4.4.18 Date.prototype.getUTCMonth",
              "21.4.4.12 Date.prototype.getUTCDate", "21.4.4.15 Date.prototype.getUTCHours",
              "21.4.4.17 Date.prototype.getUTCMinutes", "21.4.4.19 Date.prototype.getUTCSeconds",
              "21.4.4.16 Date.prototype.getUTCMilliseconds", "21.4.4.13 Date.prototype.getUTCDay" },
            { "21.4.4.4 Date.prototype.getFullYear", "21.4.4.8 Date.prototype.getMonth",
              "21.4.4.2 Date.prototype.getDate", "21.4.4.5 Date.prototype.getHours",
              "21.4.4.7 Date.prototype.getMinutes", "21.4.4.9 Date.prototype.getSeconds",
              "21.4.4.6 Date.prototype.getMilliseconds", "21.4.4.3 Date.prototype.getDay" } };
        JSValue real = !res ? JS_NAN : js_number(fields[n] - ((magic & 0x100) ? 1900 : 0));

        DCHECK(n < 8 && is_local < 2, "a Date field getter's magic names no field");
        return js_concolic_derive(ctx, js_date_value_unknown(this_val),
                                  (magic & 0x100) ? "B.2.3.1 Date.prototype.getYear" : getters[is_local][n], real);
    }
    if (!res)
        return JS_NAN;

    if (magic & 0x100) {    // getYear
        fields[0] -= 1900;
    }
    return js_number(fields[n]);
}

/* 21.4.4.27 step 1: RequireInternalSlot(dateObject, [[DateValue]]) precedes setTime's ToNumber, so a receiver
   that is not a Date throws before the page's valueOf runs rather than after it. */
static int js_date_this_precheck(JSContext *ctx, const JSStepHdr *h)
{
    double d;
    return JS_ThisTimeValue(ctx, &d, h->this_val);
}

/* The setter's last steps over an unknown t: the new time value is derived from t and the argument a coercion
   met as unknown, if any (taken off the header, since this answer is the completion), with the steps run on the
   examples as its example. Writing it is also step 7's arm for a NaN t, whose new value is NaN, the value the
   slot already holds; setFullYear's and setYear's +0 for a NaN t is in the example the fields computed.
   Named residual. Not covered: an argument with no example ends the machine at its coercion, so the slot keeps t.
   Next diff builds: answers_unknown on the setters, deriving from every unknown argument. How its absence shows:
   a Date read after `d.setHours(u)` with an exampleless `u` answering the time value it held before. */
static int js_date_set_unknown(JSContext *ctx, JSDateSet *s)
{
    JSValueConst ops[2];
    JSValue ex = JS_UNDEFINED, dv;
    JSObject *p = JS_VALUE_GET_OBJ(s->hdr.this_val);
    int magic = s->hdr.arg, n = 0;

    ops[n++] = s->t;
    if (!JS_IsUninitialized(s->hdr.unknown_operand))
        ops[n++] = s->hdr.unknown_operand;
    if (js_concolic_held(s->t))
        ex = js_float64(!s->res ? NAN : s->nonfinite || s->hdr.argc == 0 ? NAN
                                     : set_date_fields(s->fields, magic & 0xF));
    dv = g_concolic.derived(ctx, s->hdr.def->algorithm, ops, n, ex);
    JS_FreeValue(ctx, s->hdr.unknown_operand);
    s->hdr.unknown_operand = JS_UNINITIALIZED;
    DCHECK(js_value_is_concolic(dv), "a Date setter's time value over an unknown t is not unknown");
    cow_capture_obj(ctx, p);
    JS_FreeValue(ctx, p->u.object_data);
    p->u.object_data = js_dup(dv);
    s->result = dv;
    return 0;
}

/* DELETED: set_date_field and js_date_setYear. Both ran their ToNumbers with JS_ToFloat64 from a C entry, and
   js_date_setYear then re-entered set_date_field, which read [[DateValue]] a SECOND time — after the coercion
   that may have changed it. The one machine below performs the read once, at the step the spec puts it. */
static int js_date_set_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSDateSet *s = st;
    int magic = s->hdr.arg;
    int first_field = (magic >> 8) & 0xF, end_field = (magic >> 4) & 0xF, is_local = magic & 0xF;
    int r;
    double a, d;

    if (s->hdr.stage == DATESET_VALIDATE) {
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->result = JS_UNDEFINED;
        s->t = JS_UNINITIALIZED;
        /* steps 1-3: RequireInternalSlot and the [[DateValue]] read, both BEFORE the first coercion. `first_field
           == 0` is B.2.3.2 step 4 / 21.4.4.21 step 5 — the setters that write the YEAR treat a NaN time value as
           +0 instead of returning early. */
        s->res = get_date_fields(ctx, s->hdr.this_val, s->fields, is_local, first_field == 0);
        if (s->res < 0)
            return -1;
        /* An unknown t is held for the derivation below; `fields` hold its example's, and res says NaN where it
           has none. */
        if (!JS_IsUninitialized(js_date_value_unknown(s->hdr.this_val)))
            s->t = js_dup(js_date_value_unknown(s->hdr.this_val));
        s->n = min_int(s->hdr.argc, end_field - first_field);
        s->i = 0;
        s->nonfinite = false;
        s->hdr.stage = DATESET_ARGS;
    }
    /* the coercion steps: every named argument, in argument order, and one beyond the count is not read at all.
       Each is the page's code, which is the whole reason this is a machine. */
    DCHECK(s->hdr.stage == DATESET_ARGS, "a Date setter resumed in no stage");
    while (s->i < s->n) {
        r = step_tofloat64_run(ctx, &s->hdr, step_arg(&s->hdr, s->i), cb_result, &a, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        if (magic & DATE_SET_MAKEFULLYEAR) {
            /* B.2.3.2 step 5 MakeFullYear: a two-digit year is 1900-relative, and a NaN stays NaN. */
            if (isfinite(a)) {
                a = trunc(a);
                if (a >= 0 && a < 100)
                    a += 1900;
            }
        }
        if (!isfinite(a))
            s->nonfinite = true;
        s->fields[first_field + s->i] = trunc(a);
        s->i++;
    }
    JS_FreeValue(ctx, cb_result);

    if (!JS_IsUninitialized(s->t))
        return js_date_set_unknown(ctx, s);
    /* step 7: `If t is NaN, return NaN` — on the value read at step 2, and the [[DateValue]] is NOT written. */
    if (!s->res) {
        s->result = JS_NAN;
        return 0;
    }
    /* A NON-FINITE ARGUMENT is a different thing entirely: MakeTime/MakeDay yield NaN, TimeClip yields NaN, and
       step 14 STORES it. One `res` flag stood for both, so `new Date(0).setUTCHours(Infinity)` returned NaN and
       left the Date VALID — and so did setUTCFullYear(NaN) and every other setter given an infinity. It is also
       the one case set_date_fields cannot be handed: a NaN field reaches days_from_year((int)NaN). */
    d = NAN;
    if (s->hdr.argc > 0)
        d = s->nonfinite ? NAN : set_date_fields(s->fields, is_local);
    s->result = JS_SetThisTimeValue(ctx, s->hdr.this_val, d);
    return JS_IsException(s->result) ? (s->result = JS_UNDEFINED, -1) : 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit): the result and an unknown t. The captured date fields are doubles
   read before any argument was coerced, and the argument coercions run on the header's own captures. */
static void js_date_set_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSDateSet *s = st;
    v->val(ctx, &s->result);
    v->val(ctx, &s->t);
}

static JSValue js_date_set_fini(JSContext *ctx, void *st, bool take_result)
{
    JSDateSet *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

/* fmt:
   0: toUTCString: "Tue, 02 Jan 2018 23:04:46 GMT"
   1: toString: "Wed Jan 03 2018 00:05:22 GMT+0100 (CET)"
   2: toISOString: "2018-01-02T23:02:56.927Z"
   3: toLocaleString: "1/2/2018, 11:40:40 PM"
   part: 1=date, 2=time 3=all
   XXX: should use a variant of strftime().
 */
static JSValue get_date_string_of(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic);

/* The string forms (§21.4.4.35-21.4.4.43). Over an unknown [[DateValue]] the string is derived from it, with the
   string of the example time value as its example; toISOString reaches here over one only on the finite arm of
   its step 4 fork, where an example that throws leaves the derivation with none. */
static JSValue get_date_string(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv, int magic)
{
    JSValue real = get_date_string_of(ctx, this_val, argc, argv, magic);
    const char *op;

    if (!JS_IsDate(this_val) || JS_IsUninitialized(js_date_value_unknown(this_val)))
        return real;
    switch (magic) {
    case 0x13: op = "21.4.4.41 Date.prototype.toString"; break;
    case 0x03: op = "21.4.4.43 Date.prototype.toUTCString"; break;
    case 0x23: op = "21.4.4.36 Date.prototype.toISOString"; break;
    case 0x11: op = "21.4.4.35 Date.prototype.toDateString"; break;
    case 0x12: op = "21.4.4.42 Date.prototype.toTimeString"; break;
    case 0x33: op = "21.4.4.39 Date.prototype.toLocaleString"; break;
    case 0x31: op = "21.4.4.38 Date.prototype.toLocaleDateString"; break;
    case 0x32: op = "21.4.4.40 Date.prototype.toLocaleTimeString"; break;
    default:
        DFAIL("a Date string form with no algorithm to name its derivation over an unknown time value");
        op = NULL;
    }
    return js_concolic_derive(ctx, js_date_value_unknown(this_val), op, real);
}

static JSValue get_date_string_of(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv, int magic)
{
    // _string(obj, fmt, part)
    char buf[64];
    double fields[9];
    int res, fmt, part, pos;
    int y, mon, d, h, m, s, ms, wd, tz;

    fmt = (magic >> 4) & 0x0F;
    part = magic & 0x0F;

    res = get_date_fields(ctx, this_val, fields, fmt & 1, 0);
    if (res < 0)
        return JS_EXCEPTION;
    if (!res) {
        if (fmt == 2)
            return JS_ThrowRangeError(ctx, "Date value is NaN");
        else
            return js_new_string8(ctx, "Invalid Date");
    }

    y = fields[0];
    mon = fields[1];
    d = fields[2];
    h = fields[3];
    m = fields[4];
    s = fields[5];
    ms = fields[6];
    wd = fields[7];
    tz = fields[8];

    pos = 0;

    if (part & 1) { /* date part */
        switch(fmt) {
        case 0:
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                            "%.3s, %02d %.3s %0*d ",
                            day_names + wd * 3, d,
                            month_names + mon * 3, 4 + (y < 0), y);
            break;
        case 1:
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                            "%.3s %.3s %02d %0*d",
                            day_names + wd * 3,
                            month_names + mon * 3, d, 4 + (y < 0), y);
            if (part == 3) {
                buf[pos++] = ' ';
            }
            break;
        case 2:
            if (y >= 0 && y <= 9999) {
                pos += snprintf(buf + pos, sizeof(buf) - pos,
                                "%04d", y);
            } else {
                pos += snprintf(buf + pos, sizeof(buf) - pos,
                                "%+07d", y);
            }
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                            "-%02d-%02dT", mon + 1, d);
            break;
        case 3:
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                            "%02d/%02d/%0*d", mon + 1, d, 4 + (y < 0), y);
            if (part == 3) {
                buf[pos++] = ',';
                buf[pos++] = ' ';
            }
            break;
        }
    }
    if (part & 2) { /* time part */
        switch(fmt) {
        case 0:
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                            "%02d:%02d:%02d GMT", h, m, s);
            break;
        case 1:
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                            "%02d:%02d:%02d GMT", h, m, s);
            if (tz < 0) {
                buf[pos++] = '-';
                tz = -tz;
            } else {
                buf[pos++] = '+';
            }
            /* tz is >= 0, can use % */
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                            "%02d%02d", tz / 60, tz % 60);
            /* XXX: tack the time zone code? */
            break;
        case 2:
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                            "%02d:%02d:%02d.%03dZ", h, m, s, ms);
            break;
        case 3:
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                            "%02d:%02d:%02d %cM", (h + 11) % 12 + 1, m, s,
                            (h < 12) ? 'A' : 'P');
            break;
        }
    }
    if (!pos) {
        // XXX: should throw exception?
        return js_empty_string(ctx->rt);
    }
    return js_new_string8_len(ctx, buf, pos);
}

/* ECMAScript §21.4.2.1 Date ( ...values )'s "the time value (UTC) identifying the current time", which §21.4.3.1
   Date.now ( ) also returns: a §21.4.1.1 time value, or an unknown whose example is one. The host answers
   because the current time is the visitor's, and a wall-clock read here would decide every gate over it. */
static JSValue js_date_current_time(JSContext *ctx)
{
    JSValue v;

    CHECK(g_concolic.clock != NULL,
          "the current time was asked under a host that installed no JSConcolicHooks.clock, a member of "
          "JS_CONCOLIC_REQUIRED: the engine reads no clock of its own");
    v = g_concolic.clock(ctx);
    DCHECK((g_concolic.is(v)) ||
           (JS_IsNumber(v) && (JS_VALUE_GET_NORM_TAG(v) != JS_TAG_FLOAT64 ||
                               JS_VALUE_GET_FLOAT64(v) == trunc(JS_VALUE_GET_FLOAT64(v)))),
           "JSConcolicHooks.clock answered neither an integral Number nor an unknown; §21.4.1.1 makes a finite "
           "time value integral");
    return v;
}


static JSValue js_Date_UTC(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    // UTC(y, mon, d, h, m, s, ms)
    double fields[] = { 0, 0, 1, 0, 0, 0, 0 };
    int i, n;
    double a;
    JSValue u;

    /* Steps 1-7 are ToNumber over each present argument, which the coerce-then-compute declaration leaves to
       this body, and its ToPrimitive hands an unknown through uncoerced. An unknown present argument makes the
       time value unknown, derived with the time value the examples compute as its example. */
    u = js_primargs_body_unknown(ctx, js_Date_UTC, JS_DATE_UTC_ALGORITHM, this_val, argc, 7, argv);
    if (!JS_IsUninitialized(u))
        return u;

    n = argc;
    if (n == 0)
        return JS_NAN;
    if (n > 7)
        n = 7;
    for(i = 0; i < n; i++) {
        if (JS_ToFloat64(ctx, &a, argv[i]))
            return JS_EXCEPTION;
        if (!isfinite(a))
            return JS_NAN;
        fields[i] = trunc(a);
        if (i == 0 && fields[0] >= 0 && fields[0] < 100)
            fields[0] += 1900;
    }
    return js_float64(set_date_fields(fields, 0));
}

/* Date string parsing */

static bool string_skip_char(const uint8_t *sp, int *pp, int c) {
    if (sp[*pp] == c) {
        *pp += 1;
        return true;
    } else {
        return false;
    }
}

/* skip spaces, update offset, return next char */
static int string_skip_spaces(const uint8_t *sp, int *pp) {
    int c;
    while ((c = sp[*pp]) == ' ')
        *pp += 1;
    return c;
}

/* skip dashes dots and commas */
static int string_skip_separators(const uint8_t *sp, int *pp) {
    int c;
    while ((c = sp[*pp]) == '-' || c == '/' || c == '.' || c == ',')
        *pp += 1;
    return c;
}

/* skip a word, stop on spaces, digits and separators, update offset */
static int string_skip_until(const uint8_t *sp, int *pp, const char *stoplist) {
    int c;
    while (!strchr(stoplist, c = sp[*pp]))
        *pp += 1;
    return c;
}

/* parse a numeric field (max_digits = 0 -> no maximum) */
static bool string_get_digits(const uint8_t *sp, int *pp, int *pval,
                              int min_digits, int max_digits)
{
    int v = 0;
    int c, p = *pp, p_start;

    p_start = p;
    while ((c = sp[p]) >= '0' && c <= '9') {
        /* arbitrary limit to 9 digits */
        if (v >= 100000000)
            return false;
        v = v * 10 + c - '0';
        p++;
        if (p - p_start == max_digits)
            break;
    }
    if (p - p_start < min_digits)
        return false;
    *pval = v;
    *pp = p;
    return true;
}

static bool string_get_milliseconds(const uint8_t *sp, int *pp, int *pval) {
    /* parse optional fractional part as milliseconds and truncate. */
    /* spec does not indicate which rounding should be used */
    int mul = 100, ms = 0, c, p_start, p = *pp;

    c = sp[p];
    if (c == '.' || c == ',') {
        p++;
        p_start = p;
        while ((c = sp[p]) >= '0' && c <= '9') {
            ms += (c - '0') * mul;
            mul /= 10;
            p++;
            if (p - p_start == 9)
                break;
        }
        if (p > p_start) {
            /* only consume the separator if digits are present */
            *pval = ms;
            *pp = p;
        }
    }
    return true;
}

static bool string_get_tzoffset(const uint8_t *sp, int *pp, int *tzp, bool strict) {
    int tz = 0, sgn, hh, mm, p = *pp;

    sgn = sp[p++];
    if (sgn == '+' || sgn == '-') {
        int n = p;
        if (!string_get_digits(sp, &p, &hh, 1, 0))
            return false;
        n = p - n;
        /* The Date Time String Format writes the offset as `+HH:mm` -- the colon is part of the grammar. Web
           reality adds `+HHmm`, which every engine accepts there, but a BARE `+HH` is legacy-parser-only:
           `1997-03-08T11:19:10-07` is NaN in Chrome while `1997-03-08 11:19:10-07` is not. */
        if (strict && n != 2 && n != 4)
            return false;
        if (strict && n == 2 && sp[p] != ':')
            return false;
        while (n > 4) {
            n -= 2;
            hh /= 100;
        }
        if (n > 2) {
            mm = hh % 100;
            hh = hh / 100;
        } else {
            mm = 0;
            if (string_skip_char(sp, &p, ':')  /* optional separator */
            &&  !string_get_digits(sp, &p, &mm, 2, 2))
                return false;
        }
        if (hh > 23 || mm > 59)
            return false;
        tz = hh * 60 + mm;
        if (sgn != '+')
            tz = -tz;
    } else
    if (sgn != 'Z') {
        return false;
    }
    *pp = p;
    *tzp = tz;
    return true;
}

static bool string_match(const uint8_t *sp, int *pp, const char *s) {
    int p = *pp;
    while (*s != '\0') {
        if (to_upper_ascii(sp[p]) != to_upper_ascii(*s++))
            return false;
        p++;
    }
    *pp = p;
    return true;
}

static int find_abbrev(const uint8_t *sp, int p, const char *list, int count) {
    int n, i;

    for (n = 0; n < count; n++) {
        for (i = 0;; i++) {
            if (to_upper_ascii(sp[p + i]) != to_upper_ascii(list[n * 3 + i]))
                break;
            if (i == 2)
                return n;
        }
    }
    return -1;
}

static bool string_get_month(const uint8_t *sp, int *pp, int *pval) {
    int n;

    n = find_abbrev(sp, *pp, month_names, 12);
    if (n < 0)
        return false;

    *pval = n + 1;
    *pp += 3;
    return true;
}

/* Parse the Date Time String Format (21.4.1.15) and the one RELAXATION every engine shares with it: the same
   grammar with a SPACE where the `T` goes, and then months, days, hours, minutes and seconds of one OR two
   digits and a bare `+HH` offset. The relaxed form is what a page gets from a database or a `<input type=
   datetime-local>`, and it is this grammar rather than the free-form legacy scanner -- `1997-3-8 11:19:20` is
   an ISO date with a one-digit month, not a sentence about a date. The strict form stays strict: the same
   string written with a `T` is NaN. */
static bool js_date_parse_isostring(const uint8_t *sp, int fields[9], bool *is_local) {
    int sgn, i, p = 0, lo;
    bool relaxed;

    /* initialize fields to the beginning of the Epoch */
    for (i = 0; i < 9; i++) {
        fields[i] = (i == 2);
    }
    *is_local = false;

    /* year is either yyyy digits or [+-]yyyyyy */
    sgn = sp[p];
    if (sgn == '-' || sgn == '+') {
        p++;
        if (!string_get_digits(sp, &p, &fields[0], 6, 6))
            return false;
        if (sgn == '-') {
            if (fields[0] == 0)
                return false; // reject -000000
            fields[0] = -fields[0];
        }
    } else {
        if (!string_get_digits(sp, &p, &fields[0], 4, 4))
            return false;
    }
    /* Which separator follows the date decides the whole rest of the grammar, so it is read BEFORE the month
       and day: a one-digit month is legal only in the relaxed form. */
    relaxed = false;
    for (i = p; sp[i] == '-' || (sp[i] >= '0' && sp[i] <= '9') || sp[i] == ':'; i++)
        continue;
    if (sp[i] == ' ')
        relaxed = true;
    lo = relaxed ? 1 : 2;
    if (string_skip_char(sp, &p, '-')) {
        if (!string_get_digits(sp, &p, &fields[1], lo, 2))  /* month */
            return false;
        if (fields[1] < 1)
            return false;
        fields[1] -= 1;
        if (string_skip_char(sp, &p, '-')) {
            if (!string_get_digits(sp, &p, &fields[2], lo, 2))  /* day */
                return false;
            if (fields[2] < 1)
                return false;
        }
    }
    if (string_skip_char(sp, &p, relaxed ? ' ' : 'T')) {
        *is_local = true;
        if (!string_get_digits(sp, &p, &fields[3], lo, 2)  /* hour */
        ||  !string_skip_char(sp, &p, ':')
        ||  !string_get_digits(sp, &p, &fields[4], lo, 2)) {  /* minute */
            if (relaxed)
                return false;   /* not this grammar: let the legacy parser see the string */
            fields[3] = 100;  /* a `T` commits to this grammar, so a malformed time is NaN, never retried */
            return true;
        }
        if (string_skip_char(sp, &p, ':')) {
            if (!string_get_digits(sp, &p, &fields[5], lo, 2))  /* second */
                return false;
            string_get_milliseconds(sp, &p, &fields[6]);
        }
    }
    /* parse the time zone offset if present: [+-]HH:mm, [+-]HHmm, `Z`, and [+-]HH when relaxed */
    if (sp[p]) {
        *is_local = false;
        if (!string_get_tzoffset(sp, &p, &fields[8], !relaxed))
            return false;
    }
    /* error if extraneous characters */
    return sp[p] == '\0';
}

static struct {
    char name[6];
    int16_t offset;
} const js_tzabbr[] = {
    { "GMT",   0 },         // Greenwich Mean Time
    { "UTC",   0 },         // Coordinated Universal Time
    { "UT",    0 },         // Universal Time
    { "Z",     0 },         // Zulu Time
    { "EDT",  -4 * 60 },    // Eastern Daylight Time
    { "EST",  -5 * 60 },    // Eastern Standard Time
    { "CDT",  -5 * 60 },    // Central Daylight Time
    { "CST",  -6 * 60 },    // Central Standard Time
    { "MDT",  -6 * 60 },    // Mountain Daylight Time
    { "MST",  -7 * 60 },    // Mountain Standard Time
    { "PDT",  -7 * 60 },    // Pacific Daylight Time
    { "PST",  -8 * 60 },    // Pacific Standard Time
    { "WET",  +0 * 60 },    // Western European Time
    { "WEST", +1 * 60 },    // Western European Summer Time
    { "CET",  +1 * 60 },    // Central European Time
    { "CEST", +2 * 60 },    // Central European Summer Time
    { "EET",  +2 * 60 },    // Eastern European Time
    { "EEST", +3 * 60 },    // Eastern European Summer Time
};

static bool string_get_tzabbr(const uint8_t *sp, int *pp, int *offset) {
    size_t i;

    for (i = 0; i < countof(js_tzabbr); i++) {
        if (string_match(sp, pp, js_tzabbr[i].name)) {
            *offset = js_tzabbr[i].offset;
            return true;
        }
    }
    return false;
}

/* parse toString, toUTCString and other formats */
static bool js_date_parse_otherstring(const uint8_t *sp,
                                      int fields[minimum_length(9)],
                                      bool *is_local) {
    int c, i, val, p = 0, p_start;
    int num[3];
    bool has_year = false;
    bool has_mon = false;
    bool has_time = false;
    int num_index = 0;

    /* initialize fields to the beginning of 2001-01-01 */
    fields[0] = 2001;
    fields[1] = 1;
    fields[2] = 1;
    for (i = 3; i < 9; i++) {
        fields[i] = 0;
    }
    *is_local = true;

    while (string_skip_spaces(sp, &p)) {
        p_start = p;
        if ((c = sp[p]) == '+' || c == '-') {
            if (has_time && string_get_tzoffset(sp, &p, &fields[8], false)) {
                *is_local = false;
            } else {
                p++;
                if (string_get_digits(sp, &p, &val, 1, 0)) {
                    if (c == '-') {
                        if (val == 0)
                            return false;
                        val = -val;
                    }
                    fields[0] = val;
                    has_year = true;
                }
            }
        } else
        if (string_get_digits(sp, &p, &val, 1, 0)) {
            if (string_skip_char(sp, &p, ':')) {
                /* time part */
                fields[3] = val;
                if (!string_get_digits(sp, &p, &fields[4], 1, 2))
                    return false;
                if (string_skip_char(sp, &p, ':')) {
                    if (!string_get_digits(sp, &p, &fields[5], 1, 2))
                        return false;
                    string_get_milliseconds(sp, &p, &fields[6]);
                } else
                if (sp[p] != '\0' && sp[p] != ' ')
                    return false;
                has_time = true;
            } else {
                if (p - p_start > 2) {
                    if (has_year)
                        return false;
                    fields[0] = val;
                    has_year = true;
                } else
                if (val < 1 || val > 31) {
                    /* A number that cannot be a day or a month is the year -- and a string cannot have TWO
                       years. It used to overwrite the first one and leave the leftover count looking legal, so
                       `99/1/99` became 1999-01-01 (day 99 silently gone) and `0/10/0` became a date at all.
                       Both are NaN in Chrome, because neither string names three fields that fit. */
                    if (has_year)
                        return false;
                    fields[0] = val + (val < 100) * 1900 + (val < 50) * 100;
                    has_year = true;
                } else {
                    if (num_index == 3)
                        return false;
                    num[num_index++] = val;
                }
            }
        } else
        if (string_get_month(sp, &p, &fields[1])) {
            has_mon = true;
            string_skip_until(sp, &p, "0123456789 -/(");
        } else
        if (has_time && string_match(sp, &p, "PM")) {
            /* hours greater than 12 will be incremented and
               rejected by the range check in js_Date_parse */
            /* 00:00 PM will be silently converted as 12:00 */
            if (fields[3] != 12)
                fields[3] += 12;
            continue;
        } else
        if (has_time && string_match(sp, &p, "AM")) {
            /* 00:00 AM will be silently accepted */
            if (fields[3] > 12)
                return false;
            if (fields[3] == 12)
                fields[3] -= 12;
            continue;
        } else
        if (string_get_tzabbr(sp, &p, &fields[8])) {
            *is_local = false;
            continue;
        } else
        if (c == '(') {  /* skip parenthesized phrase */
            int level = 0;
            while ((c = sp[p]) != '\0') {
                p++;
                level += (c == '(');
                level -= (c == ')');
                if (!level)
                    break;
            }
            if (level > 0)
                return false;
        } else
        if (c == ')') {
            return false;
        } else {
            if (has_year + has_mon + has_time + num_index)
                return false;
            /* skip a word */
            string_skip_until(sp, &p, " -/(");
        }
        string_skip_separators(sp, &p);
    }
    if (num_index + has_year + has_mon > 3)
        return false;

    switch (num_index) {
    case 0:
        if (!has_year)
            return false;
        break;
    case 1:
        if (has_mon)
            fields[2] = num[0];
        else
            fields[1] = num[0];
        break;
    case 2:
        if (has_year) {
            fields[1] = num[0];
            fields[2] = num[1];
        } else
        if (has_mon) {
            fields[0] = num[1] + (num[1] < 100) * 1900 + (num[1] < 50) * 100;
            fields[2] = num[0];
        } else {
            fields[1] = num[0];
            fields[2] = num[1];
        }
        break;
    case 3:
        fields[0] = num[2] + (num[2] < 100) * 1900 + (num[2] < 50) * 100;
        fields[1] = num[0];
        fields[2] = num[1];
        break;
    default:
        return false;
    }
    if (fields[1] < 1 || fields[2] < 1)
        return false;
    fields[1] -= 1;
    return true;
}

static JSValue js_Date_parse(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    JSValue s, rv;
    int fields[9];
    double fields1[9];
    double d;
    int i, c;
    JSString *sp;
    uint8_t buf[128];
    bool is_local;

    rv = JS_NAN;

    {   /* §21.4.3.2 Date.parse ( string ) OVER UNKNOWN EXTERNAL INPUT. "This function applies the ToString
           operator to its argument … it returns a Number, the UTC time value" — so the operator's result is a
           NUMBER derived from the unknown, never a coercion this function's ToString below could perform (that
           boundary owes C a real JSString). THE PARSE ITSELF RUNS, on the operand's own example: §solver
           propagates an example only by performing the real operation on the concrete, so this re-enters with
           the example in place of the argument — the same path a page with a known date takes — and the
           derived unknown carries the time value this function REALLY computed. `Date.parse(x)` on a cookie or
           a query parameter is a real bundle shape (an expiry check gating a refresh call), and the branch on
           its result still forks because the result is still unknown. */
        JSValue ex = js_concolic_operand(ctx, argv[0]);
        if (!JS_IsUninitialized(ex)) {
            JSValue real = JS_UNDEFINED;
            if (js_concolic_codec_operand(argv[0], ex)) { JSValueConst a2[1]; a2[0] = ex; real = js_Date_parse(ctx, this_val, 1, a2); }
            JS_FreeValue(ctx, ex);
            return js_concolic_derive(ctx, argv[0], "Date.parse", real);
        }
    }
    DCHECK(argc < 1 || JS_VALUE_GET_TAG(argv[0]) != JS_TAG_OBJECT,
           "Date.parse's body was handed an object — its machine and the Date constructor's step 4.c.i both "
           "make the argument primitive before the body runs");
    s = JS_ToString(ctx, argv[0]);
    if (JS_IsException(s))
        return JS_EXCEPTION;

    sp = JS_VALUE_GET_STRING(s);
    /* convert the string as a byte array */
    for (i = 0; i < sp->len && i < (int)countof(buf) - 1; i++) {
        c = string_get(sp, i);
        if (c > 255)
            c = (c == 0x2212) ? '-' : 'x';
        buf[i] = c;
    }
    buf[i] = '\0';
    if (js_date_parse_isostring(buf, fields, &is_local)
    ||  js_date_parse_otherstring(buf, fields, &is_local)) {
        static int const field_max[6] = { 0, 11, 31, 24, 59, 59 };
        bool valid = true;
        /* check field maximum values */
        for (i = 1; i < 6; i++) {
            if (fields[i] > field_max[i])
                valid = false;
        }
        /* special case 24:00:00.000 */
        if (fields[3] == 24 && (fields[4] | fields[5] | fields[6]))
            valid = false;
        if (valid) {
            for(i = 0; i < 7; i++)
                fields1[i] = fields[i];
            d = set_date_fields(fields1, is_local) - fields[8] * 60000;
            rv = js_float64(d);
        }
    }
    JS_FreeValue(ctx, s);
    return rv;
}

static JSValue js_Date_now(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    /* §21.4.3.1 Date.now ( ) */
    return js_date_current_time(ctx);
}


static JSValue js_date_getTimezoneOffset(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    // getTimezoneOffset()
    double v;

    if (JS_ThisTimeValue(ctx, &v, this_val))
        return JS_EXCEPTION;
    if (!JS_IsUninitialized(js_date_value_unknown(this_val)))
        return js_concolic_derive(ctx, js_date_value_unknown(this_val),
                                  "21.4.4.11 Date.prototype.getTimezoneOffset",
                                  isnan(v) ? JS_NAN : js_int64(getTimezoneOffset((int64_t)trunc(v))));
    if (isnan(v))
        return JS_NAN;
    else
        /* assuming -8.64e15 <= v <= -8.64e15 */
        return js_int64(getTimezoneOffset((int64_t)trunc(v)));
}

static JSValue js_date_getTime(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    // getTime()
    double v;

    if (JS_ThisTimeValue(ctx, &v, this_val))
        return JS_EXCEPTION;
    /* §21.4.4.10 step 3: return dateObject.[[DateValue]], which is the unknown itself where it is one */
    if (!JS_IsUninitialized(js_date_value_unknown(this_val)))
        return js_dup(js_date_value_unknown(this_val));
    return js_float64(v);
}

static JSValue js_date_setTime(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    // setTime(v)
    double v;

    if (JS_ThisTimeValue(ctx, &v, this_val))
        return JS_EXCEPTION;
    /* §21.4.4.27 steps 3-5 over unknown input: t is the Number itself (§7.1.4 step 1), TimeClip(t) is derived
       from it with the real clip over its example, and that unknown becomes the [[DateValue]]. */
    if (js_value_is_concolic(argv[0])) {
        JSObject *p = JS_VALUE_GET_OBJ(this_val);
        JSValue ex = js_concolic_operand(ctx, argv[0]), real = JS_UNDEFINED, dv;
        double e;

        if (JS_IsNumber(ex) && JS_ToFloat64(ctx, &e, ex) == 0)
            real = js_float64(time_clip(e));
        JS_FreeValue(ctx, ex);
        dv = js_concolic_derive(ctx, argv[0], "21.4.4.27 Date.prototype.setTime TimeClip(t)", real);
        DCHECK(js_value_is_concolic(dv), "setTime's TimeClip over unknown input derived no unknown");
        cow_capture_obj(ctx, p);
        JS_FreeValue(ctx, p->u.object_data);
        p->u.object_data = dv;
        return js_dup(dv);
    }
    if (JS_ToFloat64(ctx, &v, argv[0]))
        return JS_EXCEPTION;
    return JS_SetThisTimeValue(ctx, this_val, time_clip(v));
}



static const JSCFunctionListEntry js_date_funcs[] = {
    JS_CFUNC_DEF("now", 0, js_Date_now ),
    JS_CFUNC_STEP_DEF("parse", 1, STEPDEF_DATE_PARSE ),
    JS_CFUNC_STEP_DEF("UTC", 7, STEPDEF_DATE_UTC ),
};

static const JSCFunctionListEntry js_date_proto_funcs[] = {
    JS_CFUNC_DEF("valueOf", 0, js_date_getTime ),
    JS_CFUNC_MAGIC_DEF("toString", 0, get_date_string, 0x13 ),
    JS_CFUNC_STEP_DEF("[Symbol.toPrimitive]", 1, STEPDEF_DATE_TOPRIM ),
    JS_CFUNC_MAGIC_DEF("toUTCString", 0, get_date_string, 0x03 ),
    JS_ALIAS_DEF("toGMTString", "toUTCString" ),
    JS_CFUNC_STEP_DEF("toISOString", 0, STEPDEF_DATE_TOISO ),
    JS_CFUNC_MAGIC_DEF("toDateString", 0, get_date_string, 0x11 ),
    JS_CFUNC_MAGIC_DEF("toTimeString", 0, get_date_string, 0x12 ),
    JS_CFUNC_MAGIC_DEF("toLocaleString", 0, get_date_string, 0x33 ),
    JS_CFUNC_MAGIC_DEF("toLocaleDateString", 0, get_date_string, 0x31 ),
    JS_CFUNC_MAGIC_DEF("toLocaleTimeString", 0, get_date_string, 0x32 ),
    JS_CFUNC_DEF("getTimezoneOffset", 0, js_date_getTimezoneOffset ),
    JS_CFUNC_DEF("getTime", 0, js_date_getTime ),
    JS_CFUNC_MAGIC_DEF("getYear", 0, get_date_field, 0x101 ),
    JS_CFUNC_MAGIC_DEF("getFullYear", 0, get_date_field, 0x01 ),
    JS_CFUNC_MAGIC_DEF("getUTCFullYear", 0, get_date_field, 0x00 ),
    JS_CFUNC_MAGIC_DEF("getMonth", 0, get_date_field, 0x11 ),
    JS_CFUNC_MAGIC_DEF("getUTCMonth", 0, get_date_field, 0x10 ),
    JS_CFUNC_MAGIC_DEF("getDate", 0, get_date_field, 0x21 ),
    JS_CFUNC_MAGIC_DEF("getUTCDate", 0, get_date_field, 0x20 ),
    JS_CFUNC_MAGIC_DEF("getHours", 0, get_date_field, 0x31 ),
    JS_CFUNC_MAGIC_DEF("getUTCHours", 0, get_date_field, 0x30 ),
    JS_CFUNC_MAGIC_DEF("getMinutes", 0, get_date_field, 0x41 ),
    JS_CFUNC_MAGIC_DEF("getUTCMinutes", 0, get_date_field, 0x40 ),
    JS_CFUNC_MAGIC_DEF("getSeconds", 0, get_date_field, 0x51 ),
    JS_CFUNC_MAGIC_DEF("getUTCSeconds", 0, get_date_field, 0x50 ),
    JS_CFUNC_MAGIC_DEF("getMilliseconds", 0, get_date_field, 0x61 ),
    JS_CFUNC_MAGIC_DEF("getUTCMilliseconds", 0, get_date_field, 0x60 ),
    JS_CFUNC_MAGIC_DEF("getDay", 0, get_date_field, 0x71 ),
    JS_CFUNC_MAGIC_DEF("getUTCDay", 0, get_date_field, 0x70 ),
    JS_CFUNC_STEP_DEF("setTime", 1, STEPDEF_DATE_SETTIME ),
    JS_CFUNC_STEP_DEF("setMilliseconds", 1, STEPDEF_DATE_SET_MILLISECONDS ),
    JS_CFUNC_STEP_DEF("setUTCMilliseconds", 1, STEPDEF_DATE_SET_UTC_MILLISECONDS ),
    JS_CFUNC_STEP_DEF("setSeconds", 2, STEPDEF_DATE_SET_SECONDS ),
    JS_CFUNC_STEP_DEF("setUTCSeconds", 2, STEPDEF_DATE_SET_UTC_SECONDS ),
    JS_CFUNC_STEP_DEF("setMinutes", 3, STEPDEF_DATE_SET_MINUTES ),
    JS_CFUNC_STEP_DEF("setUTCMinutes", 3, STEPDEF_DATE_SET_UTC_MINUTES ),
    JS_CFUNC_STEP_DEF("setHours", 4, STEPDEF_DATE_SET_HOURS ),
    JS_CFUNC_STEP_DEF("setUTCHours", 4, STEPDEF_DATE_SET_UTC_HOURS ),
    JS_CFUNC_STEP_DEF("setDate", 1, STEPDEF_DATE_SET_DATE ),
    JS_CFUNC_STEP_DEF("setUTCDate", 1, STEPDEF_DATE_SET_UTC_DATE ),
    JS_CFUNC_STEP_DEF("setMonth", 2, STEPDEF_DATE_SET_MONTH ),
    JS_CFUNC_STEP_DEF("setUTCMonth", 2, STEPDEF_DATE_SET_UTC_MONTH ),
    JS_CFUNC_STEP_DEF("setYear", 1, STEPDEF_DATE_SETYEAR ),
    JS_CFUNC_STEP_DEF("setFullYear", 3, STEPDEF_DATE_SET_FULLYEAR ),
    JS_CFUNC_STEP_DEF("setUTCFullYear", 3, STEPDEF_DATE_SET_UTC_FULLYEAR ),
    JS_CFUNC_STEP_DEF("toJSON", 1, STEPDEF_DATE_TOJSON ),
};

JSValue JS_NewDate(JSContext *ctx, double epoch_ms)
{
    JSValue obj = js_create_from_intrinsic(ctx, JS_CLASS_DATE);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    JS_SetObjectData(ctx, obj, js_float64(time_clip(epoch_ms)));
    return obj;
}

bool JS_IsDate(JSValueConst v)
{
    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT)
        return false;
    return JS_VALUE_GET_OBJ(v)->class_id == JS_CLASS_DATE;
}

double JS_GetDateValue(JSValueConst v)
{
    JSObject *p;

    DCHECK(JS_IsDate(v), "a [[DateValue]] was read off something that is not a Date — JS_IsDate is how a "
                         "caller finds that out, and an object of another class has no such slot");
    p = JS_VALUE_GET_OBJ(v);
    if (js_value_is_concolic(p->u.object_data))
        DFAIL("a host component read the [[DateValue]] of a Date whose time value is unknown input (setTime over "
              "one) — derive that component's answer from the unknown instead of reading a number");
    DCHECK(JS_IsNumber(p->u.object_data), "a Date's [[DateValue]] is not a number — the slot is set to a "
                                          "time-clipped double at construction or to an unknown by setTime");
    /* An integral time value is stored with the INT tag by the float constructor's own normalisation, so both
       tags are the one slot and reading only the float half would answer garbage for `new Date(0)`. */
    return JS_VALUE_GET_TAG(p->u.object_data) == JS_TAG_INT ? (double)JS_VALUE_GET_INT(p->u.object_data)
                                                            : JS_VALUE_GET_FLOAT64(p->u.object_data);
}

int JS_AddIntrinsicDate(JSContext *ctx)
{
    JSValue obj;

    obj = JS_NewCConstructor(ctx, JS_CLASS_DATE, "Date",
                             NULL, 7, JS_CFUNC_step_ctor, STEPDEF_DATE_CTOR,
                             JS_UNDEFINED,
                             js_date_funcs, countof(js_date_funcs),
                             js_date_proto_funcs, countof(js_date_proto_funcs),
                             0);
    if (JS_IsException(obj))
        return -1;
    JS_FreeValue(ctx, obj);
    return 0;
}

