/* DATES AND TIMES — HTML §2.3.5's date/time MICROSYNTAX: the grammars, and the rules that parse them.
 *
 * ONE PROBLEM: which strings are dates, months, weeks, times and local dates and times, and what components
 * they carry. It is its own component because §2.3.5 is a SHARED microsyntax whose callers are not one
 * another's: §4.10.5.1.7 through §4.10.5.1.11's value sanitization algorithms ask whether a control's value is
 * one of these strings, §4.10.5.3.7's `min`/`max` and §4.10.5.3.8's step parse the SAME grammars out of
 * attributes, §4.10.5.4's `valueAsDate` and `valueAsNumber` convert through them, and §4.7's `time`, `ins` and
 * `del` elements are defined over the same productions. Written inside any one of them it would be that
 * caller's private idea of what a date is, and the first thing to go wrong is two of them disagreeing about
 * whether `2015-02-29` is one.
 *
 * TWO QUESTIONS, ONE WALK. §2.3.5 states each syntax TWICE — once as a production ("a string is a valid date
 * string representing ... if it consists of the following components in the given order") and once as a set of
 * parsing rules that return the components. Every caller asks one or the other and several ask both in the same
 * breath (§4.10.5.1.11's sanitization keeps the value only if it IS a valid local date and time string and then
 * REWRITES it from the parsed components), so the productions here are answered BY the parse rather than by a
 * second walk: two walks are two answers that stop agreeing. Where the two genuinely differ the parse reports
 * the difference and the validity predicate reads it — see HtmlTime's `fraction_digits`, which is the ONE place
 * in §2.3.5 where the parsing rules accept a string the production does not.
 *
 * A PARSE COMPONENT ADVANCES A POSITION; A PARSE STRING CONSUMES THE WHOLE INPUT. That is the standard's own
 * split — "parse a month component" is a step of "parse a date component" is a step of "parse a local date and
 * time string" — and it is why the component forms take a `pos` in/out and the string forms do not.
 *
 * NOTHING HERE RUNS PAGE CODE. These are decisions about BYTES: there is no property to get, no coercion to
 * perform and no callback to invoke, so they are plain C functions and not step machines. A caller that holds a
 * JSValue converts it to bytes first, on its own terms. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_DATE_TIME_MICROSYNTAX_H
#define ENGINE_HOST_BROWSER_CORE_HTML_DATE_TIME_MICROSYNTAX_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* §2.3.5.1's MONTH: "a specific proleptic-Gregorian date with no time-zone information and no date information
   beyond a year and a month". `year` is greater than zero and `month` is in 1 ≤ month ≤ 12. */
typedef struct {
    int64_t year;
    int month;
} HtmlMonth;

/* §2.3.5.2's DATE: a month, plus a day in 1 ≤ day ≤ the number of days in that month of that year. */
typedef struct {
    int64_t year;
    int month;
    int day;
} HtmlDate;

/* §2.3.5.3's YEARLESS DATE: "a Gregorian month and a day within that month, but with no associated year", so
   the day's upper bound is the month's length in an ARBITRARY LEAP YEAR — which is what makes `02-29` one. */
typedef struct {
    int month;
    int day;
} HtmlYearlessDate;

/* §2.3.5.4's TIME: "an hour, a minute, a second, and a fraction of a second".
 *
 * `second` CARRIES THE FRACTION, because the standard's parse rule ends in one number: "interpret the resulting
 * sequence as a base-ten number (possibly with a fractional part). Set second to that number", range-checked as
 * 0 ≤ second < 60. It is the C library's conversion of the digits the walk consumed, one correctly-rounded
 * conversion of the whole numeral rather than digit-by-digit accumulation in a double.
 *
 * `fraction_digits` IS THE ONE PLACE §2.3.5 IS NOT ONE GRAMMAR. The production allows "One, two, or three ASCII
 * digits, representing the fractional part of second"; the parsing rules collect an UNBOUNDED run of digits and
 * full stops and only reject a run that is exactly three characters long, or longer than three with a third
 * character that is not a full stop, or that holds more than one full stop. So `12:30:59.1234` PARSES and is
 * not a valid time string, and the count is what tells the two apart. It is 0 when no full stop was present. */
typedef struct {
    int hour;             /* 0 ≤ hour ≤ 23 */
    int minute;           /* 0 ≤ minute ≤ 59 */
    double second;        /* 0 ≤ second < 60, fraction included */
    int fraction_digits;  /* ASCII digits after the U+002E FULL STOP; 0 when there was none */
} HtmlTime;

