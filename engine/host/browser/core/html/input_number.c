/* A CONTROL'S VALUE AS A NUMBER — HTML §4.10.5.1's per-state conversions and the §4.10.5.3.7/§4.10.5.3.8 domain
 * they define. See input_number.h for why this is one component and why the grammar is not in it.
 *
 * THE WINDOW. Four of these states count MILLISECONDS FROM AN EPOCH, and the instants they can name are the
 * instants a Date can name: ECMAScript §21.4.1.1's TIME VALUE range, ±8.64e15 milliseconds — ±100,000,000 days
 * — which ends inside the year 275760. That window is checked in BOTH directions and it is not a policy: a
 * §2.3.5 year is "four or more ASCII digits" with no upper bound at all, so `999999999999-01-01` parses, and
 * the day count it asks for overflows the int64 era arithmetic in date_time_microsyntax.c long before it
 * produces a wrong answer. Outside the window the conversion has no result, which is the same answer it gives
 * for a year before 1 — and the caller's own step says what to do with a conversion that returned nothing. */
#include <math.h>
#include <string.h>

#include "check.h"
#include "core/html/date_time_microsyntax.h"
#include "core/html/input_number.h"
#include "core/html/number_microsyntax.h"

#define MS_PER_DAY      86400000.0
#define TIME_VALUE_MAX  8.64e15
/* The last year the window reaches into. Checked BEFORE a year reaches the calendar, because the day count is
   an int64 and §2.3.5's unbounded year would overflow the era arithmetic rather than answer out of range. */
#define MAX_YEAR        275760

/* ---- the facts each state states about itself ---------------------------------------------------------------
 *
 * §4.10.5.1.7 through §4.10.5.1.13 each state a STEP SCALE FACTOR and a DEFAULT STEP in one sentence, and three
 * of them state one more thing — the Week state's default step base, the Range state's default minimum and
 * maximum, the Time state's periodic domain. They are collected here, once, because §4.10.5.3.8 reads them
 * generically ("the default step multiplied by the step scale factor") and a per-state branch inside that
 * algorithm would be the same table written along the other axis. */
typedef struct {
    double scale;         /* §4.10.5.3.8's STEP SCALE FACTOR */
    double dflt_step;     /* §4.10.5.3.8's DEFAULT STEP, in the state's own unit */
    bool   has_dflt_base; /* §4.10.5.3.8's DEFAULT STEP BASE — the Week state's, and no other's */
    double dflt_base;
    bool   has_dflt_min, has_dflt_max;   /* §4.10.5.3.7's defaults — the Range state's, and no other's */
    double dflt_min, dflt_max;
    bool   periodic;      /* §4.10.5.3.7's PERIODIC DOMAIN — the Time state's, and no other's */
} InputNumberFacts;

static bool input_number_facts(HtmlInputState st, InputNumberFacts *f)
{
    memset(f, 0, sizeof *f);
    f->dflt_step = 1;
    f->scale = 1;
    switch (st) {
    case INPUT_STATE_DATE:
        /* "The step attribute is expressed in days. The step scale factor is 86,400,000 (which converts the
           days to milliseconds, as used in the other algorithms). The default step is 1 day." */
        f->scale = 86400000.0;
        return true;
    case INPUT_STATE_MONTH:
        /* "The step scale factor is 1 (there is no conversion needed as the algorithms use months). The default
           step is 1 month." */
        return true;
    case INPUT_STATE_WEEK:
        /* "The step scale factor is 604,800,000 ... The default step is 1 week. The default step base is
           −259,200,000 (the start of week 1970-W01)." */
        f->scale = 604800000.0;
        f->has_dflt_base = true;
        f->dflt_base = -259200000.0;
        return true;
    case INPUT_STATE_TIME:
        /* "The step attribute is expressed in seconds. The step scale factor is 1000 ... The default step is 60
           seconds." And "the form control has a periodic domain" — the one state that does. */
        f->scale = 1000.0;
        f->dflt_step = 60;
        f->periodic = true;
        return true;
    case INPUT_STATE_DATETIME_LOCAL:
        /* The same two sentences as the Time state's, and NOT its periodic domain: a local date and time runs
           along a line, so a maximum below the minimum is the author error §4.10.5.3.7 says it is. */
        f->scale = 1000.0;
        f->dflt_step = 60;
        return true;
    case INPUT_STATE_NUMBER:
        /* "The step scale factor is 1. The default step is 1 (allowing only integers ...)." */
        return true;
    case INPUT_STATE_RANGE:
        /* The same, plus §4.10.5.1.13's "the default minimum is 0" and "the default maximum is 100" — the two
           numbers its value sanitization algorithm's default value is also made of. */
        f->has_dflt_min = f->has_dflt_max = true;
        f->dflt_max = 100;
        return true;
    default:
        return false;
    }
}

