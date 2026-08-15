/* DATES AND TIMES — HTML §2.3.5's date/time microsyntax. See date_time_microsyntax.h for why this is its own
 * component and why the productions are answered by the parse rather than by a second walk.
 *
 * THE RULES ARE TRANSCRIBED, NOT APPROXIMATED. §2.3.5 says of itself that "this specification defines parsing
 * rules in much more detail than ISO8601. Implementers are therefore encouraged to carefully examine any date
 * parsing libraries before using them", and every step below is one of its numbered steps in its order: the
 * digit runs are §2.3.1's COLLECT A SEQUENCE OF CODE POINTS with their exact required lengths (four OR MORE for
 * a year, EXACTLY TWO for a month, a day, an hour, a minute and a week), the range checks are the standard's,
 * and the failures are its "fails" — aborted at that point, returning nothing. Anything looser accepts a string
 * a browser rejects, and since a control's value is what §4.10.22.4 submits, that is a request this tool would
 * report and a browser would never send. */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/html/date_time_microsyntax.h"

/* ---- §2.3.5's CALENDAR ARITHMETIC ---------------------------------------------------------------------------- */

bool html_date_time_is_leap_year(int64_t year)
{
    /* "29 if month is 2 and year is a number divisible by 400, or if year is a number divisible by 4 but not by
       100" — the two clauses are the standard's own, and stating them as written is why this is one function. */
    return year % 400 == 0 || (year % 4 == 0 && year % 100 != 0);
}

int html_date_time_days_in_month(int64_t year, int month)
{
    static const int LEN[13] = { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

    DCHECK(month >= 1 && month <= 12,
           "§2.3.5's number of days in a month was asked about a month outside 1 ≤ month ≤ 12 — every caller "
           "has already run the range check that is a step of the rule it is in the middle of");
    return month == 2 && html_date_time_is_leap_year(year) ? 29 : LEN[month];
}

int html_date_time_jan1_weekday(int64_t year)
{
    /* THE PROLEPTIC-GREGORIAN WEEKDAY OF JANUARY 1ST, COUNTED FROM SUNDAY, DERIVED FROM THE DAY COUNT — one
       encoding of the calendar in this component and not two. It was a closed form of its own ("the day
       advances by one per common year and two per leap year since year 1"), which is a second statement of the
       leap rule: correct, and one edit away from disagreeing with html_date_time_is_leap_year at a century
       year, with nothing to say so. 1970-01-01 was a THURSDAY, which is the 4 below.
       THE YEAR IS REDUCED INTO ONE CYCLE FIRST, and that is not an optimisation: 400 Gregorian years are
       exactly 146,097 days and therefore exactly 20,871 weeks, so the weekday depends on the year only through
       its residue — while §2.3.5's year is UNBOUNDED ("four or more ASCII digits") and `2015000000000-W01`
       parses, which the day count's era arithmetic could not hold. The residue is taken into [400, 799], which
       is positive so the count itself stays on the calendar. */
    int64_t cycle = year % 400 + 400;

    DCHECK(year > 0, "§2.3.5.8's week count was asked about a year that is not greater than zero — the year of "
                     "every date and time syntax in §2.3.5 is checked for that before it is used, so a year of "
                     "zero or less means a caller used a value from a parse that failed");
    return (int)(((html_date_time_days_from_epoch(cycle, 1, 1) + 4) % 7 + 7) % 7);
}

/* ---- THE CALENDAR AS A DAY COUNT -------------------------------------------------------------------------------
 *
 * The civil-date/day-number pair, in closed form. It is the proleptic Gregorian calendar §2.3.5 is stated over
 * and nothing else: 400 years are 146,097 days (the leap rule the era arithmetic below encodes is the same
 * "divisible by 400, or by 4 but not by 100" html_date_time_is_leap_year states), and the two directions are
 * inverses that assert each other, so the encoding cannot drift from the one above it without the assert
 * firing on the first date that crosses it.
 *
 * MARCH IS THE FIRST MONTH of the internal year, which is what makes the leap day the LAST day of it and lets
 * the day-of-year be a single expression with no table and no branch on leap-ness. 719,468 is the day number
 * of 1970-01-01 in that scheme, subtracted so day 0 is the epoch. */
int64_t html_date_time_days_from_epoch(int64_t year, int month, int day)
{
    int64_t y, era, yoe, doy, doe;

    DCHECK(month >= 1 && month <= 12,
           "a day count was asked for a month outside 1 ≤ month ≤ 12 — every §2.3.5 parse range-checks the "
           "month before the components leave it, so this is a caller that filled an HtmlDate itself");
    DCHECK(day >= 1 && day <= html_date_time_days_in_month(year, month),
           "a day count was asked for a day outside the month it names — the parse bounds the day by "
           "html_date_time_days_in_month, so a day past it is a date that is not in the calendar");
    DCHECK(year > -(1LL << 40) && year < (1LL << 40),
           "a day count was asked for a year whose era arithmetic would overflow an int64 — §2.3.5's year is "
           "UNBOUNDED ('four or more ASCII digits'), so a caller holding a parsed year either reduces it (the "
           "weekday of January 1st does, modulo the calendar's 400-year cycle, which is all it depends on) or "
           "refuses the year before asking, as §4.10.5.1's conversions do at the platform's time-value range");
    y = year - (month <= 2);
    /* FLOOR division, spelled out: C truncates toward zero, and an era boundary crossed the wrong way puts a
       date in the previous 400-year cycle. */
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;                                               /* [0, 399] */
    doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;    /* [0, 365] */
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                       /* [0, 146096] */
    DCHECK(yoe >= 0 && yoe <= 399 && doy >= 0 && doy <= 365 && doe >= 0 && doe <= 146096,
           "the day count's era arithmetic left its own ranges — the year-of-era, day-of-year and day-of-era "
           "are bounded by construction, so one of them out of range is an overflow or a wrong floor");
    return era * 146097 + doe - 719468;
}

void html_date_time_date_from_days(int64_t days, HtmlDate *out)
{
    int64_t z = days + 719468, era, doe, yoe, y, doy, mp;
    int d, m;

    DCHECK(out != NULL, "a date was asked for with nowhere to put it");
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;                                            /* [0, 146096] */
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;        /* [0, 399] */
    y = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);                      /* [0, 365] */
    mp = (5 * doy + 2) / 153;                                           /* [0, 11] */
    d = (int)(doy - (153 * mp + 2) / 5 + 1);                            /* [1, 31] */
    m = (int)(mp + (mp < 10 ? 3 : -9));                                 /* [1, 12] */
    out->year = y + (m <= 2);
    out->month = m;
    out->day = d;
    /* THE INVERSE, ASSERTED. This is the whole of the checking these two need: a wrong era, a wrong floor or a
       wrong month rotation cannot survive a round trip, and the assert stands at the day it fails on rather
       than at whatever read the wrong date later reaches. */
    DCHECK(html_date_time_days_from_epoch(out->year, out->month, out->day) == days,
           "a day count and the date it names disagree — the two directions of the calendar are inverses and "
           "one of them has an era, a floor or a month rotation wrong");
}

