/* css-contain-2 §2's `contain` value. See css_contain.h. */
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "check.h"
#include "core/css/css_contain.h"

#define CT_STRICT (CSS_CONTAIN_SIZE | CSS_CONTAIN_LAYOUT | CSS_CONTAIN_STYLE | CSS_CONTAIN_PAINT)
#define CT_CONTENT (CSS_CONTAIN_LAYOUT | CSS_CONTAIN_STYLE | CSS_CONTAIN_PAINT)

/* The `||` members in the grammar's order, which is also `Canonical order: per grammar`. */
static const struct { const char *word; unsigned bit; } CT_MEMBERS[] = {
    { "size", CSS_CONTAIN_SIZE }, { "inline-size", CSS_CONTAIN_INLINE_SIZE }, { "layout", CSS_CONTAIN_LAYOUT },
    { "style", CSS_CONTAIN_STYLE }, { "paint", CSS_CONTAIN_PAINT },
};

static bool ct_word_is(const char *p, size_t len, const char *word)
{
    return strlen(word) == len && strncasecmp(p, word, len) == 0;
}

bool css_contain_parse(const char *text, unsigned *bits)
{
    const char *p = text;
    unsigned out = 0, words = 0;
    bool single = false;

    DCHECK(text != NULL && bits != NULL, "a `contain` value was parsed with no text or nowhere to put it");
    for (;;) {
        size_t len, i;
        bool known = false;

        while (*p != '\0' && isspace((unsigned char)*p)) p++;
        if (*p == '\0') break;
        for (len = 0; p[len] != '\0' && !isspace((unsigned char)p[len]); len++) { }
        /* `none`, `strict` and `content` are alternatives of the whole value, so each stands alone. */
        if (single) return false;
        if (words == 0 && ct_word_is(p, len, "none")) { known = single = true; }
        else if (words == 0 && ct_word_is(p, len, "strict")) { known = single = true; out = CT_STRICT; }
        else if (words == 0 && ct_word_is(p, len, "content")) { known = single = true; out = CT_CONTENT; }
        else {
            for (i = 0; i < sizeof CT_MEMBERS / sizeof CT_MEMBERS[0]; i++) {
                if (!ct_word_is(p, len, CT_MEMBERS[i].word)) continue;
                /* `||` takes each member at most once, and `size` and `inline-size` are one member's two
                   alternatives. */
                if (out & CT_MEMBERS[i].bit) return false;
                out |= CT_MEMBERS[i].bit;
                known = true;
            }
            if ((out & CSS_CONTAIN_SIZE) && (out & CSS_CONTAIN_INLINE_SIZE)) return false;
        }
        if (!known) return false;
        words++;
        p += len;
    }
    if (words == 0) return false;
    *bits = out;
    return true;
}

char *css_contain_serialize(unsigned bits)
{
    char buf[64];
    size_t i, at = 0;
    char *out;

    /* §6.7.2: "If component values can be omitted or replaced with a shorter representation without changing
       the meaning of the value, omit/replace them", so the two sets §2 names are written as their keywords. */
    if (bits == 0) snprintf(buf, sizeof buf, "none");
    else if (bits == CT_STRICT) snprintf(buf, sizeof buf, "strict");
    else if (bits == CT_CONTENT) snprintf(buf, sizeof buf, "content");
    else {
        buf[0] = '\0';
        for (i = 0; i < sizeof CT_MEMBERS / sizeof CT_MEMBERS[0]; i++)
            if (bits & CT_MEMBERS[i].bit)
                at += (size_t)snprintf(buf + at, sizeof buf - at, "%s%s", at ? " " : "", CT_MEMBERS[i].word);
    }
    out = strdup(buf);
    CHECK(out != NULL, "cssom: OOM serializing a `contain` value");
    return out;
}