bool input_number_applies(HtmlInputState st)
{
    InputNumberFacts f;

    return input_number_facts(st, &f);
}

bool input_number_date_applies(HtmlInputState st)
{
    /* The four states that define a "convert a string to a Date object". Written as its own list rather than as
       "applies minus datetime-local, Number and Range", because that is how the standard states it: each
       state's own section either defines the algorithm or does not, and the Local Date and Time state says so
       twice (it defines neither Date algorithm, and it names valueAsDate among the members that do not apply). */
    return st == INPUT_STATE_DATE || st == INPUT_STATE_MONTH || st == INPUT_STATE_WEEK ||
           st == INPUT_STATE_TIME;
}

/* ---- the arithmetic the four epoch-counting states are made of ----------------------------------------------- */

/* THE DAY an instant falls in, and the milliseconds into that day — one operation because every state below
   needs both, and a floor taken twice is two floors that disagree at a negative instant. False when the instant
   is outside the window. */
static bool num_split_day(double ms, int64_t *pdays, double *pms_of_day)
{
    double days;

    if (!(ms >= -TIME_VALUE_MAX && ms <= TIME_VALUE_MAX)) return false;
    days = floor(ms / MS_PER_DAY);
    *pdays = (int64_t)days;
    *pms_of_day = ms - days * MS_PER_DAY;
    DCHECK(*pms_of_day >= 0 && *pms_of_day < MS_PER_DAY,
           "an instant's milliseconds-into-the-day left [0, 86400000) — the day is FLOORED, so the remainder is "
           "non-negative on both sides of the epoch");
    return true;
}

/* The instant of midnight UTC on the morning of a date, or false when the date is outside the window. */
static bool num_midnight_of(const HtmlDate *d, double *out)
{
    double ms;

    if (d->year <= 0 || d->year > MAX_YEAR) return false;
    ms = (double)html_date_time_days_from_epoch(d->year, d->month, d->day) * MS_PER_DAY;
    if (!(ms >= -TIME_VALUE_MAX && ms <= TIME_VALUE_MAX)) return false;
    *out = ms;
    return true;
}

/* §2.3.5.4's components of a time given as milliseconds into a day. False when the count carries a FRACTION OF
   A MILLISECOND, which no valid time string can spell — its fractional part is one, two or three digits. */
static bool num_time_of_day(double ms_of_day, HtmlTime *out)
{
    int ms;

    DCHECK(ms_of_day >= 0 && ms_of_day < MS_PER_DAY, "a time-of-day was built from a count outside the day");
    if (floor(ms_of_day) != ms_of_day) return false;
    ms = (int)ms_of_day;
    out->hour = ms / 3600000;
    ms -= out->hour * 3600000;
    out->minute = ms / 60000;
    ms -= out->minute * 60000;
    out->second = (double)ms / 1000.0;
    out->fraction_digits = ms % 1000 ? 3 : 0;
    DCHECK(out->second >= 0.0 && out->second < 60.0, "a time-of-day's seconds left §2.3.5.4's [0, 60) range");
    return true;
}

/* The milliseconds from midnight to a parsed time — §4.10.5.1.10's own conversion, and the time half of
   §4.10.5.1.11's. The seconds component CARRIES ITS FRACTION (see HtmlTime), so this is one multiplication and
   not a second decimal walk. */
static double num_ms_of_time(const HtmlTime *t)
{
    return t->hour * 3600000.0 + t->minute * 60000.0 + t->second * 1000.0;
}

/* ---- §4.10.5.1's ALGORITHM TO CONVERT A STRING TO A NUMBER --------------------------------------------------- */

