/* INTEGERS — HTML §2.3.4.1's SIGNED INTEGERS and §2.3.4.2's NON-NEGATIVE INTEGERS: the two sets of rules that
 * turn a content attribute's bytes into a number.
 *
 * ONE PROBLEM, for the reason number_microsyntax.h states for §2.3.4.3 and date_time_microsyntax.h for §2.3.5:
 * the microsyntax is SHARED and its callers are not one another's. §6.6.3's tabindex value runs the signed
 * rules; §15.2's "maps to the pixel length property" runs the non-negative ones; §2.6.1's reflected `long` and
 * `unsigned long` getters run one each and then apply their OWN range. Written inside any one of those it is
 * that caller's private idea of what an integer is, and it had already been written twice — focus.c's
 * parse_integer and css_presentational_hints.c's html_parse_non_negative_integer — which disagreed about `-0`
 * (the rules parse it to 0, which is NOT less than zero, and one of the two rejected it).
 *
 * THE RESULT IS THE DIGIT RUN AS WELL AS THE NUMBER, AND THAT IS THE WHOLE DESIGN. "Interpret the resulting
 * sequence as a base-ten integer" has NO upper bound; every bound in the platform belongs to the CONSUMER and
 * they do not agree. §2.6.1's `long` getter asks "is it within the long range", its `unsigned long` getter asks
 * "is it in the range minimum to maximum", and §15.2 maps the digits into a `<length>` with no bound at all —
 * so a parser that returned only a `long` would have to pick one, and picking one is a CAP on what a page may
 * write. It reports `overflow` instead and hands back the digits, and each caller states its own range.
 *
 * WHICH IS ALSO THE BUG THIS REPLACES. focus.c accumulated `v = v * 10 + d` into a `long` with no check, so
 * `<div tabindex="99999999999999999999">` was signed-integer overflow — undefined behaviour, and the value gcc
 * happens to produce at -O2 decides whether the element is skipped from sequential focus navigation (§6.6.3
 * branches on the sign). The accumulation here is unsigned, where wrapping is defined, and is checked.
 *
 * NOTHING HERE RUNS PAGE CODE. The parse is a decision about BYTES, so it takes no context and allocates
 * nothing; the slice it returns points INTO the caller's buffer and does not outlive it. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_INTEGER_MICROSYNTAX_H
#define ENGINE_HOST_BROWSER_CORE_HTML_INTEGER_MICROSYNTAX_H
#include <stdbool.h>
#include <stddef.h>

/* What the rules returned. `digits` BORROWS from the string that was parsed. */
typedef struct {
    bool        negative;    /* §2.3.4.1's sign — "-0" is negative with a zero magnitude, and is not < 0 */
    const char *digits;      /* the collected run, leading zeros dropped; never NULL on success */
    size_t      digits_len;  /* at least 1 on success, so a zero is the one byte "0" */
    long long   value;       /* the number — MEANINGLESS when `overflow` is true */
    bool        overflow;    /* the run does not fit a long long; `digits` still describes it exactly */
} HtmlInteger;

/* §2.3.4.1's RULES FOR PARSING INTEGERS. Skips ASCII whitespace, takes one optional U+002D or U+002B, requires
   an ASCII digit next, then collects the digit run — so TRAILING CONTENT IS NOT AN ERROR and `"10px"` is ten.
   Returns the rules' own answer: true when they returned a number, false for their error. */
bool html_parse_integer(const char *s, size_t len, HtmlInteger *out);

/* §2.3.4.2's RULES FOR PARSING NON-NEGATIVE INTEGERS, which are the rules above and then "if value is less than
   zero, return an error". `-0` is zero and therefore SUCCEEDS; `-1` does not. */
bool html_parse_non_negative_integer(const char *s, size_t len, HtmlInteger *out);

#endif