int64_t html_date_time_week_start_days(int64_t week_year, int week)
{
    int jan1 = html_date_time_jan1_weekday(week_year);         /* 0 Sunday .. 6 Saturday */
    int iso = jan1 == 0 ? 7 : jan1;                            /* 1 Monday .. 7 Sunday */
    int64_t jan1_day = html_date_time_days_from_epoch(week_year, 1, 1);
    /* §2.3.5.8's week 1 is the week whose THURSDAY falls in the week-year, which is what the standard's week
       count is stated over ("53 weeks if ... January 1st is a Thursday"). So January 1st belongs to week 1
       when it is a Monday through a Thursday, and to the last week of the PREVIOUS week-year otherwise. */
    int64_t week1 = jan1_day - (iso - 1) + (iso <= 4 ? 0 : 7);

    DCHECK(week >= 1 && week <= html_date_time_weeks_in_week_year(week_year),
           "§2.3.5.8's week start was asked for a week outside the week-year's own count — the parse bounds "
           "the week by html_date_time_weeks_in_week_year, so a week past it is not one this year has");
    DCHECK(((week1 + 4) % 7 + 7) % 7 == 1,
           "§2.3.5.8's week 1 does not start on a Monday — a week is 'a seven-day period starting on a "
           "Monday', so a start that is not one means the January 1st offset above is wrong");
    return week1 + (int64_t)(week - 1) * 7;
}