bool input_number_from_string(HtmlInputState st, const char *s, size_t len, double *out)
{
    DCHECK(out != NULL, "a conversion was asked for with nowhere to put its number");
    switch (st) {
    case INPUT_STATE_NUMBER:
    case INPUT_STATE_RANGE:
        /* §4.10.5.1.12 and §4.10.5.1.13: "The algorithm to convert a string to a number, given a string input,
           is as follows: If applying the rules for parsing floating-point number values to input results in an
           error, then return an error; otherwise, return the resulting number." */
        return html_parse_floating_point(s, len, out, NULL);
    case INPUT_STATE_DATE: {
        /* §4.10.5.1.7: "If parsing a date from input results in an error, then return an error; otherwise,
           return the number of milliseconds elapsed from midnight UTC on the morning of 1970-01-01 ... to
           midnight UTC on the morning of the parsed date, ignoring leap seconds." */
        HtmlDate d;

        return html_parse_date_string(s, len, &d) && num_midnight_of(&d, out);
    }
    case INPUT_STATE_MONTH: {
        /* §4.10.5.1.8: "... otherwise, return the number of months between January 1970 and the parsed month."
           The window is applied to the month's FIRST DAY, which is the instant the same state's Date algorithm
           names for it — one window for one state, rather than a second bound invented for a month count. */
        HtmlMonth m;
        HtmlDate first;
        double unused;

        if (!html_parse_month_string(s, len, &m)) return false;
        first.year = m.year;
        first.month = m.month;
        first.day = 1;
        if (!num_midnight_of(&first, &unused)) return false;
        *out = (double)((m.year - 1970) * 12 + (m.month - 1));
        return true;
    }
    case INPUT_STATE_WEEK: {
        /* §4.10.5.1.9: "... otherwise, return the number of milliseconds elapsed from midnight UTC on the
           morning of 1970-01-01 ... to midnight UTC on the morning of the Monday of the parsed week, ignoring
           leap seconds." */
        HtmlWeek w;
        double ms;

        if (!html_parse_week_string(s, len, &w)) return false;
        if (w.year > MAX_YEAR) return false;
        ms = (double)html_date_time_week_start_days(w.year, w.week) * MS_PER_DAY;
        if (!(ms >= -TIME_VALUE_MAX && ms <= TIME_VALUE_MAX)) return false;
        *out = ms;
        return true;
    }
    case INPUT_STATE_TIME: {
        /* §4.10.5.1.10: "... otherwise, return the number of milliseconds elapsed from midnight to the parsed
           time on a day with no time changes." */
        HtmlTime t;

        if (!html_parse_time_string(s, len, &t)) return false;
        *out = num_ms_of_time(&t);
        DCHECK(*out >= 0 && *out < MS_PER_DAY,
               "a parsed time's milliseconds from midnight left the day — the parse bounds the hour, the minute "
               "and the second, so a count outside it is arithmetic and not input");
        return true;
    }
    case INPUT_STATE_DATETIME_LOCAL: {
        /* §4.10.5.1.11: "If parsing a date and time from input results in an error, then return an error;
           otherwise, return the number of milliseconds elapsed from midnight on the morning of 1970-01-01 ...
           to the parsed local date and time, ignoring leap seconds." No time zone appears anywhere in that
           sentence, which is why this is the same arithmetic as the Date state's plus the time of day. */
        HtmlDateTime dt;
        double midnight, ms;

        if (!html_parse_local_date_and_time_string(s, len, &dt)) return false;
        if (!num_midnight_of(&dt.date, &midnight)) return false;
        ms = midnight + num_ms_of_time(&dt.time);
        if (!(ms >= -TIME_VALUE_MAX && ms <= TIME_VALUE_MAX)) return false;
        *out = ms;
        return true;
    }
    default:
        DFAIL("§4.10.5.1's convert a string to a number was asked of a state that defines none — "
              "input_number_applies is that list and every caller has it in hand");
        return false;
    }
}

/* ---- §4.10.5.1's ALGORITHM TO CONVERT A NUMBER TO A STRING --------------------------------------------------- */

