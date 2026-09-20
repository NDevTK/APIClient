/* css-variables-1 §3 "Using Cascading Variables: the var() notation", the syntax half — see css_var.h for the
 * split, for why a NULL here is CSS Cascade 5 §7.3.3's `unset`, and for what is deliberately not covered. */
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/css/css_var.h"

/* ---- the scan ------------------------------------------------------------------------------------------- */

/* CSS Syntax 3 §4.2 "Definitions"' ident code point, as the question this file actually asks: may this byte
   CONTINUE an identifier? It decides two things and they are the same thing — where a `<custom-property-name>`
   ends, and whether a `var(` is a function token of its own or the tail of a longer ident. `-var(` is the
   function `-var`, not `var`, and a scan that missed that would substitute into somebody else's function.
   A byte at or above 0x80 is admitted whole rather than decoded: §4.2 makes every non-ASCII code point an
   ident code point, so every byte of a UTF-8 sequence is one and no decode can change the answer. */
static bool cv_ident_byte(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-' || c == '\\' || c >= 0x80;
}

static bool cv_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

/* Past the string that OPENS at `i` — CSS Syntax 3 §4.3.5 "Consume a string token", whose only two enders are
   the matching quote and a newline (an unterminated string). The backslash is honoured so that a `"\""` does
   not end early, which is what would otherwise let the rest of a declaration be scanned as though it were
   outside the string, and a `var(` inside somebody's `content` string would then be substituted. */
static size_t cv_skip_string(const char *s, size_t i)
{
    char q = s[i++];

    while (s[i] != '\0') {
        if (s[i] == '\\' && s[i + 1] != '\0') { i += 2; continue; }
        if (s[i] == '\n') return i;          /* §4.3.5's bad-string-token arm: the newline is not consumed */
        if (s[i] == q) return i + 1;
        i++;
    }
    return i;
}

/* Is there a `var(` FUNCTION TOKEN at `i` — the name ASCII case-insensitively, the `(` immediately after it
   (CSS Syntax 3 §4.3.1 admits no whitespace between an ident and the `(` that makes it a function), and `i`
   at an ident BOUNDARY so that the tail of a longer ident is not mistaken for one. */
static bool cv_at_var(const char *s, size_t i)
{
    const char *p = s + i;

    if (i > 0 && cv_ident_byte((unsigned char)s[i - 1])) return false;
    return (p[0] == 'v' || p[0] == 'V') && (p[1] == 'a' || p[1] == 'A') &&
           (p[2] == 'r' || p[2] == 'R') && p[3] == '(';
}

/* The index of the `)` closing a `(` whose CONTENTS begin at `i`, or the length of `s` when the value ends
   first — an unbalanced value, which CSS Syntax 3 §5.4.1 closes implicitly at EOF and which this file treats
   the same way rather than refusing, since the cascaded value it is handed came out of a parser. Strings are
   skipped whole for `cv_skip_string`'s reason and nesting is counted so that `var(--a, rgb(1,2,3))` ends at
   its own paren and not at the inner one. */
static size_t cv_match_paren(const char *s, size_t i)
{
    size_t depth = 1;

    while (s[i] != '\0') {
        char c = s[i];

        if (c == '\\' && s[i + 1] != '\0') { i += 2; continue; }
        if (c == '"' || c == '\'') { i = cv_skip_string(s, i); continue; }
        if (c == '(') depth++;
        else if (c == ')' && --depth == 0) return i;
        i++;
    }
    return i;
}

bool css_var_references(const char *value)
{
    size_t i = 0;

    if (value == NULL) return false;
    while (value[i] != '\0') {
        if (value[i] == '\\' && value[i + 1] != '\0') { i += 2; continue; }
        if (value[i] == '"' || value[i] == '\'') { i = cv_skip_string(value, i); continue; }
        if (cv_at_var(value, i)) return true;
        i++;
    }
    return false;
}

/* ---- the output buffer ---------------------------------------------------------------------------------- */

typedef struct { char *p; size_t n, cap; } CvBuf;

static void cv_put(CvBuf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t want = (b->cap ? b->cap * 2 : 64);

        while (want < b->n + n + 1) want *= 2;
        b->p = realloc(b->p, want);
        CHECK(b->p != NULL, "css-variables-1 §3: OOM composing a substituted declaration value — a dropped "
                            "value would read as a property nobody declared rather than as a failure");
        b->cap = want;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
}