bool html_date_time_week_of_days(int64_t days, HtmlWeek *out)
{
    /* The MONDAY of the week containing this day. Day 0 is a Thursday, so day −3 is a Monday and the offset
       below is taken from there, floored so it is right on both sides of the epoch. */
    int64_t mon = days - (((days + 3) % 7 + 7) % 7);
    HtmlDate thursday;
    int64_t start;

    DCHECK(out != NULL, "§2.3.5.8's week was asked for with nowhere to put it");
    /* The WEEK-YEAR is the calendar year of the week's Thursday — the same rule week 1 is picked by above. */
    html_date_time_date_from_days(mon + 3, &thursday);
    if (thursday.year <= 0) return false;
    start = html_date_time_week_start_days(thursday.year, 1);
    out->year = thursday.year;
    out->week = (int)((mon - start) / 7 + 1);
    DCHECK(mon >= start && (mon - start) % 7 == 0,
           "a day's Monday is not a whole number of weeks after its week-year's first Monday — both are "
           "Mondays of the same week-year by construction");
    DCHECK(out->week >= 1 && out->week <= html_date_time_weeks_in_week_year(out->year),
           "a day landed in a week its own week-year does not have — the week-year is the year of the week's "
           "Thursday, so every day of it is inside that year's week count");
    return true;
}

int html_date_time_weeks_in_week_year(int64_t year)
{
    int jan1 = html_date_time_jan1_weekday(year);   /* 0 Sunday .. 6 Saturday */

    /* "53 weeks if it corresponds to either a year ... that has a Thursday as its first day (January 1st), or a
       year ... that has a Wednesday as its first day (January 1st) and where year is a number divisible by 400,
       or a number divisible by 4 but not by 100. All other week-years have 52 weeks." */
    if (jan1 == 4) return 53;
    if (jan1 == 3 && html_date_time_is_leap_year(year)) return 53;
    return 52;
}

/* ---- §2.3.1's COLLECT A SEQUENCE OF CODE POINTS ---------------------------------------------------------------
 *
 * Answers how many characters were consumed — which is the length every rule below tests — and the base-ten
 * integer they express, which is the "interpret the resulting sequence as a base-ten integer" that follows every
 * one of those tests. `*povl` reports that the integer does not fit an int64_t; only the YEAR runs can reach it,
 * since every other run is bounded to two characters by its own length test. */
static size_t dtm_collect_digits(const char *s, size_t len, size_t *pos, int64_t *value, bool *povl)
{
    size_t start = *pos, sig = 0;
    uint64_t v = 0;

    *povl = false;
    while (*pos < len && s[*pos] >= '0' && s[*pos] <= '9') {
        if (sig || s[*pos] != '0') sig++;   /* leading zeros express no magnitude */
        /* The largest eighteen-digit integer is below INT64_MAX and the smallest nineteen-digit one may not be,
           so eighteen significant digits always fit and the nineteenth is where the accumulation stops. */
        if (sig <= 18) v = v * 10 + (uint64_t)(s[*pos] - '0');
        else *povl = true;
        (*pos)++;
    }
    *value = (int64_t)v;
    return *pos - start;
}

/* The YEAR of a month component and of a week string: "Collect a sequence of code points that are ASCII digits
   ... If the collected sequence is not at least four characters long, then fail ... If year is not a number
   greater than zero, then fail." One function because the two rules state it identically and a year that parses
   in a `week` and not in a `month` is two grammars. */
static bool dtm_year(const char *s, size_t len, size_t *pos, int64_t *year)
{
    bool ovl;
    size_t n = dtm_collect_digits(s, len, pos, year, &ovl);

    if (n < 4) return false;
    if (ovl)
        DFAIL("a §2.3.5 date or time syntax carries a year of more than eighteen significant digits — the "
              "standard's years are unbounded base-ten integers ('four or more ASCII digits') and this "
              "component holds one in an int64_t. Build the unbounded year: its only uses are §2.3.5's leap "
              "rule (divisible by 400, or by 4 and not by 100) and §2.3.5.8's weekday of January 1st, both of "
              "which depend on the year only through its residue modulo 400, so an exact residue carried "
              "beside the saturated value answers every question this component asks of it — but HtmlDate's "
              "and HtmlWeek's `year` is also read back by §4.10.5.4's valueAsDate and by every serializer, so "
              "the residue alone is not enough and the representation is what has to change");
    return *year > 0;
}

/* "Collect a sequence of code points that are ASCII digits ... If the collected sequence is not exactly two
   characters long, then fail." The month, the day, the hour, the minute and the week are all this. */