JSValue input_number_to_string(JSContext *ctx, HtmlInputState st, double n)
{
    char buf[HTML_NORMALIZED_LOCAL_DATE_AND_TIME_CAP];
    int64_t days;
    double ms_of_day;
    size_t written;

    switch (st) {
    case INPUT_STATE_NUMBER:
    case INPUT_STATE_RANGE:
        /* §4.10.5.1.12 and §4.10.5.1.13: "Return a valid floating-point number that represents this number." —
           §2.3.4.3's best representation, which is ToString and never a printf format. */
        return html_best_representation_of_number(ctx, n);
    case INPUT_STATE_DATE: {
        /* §4.10.5.1.7: "Return a valid date string that represents the date that, in UTC, is current input
           milliseconds after midnight UTC on the morning of 1970-01-01." CURRENT AT that instant, so an instant
           part-way through a day names that day and the milliseconds into it are dropped. */
        HtmlDate d;

        if (!num_split_day(n, &days, &ms_of_day)) return JS_UNDEFINED;
        html_date_time_date_from_days(days, &d);
        if (d.year <= 0) return JS_UNDEFINED;
        written = html_serialize_date(&d, buf, sizeof buf);
        return JS_NewStringLen(ctx, buf, written);
    }
    case INPUT_STATE_MONTH: {
        /* §4.10.5.1.8: "Return a valid month string that represents the month that has input months between it
           and January 1970." A count that is not a whole number of months names no month, which is the one
           thing this differs from the three "current at an instant" algorithms by. */
        HtmlMonth m;
        int64_t idx, y;
        double first;
        HtmlDate first_day;

        if (floor(n) != n) return JS_UNDEFINED;
        if (!(n >= -12.0 * MAX_YEAR && n <= 12.0 * MAX_YEAR)) return JS_UNDEFINED;
        idx = (int64_t)n;
        /* FLOOR division into a year and a month, spelled out: C truncates toward zero, and a month count
           before January 1970 rounded the wrong way lands in the following year. */
        y = 1970 + (idx >= 0 ? idx / 12 : -(((-idx) + 11) / 12));
        m.year = y;
        m.month = (int)(idx - (y - 1970) * 12) + 1;
        DCHECK(m.month >= 1 && m.month <= 12,
               "§4.10.5.1.8's month count split into a month outside 1 ≤ month ≤ 12 — the year is the floor of "
               "the count divided by twelve, so the remainder is one of twelve");
        if (m.year <= 0) return JS_UNDEFINED;
        first_day.year = m.year;
        first_day.month = m.month;
        first_day.day = 1;
        if (!num_midnight_of(&first_day, &first)) return JS_UNDEFINED;
        written = html_serialize_month(&m, buf, sizeof buf);
        return JS_NewStringLen(ctx, buf, written);
    }
    case INPUT_STATE_WEEK: {
        /* §4.10.5.1.9: "Return a valid week string that represents the week that, in UTC, is current input
           milliseconds after midnight UTC on the morning of 1970-01-01." */
        HtmlWeek w;

        if (!num_split_day(n, &days, &ms_of_day)) return JS_UNDEFINED;
        if (!html_date_time_week_of_days(days, &w)) return JS_UNDEFINED;
        written = html_serialize_week(&w, buf, sizeof buf);
        return JS_NewStringLen(ctx, buf, written);
    }
    case INPUT_STATE_TIME: {
        /* §4.10.5.1.10: "Return a valid time string that represents the time that is input milliseconds after
           midnight on a day with no time changes." A count outside the day is past midnight, where there is no
           time on THIS day to name — the periodic domain §4.10.5.3.7 gives the state is a range within one day,
           and a number outside it is outside the state's own domain. */
        HtmlTime t;

        if (!(n >= 0 && n < MS_PER_DAY)) return JS_UNDEFINED;
        if (!num_time_of_day(n, &t)) return JS_UNDEFINED;
        written = html_serialize_time(&t, buf, sizeof buf);
        return JS_NewStringLen(ctx, buf, written);
    }
    case INPUT_STATE_DATETIME_LOCAL: {
        /* §4.10.5.1.11: "Return a valid NORMALIZED local date and time string that represents the date and time
           that is input milliseconds after midnight on the morning of 1970-01-01." */
        HtmlDateTime dt;

        if (!num_split_day(n, &days, &ms_of_day)) return JS_UNDEFINED;
        html_date_time_date_from_days(days, &dt.date);
        if (dt.date.year <= 0) return JS_UNDEFINED;
        if (!num_time_of_day(ms_of_day, &dt.time)) return JS_UNDEFINED;
        written = html_serialize_normalized_local_date_and_time(&dt, buf, sizeof buf);
        return JS_NewStringLen(ctx, buf, written);
    }
    default:
        DFAIL("§4.10.5.1's convert a number to a string was asked of a state that defines none — "
              "input_number_applies is that list and every caller has it in hand");
        return JS_UNDEFINED;
    }
}

