/* Fetch's "determine nosniff" — see nosniff.h for why the flag is computed here and not by the caller. */
#include "network/nosniff.h"
#include "check.h"
#include <stddef.h>
#include <string.h>

/* Fetch's HTTP TAB OR SPACE, which is what "getting, decoding, and splitting" strips from each token's ends.
   It is NOT the ASCII-whitespace set: CR and LF cannot appear inside a header value at all, and a form feed is
   not stripped by this algorithm. */
static bool http_tab_or_space(char c)
{
    return c == 0x09 || c == 0x20;
}

static bool ascii_ci_equal(const char *s, size_t n, const char *lit)
{
    size_t i;
    for (i = 0; i < n; i++) {
        char a = s[i], b = lit[i];
        if (!b) return false;
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != b) return false;
    }
    return lit[n] == 0;
}

bool nosniff_determine(const char *value)
{
    size_t start, end;
    size_t i = 0;
    bool quoted = false;

    /* Step 1's "values is null" — the header was not in the list. */
    if (!value) return false;

    /* "get, decode, and split" — collect codepoints up to the first U+002C that is not inside a quoted string,
       which is the FIRST value and the only one this algorithm looks at. The quoted-string state is what makes
       this a split rather than a `strchr`: `X-Content-Type-Options: "a,b", nosniff` has ONE comma at value
       level, and a naive cut would hand step 3 the token `"a` and answer the wrong question about it. */
    while (value[i]) {
        char c = value[i];
        if (quoted) {
            if (c == '\\' && value[i + 1]) { i += 2; continue; }
            if (c == '"') quoted = false;
        } else {
            if (c == ',') break;
            if (c == '"') quoted = true;
        }
        i++;
    }

    /* "strip leading and trailing HTTP tab or space bytes from value" */
    start = 0;
    end = i;
    while (start < end && http_tab_or_space(value[start])) start++;
    while (end > start && http_tab_or_space(value[end - 1])) end--;

    DCHECK(end >= start, "the nosniff header's first value ended before it began — both bounds walk the same "
                         "buffer from opposite ends and stop at each other, so a crossed pair is a strip that "
                         "ran past its own guard and would be read as a length of nearly SIZE_MAX");

    /* Step 3: an ASCII case-insensitive match for "nosniff". Not a substring test — that is what the JS this
       replaces did, and it set the flag for `foo, nosniff`, whose FIRST value is `foo`. */
    return ascii_ci_equal(value + start, end - start, "nosniff");
}