static bool dtm_two_digits(const char *s, size_t len, size_t *pos, int *out)
{
    int64_t v;
    bool ovl;

    if (dtm_collect_digits(s, len, pos, &v, &ovl) != 2) return false;
    DCHECK(!ovl, "two ASCII digits overflowed an int64_t — dtm_collect_digits reports overflow only past "
                 "eighteen significant digits and this run is exactly two characters long");
    *out = (int)v;
    return true;
}

/* "If position is beyond the end of input or if the character at position is not a U+XXXX character, then fail.
   Otherwise, move position forwards one character." */
static bool dtm_literal(const char *s, size_t len, size_t *pos, char c)
{
    if (*pos >= len || s[*pos] != c) return false;
    (*pos)++;
    return true;
}

static void dtm_check_entry(const char *s, size_t len, const size_t *pos)
{
    DCHECK(s != NULL || len == 0, "a §2.3.5 parse was given a null input with a nonzero length");
    DCHECK(*pos <= len, "a §2.3.5 parse component was entered with a position past the end of its input — the "
                        "standard's rules advance a position only over characters they have already tested to "
                        "be within the input, so a position past the end is a caller that resumed a rule which "
                        "had failed");
}

/* ---- §2.3.5.1's MONTHS ----------------------------------------------------------------------------------------- */

bool html_parse_month_component(const char *s, size_t len, size_t *pos, HtmlMonth *out)
{
    int64_t year;
    int month;

    dtm_check_entry(s, len, pos);
    if (!dtm_year(s, len, pos, &year)) return false;
    if (!dtm_literal(s, len, pos, '-')) return false;
    if (!dtm_two_digits(s, len, pos, &month)) return false;
    if (month < 1 || month > 12) return false;
    out->year = year;
    out->month = month;
    return true;
}

bool html_parse_month_string(const char *s, size_t len, HtmlMonth *out)
{
    size_t pos = 0;
    HtmlMonth m;

    /* "Parse a month component to obtain year and month. If this returns nothing, then fail. If position is not
       beyond the end of input, then fail." The components go into a local and reach `out` only once BOTH steps
       have passed, so a caller that ignores the answer cannot read a half-parsed month back out of its own
       struct. */
    if (!html_parse_month_component(s, len, &pos, &m) || pos != len) return false;
    *out = m;
    return true;
}

bool html_is_valid_month_string(const char *s, size_t len)
{
    HtmlMonth m;

    /* The production — "four or more ASCII digits, representing year, where year > 0; a U+002D HYPHEN-MINUS
       character; two ASCII digits, representing the month, in the range 1 ≤ month ≤ 12" — is character for
       character what the rules to parse a month string accept and reject, so it is answered by them. */
    return html_parse_month_string(s, len, &m);
}

/* ---- §2.3.5.2's DATES ------------------------------------------------------------------------------------------ */

bool html_parse_date_component(const char *s, size_t len, size_t *pos, HtmlDate *out)
{
    HtmlMonth m;
    int maxday, day;

    dtm_check_entry(s, len, pos);
    if (!html_parse_month_component(s, len, pos, &m)) return false;
    maxday = html_date_time_days_in_month(m.year, m.month);
    if (!dtm_literal(s, len, pos, '-')) return false;
    if (!dtm_two_digits(s, len, pos, &day)) return false;
    if (day < 1 || day > maxday) return false;
    out->year = m.year;
    out->month = m.month;
    out->day = day;
    return true;
}

bool html_parse_date_string(const char *s, size_t len, HtmlDate *out)
{
    size_t pos = 0;
    HtmlDate d;

    if (!html_parse_date_component(s, len, &pos, &d) || pos != len) return false;
    *out = d;
    return true;
}

bool html_is_valid_date_string(const char *s, size_t len)
{
    HtmlDate d;

    /* "A valid month string, representing year and month; a U+002D HYPHEN-MINUS character; two ASCII digits,
       representing day, in the range 1 ≤ day ≤ maxday" — the rules to parse a date string, exactly. */
    return html_parse_date_string(s, len, &d);
}

/* ---- §2.3.5.3's YEARLESS DATES --------------------------------------------------------------------------------- */