/* ---- §4.10.5.1's TWO Date OBJECT ALGORITHMS ------------------------------------------------------------------
 *
 * They are NOT the two above with a different spelling, and the Month and Time states are why: the Month
 * state's number is a count of months while its Date is midnight on the first day of that month, and the Time
 * state's Date carries the date 1970-01-01 which the conversion back drops on the floor ("the UTC TIME
 * COMPONENT that is represented by input"). Where a state's two algorithms genuinely are the same instant —
 * Date and Week — this says so by calling the one above rather than by restating it. */

bool input_number_date_from_string(HtmlInputState st, const char *s, size_t len, double *ptime)
{
    switch (st) {
    case INPUT_STATE_DATE:
        /* §4.10.5.1.7: "... otherwise, return a new Date object representing midnight UTC on the morning of the
           parsed date" — the instant its number algorithm already counts. */
    case INPUT_STATE_WEEK:
        /* §4.10.5.1.9: "... midnight UTC on the morning of the Monday of the parsed week" — likewise. */
    case INPUT_STATE_TIME:
        /* §4.10.5.1.10: "... the parsed time in UTC on 1970-01-01" — which is the milliseconds from midnight
           its number algorithm counts, because that morning IS the epoch. */
        return input_number_from_string(st, s, len, ptime);
    case INPUT_STATE_MONTH: {
        /* §4.10.5.1.8: "... midnight UTC on the morning of the FIRST DAY of the parsed month" — an instant, not
           the month count its number algorithm answers with. */
        HtmlMonth m;
        HtmlDate first;

        if (!html_parse_month_string(s, len, &m)) return false;
        first.year = m.year;
        first.month = m.month;
        first.day = 1;
        return num_midnight_of(&first, ptime);
    }
    default:
        DFAIL("§4.10.5.1's convert a string to a Date object was asked of a state that defines none — "
              "input_number_date_applies is that list and every caller has it in hand");
        return false;
    }
}

JSValue input_number_date_to_string(JSContext *ctx, HtmlInputState st, double time_value)
{
    char buf[HTML_TIME_CAP];
    int64_t days;
    double ms_of_day;

    /* An INVALID Date has no date and no time current at it. The caller reaches this only for a Date object,
       and §4.10.5.4's setter has already sent the NaN time value down its own branch — this is the arithmetic's
       own guard, for the instant outside the window that no algorithm below could name either. */
    if (!(time_value >= -TIME_VALUE_MAX && time_value <= TIME_VALUE_MAX)) return JS_UNDEFINED;
    switch (st) {
    case INPUT_STATE_DATE:
        /* §4.10.5.1.7: "Return a valid date string that represents the date current at the time represented by
           input in the UTC time zone" — the same date its number algorithm names for that instant. */
    case INPUT_STATE_WEEK:
        /* §4.10.5.1.9: "... the week current at the time represented by input in the UTC time zone." */
        return input_number_to_string(ctx, st, time_value);
    case INPUT_STATE_MONTH: {
        /* §4.10.5.1.8: "... the month current at the time represented by input in the UTC time zone" — the
           month of that instant's DATE, not the month count its number algorithm takes. */
        HtmlDate d;
        HtmlMonth m;
        char mbuf[HTML_MONTH_CAP];
        size_t written;

        if (!num_split_day(time_value, &days, &ms_of_day)) return JS_UNDEFINED;
        html_date_time_date_from_days(days, &d);
        if (d.year <= 0) return JS_UNDEFINED;
        m.year = d.year;
        m.month = d.month;
        written = html_serialize_month(&m, mbuf, sizeof mbuf);
        return JS_NewStringLen(ctx, mbuf, written);
    }
    case INPUT_STATE_TIME: {
        /* §4.10.5.1.10: "Return a valid time string that represents the UTC TIME COMPONENT that is represented
           by input" — the time of day at that instant, with the date discarded. A Date on any day answers, and
           an instant before the epoch answers from the floored day like every other. */
        HtmlTime t;
        size_t written;

        if (!num_split_day(time_value, &days, &ms_of_day)) return JS_UNDEFINED;
        if (!num_time_of_day(ms_of_day, &t)) return JS_UNDEFINED;
        written = html_serialize_time(&t, buf, sizeof buf);
        return JS_NewStringLen(ctx, buf, written);
    }
    default:
        DFAIL("§4.10.5.1's convert a Date object to a string was asked of a state that defines none — "
              "input_number_date_applies is that list and every caller has it in hand");
        return JS_UNDEFINED;
    }
}