/* §2.3.5.5's LOCAL DATE AND TIME: a date and a time, expressed without a time zone. */
typedef struct {
    HtmlDate date;
    HtmlTime time;
} HtmlDateTime;

/* §2.3.5.8's WEEK: "a week-year number and a week number representing a seven-day period starting on a Monday".
   `year` is greater than zero and `week` is in 1 ≤ week ≤ the number of weeks in that week-year. */
typedef struct {
    int64_t year;
    int week;
} HtmlWeek;

/* ---- §2.3.5's CALENDAR ARITHMETIC ---------------------------------------------------------------------------
 *
 * "The number of days in month month of year year is: 31 if month is 1, 3, 5, 7, 8, 10, or 12; 30 if month is
 * 4, 6, 9, or 11; 29 if month is 2 and year is a number divisible by 400, or if year is a number divisible by 4
 * but not by 100; and 28 otherwise." Stated once, here, because §2.3.5.2's date and §2.3.5.3's yearless date
 * both bound their day by it and a second copy is a second leap rule. */
bool html_date_time_is_leap_year(int64_t year);
int  html_date_time_days_in_month(int64_t year, int month);

/* §2.3.5.8's WEEK COUNT: "A week-year with a number year has 53 weeks if it corresponds to either a year year
   in the proleptic Gregorian calendar that has a Thursday as its first day (January 1st), or a year year in the
   proleptic Gregorian calendar that has a Wednesday as its first day (January 1st) and where year is a number
   divisible by 400, or a number divisible by 4 but not by 100. All other week-years have 52 weeks." Answers 52
   or 53, and nothing else. */
int  html_date_time_weeks_in_week_year(int64_t year);
/* The weekday of January 1st of `year` in the proleptic Gregorian calendar, 0 for Sunday through 6 for
   Saturday — the one calendar fact the week count is stated over, exposed because §2.3.5.8's own definition
   names it and a caller computing it a second way would compute it differently. */
int  html_date_time_jan1_weekday(int64_t year);

/* ---- THE CALENDAR AS A DAY COUNT ------------------------------------------------------------------------------
 *
 * §4.10.5.1.7 through §4.10.5.1.11 are ARITHMETIC over these syntaxes — "the number of milliseconds elapsed
 * from midnight UTC on the morning of 1970-01-01 ... to midnight UTC on the morning of the parsed date", "to
 * the Monday of the parsed week" — and every one of them is a day count times 86,400,000. The count belongs
 * HERE, beside the leap rule it is made of, for the reason §2.3.5's own arithmetic does: a second place that
 * decides which years have 366 days is a second leap rule, and the two stop agreeing at the first century year.
 * That is not hypothetical — the weekday of January 1st was a closed form of its own until this landed, and it
 * is now derived from the day count, so there is ONE encoding of the calendar in this component and not two.
 *
 * Day 0 is 1970-01-01 and the count is negative before it. `year` may be ZERO OR NEGATIVE here, deliberately:
 * a date arrived at by counting days back from the epoch lands there, and it is the CALLER that then has no
 * valid date string to write (every §2.3.5 production requires year > 0). The parse's own year check is what
 * keeps such a year from ever reaching the syntaxes. */
int64_t html_date_time_days_from_epoch(int64_t year, int month, int day);
void    html_date_time_date_from_days(int64_t days, HtmlDate *out);

/* §2.3.5.8's WEEK, as days: the day count of the MONDAY that starts week `week` of week-year `week_year`, and
   the week-year and week that a day belongs to. The two are inverses and each asserts the other.
   `html_date_time_week_of_days` answers false when the week-year would not be greater than zero — the one
   input for which there is no week to name, since §2.3.5.8's year is "greater than zero". */
int64_t html_date_time_week_start_days(int64_t week_year, int week);
bool    html_date_time_week_of_days(int64_t days, HtmlWeek *out);

/* ---- THE PARSING RULES ---------------------------------------------------------------------------------------
 *
 * Each `_component` form is §2.3.5's rule of that name: it reads from `*pos` and, on success, leaves `*pos` one
 * past the last character it consumed. On failure `*pos` is UNSPECIFIED — the standard's rules abort where they
 * fail and no caller of a failed component reads the position, because "if this returns nothing, then fail".
 * Each `_string` form is the rule that runs its component and then requires the input to be exhausted.
 * `out` is written only on success. */
bool html_parse_month_component(const char *s, size_t len, size_t *pos, HtmlMonth *out);
bool html_parse_month_string(const char *s, size_t len, HtmlMonth *out);

