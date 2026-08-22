/* NUMBERS — HTML §2.3.4.3's floating-point numbers. See number_microsyntax.h for why this is its own component
 * and why the production is answered by the same walk that parses.
 *
 * THE RULES ARE TRANSCRIBED, NOT APPROXIMATED, exactly as §2.3.5's are next door: every branch below is one of
 * the algorithm's numbered steps, and the two places it can leave a character UNCONSUMED (a full stop with no
 * digit after it, an E with no digits after it) are the steps that "jump to the step labeled conversion" — the
 * whole reason `1.` denotes 1 and is not a valid floating-point number. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/html/number_microsyntax.h"

bool html_parse_floating_point(const char *s, size_t len, double *out, bool *pvalid)
{
    size_t i = 0, start, n, digits = 0;
    bool lead_ws = false, plus = false;
    char buf[64], *heap = NULL, *p;
    double v;

    DCHECK(s != NULL || len == 0, "§2.3.4.3's parse was given a null input with a nonzero length");
    if (pvalid) *pvalid = false;
    /* Step 6: "Skip ASCII whitespace within input given position." The production has no such step, which is
       the first of the two things that tell the two questions apart. */
    while (i < len && (s[i] == '\t' || s[i] == '\n' || s[i] == '\f' || s[i] == '\r' || s[i] == ' ')) {
        i++;
        lead_ws = true;
    }
    start = i;
    /* Steps 8 and 9: a U+002D makes the number negative; a U+002B is IGNORED, "but it is not conforming" —
       the second thing that tells the production from the rules. */
    if (i < len && (s[i] == '-' || s[i] == '+')) {
        plus = s[i] == '+';
        i++;
    }
    while (i < len && s[i] >= '0' && s[i] <= '9') { i++; digits++; }
    if (i < len && s[i] == '.') {
        size_t frac = 0, dot = i;

        i++;
        while (i < len && s[i] >= '0' && s[i] <= '9') { i++; frac++; }
        /* The production's "one or both of the following, in the given order": a series of digits, or a full
           stop followed by one. A stop with digits on NEITHER side is neither, and a stop with digits only
           BEFORE it is the rules' "jump to the step labeled conversion" — the stop is not part of the numeral,
           which is what makes `1.` parse as 1 and fail the production. */
        if (!frac && !digits) return false;
        if (!frac) i = dot;
    } else if (!digits) {
        return false;
    }
    if (i < len && (s[i] == 'e' || s[i] == 'E')) {
        size_t e = i, expdigits = 0;

        i++;
        if (i < len && (s[i] == '-' || s[i] == '+')) i++;
        while (i < len && s[i] >= '0' && s[i] <= '9') { i++; expdigits++; }
        if (!expdigits) i = e;   /* the algorithm jumps to conversion, leaving the E unconsumed */
    }
    n = i - start;
    DCHECK(n > 0, "§2.3.4.3's parse consumed nothing after accepting a numeral — every path that reaches here "
                  "has passed at least one digit, so an empty span is a walk that lost its position");
    /* The PRODUCTION: no leading whitespace, no leading U+002B, and the whole string consumed. */
    if (pvalid) *pvalid = !lead_ws && !plus && i == len;
    p = buf;
    if (n + 1 > sizeof buf) {
        /* An ALLOCATION SIZE and not a length limit: §2.3.4.3 puts no bound on how many digits a numeral has. */
        heap = malloc(n + 1);
        CHECK(heap != NULL, "numbers: OOM converting a numeral — a dropped value is a missing endpoint "
                            "parameter and a constraint this engine would then answer from nothing");
        p = heap;
    }
    memcpy(p, s + start, n);
    p[n] = 0;
    v = strtod(p, NULL);
    free(heap);
    /* Conversion step 1: "Let S be the set of finite IEEE 754 double-precision floating-point values EXCEPT −0,
       but with two special values added: 2^1024 and −2^1024." So no numeral denotes negative zero — `-0` is
       the positive one — and this is the assignment that says so (v == 0 is true for −0). */
    if (v == 0) v = 0;
    *out = v;
    /* Conversion step 3: "If rounded-value is 2^1024 or −2^1024, return an error." The two special values are
       in the set exactly so an overflowing numeral is an ERROR rather than an infinity. */
    return isfinite(v);
}

JSValue html_best_representation_of_number(JSContext *ctx, double n)
{
    JSValue num = JS_NewFloat64(ctx, n);
    JSValue s = JS_ToString(ctx, num);

    JS_FreeValue(ctx, num);
    CHECK(!JS_IsException(s), "numbers: the best representation of a number could not be built — ToString of a "
                              "double allocates a string and reaches none of the page's code, so the only way "
                              "it fails is an allocation this engine cannot proceed past");
    return s;
}