/* ---- §4.10.5.3.7's MINIMUM and MAXIMUM, §4.10.5.3.8's ALLOWED VALUE STEP and STEP BASE ------------------------ */

/* An attribute's value, converted by THIS STATE's algorithm — which is what §4.10.5.3.7 and §4.10.5.3.8 say
   `min`, `max` and the `value` content attribute are read by, and is the whole reason a date control's
   `min="2015-06-06"` is a bound and not an error. */
static bool num_attr_value(lxb_dom_element_t *el, const char *name, HtmlInputState st, double *out)
{
    size_t len = 0;
    const char *v = (const char *)lxb_dom_element_get_attribute(el, (const lxb_char_t *)name, strlen(name), &len);

    return v != NULL && input_number_from_string(st, v, len, out);
}

void input_step_range_of(lxb_dom_element_t *el, HtmlInputState st, InputStepRange *r)
{
    InputNumberFacts f;
    size_t len = 0;
    const char *a;
    double d;

    memset(r, 0, sizeof *r);
    DCHECK(el != NULL, "§4.10.5.3.7's range was asked for with no element");
    DCHECK(input_number_applies(st),
           "§4.10.5.3.7's minimum and maximum were asked of a state with no convert a string to a number — the "
           "two attributes are stated over that algorithm, so a state without one has no range at all");
    if (!input_number_facts(st, &f)) return;
    DCHECK(f.scale > 0 && f.dflt_step > 0,
           "a state declared a step scale factor or a default step that is not positive — §4.10.5.3.8 states "
           "both as counts, and a non-positive one makes every value a step mismatch");

    /* §4.10.5.3.7: "If the element has a min attribute, and the result of applying the algorithm to convert a
       string to a number to the value of the min attribute is a number, then that number is the element's
       minimum; otherwise, if the type attribute's current state defines a default minimum, then that is the
       minimum; otherwise, the element has no minimum." And the same sentence again for the maximum. */
    if (num_attr_value(el, "min", st, &d)) { r->has_min = true; r->min = d; }
    else if (f.has_dflt_min) { r->has_min = true; r->min = f.dflt_min; }
    if (num_attr_value(el, "max", st, &d)) { r->has_max = true; r->max = d; }
    else if (f.has_dflt_max) { r->has_max = true; r->max = f.dflt_max; }
    /* "An element has a REVERSED RANGE if it has a periodic domain and its maximum is less than its minimum."
       Only the Time state has one, so for every other state a maximum below the minimum stays what §4.10.5.3.7
       says it is: an author error under which the element is always suffering from an underflow or an
       overflow, which falls out of the two ordinary comparisons with no case of its own. */
    r->reversed = f.periodic && r->has_min && r->has_max && r->max < r->min;

    /* §4.10.5.3.8's ALLOWED VALUE STEP. The `step` attribute is read by the RULES FOR PARSING FLOATING-POINT
       NUMBER VALUES in every state — it is a count of the state's own unit and never that state's syntax, which
       is why `step=3600` on a time control is an hour and not the string "3600" parsed as a time. */
    a = (const char *)lxb_dom_element_get_attribute(el, (const lxb_char_t *)"step", 4, &len);
    if (a && len == 3 && (a[0] == 'a' || a[0] == 'A') && (a[1] == 'n' || a[1] == 'N') &&
        (a[2] == 'y' || a[2] == 'Y')) {
        /* "Otherwise, if the attribute's value is an ASCII case-insensitive match for the string 'any', then
           there is no allowed value step." The one spelling that removes the constraint. */
        r->has_step = false;
    } else {
        /* "Otherwise, if the attribute is absent, the allowed value step is the default step multiplied by the
           step scale factor" — so an ABSENT step is a constraint and not the absence of one — and "otherwise,
           if the rules ... return an error, zero, or a number less than zero, then the allowed value step is
           the default step multiplied by the step scale factor". */
        r->has_step = true;
        r->step = ((a && html_parse_floating_point(a, len, &d, NULL) && d > 0) ? d : f.dflt_step) * f.scale;
        DCHECK(r->step > 0, "§4.10.5.3.8's allowed value step came out non-positive — every branch above ends "
                            "in a positive count multiplied by a positive scale factor");
    }

    /* §4.10.5.3.8's STEP BASE: the `min` content attribute's number, else the `value` CONTENT attribute's, else
       the state's default step base, else zero. The value CONTENT attribute and not the element's value — the
       default the markup carries is what the author's steps are counted from, which is why
       `<input type=number value=1 step=2>` accepts 3 and not 2. */
    if (num_attr_value(el, "min", st, &d)) r->base = d;
    else if (num_attr_value(el, "value", st, &d)) r->base = d;
    else if (f.has_dflt_base) r->base = f.dflt_base;
    else r->base = 0;
}

