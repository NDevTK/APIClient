/* CSSOM §2.1's common serializing idioms. See css_serialize.h for why the three live together and why the walk
 * is over CODE POINTS. */
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/css/css_serialize.h"

typedef struct { char *s; size_t len, cap; } SBuf;

static void sbuf_add_n(SBuf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 32;
        char *grown;

        while (cap < b->len + n + 1) cap *= 2;
        grown = realloc(b->s, cap);
        CHECK(grown != NULL, "cssom: OOM serializing a CSS string or identifier");
        b->s = grown;
        b->cap = cap;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = '\0';
}

static void sbuf_add(SBuf *b, const char *s) { sbuf_add_n(b, s, strlen(s)); }

/* §2.1's "escape a character": `\` followed by the character itself. */
static void sbuf_escape_char(SBuf *b, char c) { char two[2] = { '\\', c }; sbuf_add_n(b, two, 2); }

/* §2.1's "escape a character as code point": `\`, the code point in the FEWEST lowercase hex digits, and a
   single SPACE. The trailing space is not optional and is not cosmetic — `\31 23` is the identifier `123` and
   `\3123` is one astral code point. */
static void sbuf_escape_cp(SBuf *b, unsigned cp)
{
    char out[12];

    snprintf(out, sizeof out, "\\%x ", cp);
    sbuf_add(b, out);
}

/* ONE CODE POINT of a UTF-8 string: its value, and how many bytes it took. The length is what the caller
   advances by, so a walk cannot desynchronise from the decode. */
static unsigned cs_next_cp(const char *s, size_t len, size_t i, size_t *plen)
{
    unsigned char c = (unsigned char)s[i];
    size_t n;
    unsigned cp;

    if (c < 0x80)        { *plen = 1; return c; }
    else if (c < 0xE0)   { n = 2; cp = c & 0x1Fu; }
    else if (c < 0xF0)   { n = 3; cp = c & 0x0Fu; }
    else                 { n = 4; cp = c & 0x07u; }
    DCHECK(c >= 0xC2 && i + n <= len,
           "a CSS string reached §2.1's serializer as ILL-FORMED UTF-8. CSSOMString is a USVString in this "
           "binding and the CSS tokenizer's own output is well-formed, so what arrives here is already scalar "
           "values — substituting a replacement character would hide the decoder bug that produced this");
    if (c < 0xC2 || i + n > len) { *plen = 1; return c; }
    {
        size_t k;

        for (k = 1; k < n; k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3Fu);
    }
    *plen = n;
    return cp;
}

char *css_serialize_string(const char *s, size_t len)
{
    SBuf b = { NULL, 0, 0 };
    size_t i = 0;

    DCHECK(s != NULL, "§2.1's serialize a string was given no string — an EMPTY string is \"\", which is a "
                      "value, and the absence of one is a caller that has not decided what it is serializing");
    sbuf_add(&b, "\"");
    while (i < len) {
        size_t n = 1;
        unsigned cp = cs_next_cp(s, len, i, &n);

        if (cp == 0)                              sbuf_add(&b, "\xEF\xBF\xBD");   /* U+FFFD */
        else if (cp <= 0x1F || cp == 0x7F)        sbuf_escape_cp(&b, cp);
        else if (cp == '"' || cp == '\\')         sbuf_escape_char(&b, (char)cp);
        else                                      sbuf_add_n(&b, s + i, n);
        i += n;
    }
    sbuf_add(&b, "\"");
    return b.s;
}

char *css_serialize_identifier(const char *s, size_t len)
{
    SBuf b = { NULL, 0, 0 };
    size_t i = 0;
    unsigned nth = 0;   /* which CHARACTER this is, which §2.1's first/second rules ask about */

    DCHECK(s != NULL, "§2.1's serialize an identifier was given no identifier");
    while (i < len) {
        size_t n = 1;
        unsigned cp = cs_next_cp(s, len, i, &n);

        if (cp == 0)                                       sbuf_add(&b, "\xEF\xBF\xBD");
        else if (cp <= 0x1F || cp == 0x7F)                 sbuf_escape_cp(&b, cp);
        else if (nth == 0 && cp >= '0' && cp <= '9')       sbuf_escape_cp(&b, cp);
        else if (nth == 1 && cp >= '0' && cp <= '9' && s[0] == '-') sbuf_escape_cp(&b, cp);
        /* "the first character and is a `-`, and there is NO SECOND CHARACTER" — a lone `-` is not an
           identifier, so it is escaped as a character; `-x` and `--x` are, and are not. */
        else if (nth == 0 && cp == '-' && len == 1)        sbuf_escape_char(&b, '-');
        else if (cp >= 0x80 || cp == '-' || cp == '_' ||
                 (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z'))
            sbuf_add_n(&b, s + i, n);
        else                                               sbuf_escape_char(&b, (char)cp);
        i += n;
        nth++;
    }
    /* The EMPTY identifier serializes to the empty string, and the buffer above never allocated for it. */
    if (!b.s) { b.s = malloc(1); CHECK(b.s != NULL, "cssom: OOM serializing an empty identifier"); b.s[0] = '\0'; }
    return b.s;
}

char *css_serialize_url(const char *s, size_t len)
{
    char *str = css_serialize_string(s, len);
    size_t n = strlen(str);
    char *out = malloc(n + 6);

    CHECK(out != NULL, "cssom: OOM serializing a URL");
    memcpy(out, "url(", 4);
    memcpy(out + 4, str, n);
    out[4 + n] = ')';
    out[5 + n] = '\0';
    free(str);
    return out;
}