bool html_parse_yearless_date_component(const char *s, size_t len, size_t *pos, HtmlYearlessDate *out)
{
    size_t hyphens = 0;
    int month, day, maxday;

    dtm_check_entry(s, len, pos);
    /* "Collect a sequence of code points that are U+002D HYPHEN-MINUS characters ... If the collected sequence
       is not exactly zero or two characters long, then fail." */
    while (*pos < len && s[*pos] == '-') { (*pos)++; hyphens++; }
    if (hyphens != 0 && hyphens != 2) return false;
    if (!dtm_two_digits(s, len, pos, &month)) return false;
    if (month < 1 || month > 12) return false;
    /* "Let maxday be the number of days in month month of ANY ARBITRARY LEAP YEAR (e.g. 4 or 2000)" — which is
       why `02-29` is a valid yearless date and `2015-02-29` is not a valid date. */
    maxday = html_date_time_days_in_month(2000, month);
    if (!dtm_literal(s, len, pos, '-')) return false;
    if (!dtm_two_digits(s, len, pos, &day)) return false;
    if (day < 1 || day > maxday) return false;
    out->month = month;
    out->day = day;
    return true;
}

bool html_parse_yearless_date_string(const char *s, size_t len, HtmlYearlessDate *out)
{
    size_t pos = 0;
    HtmlYearlessDate d;

    if (!html_parse_yearless_date_component(s, len, &pos, &d) || pos != len) return false;
    *out = d;
    return true;
}

bool html_is_valid_yearless_date_string(const char *s, size_t len)
{
    HtmlYearlessDate d;

    /* "Optionally, two U+002D HYPHEN-MINUS characters; two ASCII digits, representing the month, in the range
       1 ≤ month ≤ 12; a U+002D HYPHEN-MINUS character; two ASCII digits, representing day, in the range
       1 ≤ day ≤ maxday" — the rules to parse a yearless date string, exactly. */
    return html_parse_yearless_date_string(s, len, &d);
}

/* ---- §2.3.5.4's TIMES ------------------------------------------------------------------------------------------ */

/* "Interpret the resulting sequence as a base-ten number (possibly with a fractional part)" over the seconds
   run, which is a run of ASCII digits and at most one U+002E FULL STOP and has already passed every shape test
   the rule states. THE VALUE is the C library's conversion of exactly those bytes — one correctly-rounded
   conversion of the whole numeral, which digit-by-digit accumulation in a double is not. Nothing in this engine
   calls setlocale, so the process is in the "C" locale where the decimal separator is that same full stop. */
static double dtm_seconds_value(const char *s, size_t n)
{
    char buf[64], *heap = NULL, *p = buf, *end = NULL;
    double v;

    if (n + 1 > sizeof buf) {
        /* AN ALLOCATION SIZE AND NOT A LENGTH LIMIT: the rule puts no bound on how many digits the fractional
           part has, and the shape tests above have already accepted however many there are. */
        heap = malloc(n + 1);
        CHECK(heap != NULL, "date/time: OOM converting a seconds component — a dropped value is a control whose "
                            "value this engine would then report as something a browser never held");
        p = heap;
    }
    memcpy(p, s, n);
    p[n] = 0;
    v = strtod(p, &end);
    DCHECK(end == p + n, "§2.3.5.4's seconds run was not consumed whole by its conversion — the run is ASCII "
                         "digits and at most one full stop with a digit on each side of it, which is a numeral "
                         "strtod reads to the end of");
    free(heap);
    return v;
}