bool html_parse_date_component(const char *s, size_t len, size_t *pos, HtmlDate *out);
bool html_parse_date_string(const char *s, size_t len, HtmlDate *out);

bool html_parse_yearless_date_component(const char *s, size_t len, size_t *pos, HtmlYearlessDate *out);
bool html_parse_yearless_date_string(const char *s, size_t len, HtmlYearlessDate *out);

bool html_parse_time_component(const char *s, size_t len, size_t *pos, HtmlTime *out);
bool html_parse_time_string(const char *s, size_t len, HtmlTime *out);

bool html_parse_local_date_and_time_string(const char *s, size_t len, HtmlDateTime *out);

/* §2.3.5.8's week string has no component form — the standard states it inline, with its own four-or-more-digit
   year rather than a month component's. */
bool html_parse_week_string(const char *s, size_t len, HtmlWeek *out);

/* ---- THE PRODUCTIONS -----------------------------------------------------------------------------------------
 *
 * "A string is a valid X string representing ... if it consists of the following components in the given
 * order." Answered by the parse above, plus — for the two that embed a time — the fraction-digit bound the
 * parse rules do not impose.
 *
 * THE TWO THAT EMBED A TIME ALSO HAND BACK THE COMPONENTS (`out`, which may be NULL), and the other four do
 * not, for one reason: where the production and the parse accept the same strings a caller that wants the
 * components just calls the parse, but where the production is NARROWER a caller that wanted both would have to
 * restate the bound at its own call site — which is the second copy of a rule that then stops agreeing with the
 * first. §4.10.5.1.11's sanitization is exactly that caller: it keeps the value only if it is a valid local
 * date and time string and then rewrites it from "the same date and time". */
bool html_is_valid_month_string(const char *s, size_t len);
bool html_is_valid_date_string(const char *s, size_t len);
bool html_is_valid_yearless_date_string(const char *s, size_t len);
bool html_is_valid_time_string(const char *s, size_t len, HtmlTime *out);
bool html_is_valid_local_date_and_time_string(const char *s, size_t len, HtmlDateTime *out);
bool html_is_valid_week_string(const char *s, size_t len);

/* ---- WRITING ONE BACK ------------------------------------------------------------------------------------------
 *
 * The productions above, as the strings they are: each writes a valid X string for the components it is given,
 * plus a NUL, into `out`, and answers its length. They are HERE because a production is what this component
 * decides, and a caller that formatted one itself would be a second statement of the grammar — which is the
 * same defect as a second parse, seen from the other side. §4.10.5.1.7 through §4.10.5.1.11's convert a number
 * to a string is the caller these exist for: it does the ARITHMETIC and hands the components back.
 *
 * The YEAR is written with FOUR OR MORE digits, which for a year below 1000 means leading zeros and for a year
 * above 9999 means as many digits as it has — both are the production, which says "four or more". Every one of
 * them asserts its components are the ones its own parse would have produced, because a caller that computed
 * a month of 13 has a bug this catches at the write rather than at the next read.
 *
 * `cap` must admit the longest form the production can produce; each has a CAP for the caller to declare. */
#define HTML_MONTH_CAP 32
#define HTML_DATE_CAP  32
#define HTML_TIME_CAP  16
#define HTML_WEEK_CAP  32
size_t html_serialize_month(const HtmlMonth *m, char *out, size_t cap);
size_t html_serialize_date(const HtmlDate *d, char *out, size_t cap);
/* §2.3.5.4's valid time string, "expressed as the SHORTEST POSSIBLE STRING for the given time (e.g. omitting
   the seconds component entirely if the given time is zero seconds past the minute)" — §2.3.5.5's wording for
   the time half of its normalized form, which is where the shortest-form rule is stated and why it lives in
   one place rather than in each caller that wants a time written. */
size_t html_serialize_time(const HtmlTime *t, char *out, size_t cap);
size_t html_serialize_week(const HtmlWeek *w, char *out, size_t cap);

/* §2.3.5.5's VALID NORMALIZED LOCAL DATE AND TIME STRING — "a valid date string representing the date, a U+0054
 * LATIN CAPITAL LETTER T character (T), a valid time string representing the time, expressed as the shortest
 * possible string for the given time". Which is what it is BUILT of: the date production, the T, and the time
 * production, rather than a third format string that could disagree with either. */
#define HTML_NORMALIZED_LOCAL_DATE_AND_TIME_CAP 48
size_t html_serialize_normalized_local_date_and_time(const HtmlDateTime *dt, char *out, size_t cap);

#endif
