/* INFRA STANDARD §4.7 Strings — see infra_strings.h for why this is one file and why it lives at core/. */
#include <stdlib.h>

#include "check.h"
#include "core/infra_strings.h"

char *infra_normalize_newlines(const char *s, size_t n, size_t *out_n)
{
    char *out;
    size_t i, w = 0;

    DCHECK(s != NULL || n == 0, "Infra §4.7's normalize newlines was given a null string with a non-zero length");
    out = malloc(n + 1);
    CHECK(out != NULL, "infra: OOM normalizing a string's newlines");
    for (i = 0; i < n; i++) {
        if (s[i] == '\r') {
            out[w++] = '\n';
            /* The CR LF PAIR is ONE code point after the first rule, so its LF is consumed HERE. Written as a
               second sweep it would be a newline of its own — see the header. */
            if (i + 1 < n && s[i + 1] == '\n') i++;
        } else {
            out[w++] = s[i];
        }
    }
    out[w] = 0;
    DCHECK(w <= n, "Infra §4.7's normalize newlines produced a longer string than it was given — every rule it "
                   "states replaces one or two code points with exactly one");
    if (out_n) *out_n = w;
    return out;
}