bool html_parse_time_component(const char *s, size_t len, size_t *pos, HtmlTime *out)
{
    int hour, minute;
    double second = 0.0;
    int fraction_digits = 0;

    dtm_check_entry(s, len, pos);
    if (!dtm_two_digits(s, len, pos, &hour)) return false;
    if (hour < 0 || hour > 23) return false;
    if (!dtm_literal(s, len, pos, ':')) return false;
    if (!dtm_two_digits(s, len, pos, &minute)) return false;
    if (minute < 0 || minute > 59) return false;
    /* "Let second be 0. If position is not beyond the end of input and the character at position is U+003A." */
    if (*pos < len && s[*pos] == ':') {
        size_t start, n, dots = 0, i;

        (*pos)++;
        /* "If position is beyond the end of input, or at the last character in input, or if the next two
           characters in input starting at position are not both ASCII digits, then fail." */
        if (*pos >= len || *pos == len - 1) return false;
        if (!(s[*pos] >= '0' && s[*pos] <= '9') || !(s[*pos + 1] >= '0' && s[*pos + 1] <= '9')) return false;
        /* "Collect a sequence of code points that are either ASCII digits or U+002E FULL STOP characters." */
        start = *pos;
        while (*pos < len && ((s[*pos] >= '0' && s[*pos] <= '9') || s[*pos] == '.')) (*pos)++;
        n = *pos - start;
        for (i = 0; i < n; i++) if (s[start + i] == '.') dots++;
        /* "If the collected sequence is three characters long, or if it is longer than three characters long
           and the third character is not a U+002E FULL STOP character, or if it has more than one U+002E FULL
           STOP character, then fail." */
        if (n == 3) return false;
        if (n > 3 && s[start + 2] != '.') return false;
        if (dots > 1) return false;
        DCHECK(n >= 2, "§2.3.5.4's seconds run came out shorter than the two ASCII digits the step before it "
                       "required to be there");
        DCHECK(dots == (size_t)(n > 3), "§2.3.5.4's seconds run holds a full stop somewhere other than its "
                                        "third character — the two shape tests above admit one only there");
        second = dtm_seconds_value(s + start, n);
        /* The digits AFTER the full stop, which is what tells the parsing rules apart from the production: the
           production allows one, two or three of them and the rules allow any number. */
        fraction_digits = n > 3 ? (int)(n - 3) : 0;
        /* "If second is not a number in the range 0 ≤ second < 60, then fail." */
        if (!(second >= 0.0 && second < 60.0)) return false;
    }
    out->hour = hour;
    out->minute = minute;
    out->second = second;
    out->fraction_digits = fraction_digits;
    return true;
}

bool html_parse_time_string(const char *s, size_t len, HtmlTime *out)
{
    size_t pos = 0;
    HtmlTime t;

    if (!html_parse_time_component(s, len, &pos, &t) || pos != len) return false;
    *out = t;
    return true;
}

bool html_is_valid_time_string(const char *s, size_t len, HtmlTime *out)
{
    HtmlTime t;

    /* THE ONE PLACE THE PRODUCTION AND THE PARSING RULES DIVERGE. The production's fractional part is "one, two,
       or three ASCII digits"; the rules collect an unbounded run and reject only the three shapes named at the
       parse. So `12:30:59.1234` parses to a time and is not a valid time string, and the count the parse
       recorded is what says so. Everything else about the two is the same walk. */
    if (!html_parse_time_string(s, len, &t) || t.fraction_digits > 3) return false;
    if (out) *out = t;
    return true;
}

/* ---- §2.3.5.5's LOCAL DATES AND TIMES -------------------------------------------------------------------------- */

bool html_parse_local_date_and_time_string(const char *s, size_t len, HtmlDateTime *out)
{
    size_t pos = 0;
    HtmlDateTime dt;

    if (!html_parse_date_component(s, len, &pos, &dt.date)) return false;
    /* "If position is beyond the end of input or if the character at position is neither a U+0054 LATIN CAPITAL
       LETTER T character (T) nor a U+0020 SPACE character, then fail." A lower-case `t` is neither. */
    if (pos >= len || (s[pos] != 'T' && s[pos] != ' ')) return false;
    pos++;
    if (!html_parse_time_component(s, len, &pos, &dt.time)) return false;
    if (pos != len) return false;
    *out = dt;
    return true;
}

bool html_is_valid_local_date_and_time_string(const char *s, size_t len, HtmlDateTime *out)
{
    HtmlDateTime dt;

    /* "A valid date string representing the date; a U+0054 LATIN CAPITAL LETTER T character (T) or a U+0020
       SPACE character; a valid time string representing the time" — so the SEPARATOR may be either, and the
       time half carries the production's fractional-digit bound that its parsing rules do not. */
    if (!html_parse_local_date_and_time_string(s, len, &dt) || dt.time.fraction_digits > 3) return false;
    if (out) *out = dt;
    return true;
}

/* ---- WRITING ONE BACK ------------------------------------------------------------------------------------------ */

/* The one CHECK all four productions end in. A truncated value is a control reporting a date it does not hold,
   and §4.10.22.4 would then submit it. */
static size_t dtm_wrote(int n, size_t cap)
{
    CHECK(n > 0 && (size_t)n < cap,
          "date/time: a §2.3.5 production did not fit the buffer it was given — a truncated value is a control "
          "reporting a date it does not hold");
    return (size_t)n;
}