/* ---- §3's substitution ---------------------------------------------------------------------------------- */

char *css_var_substitute(const char *value, CssVarResolve resolve, void *ud)
{
    CvBuf out = { NULL, 0, 0 };
    size_t i = 0, n;

    DCHECK(resolve != NULL,
           "css-variables-1 §3's substitution was asked to run with no resolver — every `var()` names a custom "
           "property and this file answers for none of them by itself, so a NULL resolver is a caller that has "
           "not said which element the names belong to");
    if (value == NULL) return NULL;
    n = strlen(value);
    cv_put(&out, "", 0);                       /* so a value that is entirely one var() still owns a buffer */

    while (value[i] != '\0') {
        size_t open, close, j, name, name_n;
        char *sub = NULL;
        CssVarResolution r;

        if (value[i] == '\\' && value[i + 1] != '\0') { cv_put(&out, value + i, 2); i += 2; continue; }
        if (value[i] == '"' || value[i] == '\'') {
            size_t e = cv_skip_string(value, i);

            cv_put(&out, value + i, e - i);
            i = e;
            continue;
        }
        if (!cv_at_var(value, i)) { cv_put(&out, value + i, 1); i++; continue; }

        open  = i + 4;                          /* past `var(` */
        close = cv_match_paren(value, open);

        /* §3's grammar: `var( <custom-property-name> , <declaration-value>? )`. The first argument is a
           custom property name and nothing else, so a `var(--)`, a `var(width)` or an empty one is a value
           this engine cannot compute — and the honest answer for it is the one §2.2 gives a guaranteed-invalid
           value rather than passing the text through to a property grammar that will report it as a keyword. */
        j = open;
        while (j < close && cv_space((unsigned char)value[j])) j++;
        name = j;
        if (!(j + 1 < close && value[j] == '-' && value[j + 1] == '-')) { free(out.p); return NULL; }
        j += 2;
        while (j < close && cv_ident_byte((unsigned char)value[j])) j++;
        name_n = j - name - 2;                  /* the name WITHOUT its `--`, which css_var.h states */
        if (name_n == 0) { free(out.p); return NULL; }
        while (j < close && cv_space((unsigned char)value[j])) j++;
        if (j < close && value[j] != ',') { free(out.p); return NULL; }

        r = resolve(ud, value + name + 2, name_n, &sub);
        if (r == CSS_VAR_CYCLE) { free(sub); free(out.p); return NULL; }
        if (r == CSS_VAR_RESOLVED) {
            DCHECK(sub != NULL,
                   "a `var()` resolver answered CSS_VAR_RESOLVED and left no value — css_var.h makes the "
                   "value OWNED and non-NULL on that arm, and the two other answers are what a resolver with "
                   "nothing to hand over is for");
            cv_put(&out, sub, strlen(sub));
            free(sub);
            i = close < n ? close + 1 : close;
            continue;
        }
        free(sub);

        /* §2.2's arm: the property is guaranteed-invalid, so "the fallback, if any, is used instead". The
           fallback is EVERYTHING after the first comma — commas included, which is why it is taken as a span
           and not tokenized — and it is `<declaration-value>`, so it may itself contain a `var()` and is
           substituted by the same steps. A `var()` with no comma at all has no fallback to use, and that is
           where the whole declaration becomes invalid at computed-value time. */
        if (j >= close || value[j] != ',') { free(out.p); return NULL; }
        {
            size_t fb = j + 1, fb_n;
            char *raw, *done;

            while (fb < close && cv_space((unsigned char)value[fb])) fb++;
            fb_n = close > fb ? close - fb : 0;
            raw = malloc(fb_n + 1);
            CHECK(raw != NULL, "css-variables-1 §3: OOM taking a `var()` fallback");
            memcpy(raw, value + fb, fb_n);
            raw[fb_n] = '\0';
            done = css_var_substitute(raw, resolve, ud);
            free(raw);
            if (done == NULL) { free(out.p); return NULL; }
            cv_put(&out, done, strlen(done));
            free(done);
        }
        i = close < n ? close + 1 : close;
    }
    return out.p;
}
