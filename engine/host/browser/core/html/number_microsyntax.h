/* NUMBERS — HTML §2.3.4.3's FLOATING-POINT NUMBERS: the production, the rules that parse one, and the best
 * representation of a number as one.
 *
 * ONE PROBLEM: which strings are floating-point numbers, what number one denotes, and how a number is written
 * back. It is its own component for the reason date_time_microsyntax.h states for §2.3.5 — the microsyntax is
 * SHARED and its callers are not one another's. §4.10.5.1.12's value sanitization asks the PRODUCTION ("if the
 * value of the element is not a valid floating-point number, then set it to the empty string"); §4.10.5.1.12
 * and §4.10.5.1.13's convert a string to a number run THE RULES over the element's value; §4.10.5.3.8's allowed
 * value step runs the same rules over the `step` attribute of EVERY state that has one, date and time states
 * included; and §4.10.5.1.13's default value is written back with the best representation. Written inside any
 * one of them it was that caller's private idea of what a number is, and it had already been written THREE
 * times — input_value.c's iv_fpn, constraint_validation.c's cv_parse_double and the walk this file now is —
 * two of which disagreed about whether `1e999` parses (§2.3.4.3's conversion step says it is an ERROR, and one
 * of the two returned an infinity).
 *
 * TWO QUESTIONS, ONE WALK. The production forbids leading whitespace and a leading U+002B, and consumes the
 * WHOLE string; the parsing rules skip whitespace, ignore a U+002B, and stop wherever they stop. They are
 * otherwise the same walk, and `1.` — which parses as 1 and is not a valid floating-point number — is the case
 * that only a shared walk gets both halves of right.
 *
 * NOTHING HERE RUNS PAGE CODE. The parse is a decision about BYTES. The best representation is ToString of a
 * DOUBLE, which is the engine's number-to-string and never a printf format, so it takes a context and allocates
 * a string; it still reaches none of the page's code. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_NUMBER_MICROSYNTAX_H
#define ENGINE_HOST_BROWSER_CORE_HTML_NUMBER_MICROSYNTAX_H
#include <stdbool.h>
#include <stddef.h>

#include "quickjs.h"

/* §2.3.4.3's RULES FOR PARSING FLOATING-POINT NUMBER VALUES, answering the PRODUCTION on the way past.
 *
 * The return value is the RULES' own: true when they returned a number, false for their error — which includes
 * the conversion step's "if rounded-value is 2^1024 or −2^1024, return an error", so an overflowing numeral is
 * an error and never an infinity. `*out` is written only then.
 *
 * `*pvalid` (may be NULL) answers "is this string a VALID FLOATING-POINT NUMBER" — the production, which is a
 * different question with a different answer, and is written even when the rules return an error: `1e999` IS a
 * valid floating-point number and does not parse.
 *
 * THE VALUE is the C library's conversion of the span the walk consumed, because §2.3.4.3's conversion step is
 * "the number in the set of doubles CLOSEST to value" — one correctly-rounded conversion of the whole numeral,
 * which digit-by-digit accumulation in a double is not. That set EXCLUDES −0, which is why `-0` parses to
 * positive zero. Nothing in this engine calls setlocale, so the process is in the "C" locale where the decimal
 * separator is the U+002E the algorithm names. */
bool html_parse_floating_point(const char *s, size_t len, double *out, bool *pvalid);

/* §2.3.4.3's "THE BEST REPRESENTATION OF THE NUMBER n AS A FLOATING-POINT NUMBER is the string obtained from
   running ToString(n)" — ECMAScript's own Number-to-String, which is the engine's. OWNED. */
JSValue html_best_representation_of_number(JSContext *ctx, double n);

#endif