static void dtm_check_year(int64_t year)
{
    DCHECK(year > 0,
           "a §2.3.5 production was asked to write a year that is not greater than zero — every one of these "
           "syntaxes states `where year > 0`, so such a year has no valid string and the caller owed this the "
           "check before it asked");
}

size_t html_serialize_month(const HtmlMonth *m, char *out, size_t cap)
{
    DCHECK(m != NULL && out != NULL, "§2.3.5.1's month was asked for with no month or nowhere to put it");
    DCHECK(cap >= HTML_MONTH_CAP, "§2.3.5.1's month was given a buffer smaller than HTML_MONTH_CAP");
    dtm_check_year(m->year);
    DCHECK(m->month >= 1 && m->month <= 12,
           "§2.3.5.1's month was asked to write a month outside 1 ≤ month ≤ 12 — the parse range-checks it, so "
           "one outside is a caller that filled the struct itself");
    /* "Four or more ASCII digits, representing year, where year > 0; a U+002D HYPHEN-MINUS character; two ASCII
       digits, representing the month." */
    return dtm_wrote(snprintf(out, cap, "%04" PRId64 "-%02d", m->year, m->month), cap);
}

size_t html_serialize_date(const HtmlDate *d, char *out, size_t cap)
{
    DCHECK(d != NULL && out != NULL, "§2.3.5.2's date was asked for with no date or nowhere to put it");
    DCHECK(cap >= HTML_DATE_CAP, "§2.3.5.2's date was given a buffer smaller than HTML_DATE_CAP");
    dtm_check_year(d->year);
    DCHECK(d->month >= 1 && d->month <= 12 && d->day >= 1 &&
           d->day <= html_date_time_days_in_month(d->year, d->month),
           "§2.3.5.2's date was asked to write a date that is not one — every field of an HtmlDate comes out of "
           "a parse that range-checked it, so a date outside the calendar is a caller that filled the struct "
           "itself");
    /* "A valid month string, representing year and month; a U+002D HYPHEN-MINUS character; two ASCII digits,
       representing day." */
    return dtm_wrote(snprintf(out, cap, "%04" PRId64 "-%02d-%02d", d->year, d->month, d->day), cap);
}

size_t html_serialize_time(const HtmlTime *t, char *out, size_t cap)
{
    int isec, ms, fd = 3;
    double frac;
    char f[4];

    DCHECK(t != NULL && out != NULL, "§2.3.5.4's time was asked for with no time or nowhere to put it");
    DCHECK(cap >= HTML_TIME_CAP, "§2.3.5.4's time was given a buffer smaller than HTML_TIME_CAP");
    DCHECK(t->hour >= 0 && t->hour <= 23 && t->minute >= 0 && t->minute <= 59 &&
           t->second >= 0.0 && t->second < 60.0,
           "§2.3.5.4's time was asked to write a time that is not one — the parse range-checks all three "
           "components, and the seconds bound is the strict `second < 60` that makes leap seconds "
           "unrepresentable");
    DCHECK(t->fraction_digits >= 0 && t->fraction_digits <= 3,
           "§2.3.5.4's time was asked to write a fractional second of more than three ASCII digits — a VALID "
           "time string's fractional part is one, two or three digits, so such a time has no valid time string "
           "to be expressed as; the caller owed this an html_is_valid_time_string and asked the parse instead");

    isec = (int)t->second;
    frac = t->second - (double)isec;
    ms = (int)(frac * 1000.0 + 0.5);
    DCHECK(fabs(frac * 1000.0 - (double)ms) < 1e-6,
           "§2.3.5.4's time could not recover the fractional second's digits — at most three decimal digits go "
           "into the double a parse produces, so scaling it by a thousand lands on an integer to well within "
           "this tolerance, and missing it means the seconds value did not come from that parse");
    DCHECK(ms >= 0 && ms <= 999, "a fractional second came out of §2.3.5.4's [0, 1) range");

    /* The SHORTEST POSSIBLE STRING for the given time — §2.3.5.5's wording for its time half, which is the one
       place the standard states which of a time's several valid strings to write: "e.g. omitting the seconds
       component entirely if the given time is zero seconds past the minute". A trailing zero in the fraction is
       a digit the time does not need either, and dropping it is the same rule. */
    if (isec == 0 && ms == 0)
        return dtm_wrote(snprintf(out, cap, "%02d:%02d", t->hour, t->minute), cap);
    if (ms == 0)
        return dtm_wrote(snprintf(out, cap, "%02d:%02d:%02d", t->hour, t->minute, isec), cap);
    snprintf(f, sizeof f, "%03d", ms);
    while (fd > 1 && f[fd - 1] == '0') fd--;
    return dtm_wrote(snprintf(out, cap, "%02d:%02d:%02d.%.*s", t->hour, t->minute, isec, fd, f), cap);
}

