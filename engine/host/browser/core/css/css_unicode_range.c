/* css-fonts-4 §4.5 "Character range: the unicode-range descriptor". See css_unicode_range.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/core/base.h>   /* lexbor_realloc: the allocator the engine gives its refusal edge */

#include "check.h"
#include "core/css/css_unicode_range.h"

static bool ur_is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

static int ur_hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Up to six hexadecimal digits at `*p` into `*out`. Returns how many were read. */
static int ur_digits(const char **p, uint32_t *out)
{
    int n = 0;

    *out = 0;
    while (n < 6 && ur_hex(**p) >= 0) { *out = *out * 16 + (uint32_t)ur_hex(*(*p)++); n++; }
    return n;
}

/* One `<unicode-range-token>` at `*p`: §4.5's single codepoint, interval range or wildcard range. "Unicode
   codepoint values must be between 0 and 10FFFF inclusive", "the end codepoint must be greater than or equal
   to the start codepoint", and "Wildcard ranges that extend beyond the range of Unicode codepoints are
   invalid". */
static bool ur_token(const char **p, CssUnicodeRange *out)
{
    const char *s = *p;
    uint32_t lo, hi;
    int digits, wild = 0;

    if ((s[0] != 'u' && s[0] != 'U') || s[1] != '+') return false;
    s += 2;
    digits = ur_digits(&s, &lo);
    while (digits + wild < 6 && *s == '?') { s++; wild++; }
    if (digits + wild == 0) return false;
    if (wild > 0) {
        hi = lo;
        while (wild-- > 0) { lo *= 16; hi = hi * 16 + 15; }
    } else if (*s == '-') {
        s++;
        if (ur_digits(&s, &hi) == 0) return false;
    } else {
        hi = lo;
    }
    if (ur_hex(*s) >= 0 || *s == '?' || *s == '-') return false;   /* a seventh digit, or a second range */
    if (hi > 0x10FFFF || lo > hi) return false;
    out->lo = lo;
    out->hi = hi;
    *p = s;
    return true;
}

bool css_unicode_range_parse(const char *value, CssUnicodeRangeSet *out)
{
    const char *p = value;

    DCHECK(value != NULL && out != NULL && out->v == NULL && out->n == 0,
           "css-fonts-4 §4.5's parse was asked of no value or into a set that already holds ranges");
    for (;;) {
        CssUnicodeRange r;

        while (ur_is_ws(*p)) p++;
        if (!ur_token(&p, &r)) break;
        if (out->n == out->cap) {
            size_t cap = out->cap ? out->cap * 2 : 4;
            CssUnicodeRange *grown = lexbor_realloc(out->v, cap * sizeof *grown);

            CHECK(grown != NULL, "cssom: OOM growing a css-fonts-4 §4.5 unicode-range list");
            out->v = grown;
            out->cap = cap;
        }
        out->v[out->n++] = r;
        while (ur_is_ws(*p)) p++;
        if (*p == '\0') return true;
        if (*p != ',') break;
        p++;
    }
    css_unicode_range_free(out);
    return false;
}

void css_unicode_range_free(CssUnicodeRangeSet *s)
{
    free(s->v);
    s->v = NULL;
    s->n = s->cap = 0;
}

bool css_unicode_range_includes(const CssUnicodeRangeSet *s, uint32_t cp)
{
    size_t i;

    for (i = 0; i < s->n; i++) {
        DCHECK(s->v[i].lo <= s->v[i].hi && s->v[i].hi <= 0x10FFFF,
               "a parsed unicode-range interval is inverted or runs past U+10FFFF, which the parse refuses");
        if (cp >= s->v[i].lo && cp <= s->v[i].hi) return true;
    }
    return false;
}

char *css_unicode_range_descriptor_value(const char *value)
{
    CssUnicodeRangeSet set = { NULL, 0, 0 };
    char *out;
    size_t i, at = 0;

    if (value == NULL || !css_unicode_range_parse(value, &set)) return NULL;
    /* "U+10FFFF-10FFFF, " is the longest item: 17 bytes. */
    out = lexbor_malloc(set.n * 17 + 1);
    CHECK(out != NULL, "cssom: OOM serializing a css-fonts-4 §4.5 unicode-range list");
    for (i = 0; i < set.n; i++) {
        if (i) at += (size_t)sprintf(out + at, ", ");
        if (set.v[i].lo == set.v[i].hi) at += (size_t)sprintf(out + at, "U+%X", (unsigned)set.v[i].lo);
        else at += (size_t)sprintf(out + at, "U+%X-%X", (unsigned)set.v[i].lo, (unsigned)set.v[i].hi);
    }
    out[at] = '\0';
    css_unicode_range_free(&set);
    return out;
}
