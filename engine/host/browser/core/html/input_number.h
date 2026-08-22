/* A CONTROL'S VALUE AS A NUMBER — HTML §4.10.5.1's four PER-STATE CONVERSIONS, and the §4.10.5.3.7/§4.10.5.3.8
 * DOMAIN they are the whole of.
 *
 * ONE PROBLEM: what NUMBER the string in a form control denotes, and what string a number denotes back. Seven
 * states define that pair — Date, Month, Week, Time, Local Date and Time, Number and Range — and each defines
 * it DIFFERENTLY: the Date state counts milliseconds from midnight UTC on 1970-01-01, the Month state counts
 * MONTHS from January 1970, the Week state counts milliseconds to the MONDAY of the week, the Time state
 * counts milliseconds from midnight with no date at all, and Number and Range run §2.3.4.3's floating-point
 * parse. Four of them also define a pair over a Date OBJECT, and those are NOT the same pair — the Month
 * state's number is a month count while its Date is the first day of that month, and the Time state's Date
 * carries a date the conversion back throws away.
 *
 * IT IS A COMPONENT BECAUSE ITS CALLERS ARE NOT ONE ANOTHER'S. §4.10.5.3.7's minimum and maximum, §4.10.5.3.8's
 * allowed value step and step base, §4.10.21.1's underflow, overflow and step mismatch, and §4.10.5.4's
 * valueAsNumber, valueAsDate, stepUp() and stepDown() are all stated over "the algorithm to convert a string to
 * a number" WITHOUT naming a state — they are one text that reads the per-state conversion out of the type
 * attribute. Written inside constraint validation it would be validation's private idea of what a date is
 * worth, and §4.10.5.4's members would need a second one that agreed with it.
 *
 * THE ARITHMETIC IS HERE AND THE GRAMMAR IS NOT. Every string this file reads it reads through
 * date_time_microsyntax.c or number_microsyntax.c, and every string it writes it writes through their
 * productions: this component knows how to count days between two dates and nothing at all about how a date is
 * spelled.
 *
 * NOTHING HERE RUNS PAGE CODE. It reads content attributes and converts numbers; the one operation that takes a
 * context is §2.3.4.3's best representation, which is the engine's ToString of a double. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_INPUT_NUMBER_H
#define ENGINE_HOST_BROWSER_CORE_HTML_INPUT_NUMBER_H
#include <stdbool.h>
#include <stddef.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"
#include "core/html/html_form.h"

/* DOES THIS STATE DEFINE A "CONVERT A STRING TO A NUMBER"? Which is the same question as whether §4.10.5.4's
   `valueAsNumber`, `stepUp()` and `stepDown()` apply, and whether `min`, `max` and `step` mean anything — every
   one of those is stated over that conversion, so the seven states that define one are exactly the seven whose
   member lists name those members. A state that answers false has NO minimum, NO maximum and NO allowed value
   step, and none of the constraints below can be suffered. */
bool input_number_applies(HtmlInputState st);

/* DOES THIS STATE DEFINE A "CONVERT A STRING TO A DATE OBJECT"? — §4.10.5.4's `valueAsDate`, which the Date,
   Month, Week and Time states define and which the Local Date and Time state explicitly does NOT ("the
   following IDL attributes and methods do not apply to the element: ... valueAsDate"), because a local date and
   time names no instant. Number and Range do not either. */
bool input_number_date_applies(HtmlInputState st);

/* §4.10.5.1's ALGORITHM TO CONVERT A STRING TO A NUMBER, for the state's own definition of one. False is the
   algorithm's "return an error" — the string is not the syntax that state parses, or the number it names is
   outside what this engine can carry back (see the time-value window in input_number.c). `*out` is written only
   on success. Asserting that the state HAS a conversion is this function's own job. */
bool input_number_from_string(HtmlInputState st, const char *s, size_t len, double *out);

/* §4.10.5.1's ALGORITHM TO CONVERT A NUMBER TO A STRING. OWNED, and JS_UNDEFINED when there is no valid string
   for that number in this state's syntax — a date before year 1, a time outside the day, a fraction of a
   millisecond no valid time string can spell. That is not a failure to report: the algorithm has no result,
   and every caller of it in §4.10.5.4 is setting the element's value, whose sanitization algorithm empties a
   value that is not this state's syntax anyway. */
JSValue input_number_to_string(JSContext *ctx, HtmlInputState st, double n);

/* §4.10.5.1's ALGORITHM TO CONVERT A STRING TO A DATE OBJECT, as the TIME VALUE that Date represents. The
   object itself is minted by the caller, in the realm doing the asking — a Date is a per-realm object and this
   component holds no realm. False is the algorithm's error. */
bool input_number_date_from_string(HtmlInputState st, const char *s, size_t len, double *ptime);

/* §4.10.5.1's ALGORITHM TO CONVERT A DATE OBJECT TO A STRING, over that object's time value. OWNED, and
   JS_UNDEFINED when there is no such string — same contract as the number-to-string above. */
JSValue input_number_date_to_string(JSContext *ctx, HtmlInputState st, double time_value);

/* §4.10.5.3.7's MINIMUM and MAXIMUM and §4.10.5.3.8's ALLOWED VALUE STEP and STEP BASE, as the element declares
 * them — the whole numeric domain of one control, read once because every question about it needs all of it.
 *
 * `reversed` is §4.10.5.3.7's REVERSED RANGE: "an element has a reversed range if it has a PERIODIC DOMAIN and
 * its maximum is less than its minimum". The Time state is the one control with a periodic domain — its
 * broadest range is midnight to midnight — which is why `min=21:00 max=06:00` is a range that SPANS midnight
 * and not an author error, and why a value between 6am and 9pm is then suffering from an underflow and an
 * overflow at the same time. For every other state a maximum below the minimum is exactly the author error the
 * standard says it is, and every value is out of range. */
typedef struct {
    bool   has_min, has_max, has_step, reversed;
    double min, max, step, base;
} InputStepRange;

void input_step_range_of(lxb_dom_element_t *el, HtmlInputState st, InputStepRange *r);

/* §4.10.5.3.8's "that number subtracted from the step base is not an INTEGRAL MULTIPLE of the allowed value
   step" — the STEP MISMATCH predicate, in exact arithmetic. False when the element has no allowed value step,
   which is what `step=any` means. */
bool input_step_mismatch(double value, const InputStepRange *r);

/* THE TWO ALIGNED VALUES §4.10.5.4's stepUp()/stepDown() are stated over: the smallest value not less than `v`
   that is an integral multiple of the allowed value step above the step base, and the largest not greater. The
   method's steps name them four times between them (snapping an unaligned value, and clamping to the minimum
   and to the maximum), so they are one implementation and not four. */
double input_step_align_up(double v, const InputStepRange *r);
double input_step_align_down(double v, const InputStepRange *r);

#endif