size_t html_serialize_week(const HtmlWeek *w, char *out, size_t cap)
{
    DCHECK(w != NULL && out != NULL, "§2.3.5.8's week was asked for with no week or nowhere to put it");
    DCHECK(cap >= HTML_WEEK_CAP, "§2.3.5.8's week was given a buffer smaller than HTML_WEEK_CAP");
    dtm_check_year(w->year);
    DCHECK(w->week >= 1 && w->week <= html_date_time_weeks_in_week_year(w->year),
           "§2.3.5.8's week was asked to write a week the week-year does not have — the count is 52 or 53 and "
           "the parse bounds the week by it");
    /* "Four or more ASCII digits, representing year, where year > 0; a U+002D HYPHEN-MINUS character; a U+0057
       LATIN CAPITAL LETTER W character; two ASCII digits, representing the week." */
    return dtm_wrote(snprintf(out, cap, "%04" PRId64 "-W%02d", w->year, w->week), cap);
}

size_t html_serialize_normalized_local_date_and_time(const HtmlDateTime *dt, char *out, size_t cap)
{
    size_t n;

    DCHECK(dt != NULL && out != NULL, "§2.3.5.5's normalized form was asked for with no time or nowhere to put "
                                      "it");
    DCHECK(cap >= HTML_NORMALIZED_LOCAL_DATE_AND_TIME_CAP,
           "§2.3.5.5's normalized form was given a buffer smaller than the longest string it can produce — "
           "HTML_NORMALIZED_LOCAL_DATE_AND_TIME_CAP is that size and every caller declares one");
    /* "A valid date string representing the date, a U+0054 LATIN CAPITAL LETTER T character (T), a valid time
       string representing the time, expressed as the shortest possible string for the given time" — the two
       productions and the T, which is what the sentence says it is made of. The components' own range checks
       are the two productions' and are not restated here. */
    n = html_serialize_date(&dt->date, out, cap);
    CHECK(n + 1 < cap, "date/time: §2.3.5.5's normalized form has no room for its separator");
    out[n++] = 'T';
    n += html_serialize_time(&dt->time, out + n, cap - n);
    DCHECK(out[n] == 0, "§2.3.5.5's normalized form was not NUL-terminated by the production that ended it");
    return n;
}

/* ---- §2.3.5.8's WEEKS ------------------------------------------------------------------------------------------ */

bool html_parse_week_string(const char *s, size_t len, HtmlWeek *out)
{
    size_t pos = 0;
    int64_t year;
    int week, maxweek;

    dtm_check_entry(s, len, &pos);
    /* The week string states its own year rather than reaching for a month component, because there is no
       month in it — but the year is the same "four or more ASCII digits ... greater than zero". */
    if (!dtm_year(s, len, &pos, &year)) return false;
    if (!dtm_literal(s, len, &pos, '-')) return false;
    if (!dtm_literal(s, len, &pos, 'W')) return false;   /* U+0057 LATIN CAPITAL LETTER W, and not `w` */
    if (!dtm_two_digits(s, len, &pos, &week)) return false;
    maxweek = html_date_time_weeks_in_week_year(year);
    DCHECK(maxweek == 52 || maxweek == 53,
           "§2.3.5.8's week count answered something other than 52 or 53 — those are the only two the standard "
           "defines, and every week-year is one of them");
    if (week < 1 || week > maxweek) return false;
    if (pos != len) return false;
    out->year = year;
    out->week = week;
    return true;
}

bool html_is_valid_week_string(const char *s, size_t len)
{
    HtmlWeek w;

    /* "Four or more ASCII digits, representing year, where year > 0; a U+002D HYPHEN-MINUS character; a U+0057
       LATIN CAPITAL LETTER W character; two ASCII digits, representing the week, in the range
       1 ≤ week ≤ maxweek" — the rules to parse a week string, exactly. */
    return html_parse_week_string(s, len, &w);
}