bool input_step_mismatch(double value, const InputStepRange *r)
{
    double diff = fabs(value - r->base), rem, tol;

    if (!r->has_step) return false;
    DCHECK(r->step > 0, "a step mismatch was asked about an allowed value step that is not positive");
    /* "That number subtracted from the step base is not an INTEGRAL MULTIPLE of the allowed value step", in
       exact arithmetic — which is what the standard writes and what a double's remainder is not: 0.07 IS an
       integral multiple of 0.01 and fmod says otherwise, because neither numeral is exactly representable. The
       tolerance recovers the exact-math answer rather than relaxing it; it is scaled by the step so that a step
       of 1e-9 is still decided at its own magnitude. */
    rem = fmod(diff, r->step);
    tol = r->step / 16777216.0;   /* the float mantissa's 2^24, the scale a double's accumulated error lives at */
    return rem > tol && (r->step - rem) > tol;
}

/* THE ALIGNED VALUE ON ONE SIDE OF A NUMBER, over the same remainder the mismatch above is decided by — and
   with the same tolerance, because two answers to "is this an integral multiple" that disagree would step a
   value the constraint then calls a mismatch. It is the remainder and never a division: fmod is exact, and
   `(v - base) / step` is not once the quotient is large enough to matter. */
static double num_align(double v, const InputStepRange *r, bool up)
{
    double off, rem, tol;

    DCHECK(r->has_step && r->step > 0, "an aligned value was asked for with no allowed value step — `step=any` "
                                       "removes the constraint, and §4.10.5.4's stepUp() throws before this");
    off = v - r->base;
    rem = fmod(off, r->step);   /* in (−step, step), with the sign of off */
    tol = r->step / 16777216.0;
    if (fabs(rem) <= tol) return v - rem;                                   /* already a multiple */
    if (r->step - fabs(rem) <= tol) return v - rem + (rem > 0 ? r->step : -r->step);   /* the next one, exactly */
    if (up) return rem > 0 ? v - rem + r->step : v - rem;
    return rem > 0 ? v - rem : v - rem - r->step;
}

double input_step_align_up(double v, const InputStepRange *r)
{
    double aligned = num_align(v, r, true);

    DCHECK(!isfinite(aligned) || aligned >= v - r->step / 16777216.0,
           "the smallest aligned value not below a number came out below it");
    return aligned;
}

double input_step_align_down(double v, const InputStepRange *r)
{
    double aligned = num_align(v, r, false);

    DCHECK(!isfinite(aligned) || aligned <= v + r->step / 16777216.0,
           "the largest aligned value not above a number came out above it");
    return aligned;
}
