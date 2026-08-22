/* HTML §2.3.4.1 and §2.3.4.2 — see integer_microsyntax.h for why this is its own component. */
#include <limits.h>

#include "check.h"
#include "core/html/integer_microsyntax.h"

/* §2.3.1's SKIP ASCII WHITESPACE — TAB, LF, FF, CR and SPACE, and nothing else. */
static bool ascii_whitespace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

bool html_parse_integer(const char *s, size_t len, HtmlInteger *out)
{
    size_t i = 0, start;
    unsigned long long mag = 0, limit;

    DCHECK(out != NULL, "§2.3.4.1 was asked to parse into nothing");
    DCHECK(s != NULL || len == 0, "§2.3.4.1 was given a null string with a non-zero length");
    out->negative = false;
    out->digits = NULL;
    out->digits_len = 0;
    out->value = 0;
    out->overflow = false;

    while (i < len && ascii_whitespace(s[i])) i++;              /* step 4 */
    if (i == len) return false;                                 /* step 5 */
    if (s[i] == '-') { out->negative = true; i++; }             /* step 6 */
    else if (s[i] == '+') i++;                                  /* step 7 — ignored, not conforming */
    if (i == len) return false;
    if (s[i] < '0' || s[i] > '9') return false;                 /* step 8 */

    start = i;                                                  /* step 9 — collect the digits */
    while (i < len && s[i] >= '0' && s[i] <= '9') i++;
    /* The leading zeros are dropped so the run is the CANONICAL description of the value: a consumer that maps
       the digits straight into a `<length>` must serialize `007` as `7`, and one comparing runs by length would
       otherwise call `007` bigger than `10`. At least one digit always survives, so a zero is the byte "0". */
    while (start + 1 < i && s[start] == '0') start++;
    out->digits = s + start;
    out->digits_len = i - start;

    /* "Interpret the resulting sequence as a base-ten integer" is unbounded; this reports whether it fits. The
       accumulation is UNSIGNED, where overflow is defined as wrapping rather than undefined — and it is checked
       before it can wrap, so it never does. The negative side reaches one further than the positive one, which
       is the whole reason `limit` is not simply LLONG_MAX. */
    limit = (unsigned long long)LLONG_MAX + (out->negative ? 1u : 0u);
    for (i = 0; i < out->digits_len; i++) {
        unsigned d = (unsigned)(out->digits[i] - '0');

        if (mag > (limit - d) / 10) { out->overflow = true; break; }
        mag = mag * 10 + d;
    }
    if (!out->overflow)
        out->value = out->negative ? -(long long)mag : (long long)mag;
    return true;                                                /* step 10 — the rules returned a number */
}

bool html_parse_non_negative_integer(const char *s, size_t len, HtmlInteger *out)
{
    if (!html_parse_integer(s, len, out)) return false;         /* step 2 */
    /* Step 3, "if value is less than zero, return an error" — and `-0` is NOT less than zero. The rules define
       a U+002D prefix as "the digits subtracted from zero", so `-0` is the number 0 and parses successfully;
       rejecting on the sign alone is a different algorithm that answers `<body marginwidth="-0">` wrong. */
    return !(out->negative && !(out->digits_len == 1 && out->digits[0] == '0'));
}
