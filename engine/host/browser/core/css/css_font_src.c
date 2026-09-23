/* See css_font_src.h. */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/css/css_code_point.h"
#include "core/css/css_font_family.h"
#include "core/css/css_font_src.h"
#include "core/css/css_serialize.h"
#include "core/css/css_var.h"
#include "core/html/enumerated_attribute.h"

/* ---- the LIST css-fonts-4 §4.3.1's `<font-src-list>` describes ---------------------------------------------
 *
 * ONE ITEM IS EXACTLY ONE ALTERNATIVE OF `<font-src> = <url> [ format( <font-format> ) ]? [ tech( <font-tech># )
 * ]? | local( <font-family-name> )`, and the struct says so: `url` and `local` are the two alternatives and
 * precisely one is non-NULL. They are not one field because they SERIALIZE BY DIFFERENT RULES — a url goes out
 * through CSSOM §2.1 "Common Serializing Idioms"' serialize a URL and a local name through the entry
 * core/css/css_font_family.h owns — and `format`/`tech` belong to the `<url>` alternative ALONE, which a single
 * field could not express.
 *
 * THE LIST GROWS AND IS NOT A FIXED ARRAY, for the reason core/css/css_font_family.c states about §2.1's `#`:
 * the list has no upper bound, so a fixed buffer that dropped a long fallback stack would be a CAP wearing a
 * parse error — and a real page's `src` is exactly the value that stacks woff2, woff and ttf against one
 * family. */
typedef struct {
    char *url;              /* the DECODED `<url>`, UTF-8, OWNED. NULL for a `local()` item. */
    char *local;            /* the SERIALIZED `<font-family-name>`, OWNED. NULL for a `<url>` item. */
    const char *format_kw;  /* a row of FONT_FORMAT below, or NULL. NOT owned. */
    char *format_str;       /* the DECODED `<string>` arm of `<font-format>`, OWNED, or NULL. */
    const char **tech;      /* rows of FONT_TECH below, in the page's own order. The ARRAY is owned; the rows are not. */
    size_t ntech, captech;
} SrcItem;

typedef struct {
    SrcItem *v;
    size_t n, cap;
} SrcList;

typedef struct {
    char *s;
    size_t n, cap;
} SrcBuf;

/* css-fonts-4 §4.3.1 "Parsing the src descriptor"'s `<font-format>`, KEYWORD ARMS ONLY — its `<string>` arm is
   an open set and is carried on the item instead. The spec writes them
     `<font-format> = [ <string> | collection | embedded-opentype | opentype | svg | truetype | woff | woff2 ]`
   and this table is already the ASCII-LOWERCASED spelling, which is both the canonical form a CSS keyword
   serializes as and the form the corpus asserts — `css/css-fonts/parsing/font-face-src-tech.html` compares a
   round-tripped keyword list against the page's own "toLowerCase()"d one and cites CSSOM's serialize a CSS
   component value for it. Matching is ASCII case-insensitive either way, so no information is lost by storing
   the answer rather than the author's spelling. */
static const char *const FONT_FORMAT[] = {
    "collection", "embedded-opentype", "opentype", "svg", "truetype", "woff", "woff2",
};

/* §4.3.1's `<font-tech>`, FLATTENED FROM ITS THREE PRODUCTIONS because they are alternatives of one another and
   nothing here asks which of the three a keyword came from:
     `<font-tech> = [ <font-features-tech> | <color-font-tech> | variations | palettes | incremental ]`
     `<font-features-tech> = [ features-opentype | features-aat | features-graphite ]`
     `<color-font-tech> = [ color-COLRv0 | color-COLRv1 | color-SVG | color-sbix | color-CBDT ]`
   ASCII-LOWERCASED for the reason the table above gives; the spec's own mixed-case spelling is in the lines
   above so a reader comparing the two can see that lowercasing is the only difference.
   THERE IS NO `<string>` ARM HERE AND THAT IS THE DIFFERENCE FROM `<font-format>`, not an omission: the
   corpus asserts `tech("features-opentype")` INVALID beside `format("woff")` valid, which is the two
   productions' `Value:` lines read literally. */
static const char *const FONT_TECH[] = {
    "features-opentype", "features-aat", "features-graphite",
    "color-colrv0", "color-colrv1", "color-svg", "color-sbix", "color-cbdt",
    "variations", "palettes", "incremental",
};

/* ---- the buffer -------------------------------------------------------------------------------------------- */

static void src_buf_reserve(SrcBuf *b, size_t more)
{
    if (b->s != NULL && b->n + more + 1 <= b->cap) return;
    while (b->cap < b->n + more + 1) b->cap = b->cap ? b->cap * 2 : 32;
    b->s = realloc(b->s, b->cap);
    CHECK(b->s != NULL, "cssom: OOM building a css-fonts-4 §4.3 `src` value — a dropped declaration reads back "
                        "as UNDECLARED, and §4.1 says an `@font-face` rule without a `src` is not considered "
                        "when performing the font matching algorithm at all");
    b->s[b->n] = '\0';
}

static void src_buf_add(SrcBuf *b, const char *s, size_t n)
{
    src_buf_reserve(b, n);
    if (n) memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = '\0';
}

/* A CODE POINT, RE-ENCODED RATHER THAN COPIED, which core/css/css_code_point.h requires of every caller that
   copies source text: "for a filtered code point they are different: the two bytes of a CRLF, the one byte of
   a NUL and the bytes of an ill-formed sequence each stand for a code point the source does not spell." */
static void src_buf_add_cp(SrcBuf *b, uint32_t cp)
{
    DCHECK(cp <= 0x10FFFF,
           "a code point above CSS Syntax §4.2 \"Definitions\"' maximum allowed code point reached the `src` "
           "encoder. Its only two producers are css_cp_at, which never answers one, and CSS Syntax §4.3.7 "
           "\"Consume an escaped code point\", whose own step answers U+FFFD for a value \"greater than the "
           "maximum allowed code point\"");
    src_buf_reserve(b, 4);
    if (cp < 0x80) {
        b->s[b->n++] = (char)cp;
    } else if (cp < 0x800) {
        b->s[b->n++] = (char)(0xC0 | (cp >> 6));
        b->s[b->n++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        b->s[b->n++] = (char)(0xE0 | (cp >> 12));
        b->s[b->n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b->s[b->n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        b->s[b->n++] = (char)(0xF0 | (cp >> 18));
        b->s[b->n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b->s[b->n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b->s[b->n++] = (char)(0x80 | (cp & 0x3F));
    }
    b->s[b->n] = '\0';
}

/* ---- CSS Syntax's code-point walk ---------------------------------------------------------------------------
 *
 * THE §4.3 SUBSET THIS GRAMMAR NEEDS, WRITTEN HERE RATHER THAN SHARED, WHICH IS THIS TREE'S STATED SHAPE AND
 * NOT A FOURTH COPY BY ACCIDENT. core/css/css_code_point.h says it in its own words: "§4.3's algorithms — a
 * valid escape, would-start-an-ident-sequence, consume an ident sequence — are each grammar's own, because
 * each of the three callers implements only the part of §4.3 its own production needs and a shared one would
 * be a fourth CSS tokenizer beside lexbor's. This file answers about ONE code point, which is the part all
 * three genuinely share." What IS shared is that file — every predicate below asks it rather than testing
 * bytes, which is what keeps `\xC3\x97` out of an identifier here as well.
 * THIS GRAMMAR NEEDS ONE ALGORITHM NONE OF THE OTHERS DOES: §4.3.6 "Consume a url token", which is the whole
 * reason `url(not a valid url/bar.ttf)` is a parse error and a balanced-paren read would accept it. */

/* CSS Syntax §4.2 "Definitions"' WHITESPACE: "A newline, U+0009 CHARACTER TABULATION, or U+0020 SPACE." §3.3
   "Preprocessing the input stream" has already turned every CR, FF and CRLF into ONE U+000A. */
static bool src_is_ws(uint32_t cp)
{
    return cp == 0x000A || cp == 0x0009 || cp == 0x0020;
}

/* §4.2's NON-PRINTABLE CODE POINT: "A code point between U+0000 NULL and U+0008 BACKSPACE inclusive, or U+000B
   LINE TABULATION, or a code point between U+000E SHIFT OUT and U+001F INFORMATION SEPARATOR ONE inclusive, or
   U+007F DELETE." Asked by §4.3.6 alone, which is why it is here and not in the shared file. */
static bool src_is_non_printable(uint32_t cp)
{
    return cp <= 0x0008 || cp == 0x000B || (cp >= 0x000E && cp <= 0x001F) || cp == 0x007F;
}

static void src_skip_ws(const char **p, const char *end)
{
    size_t n = 0;

    while (src_is_ws(css_cp_at(*p, end, &n))) *p += n;
}

/* CSS Syntax §4.3.8 "Check if two code points are a valid escape", asked where the `\` is the code point AT
   `p`: "If the first code point is not U+005C REVERSE SOLIDUS (\), return false. Otherwise, if the second code
   point is a newline, return false. Otherwise, return true." */
static bool src_valid_escape(const char *p, const char *end)
{
    size_t n = 0;
    uint32_t cp = css_cp_at(p, end, &n);

    if (cp != '\\') return false;
    cp = css_cp_at(p + n, end, NULL);
    return cp != CSS_CP_EOF && cp != 0x000A;
}

/* CSS Syntax §4.3.7 "Consume an escaped code point", entered with the `\` at `*p` NOT yet consumed. */
static uint32_t src_consume_escape(const char **p, const char *end)
{
    size_t n = 0;
    uint32_t cp, v = 0;
    int digits = 0;

    cp = css_cp_at(*p, end, &n);
    DCHECK(cp == '\\', "CSS Syntax §4.3.7 \"Consume an escaped code point\" was entered at a code point that "
                       "is not U+005C REVERSE SOLIDUS. It \"assumes that the U+005C REVERSE SOLIDUS (\\) has "
                       "already been consumed\", so every caller here stands at the backslash itself and one "
                       "that did not would read the following code point as a hex digit count");
    *p += n;
    cp = css_cp_at(*p, end, &n);
    if (cp == CSS_CP_EOF) return 0xFFFD;        /* "EOF / This is a parse error. Return U+FFFD" */
    if (!((cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'f') || (cp >= 'A' && cp <= 'F'))) {
        *p += n;
        return cp;                              /* "anything else / Return the current input code point." */
    }
    /* "Consume as many hex digits as possible, but no more than 5. Note that this means 1-6 hex digits have
       been consumed in total." */
    while (digits < 6) {
        cp = css_cp_at(*p, end, &n);
        if (cp >= '0' && cp <= '9')      v = v * 16 + (cp - '0');
        else if (cp >= 'a' && cp <= 'f') v = v * 16 + (cp - 'a' + 10);
        else if (cp >= 'A' && cp <= 'F') v = v * 16 + (cp - 'A' + 10);
        else break;
        *p += n;
        digits++;
    }
    /* "If the next input code point is whitespace, consume it as well." ONE code point and not a run. */
    cp = css_cp_at(*p, end, &n);
    if (src_is_ws(cp)) *p += n;
    /* "If this number is zero, or is for a surrogate, or is greater than the maximum allowed code point,
       return U+FFFD REPLACEMENT CHARACTER." */
    if (v == 0 || (v >= 0xD800 && v <= 0xDFFF) || v > 0x10FFFF) return 0xFFFD;
    return v;
}

/* CSS Syntax §4.3.9 "Check if three code points would start an ident sequence", over the code points at `p`. */
static bool src_would_start_ident(const char *p, const char *end)
{
    size_t n = 0;
    uint32_t cp = css_cp_at(p, end, &n), cp2;

    if (cp == '-') {
        cp2 = css_cp_at(p + n, end, NULL);
        return css_cp_is_ident_start(cp2) || cp2 == '-' || src_valid_escape(p + n, end);
    }
    if (cp != CSS_CP_EOF && css_cp_is_ident_start(cp)) return true;
    return cp == '\\' && src_valid_escape(p, end);
}

/* CSS Syntax §4.3.12 "Consume an ident sequence". THE ANSWER IS THE DECODED SEQUENCE AND NOT ITS SPELLING, so
   `\66 ormat(x)` names the same function `format(x)` does — which is the whole reason the lead token of a
   `<font-src>` is read by this walk rather than compared as bytes. */
static char *src_consume_ident(const char **p, const char *end)
{
    SrcBuf b = { 0 };
    size_t n = 0;
    uint32_t cp;

    DCHECK(src_would_start_ident(*p, end),
           "CSS Syntax §4.3.12 \"Consume an ident sequence\" was entered where §4.3.9 \"Check if three code "
           "points would start an ident sequence\" answers false. The two are one step of one algorithm and "
           "the failure is silent: an ident consumed from a position that starts none is a `<font-src>` this "
           "engine accepts and a browser refuses");
    for (;;) {
        cp = css_cp_at(*p, end, &n);
        if (cp != CSS_CP_EOF && css_cp_is_ident(cp)) { src_buf_add_cp(&b, cp); *p += n; continue; }
        if (src_valid_escape(*p, end))               { src_buf_add_cp(&b, src_consume_escape(p, end)); continue; }
        break;
    }
    DCHECK(b.s != NULL && b.n > 0,
           "CSS Syntax §4.3.12 \"Consume an ident sequence\" produced an EMPTY ident from a position §4.3.9 "
           "answered true for. Each of §4.3.9's three arms names a code point §4.3.12's loop then consumes, so "
           "an empty result means the two have come apart");
    return b.s;
}

/* CSS Syntax §4.3.5 "Consume a string token", entered with the opening quote at `*p` NOT yet consumed. NULL is
   §4.3.5's `<bad-string-token>` — the newline arm — which no grammar admits. An unterminated string at EOF is
   a `<string-token>` and NOT a bad one, which is §4.3.5's own EOF arm. */
static char *src_consume_string(const char **p, const char *end)
{
    SrcBuf b = { 0 };
    size_t n = 0, n2 = 0;
    uint32_t quote, cp, next;

    quote = css_cp_at(*p, end, &n);
    DCHECK(quote == '"' || quote == '\'',
           "CSS Syntax §4.3.5 \"Consume a string token\" was entered at a code point that is neither of the two "
           "quotes its \"ending code point\" can be, so the token would end at the first of EITHER quote and a "
           "url carrying the other would be split in half");
    *p += n;
    for (;;) {
        cp = css_cp_at(*p, end, &n);
        if (cp == CSS_CP_EOF) break;                    /* the EOF arm: the `<string-token>` stands */
        if (cp == quote) { *p += n; break; }
        if (cp == 0x000A) { free(b.s); return NULL; }   /* the newline arm: a `<bad-string-token>` */
        if (cp == '\\') {
            next = css_cp_at(*p + n, end, &n2);
            /* CSS Syntax §4.3.5's U+005C arm: "If the next input code point is EOF, do nothing." and "Otherwise, if the
               next input code point is a newline, consume it." BOTH APPEND NOTHING. */
            if (next == CSS_CP_EOF) { *p += n; continue; }
            if (next == 0x000A)     { *p += n + n2; continue; }
            src_buf_add_cp(&b, src_consume_escape(p, end));
            continue;
        }
        src_buf_add_cp(&b, cp);
        *p += n;
    }
    /* `url("")` is a `<string>` whose value is the empty string — an EMPTY url, which is a FETCHING question
       and not a parse one, so the empty buffer is materialized rather than refused. */
    if (!b.s) src_buf_reserve(&b, 0);
    return b.s;
}

/* CSS Syntax §4.3.6 "Consume a url token", entered just past the `url(` with `end` standing where the token's
   own U+0029 RIGHT PARENTHESIS would be — the caller has already found the balanced close, so §4.3.6's
   "U+0029 RIGHT PARENTHESIS" and "EOF" arms coincide here and both "Return the <url-token>".
   NULL IS §4.3.6's `<bad-url-token>`, which no grammar admits, and it is the whole reason this walk exists
   rather than a span copy: `url(not a valid url/bar.ttf)` is a bad url token — the whitespace arm requires
   U+0029 or EOF after the run and finds `a` — and the corpus asserts that component is a parse error.
   §4.3.15 "Consume the remnants of a bad url" IS NOT PERFORMED, and the reason is that its whole job is to
   put the STREAM back at a recoverable point: "Its sole use is to consume enough of the input stream to reach
   a recovery point." The caller has already sliced this component out at a top-level comma, so the recovery
   point is the end of the slice and there is nothing left to consume to. */
static char *src_consume_url_token(const char *p, const char *end)
{
    SrcBuf b = { 0 };
    size_t n = 0;
    uint32_t cp;

    src_skip_ws(&p, end);                       /* "Consume as much whitespace as possible." */
    for (;;) {
        cp = css_cp_at(p, end, &n);
        if (cp == CSS_CP_EOF) break;            /* the EOF arm, which is this caller's U+0029 */
        if (src_is_ws(cp)) {
            src_skip_ws(&p, end);
            /* "If the next input code point is U+0029 RIGHT PARENTHESIS ()) or EOF, consume it and return the
               <url-token> ... otherwise, consume the remnants of a bad url, create a <bad-url-token>". */
            if (css_cp_at(p, end, NULL) == CSS_CP_EOF) break;
            free(b.s);
            return NULL;
        }
        if (cp == '"' || cp == '\'' || cp == '(' || src_is_non_printable(cp)) { free(b.s); return NULL; }
        if (cp == '\\') {
            if (!src_valid_escape(p, end)) { free(b.s); return NULL; }
            src_buf_add_cp(&b, src_consume_escape(&p, end));
            continue;
        }
        src_buf_add_cp(&b, cp);                 /* "anything else / Append the current input code point" */
        p += n;
    }
    /* "Initially create a <url-token> with its value set to the empty string" — `url()` is a url token whose
       value is empty, exactly as `url("")` is. */
    if (!b.s) src_buf_reserve(&b, 0);
    return b.s;
}

/* ---- the COMMENT PASS and the TOP-LEVEL SPLIT --------------------------------------------------------------- */

/* CSS Syntax §4.3.1 "Consume a token"'s first step — "Consume comments" — over the WHOLE value, with each
 * comment REPLACED BY ONE U+0020 SPACE rather than deleted.
 *
 * THE SPACE IS SOUND BECAUSE OF THIS GRAMMAR AND NOT IN GENERAL, which is the same sentence
 * core/css/css_font_family.c writes about its own pass and the reason neither is shared. In `<font-src>` a
 * comment only ever stands where whitespace may stand, and whitespace only ever SEPARATES tokens: the walk
 * below skips a run of it at every boundary. The one place the DIFFERENCE is observable is a comment between
 * a function's name and its parenthesis — CSS Syntax §4.3.4 "Consume an ident-like token" makes a
 * `<function-token>` only when "the next input code point is U+0028 LEFT PARENTHESIS", IMMEDIATELY — and a
 * space there is exactly what makes a `format` written with a comment between its name and its parenthesis
 * TWO tokens rather than one function, which is the browser's answer and a deletion's would not be.
 *
 * IT SKIPS A `<string>` AND AN ESCAPE THROUGH THE WALKS ABOVE, so nothing here holds a second opinion about
 * where either ends: a SOLIDUS-ASTERISK inside a quoted url is two characters of that url.
 * OWNED, and never NULL — a value that is nothing but a comment answers the EMPTY STRING, which the split
 * below produces one empty component from and the item parse then refuses. */
static char *src_strip_comments(const char *value)
{
    SrcBuf out = { 0 };
    const char *p = value, *end = value + strlen(value);

    while (p < end) {
        size_t n = 0, n2 = 0;
        uint32_t cp = css_cp_at(p, end, &n);

        if (cp == '/' && css_cp_at(p + n, end, &n2) == '*') {
            p += n + n2;
            for (;;) {
                cp = css_cp_at(p, end, &n);
                /* "up to and including the first U+002A ASTERISK (*) followed by a U+002F SOLIDUS (/), or up
                   to an EOF code point" — the EOF arm is §4.3.2's own parse error and the comment still stands
                   as consumed, so an unterminated comment ends the value rather than invalidating it. */
                if (cp == CSS_CP_EOF) break;
                if (cp == '*' && css_cp_at(p + n, end, &n2) == '/') { p += n + n2; break; }
                p += n;
            }
            src_buf_add(&out, " ", 1);
            continue;
        }
        if (cp == '"' || cp == '\'') {
            const char *begin = p;

            free(src_consume_string(&p, end));
            DCHECK(p > begin,
                   "CSS Syntax §4.3.5 \"Consume a string token\" advanced over NO code points from a quote. "
                   "Every arm of it consumes at least the opening quote, so a walk that did not move would "
                   "re-enter this arm at the same byte and never reach the end of the value");
            src_buf_add(&out, begin, (size_t)(p - begin));
            continue;
        }
        if (cp == '\\' && src_valid_escape(p, end)) {
            css_cp_at(p + n, end, &n2);
            src_buf_add(&out, p, n + n2);
            p += n + n2;
            continue;
        }
        /* THE BYTES AND NOT THE CODE POINT, which is what core/css/css_code_point.h's rule permits for a pass
           that hands its own walk back to itself rather than copying text FOR a consumer: what this emits is
           read by css_cp_at again, so a CRLF re-filtered to one U+000A is the identical answer. */
        src_buf_add(&out, p, n);
        p += n;
    }
    if (!out.s) src_buf_reserve(&out, 0);
    return out.s;
}

/* THE NEXT TOP-LEVEL COMPONENT of the comma-separated list, from `*p`, reported as the span [`*start`,`*stop`)
   and leaving `*p` past the comma (or at `end`). Answers false only when `*p` is already `end` AND the
   previous call ended at `end` rather than at a comma — which is what makes `a, b,` three components, the
   last of them empty, rather than two.
   IT IS A LEXICAL SPLIT AND NOT A TOKENIZATION, and the bound on that is what makes it safe: it skips strings
   and escapes and counts parentheses, so the only way it can DISAGREE with CSS Syntax is by treating a comma
   as nested when a real tokenizer would not — which MERGES two components into one that then fails the item
   grammar. It can never split one component into two, so no value it mis-reads is ACCEPTED; §4.3.1's "If
   there are no supported entries at the end of this process" is the outcome either way. */
static void src_next_component(const char **p, const char *end, const char **start, const char **stop)
{
    const char *q = *p;
    unsigned depth = 0;

    *start = q;
    while (q < end) {
        size_t n = 0;
        uint32_t cp = css_cp_at(q, end, &n);

        if (cp == '"' || cp == '\'') { free(src_consume_string(&q, end)); continue; }
        if (cp == '\\' && src_valid_escape(q, end)) {
            size_t n2 = 0;

            css_cp_at(q + n, end, &n2);
            q += n + n2;
            continue;
        }
        if (cp == '(') depth++;
        else if (cp == ')') { if (depth) depth--; }
        else if (cp == ',' && depth == 0) { *stop = q; *p = q + n; return; }
        q += n;
    }
    *stop = end;
    *p = end;
}

/* ---- css-fonts-4 §4.3.1's PARSE ------------------------------------------------------------------------------ */

static void src_item_free(SrcItem *it)
{
    free(it->url);
    free(it->local);
    free(it->format_str);
    free(it->tech);
    memset(it, 0, sizeof(*it));
}

static void src_list_free(SrcList *l)
{
    size_t i;

    for (i = 0; i < l->n; i++) src_item_free(&l->v[i]);
    free(l->v);
    l->v = NULL;
    l->n = l->cap = 0;
}

static void src_list_add(SrcList *l, const SrcItem *it)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 4;
        l->v = realloc(l->v, l->cap * sizeof(*l->v));
        CHECK(l->v != NULL, "cssom: OOM growing a css-fonts-4 §4.3 `<font-src-list>`, whose comma-separated "
                            "list has no upper bound");
    }
    l->v[l->n++] = *it;
}

static void src_tech_add(SrcItem *it, const char *kw)
{
    if (it->ntech == it->captech) {
        it->captech = it->captech ? it->captech * 2 : 4;
        it->tech = realloc(it->tech, it->captech * sizeof(*it->tech));
        CHECK(it->tech != NULL, "cssom: OOM growing a css-fonts-4 §4.3.1 `<font-tech>#` list, whose `#` has no "
                                "upper bound");
    }
    it->tech[it->ntech++] = kw;
}

/* A NUL-terminated copy of a span, for the one place a span has to leave this file: `local()`'s argument goes
   to core/css/css_font_family.h's descriptor entry, which takes a C string. OWNED. */
static char *src_span_dup(const char *s, const char *e)
{
    size_t n = (size_t)(e - s);
    char *out = malloc(n + 1);

    CHECK(out != NULL, "cssom: OOM copying a css-fonts-4 §4.3.1 component value");
    if (n) memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

/* IS THE NEXT TOKEN THE FUNCTION `name`, and if so what is its ARGUMENT SPAN. On true `*p` is advanced past
   the whole notation; on false NOTHING is consumed, which is what lets `format` and `tech` be tried in the
   order `<font-src>` writes them without either one eating the other's token.
   AN UNCLOSED FUNCTION'S ARGUMENT RUNS TO THE END OF THE COMPONENT, which is CSS Syntax §5.5.10 "Consume a
   function"'s own arm for it: that step "Process input" gives `<eof-token>` and `<)-token>` ONE answer —
   "Discard a token from input. Return function." — so a function that meets the end of the stream is the same
   function a closing parenthesis would have produced, and nothing here has to invent a recovery.
   THAT CITATION READ `§4.3.3 "Consume a function"`, WITH `This is a parse error. Return the function.`
   UNDER IT, AND BOTH HALVES WERE WRONG — csssyntax3 numbers "Consume a numeric token" at §4.3.3, and the
   retired sentence is an earlier edition's tokenizer wording the current draft does not carry anywhere. It is
   recorded rather than quietly repaired because of WHERE it came from: consuming a function is a §5 PARSING
   step and not a §4 TOKENIZING one, so a reader who reaches for it while writing a tokenizer walk lands in
   §4.3 and finds a plausible number there. Neither the title channel nor the quotation channel reported it;
   reading the corpus's own heading list did, which is why the retired spelling is shown in BACKTICKS here —
   a run in quotation marks is a CLAIM this tree makes, and this one is being withdrawn. */
static bool src_take_function(const char **p, const char *end, const char *name,
                              const char **astart, const char **astop)
{
    const char *q = *p, *a;
    char *ident;
    unsigned depth = 1;
    bool is;

    src_skip_ws(&q, end);
    if (!src_would_start_ident(q, end)) return false;
    ident = src_consume_ident(&q, end);
    is = enumerated_attribute_keyword_match(name, ident, strlen(ident));
    free(ident);
    /* §4.3.4 "Consume an ident-like token": a `<function-token>` is an ident sequence whose next input code
       point is U+0028 LEFT PARENTHESIS, with nothing between — so `format (woff2)` is an ident followed by a
       block and is not this function. */
    if (!is || css_cp_at(q, end, NULL) != '(') return false;
    q++;                        /* U+0028 is one byte, and css_cp_at answered it as one code point */
    a = q;
    while (q < end && depth) {
        size_t n = 0;
        uint32_t cp = css_cp_at(q, end, &n);

        if (cp == '"' || cp == '\'') { free(src_consume_string(&q, end)); continue; }
        if (cp == '\\' && src_valid_escape(q, end)) {
            size_t n2 = 0;

            css_cp_at(q + n, end, &n2);
            q += n + n2;
            continue;
        }
        if (cp == '(') depth++;
        else if (cp == ')') { depth--; if (!depth) { *astart = a; *astop = q; *p = q + n; return true; } }
        q += n;
    }
    *astart = a;
    *astop = end;
    *p = end;
    return true;
}

/* `<url>`'s two written forms, over a `url(`'s or a `src(`'s ARGUMENT SPAN. css-values-4 §4.5 "Resource
 * Locators: the <url> type":
 *     <url> = <url()> | <src()>
 *     <url()> = url( <string> <url-modifier>* ) | <url-token>
 *     <src()> = src( <string> <url-modifier>* )
 * THE `<url-modifier>*` TAIL IS ACCEPTED WHOLE and that is the grammar rather than a shortcut, which is the
 * same reading core/css/css_image.c states at its own `<url>`: css-values-4 §4.5.3 "URL Modifiers" says "This
 * specification does not define any <url-modifier>s, but other specs may do so" and makes one "either an
 * <ident> or a functional notation", so every remainder of a balanced argument list matches.
 * WHICH ARM `url(` TAKES IS §4.3.4's AND NOT A PREFERENCE: "if the next one or two input code points are
 * U+0022 QUOTATION MARK ("), U+0027 APOSTROPHE ('), or whitespace followed by U+0022 QUOTATION MARK (") or
 * U+0027 APOSTROPHE ('), then create a <function-token> ... otherwise, consume a url token and return it." So
 * a quote after any run of whitespace is the function arm, and anything else is §4.3.6's.
 * `src()` HAS NO url-token ARM AT ALL, so an unquoted first argument is outside the grammar. */
static char *src_take_url(const char *a, const char *e, bool url_form)
{
    const char *p = a;
    uint32_t cp;

    src_skip_ws(&p, e);
    cp = css_cp_at(p, e, NULL);
    if (cp == '"' || cp == '\'') {
        char *s = src_consume_string(&p, e);

        if (!s) return NULL;                    /* a `<bad-string-token>`, which no production admits */
        return s;                               /* the `<url-modifier>*` tail is accepted whole */
    }
    if (!url_form) return NULL;                 /* `<src()>`'s first argument must be a `<string>` */
    return src_consume_url_token(a, e);
}

/* `format( <font-format> )`'s ARGUMENT, over the span. `<font-format>` is ONE component value — there is no
   multiplier on it — so a second one is outside the grammar, which is what makes `format(opentype, truetype)`
   and `format("opentype", "truetype")` parse errors and not two-entry lists. */
static bool src_take_format(SrcItem *it, const char *a, const char *e)
{
    const char *p = a;
    uint32_t cp;
    size_t i;

    src_skip_ws(&p, e);
    cp = css_cp_at(p, e, NULL);
    if (cp == '"' || cp == '\'') {
        /* The `<string>` arm is an OPEN SET: §4.3.1's legacy table names nine strings that "have the same
           effect as if the equivalent modern syntax had been used", and the production admits any `<string>`
           at all. A string outside the table names a format nothing activates, which is §4.3.3 "Selecting
           items in the src"'s skip-downloading question and not this one. */
        it->format_str = src_consume_string(&p, e);
        if (!it->format_str) return false;
    } else {
        char *ident;

        if (!src_would_start_ident(p, e)) return false;     /* `format()` is empty and matches nothing */
        ident = src_consume_ident(&p, e);
        for (i = 0; i < COUNTOF(FONT_FORMAT); i++)
            if (enumerated_attribute_keyword_match(FONT_FORMAT[i], ident, strlen(ident))) {
                it->format_kw = FONT_FORMAT[i];
                break;
            }
        free(ident);
        if (!it->format_kw) return false;       /* an identifier outside §4.3.1's closed keyword set */
    }
    src_skip_ws(&p, e);
    return css_cp_at(p, e, NULL) == CSS_CP_EOF;
}

/* `tech( <font-tech># )`'s ARGUMENT, over the span. `#` is "one or more, comma-separated", so an EMPTY
   `tech()` matches nothing and a whitespace-separated pair is not a list — both of which the corpus asserts.
   THERE IS NO `<string>` ARM, unlike `format()`: `<font-tech>`'s `Value:` line is keywords only. */
static bool src_take_tech(SrcItem *it, const char *a, const char *e)
{
    const char *p = a;

    for (;;) {
        char *ident;
        const char *kw = NULL;
        size_t i;

        src_skip_ws(&p, e);
        if (!src_would_start_ident(p, e)) return false;
        ident = src_consume_ident(&p, e);
        for (i = 0; i < COUNTOF(FONT_TECH); i++)
            if (enumerated_attribute_keyword_match(FONT_TECH[i], ident, strlen(ident))) { kw = FONT_TECH[i]; break; }
        free(ident);
        if (!kw) return false;                  /* an identifier outside §4.3.1's closed keyword set */
        src_tech_add(it, kw);
        src_skip_ws(&p, e);
        if (css_cp_at(p, e, NULL) == CSS_CP_EOF) break;
        if (css_cp_at(p, e, NULL) != ',') return false;
        p++;                                    /* U+002C COMMA is one byte */
    }
    DCHECK(it->ntech > 0,
           "css-fonts-4 §4.3.1's `<font-tech>#` accepted a `tech()` with NO keywords in it. `#` is \"one or "
           "more\", and the loop above only leaves through the break AFTER a keyword has been added — so an "
           "empty list means the emptiness test and the loop bound have come apart and a `tech()` a browser "
           "refuses would be serialized back out as an empty notation");
    return true;
}

/* ONE `<font-src>`, over the component span. FALSE is §4.3.1's "If parsing a component value results in a
   parsing error or its format or tech are unsupported, do not add it to the list of supported sources" —
   whose FIRST half is what this file decides and whose second is the residual css_font_src.h names, and which
   drops THIS COMPONENT and not the declaration, and is the whole difference between this list and an ordinary `#`: "These parsing rules allow
   for graceful fallback of fonts for user agents which don't support a particular font tech or font format."
   ON FALSE `out` may hold allocations, and the caller frees it. */
static bool src_parse_item(const char *a, const char *e, SrcItem *out)
{
    const char *p = a, *fa, *fe;
    bool url_form;

    memset(out, 0, sizeof(*out));
    /* `<font-src>`'s TWO ALTERNATIVES, each asked as a whole functional notation rather than by reading a lead
       token and then dispatching on it. Every arm of the production opens with one — `url(`, `src(` or
       `local(` — including `<url>`'s unquoted form, which CSS Syntax §4.3.4 "Consume an ident-like token"
       also reaches through the ident `url` followed immediately by U+0028. Each test consumes NOTHING when it
       does not match, so all three are asked over the same position and a bare ident with no parenthesis
       after it matches none of them. */
    if (src_take_function(&p, e, "local", &fa, &fe)) {
        /* `local( <font-family-name> )` — THE SAME `<font-family-name>` css-fonts-4 §4.2 "Font family: the
           font-family descriptor" takes, so it is ASKED of the component that owns that production rather
           than re-spelled here. A second `<custom-ident>+` walk beside that one is two right answers to one
           question, and the two would disagree the first time either exclusion list moved. */
        char *arg = src_span_dup(fa, fe);

        out->local = css_font_family_descriptor_value(arg);
        free(arg);
        if (!out->local) return false;
        /* §4.3.1's `local()` alternative carries NO `format()` and NO `tech()` — those belong to the `<url>`
           alternative alone — so anything at all after the notation is outside the grammar. */
        src_skip_ws(&p, e);
        return css_cp_at(p, e, NULL) == CSS_CP_EOF;
    }
    if (src_take_function(&p, e, "url", &fa, &fe))      url_form = true;
    else if (src_take_function(&p, e, "src", &fa, &fe)) url_form = false;
    else                                                return false;

    out->url = src_take_url(fa, fe, url_form);
    if (!out->url) return false;
    /* `[ format( <font-format> ) ]? [ tech( <font-tech># ) ]?` — IN THAT ORDER, which the corpus asserts
       directly: `url("foo.ttf") format(opentype) tech(features-opentype)` is valid and the same two
       notations the other way round are not. Each is tried without consuming anything when it does not
       match, so a `tech()` with no `format()` in front of it is reached by the second test. */
    if (src_take_function(&p, e, "format", &fa, &fe) && !src_take_format(out, fa, fe)) return false;
    if (src_take_function(&p, e, "tech", &fa, &fe) && !src_take_tech(out, fa, fe)) return false;
    src_skip_ws(&p, e);
    /* Anything left is outside `<font-src>` — a third notation, a stray ident, a second url. CSS Syntax has
       no arm that admits it, so the component is a parse error. */
    return css_cp_at(p, e, NULL) == CSS_CP_EOF;
}

/* ---- the SERIALIZER over the list ---------------------------------------------------------------------------- */

static void src_serialize_item(SrcBuf *out, const SrcItem *it)
{
    size_t i;

    DCHECK((it->url == NULL) != (it->local == NULL),
           "a css-fonts-4 §4.3.1 `<font-src>` is both a `<url>` and a `local()`, or neither. The production's "
           "two alternatives are exclusive and they serialize by different rules, so an item answering both "
           "would be emitted under whichever branch happened to be tested first");
    if (it->local) {
        /* The name is already core/css/css_font_family.h's serialization, which is the one form that
           re-parses through that same entry — so `local("A B")` and `local(A B)` name one font and go out
           spelled one way. */
        src_buf_add(out, "local(", 6);
        src_buf_add(out, it->local, strlen(it->local));
        src_buf_add(out, ")", 1);
        return;
    }
    {
        /* CSSOM §2.1 "Common Serializing Idioms"' SERIALIZE A URL, which core/css/css_serialize.h states has
           no unquoted arm at all — so a page's `url(f.woff2)` reads back as `url("f.woff2")`, which is what
           every engine does with `@import url(a.css)` and which re-parses through §4.3.4's function arm.
           A `src()` GOES OUT AS `url()` FOR THE SAME REASON AND THAT IS NOT A LOSS OF THE VALUE: css-values-4
           §4.5's two written forms are one `<url>`, and §2.1 names ONE serialization for it. What the two
           spellings differ in is what §4.5 says they differ in — `url()`'s special url-token parsing, which
           is why "to provide a url by functions such as var(), use the src() notation" — and a value that
           needed it still carries its `var()`, which this file's own first arm hands back untouched. */
        char *u = css_serialize_url(it->url, strlen(it->url));

        CHECK(u != NULL, "cssom: CSSOM §2.1 \"Common Serializing Idioms\"' serialize a URL answered nothing "
                         "for a `<font-src>`'s `<url>`");
        src_buf_add(out, u, strlen(u));
        free(u);
    }
    DCHECK(!(it->format_kw != NULL && it->format_str != NULL),
           "a css-fonts-4 §4.3.1 `format()` carries BOTH a keyword and a string. `<font-format>` is a choice "
           "of one alternative, and the parse writes exactly one field — so two would serialize a notation "
           "with two arguments in it, which re-parses as the parse error `format(a, b)` already is");
    if (it->format_kw) {
        src_buf_add(out, " format(", 8);
        src_buf_add(out, it->format_kw, strlen(it->format_kw));
        src_buf_add(out, ")", 1);
    } else if (it->format_str) {
        /* THE STRING ARM IS EMITTED AS A STRING AND IS NOT FOLDED TO ITS KEYWORD, which is a decision and not
           an omission. §4.3.1's legacy table says the nine strings "have the same effect as if the equivalent
           modern syntax had been used" — a statement about EFFECT, which is what a font load does with the
           value, and not about serialization; and three of the nine have no single-keyword equivalent at all
           (`format("woff2-variations")` is `format(woff2) tech(variations)`), so a fold would have to invent
           a `tech()` the page never wrote. Emitting the string preserves the value and re-parses to it. */
        char *s = css_serialize_string(it->format_str, strlen(it->format_str));

        CHECK(s != NULL, "cssom: CSSOM §2.1 \"Common Serializing Idioms\"' serialize a string answered nothing "
                         "for a `<font-format>`");
        src_buf_add(out, " format(", 8);
        src_buf_add(out, s, strlen(s));
        src_buf_add(out, ")", 1);
        free(s);
    }
    if (it->ntech) {
        src_buf_add(out, " tech(", 6);
        for (i = 0; i < it->ntech; i++) {
            /* CSSOM §2.1's SERIALIZE A COMMA-SEPARATED LIST: "concatenate all items of the list in list order
               while separating them by ", ", i.e., COMMA (U+002C) followed by a single SPACE". */
            if (i) src_buf_add(out, ", ", 2);
            src_buf_add(out, it->tech[i], strlen(it->tech[i]));
        }
        src_buf_add(out, ")", 1);
    }
}

char *css_font_src_descriptor_value(const char *value)
{
    SrcList list = { 0 };
    SrcBuf out = { 0 };
    const char *p, *end;
    char *text;
    size_t i;

    if (!value) return NULL;
    /* css-values-5 "Appendix A: Arbitrary Substitution Functions" — AN APPENDIX, so the title is the citation
       and no § is written beside it — "If a property value contains one or more arbitrary substitution
       functions, and all of those functions are themselves syntactically valid according to their argument
       grammars, the entire value's grammar must be assumed to be valid at parse time." So a `src` carrying a
       `var()` is NOT asked this grammar at all, and the value stands as the page wrote it for core/css/
       css_computed_value.c to substitute into. THE SCAN IS core/css/css_var.h's OWN, which is what keeps this
       arm from disagreeing with the substitution that follows it about what a function token is.
       IT IS AHEAD OF THE COMMENT PASS BECAUSE THAT PASS IS NOT PERFORMED ON THIS ARM: the answer here is the
       value VERBATIM, and stripping comments out of it would hand the substitution step a string the page did
       not write. */
    if (css_var_references(value)) {
        size_t n = strlen(value);
        char *v;

        while (n > 0 && src_is_ws(css_cp_at(value + n - 1, value + n, NULL))) n--;
        while (n > 0 && src_is_ws(css_cp_at(value, value + n, NULL))) { value++; n--; }
        v = malloc(n + 1);
        CHECK(v != NULL, "cssom: OOM copying a `src` whose value carries an arbitrary substitution function");
        if (n) memcpy(v, value, n);
        v[n] = '\0';
        return v;
    }
    /* CSS Syntax §4.3.1 "Consume a token"'s first step, ahead of every question below and owed to the WHOLE
       value rather than to each token boundary — see src_strip_comments. */
    text = src_strip_comments(value);
    p = text;
    end = text + strlen(text);

    /* §4.3.1: "To parse a <font-src-list> production, parse a list of <font-src>s." Each component is parsed
       INDEPENDENTLY and a failing one is dropped rather than invalidating the list, which is the sentence
       after it: "If parsing a component value results in a parsing error or its format or tech are
       unsupported, do not add it to the list of supported sources." */
    for (;;) {
        const char *a, *e;
        SrcItem it;

        src_next_component(&p, end, &a, &e);
        if (src_parse_item(a, e, &it)) src_list_add(&list, &it);
        else                           src_item_free(&it);
        if (p == end) break;
    }
    free(text);

    /* §4.3.1: "If there are no supported entries at the end of this process, the value for the src descriptor
       is a parse error." CSS Syntax drops the declaration whole, and §4.1 "The @font-face rule" says what a
       rule without one is worth: "@font-face rules require a font-family and src descriptor; if either of
       these are missing, the @font-face rule must not be considered when performing the font matching
       algorithm." */
    if (list.n == 0) { src_list_free(&list); return NULL; }

    /* CSSOM §2.1's SERIALIZE A COMMA-SEPARATED LIST, over the entries that survived. */
    for (i = 0; i < list.n; i++) {
        if (i) src_buf_add(&out, ", ", 2);
        src_serialize_item(&out, &list.v[i]);
    }
    src_list_free(&list);
    DCHECK(out.s != NULL && out.n > 0,
           "css-fonts-4 §4.3.1's serializer produced NO TEXT from a non-empty `<font-src-list>`. Every arm of "
           "the item serializer appends at least one code point, so an empty answer would be stored as an "
           "empty declaration — which CSSOM reads back as UNDECLARED and which the block's own re-parse "
           "cannot turn back into this list");
    return out.s;
}
