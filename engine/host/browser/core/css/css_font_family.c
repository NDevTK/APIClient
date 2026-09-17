/* See css_font_family.h. */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/css/css_code_point.h"
#include "core/css/css_defaulting.h"
#include "core/css/css_font_family.h"
#include "core/css/css_serialize.h"
#include "core/html/enumerated_attribute.h"

/* ---- the LIST css-fonts-4 §2.1's `#` describes ------------------------------------------------------------
 *
 * `<'font-family'> = [ <family-name> | <generic-family> ]#`, so an item is EXACTLY ONE of the two and the
 * struct says so: `generic` is a row of the table below and `name` is a decoded `<font-family-name>`, and
 * precisely one of them is non-NULL. They are not one field because they SERIALIZE BY DIFFERENT RULES — a
 * generic keyword is emitted as itself and can never be quoted, a name is emitted under the choice this file's
 * serializer makes — and a single field would make that choice depend on comparing the name against the
 * keyword table, which is the very confusion §2.1.1's exclusion exists to prevent.
 *
 * THE LIST GROWS AND IS NOT A FIXED ARRAY, for the reason core/css/css_font_shorthand.c already states about
 * §2.7's tokenizer: `#` HAS NO UPPER BOUND, so a fixed buffer that dropped a long family list would be a CAP
 * wearing a parse error, and a bundle's own `font-family: A, B, C, …` stack is exactly the value that hits
 * it. */
typedef struct {
    const char *generic;   /* a row of GENERIC_FAMILY below, or NULL */
    char *name;            /* the DECODED `<font-family-name>`, UTF-8, or NULL. OWNED. */
} FfItem;

typedef struct { FfItem *v; size_t n, cap; } FfList;

/* css-fonts-4 §2.1.2 "Syntax of <generic-font-family>"'s TWO PLAIN-IDENT ARMS, transcribed in the standard's
 * own order: `<generic-font-complete> = serif | sans-serif | system-ui | cursive | fantasy | math | monospace`
 * and `<generic-font-incomplete> = ui-serif | ui-sans-serif | ui-monospace | ui-rounded`.
 *
 * NAMED RESIDUAL — §2.1.2's THIRD ARM IS A FUNCTION AND IS NOT BUILT.
 *   WHAT IS NOT COVERED: `<generic-font-script-specific> = generic(fangsong) | generic(kai) |
 *   generic(khmer-mul) | generic(nastaliq)`. It is a `<function-token>` rather than an ident, so it reaches
 *   the `<custom-ident>+` arm below, where the ident walk stops at the `(` — which is neither whitespace nor a
 *   comma — and the item is refused.
 *   WHAT THE NEXT DIFF BUILDS: the `generic()` arm as a third item kind, over the four names, parsed as
 *   CSS Syntax §4.3.4 "Consume an ident-like token" — whose `(` arm is the one that produces a
 *   `<function-token>` at all, there being no algorithm of that name in §4.3 — and serialized through
 *   CSSOM §2.1 "Common Serializing Idioms"' serialize a function, which lowercases the function name and is
 *   why the arm can never be a `<custom-ident>` that happens to carry a parenthesis.
 *   HOW ITS ABSENCE WOULD SHOW: a declaration naming a script-specific generic is dropped WHOLE rather than
 *   keeping the universal generic beside it, so a page that writes one reads back the empty string from
 *   `getPropertyValue("font-family")` where a browser reads back every item. */
static const char *const GENERIC_FAMILY[] = {
    "serif", "sans-serif", "system-ui", "cursive", "fantasy", "math", "monospace",
    "ui-serif", "ui-sans-serif", "ui-monospace", "ui-rounded",
};

/* ---- bytes ------------------------------------------------------------------------------------------------ */

typedef struct { char *s; size_t n, cap; } FfBuf;

static void ff_buf_reserve(FfBuf *b, size_t more)
{
    if (b->s != NULL && b->n + more + 1 <= b->cap) return;
    while (b->cap < b->n + more + 1) b->cap = b->cap ? b->cap * 2 : 32;
    b->s = realloc(b->s, b->cap);
    CHECK(b->s != NULL, "cssom: OOM building a css-fonts-4 §2.1 `font-family` value — a dropped declaration "
                        "reads back as UNDECLARED, which is a different font from the one it names");
    b->s[b->n] = '\0';
}

static void ff_buf_add(FfBuf *b, const char *s, size_t n)
{
    ff_buf_reserve(b, n);
    if (n) memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = '\0';
}

/* A CODE POINT, RE-ENCODED RATHER THAN COPIED, which core/css/css_code_point.h requires of every caller that
   copies source text: "for a filtered code point they are different: the two bytes of a CRLF, the one byte of
   a NUL and the bytes of an ill-formed sequence each stand for a code point the source does not spell." */
static void ff_buf_add_cp(FfBuf *b, uint32_t cp)
{
    DCHECK(cp <= 0x10FFFF,
           "a code point above CSS Syntax §4.2 \"Definitions\"' maximum allowed code point reached the "
           "`font-family` encoder. Its only two producers are css_cp_at, which never answers one, and CSS "
           "Syntax §4.3.7 \"Consume an escaped code point\", whose own step answers U+FFFD for a value "
           "\"greater than the maximum allowed code point\"");
    ff_buf_reserve(b, 4);
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

/* ---- CSS Syntax's code-point walk ------------------------------------------------------------------------- */

/* CSS Syntax §4.2 "Definitions"' WHITESPACE: "A newline, U+0009 CHARACTER TABULATION, or U+0020 SPACE." §3.3
   "Preprocessing the input stream" has already turned every CR, FF and CRLF into ONE U+000A, which is why
   this is three cases and not six — core/css/css_code_point.h performs that filter and this reads its
   answer rather than restating it. */
static bool ff_is_ws(uint32_t cp)
{
    return cp == 0x000A || cp == 0x0009 || cp == 0x0020;
}

/* CSS Syntax §4.3.8 "Check if two code points are a valid escape", asked where the `\` is the code point AT
   `p`: "If the first code point is not U+005C REVERSE SOLIDUS (\), return false. Otherwise, if the second code
   point is a newline, return false. Otherwise, return true." */
static bool ff_valid_escape(const char *p, const char *end)
{
    size_t n = 0;
    uint32_t cp = css_cp_at(p, end, &n);

    if (cp != '\\') return false;
    cp = css_cp_at(p + n, end, NULL);
    return cp != CSS_CP_EOF && cp != 0x000A;
}

/* CSS Syntax §4.3.7 "Consume an escaped code point", entered with the `\` at `*p` NOT yet consumed — this
   consumes it, so a caller advances by nothing else. Its EOF arm stays even though every caller here asks
   §4.3.8 first, because the algorithm has it and because a caller added later would otherwise get a walk that
   does not terminate. */
static uint32_t ff_consume_escape(const char **p, const char *end)
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
    /* "If the next input code point is whitespace, consume it as well." ONE code point and not a run: the
       second space of `\31  0th` is the ident sequence's own separator and not part of the escape. */
    cp = css_cp_at(*p, end, &n);
    if (ff_is_ws(cp)) *p += n;
    /* "If this number is zero, or is for a surrogate, or is greater than the maximum allowed code point,
       return U+FFFD REPLACEMENT CHARACTER." */
    if (v == 0 || (v >= 0xD800 && v <= 0xDFFF) || v > 0x10FFFF) return 0xFFFD;
    return v;
}

/* CSS Syntax §4.3.9 "Check if three code points would start an ident sequence", over the code points at `p`.
   Its U+005C arm forwards to §4.3.8, and its U+002D arm admits a second HYPHEN-MINUS as well as an ident-start
   code point — transcribed rather than narrowed, because a family name may legitimately be spelled `--x`. */
static bool ff_would_start_ident(const char *p, const char *end)
{
    size_t n = 0;
    uint32_t cp = css_cp_at(p, end, &n), cp2;

    if (cp == '-') {
        cp2 = css_cp_at(p + n, end, NULL);
        return css_cp_is_ident_start(cp2) || cp2 == '-' || ff_valid_escape(p + n, end);
    }
    if (cp != CSS_CP_EOF && css_cp_is_ident_start(cp)) return true;
    return cp == '\\' && ff_valid_escape(p, end);
}

/* CSS Syntax §4.3.12 "Consume an ident sequence" — "Repeatedly consume the next input code point from the
   stream: ident code point / Append the code point to result. / the stream starts with a valid escape /
   Consume an escaped code point. Append the returned code point to result. / anything else / Reconsume the
   current input code point. Return result."
   THE ANSWER IS THE DECODED SEQUENCE AND NOT ITS SPELLING, which is the whole reason this is a walk rather
   than a span: `\31 0th` and `10th` are ONE family name written two ways, and a serializer over spellings
   would emit a value that does not re-parse to what it came from. */
static char *ff_consume_ident(const char **p, const char *end)
{
    FfBuf b = { 0 };
    size_t n = 0;
    uint32_t cp;

    DCHECK(ff_would_start_ident(*p, end),
           "CSS Syntax §4.3.12 \"Consume an ident sequence\" was entered where §4.3.9 \"Check if three code "
           "points would start an ident sequence\" answers false. The two are one step of one algorithm and "
           "the failure is silent: an ident consumed from a position that starts none is a family name this "
           "engine accepts and a browser refuses");
    for (;;) {
        cp = css_cp_at(*p, end, &n);
        if (cp != CSS_CP_EOF && css_cp_is_ident(cp)) { ff_buf_add_cp(&b, cp); *p += n; continue; }
        if (ff_valid_escape(*p, end))               { ff_buf_add_cp(&b, ff_consume_escape(p, end)); continue; }
        break;
    }
    DCHECK(b.s != NULL && b.n > 0,
           "CSS Syntax §4.3.12 \"Consume an ident sequence\" produced an EMPTY ident from a position §4.3.9 "
           "answered true for. Each of §4.3.9's three arms names a code point §4.3.12's loop then consumes, "
           "so an empty result means the two have come apart and every family name silently loses a token");
    return b.s;
}

/* CSS Syntax §4.3.5 "Consume a string token", entered with the opening quote at `*p` NOT yet consumed. NULL is
   §4.3.5's `<bad-string-token>` — "newline / This is a parse error. Reconsume the current input code point,
   create a <bad-string-token>, and return it." — which no grammar admits, so the declaration is invalid.
   AN UNTERMINATED STRING AT EOF IS A `<string-token>` AND NOT A BAD ONE, which is §4.3.5's own EOF arm ("EOF /
   This is a parse error. Return the <string-token>.") and is one of the two places this differs from the
   §2.1.1 predicate it replaces: that one refused an unclosed quote outright, so `font: 12px "Helvetica` set
   none of §2.7's nineteen longhands where a browser sets all nineteen. */
static char *ff_consume_string(const char **p, const char *end)
{
    FfBuf b = { 0 };
    size_t n = 0, n2 = 0;
    uint32_t quote, cp, next;

    quote = css_cp_at(*p, end, &n);
    DCHECK(quote == '"' || quote == '\'',
           "CSS Syntax §4.3.5 \"Consume a string token\" was entered at a code point that is neither of the "
           "two quotes its \"ending code point\" can be, so the token would end at the first of EITHER quote "
           "and a family name carrying the other would be split in half");
    *p += n;
    for (;;) {
        cp = css_cp_at(*p, end, &n);
        if (cp == CSS_CP_EOF) break;                    /* the EOF arm: the `<string-token>` stands */
        if (cp == quote) { *p += n; break; }
        if (cp == 0x000A) { free(b.s); return NULL; }   /* the newline arm: a `<bad-string-token>` */
        if (cp == '\\') {
            next = css_cp_at(*p + n, end, &n2);
            /* §4.3.5's U+005C arm: "If the next input code point is EOF, do nothing." and "Otherwise, if the
               next input code point is a newline, consume it." BOTH APPEND NOTHING — a `\` at the end of a
               line is a CONTINUATION and not a character of the value. */
            if (next == CSS_CP_EOF) { *p += n; continue; }
            if (next == 0x000A)     { *p += n + n2; continue; }
            ff_buf_add_cp(&b, ff_consume_escape(p, end));
            continue;
        }
        ff_buf_add_cp(&b, cp);
        *p += n;
    }
    /* `font-family: ""` is a `<string>` whose value is the empty string — a `<family-name>` that matches no
       font, which is a MATCHING question and not a parse one, so the empty buffer is materialized rather than
       refused. The serializer emits it as `""`, which re-parses to the same item. */
    if (!b.s) ff_buf_reserve(&b, 0);
    return b.s;
}

/* ---- css-fonts-4 §2.1's PARSE ------------------------------------------------------------------------------ */

/* The engine's ONE Infra ASCII case-insensitive match, asked rather than re-spelled: a keyword comparison
   written out again here is the hand-rolled `strcasecmp`-shaped loop core/html/enumerated_attribute.h records
   itself as having replaced in four places, and `strcasecmp` folds by the current LOCALE — a Turkish locale
   does not fold `SERIF` to `serif`. core/css/css_presentational_hints.c asks the same entry for the same
   reason, so a CSS caller here is not a new direction. */
static const char *ff_generic_of(const char *s, size_t n)
{
    size_t i;

    for (i = 0; i < sizeof(GENERIC_FAMILY) / sizeof(GENERIC_FAMILY[0]); i++)
        if (enumerated_attribute_keyword_match(GENERIC_FAMILY[i], s, n)) return GENERIC_FAMILY[i];
    return NULL;
}

/* css-fonts-4 §2.1.1 "Syntax of <font-family-name>"'s EXCLUSION, over ONE identifier: "Any identifier which
   could be misinterpreted as a pre-defined keyword in the font-family value definition, or the CSS-wide
   keywords, is not allowed", and its closing paragraph — "UAs must not consider these keywords as matching
   the <font-family-name> type."
   ONE FUNCTION FOR THE PARSE AND THE SERIALIZER, which is what keeps them from drifting: the parse refuses an
   identifier this answers TRUE for, and the serializer QUOTES a name carrying one, so the two are the same
   sentence read forwards and backwards. Two copies would disagree the first time a keyword was added.
   core/css/css_defaulting.h owns the CSS-wide-plus-`default` floor and its header says each caller "asks this
   and then states its OWN additions beside it"; §2.1.2's generic keywords are this grammar's addition. */
static bool ff_ident_excluded(const char *s, size_t n)
{
    char *one;
    bool bad;

    if (ff_generic_of(s, n) != NULL) return true;
    one = malloc(n + 1);
    CHECK(one != NULL, "cssom: OOM testing a `<custom-ident>` against css-fonts-4 §2.1.1's exclusion");
    if (n) memcpy(one, s, n);
    one[n] = '\0';
    bad = css_custom_ident_excluded(one);
    free(one);
    return bad;
}

static void ff_list_free(FfList *l)
{
    size_t i;

    for (i = 0; i < l->n; i++) free(l->v[i].name);
    free(l->v);
    l->v = NULL;
    l->n = l->cap = 0;
}

static void ff_list_add(FfList *l, const char *generic, char *name)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 4;
        l->v = realloc(l->v, l->cap * sizeof(*l->v));
        CHECK(l->v != NULL, "cssom: OOM growing a css-fonts-4 §2.1 family list, whose `#` has no upper bound");
    }
    l->v[l->n].generic = generic;
    l->v[l->n].name = name;
    l->n++;
}

static void ff_skip_ws(const char **p, const char *end)
{
    size_t n = 0;

    while (ff_is_ws(css_cp_at(*p, end, &n))) *p += n;
}

/* ONE ITEM of §2.1's `[ <family-name> | <generic-family> ]#`, from `*p` up to the next top-level comma or the
   end of the value. FALSE is an item outside the grammar, and CSS Syntax drops the WHOLE declaration for one:
   a family list is `#` and not a best-effort collection, so §2.1.1's own `font-family: Ahem!, sans-serif`
   sets nothing rather than keeping the `sans-serif` a lenient reader would salvage. */
static bool ff_parse_item(const char **p, const char *end, FfList *out)
{
    const char *generic;
    FfBuf joined = { 0 };
    unsigned nparts = 0;
    uint32_t cp;

    ff_skip_ws(p, end);
    cp = css_cp_at(*p, end, NULL);
    if (cp == CSS_CP_EOF || cp == ',') return false;   /* an empty item: `font-family: a,,b` is not `#` */

    if (cp == '"' || cp == '\'') {
        char *s = ff_consume_string(p, end);

        if (!s) return false;
        ff_skip_ws(p, end);
        cp = css_cp_at(*p, end, NULL);
        /* §2.1.1's own invalid example `"Lucida" Grande`: a `<font-family-name>` is `<string>` OR
           `<custom-ident>+`, never a string followed by anything at all. */
        if (cp != CSS_CP_EOF && cp != ',') { free(s); return false; }
        ff_list_add(out, NULL, s);
        return true;
    }

    /* `<custom-ident>+`, joined by SINGLE SPACES per §2.1.1: "If a sequence of identifiers is given as a
       <font-family-name>, the computed value is the name converted to a string by joining all the identifiers
       in the sequence by single spaces." */
    for (;;) {
        char *ident;

        cp = css_cp_at(*p, end, NULL);
        if (cp == CSS_CP_EOF || cp == ',') break;
        if (!ff_would_start_ident(*p, end)) { free(joined.s); return false; }
        ident = ff_consume_ident(p, end);
        if (nparts) ff_buf_add(&joined, " ", 1);
        ff_buf_add(&joined, ident, strlen(ident));
        free(ident);
        nparts++;
        ff_skip_ws(p, end);
        /* After an identifier the only continuations §2.1.1 admits are ANOTHER identifier, the list's comma,
           or the end. §2.1.1's `Red/Black`, `Ahem!`, `test@foo` and `Hawaii 5-0` each land here, because the
           code point that stopped the ident walk is none of the three. */
        cp = css_cp_at(*p, end, NULL);
        if (cp != CSS_CP_EOF && cp != ',' && !ff_would_start_ident(*p, end)) { free(joined.s); return false; }
    }
    DCHECK(nparts > 0 && joined.s != NULL,
           "css-fonts-4 §2.1.1's `<custom-ident>+` accepted an item with NO identifiers in it. The `+` is "
           "\"one or more\", and the loop above only exits at the end or a comma — both of which the item's "
           "first code point was already tested against — so an empty join means the emptiness test and the "
           "loop bound have come apart and an empty family name would be stored as a real one");

    /* §2.1's `[ <family-name> | <generic-family> ]`: the GENERIC arm is tried FIRST, and only for a single
       identifier, because that is the only shape a `<generic-family>` has. Asking the exclusion below first
       would make `font-family: serif` an invalid declaration. */
    if (nparts == 1 && (generic = ff_generic_of(joined.s, joined.n)) != NULL) {
        free(joined.s);
        ff_list_add(out, generic, NULL);
        return true;
    }
    /* §2.1.1's exclusion, PER IDENTIFIER. Per identifier and not per item is what refuses §2.1.1's own
       invalid example `font-family: cursive serif`, in which NEITHER identifier is a CSS-wide keyword and the
       joined name `cursive serif` is not one either — so an exclusion asked of the joined name admits it. */
    {
        const char *q = joined.s, *stop = joined.s + joined.n;

        while (q < stop) {
            const char *sp = memchr(q, ' ', (size_t)(stop - q));
            size_t len = sp ? (size_t)(sp - q) : (size_t)(stop - q);

            if (ff_ident_excluded(q, len)) { free(joined.s); return false; }
            q = sp ? sp + 1 : stop;
        }
    }
    ff_list_add(out, NULL, joined.s);
    return true;
}

/* ---- the SERIALIZER over the list -------------------------------------------------------------------------- */

/* IS THIS NAME RE-EMITTABLE AS §2.1.1's `<custom-ident>+` — every space-separated part a bare CSS ident that
 * needs no escaping, and no part a keyword the parse above refuses?
 *
 * CSSOM §2.1 "Common Serializing Idioms"' SERIALIZE AN IDENTIFIER IS NOT THE QUESTION, AND ASKING IT WOULD BE
 * WRONG IN ONE DIRECTION. CSSOM §2.1 emits every code point "greater than or equal to U+0080" as itself, so it
 * leaves an identifier opening with U+00D7 MULTIPLICATION SIGN unescaped — and CSS Syntax §4.2 "Definitions"'
 * non-ASCII ident code point is an ENUMERATED set with U+00D7 OUTSIDE it, so that identifier does not re-parse
 * as one. A serializer that trusted CSSOM §2.1's table would emit a value this file's own parse then refuses, which
 * is the round-trip failing against itself. §4.2 is strictly the stronger test and is the one asked here; a
 * part that passes it is one CSSOM §2.1 would have left untouched anyway, so nothing is lost by not calling it.
 *
 * AND THE KEYWORD TEST IS THE PARSE'S OWN EXCLUSION READ BACKWARDS. A name whose single part is `serif` or
 * `inherit` can only have come from a `<string>`, and emitting it unquoted would re-parse as the
 * `<generic-family>` arm or as a CSS-wide keyword. §2.1.1 states exactly that — "Font family names that happen
 * to be the same as a font-family keyword value (e.g. CSS-wide keywords such as inherit, or
 * <generic-font-family> keywords such as serif) must be quoted to prevent confusion with the keywords of the
 * same names" — and lists `"sans-serif"`, `"default"`, `"initial"` and `"inherit"` as valid BECAUSE quoted. */
static bool ff_name_is_ident_sequence(const char *name)
{
    const char *p = name, *end = name + strlen(name);

    if (p == end) return false;                       /* the empty name is only ever a `<string>` */
    while (p < end) {
        const char *sp = memchr(p, ' ', (size_t)(end - p));
        const char *stop = sp ? sp : end;
        const char *q = p;

        /* An empty part is a leading, trailing or doubled space, which §2.1.1's join by SINGLE spaces cannot
           produce — so the name came from a `<string>`, and only a `<string>` can carry it back. */
        if (stop == p) return false;
        if (!ff_would_start_ident(p, stop)) return false;
        while (q < stop) {
            size_t n = 0;
            uint32_t cp = css_cp_at(q, stop, &n);

            if (!css_cp_is_ident(cp)) return false;   /* an escape would be needed, so quote the whole name */
            q += n;
        }
        if (ff_ident_excluded(p, (size_t)(stop - p))) return false;
        p = sp ? sp + 1 : end;
    }
    return true;
}

/* ---- CSS Syntax §4.3.1 "Consume a token"'s FIRST STEP, over text that has never been through §4.3.1 ------- */

/* CSS Syntax §4.3.2 "Consume comments", RUN ONCE OVER THE WHOLE VALUE BEFORE ANY PRODUCTION BELOW IS ASKED.
 *
 * THIS ENTRY'S INPUT IS PRE-TOKENIZER SOURCE TEXT AT EVERY CALLER, WHICH IS WHY THE STEP IS OWED HERE. §4.3.1
 * makes "Consume comments" the FIRST step of consuming EVERY token, so every production this file states —
 * §4.3.5's string, §4.3.12's ident sequence, §4.2's code-point classes — is written over a stream a comment
 * can no longer be in. The text handed to this entry has been through §3.3 "Preprocessing the input stream"
 * (core/css/css_code_point.h performs that filter at each read) and through nothing else: a page's own string
 * where CSSOM parses a value, and a span of a declaration's own source where CSSOM reads one back. Without
 * this pass the opening SOLIDUS of a comment stops the ident walk, ff_parse_item refuses the item, and a
 * declaration a browser accepts is dropped WHOLE.
 *
 * A COMMENT IS REPLACED BY ONE U+0020 SPACE AND IS NOT DELETED, AND THE DIFFERENCE IS A DIFFERENT FAMILY
 * RATHER THAN A TIDIER SPELLING. A comment produces no token, but it still ENDS the token in front of it, so
 * two identifiers written with a comment between them and no space are TWO `<ident-token>`s — and
 * css-fonts-4 §2.1.1 "Syntax of <font-family-name>" says what two of them are worth: "If a sequence of
 * identifiers is given as a <font-family-name>, the computed value is the name converted to a string by
 * joining all the identifiers in the sequence by single spaces." Deleting the comment would run them into ONE
 * identifier and name a family the page never wrote.
 *
 * THE SPACE IS SOUND BECAUSE OF THIS GRAMMAR AND NOT IN GENERAL, WHICH IS WHY THIS PASS IS NOT SHARED AND IS
 * NOT TAKEN WHERE THE READ PATH'S SPAN IS SLICED. Whitespace in §2.1's value only ever SEPARATES tokens —
 * ff_skip_ws runs at each item boundary and §2.1.1 joins by single spaces however many code points stood
 * between — so here a space and a comment separate the same pair. A grammar in which the PRESENCE of a
 * `<whitespace-token>` is itself significant does not have that property, and a slicer that serves every
 * property cannot know which of the two it is feeding; the same pass placed there would be answering a
 * question about a grammar it cannot see.
 *
 * IT SKIPS A `<string>` AND AN ESCAPE THROUGH THE WALKS THIS FILE ALREADY HAS, so nothing here holds a second
 * opinion about where either one ends: an opening SOLIDUS-ASTERISK inside a quoted family name is two
 * characters of that name, and §4.3.8's valid escape can carry a SOLIDUS that opens nothing. §4.3.5's own walk
 * is asked WHERE its token ends rather than what it decodes, which is why the name it returns is freed unread.
 * THE ESCAPE UNIT IS §4.3.8's TWO CODE POINTS AND NOT §4.3.7's WHOLE ESCAPE, deliberately: the code points
 * §4.3.7 would go on to consume are hex digits and at most one whitespace, and U+002F SOLIDUS is neither, so
 * no comment can open inside the part this pass does not walk.
 * OWNED, and never NULL — a value that is nothing but a comment answers the EMPTY STRING, which is a value the
 * walk below already refuses as `#` over no items rather than one this pass has to judge. */
static char *ff_strip_comments(const char *value)
{
    FfBuf out = { 0 };
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
            ff_buf_add(&out, " ", 1);
            continue;
        }
        if (cp == '"' || cp == '\'') {
            const char *begin = p;

            free(ff_consume_string(&p, end));
            DCHECK(p > begin,
                   "CSS Syntax §4.3.5 \"Consume a string token\" advanced over NO code points from a quote. "
                   "Every arm of it consumes at least the opening quote, so a walk that did not move would "
                   "re-enter this arm at the same byte and never reach the end of the value");
            ff_buf_add(&out, begin, (size_t)(p - begin));
            continue;
        }
        if (cp == '\\' && ff_valid_escape(p, end)) {
            css_cp_at(p + n, end, &n2);
            ff_buf_add(&out, p, n + n2);
            p += n + n2;
            continue;
        }
        /* THE BYTES AND NOT THE CODE POINT, which is the opposite of what core/css/css_code_point.h demands of
           a caller that COPIES source text — and it is right here because this pass does not copy text FOR a
           consumer, it hands the same walk back to itself: what it emits is read by css_cp_at again, so a
           CRLF re-filtered to one U+000A is the identical answer, while re-encoding it would spell a code
           point the source does not have. */
        ff_buf_add(&out, p, n);
        p += n;
    }
    if (!out.s) ff_buf_reserve(&out, 0);
    return out.s;
}

char *css_font_family_value(const char *value)
{
    FfList list = { 0 };
    FfBuf out = { 0 };
    const char *p, *end, *k;
    char *text;
    size_t i, len;

    if (!value) return NULL;
    /* CSS Syntax §4.3.1 "Consume a token"'s first step, ahead of every question this entry asks — see
       ff_strip_comments for why it is owed here and why it is owed to the WHOLE value rather than to each
       token boundary. It precedes the CSS-wide keyword test as well, because a comment is not whitespace: the
       test below trims whitespace off both ends before comparing, so `inherit` written with a comment after it
       would otherwise fall past §7.3 into §2.1's own grammar and be refused there as an excluded identifier. */
    text = ff_strip_comments(value);
    /* CSS Cascade 5 §7.3 "Explicit Defaulting"'s keywords are a value for EVERY property, so they precede
       §2.1's own grammar and are handed on for §7's DEFAULTING step to resolve (core/css/css_computed_value.h
       says where). ASCII-LOWERCASED because a keyword serializes canonically and `css_wide_keyword` matched
       case-insensitively, so lowercasing IS that canonical spelling.
       IT IS LOWERCASED OVER THE TRIMMED SPAN AND NOT OVER THE WHOLE TEXT, because `css_wide_keyword` matched
       THROUGH the edges rather than in spite of them: it compares after trimming, so the text it accepted may
       carry whitespace the keyword does not, and lowercasing all of it answers a value CSSOM reads back with
       those edges still on. §4.2 "Definitions"' whitespace is the set trimmed here, which is the one §2.1's
       own walk below skips and a strict subset of what the test accepted — so no text this arm admits can
       reach the copy with an edge the test itself would have kept. */
    if (css_wide_keyword(text)) {
        char *v;

        k = text;
        end = text + strlen(text);
        while (ff_is_ws(css_cp_at(k, end, &len))) k += len;
        while (end > k && ff_is_ws(css_cp_at(end - 1, end, NULL))) end--;
        len = (size_t)(end - k);
        v = malloc(len + 1);
        CHECK(v != NULL, "cssom: OOM copying a CSS-wide keyword out of a `font-family` declaration");
        for (i = 0; i < len; i++)
            v[i] = (k[i] >= 'A' && k[i] <= 'Z') ? (char)(k[i] - 'A' + 'a') : k[i];
        v[len] = '\0';
        free(text);
        return v;
    }

    p = text;
    end = text + strlen(text);
    for (;;) {
        uint32_t cp;

        if (!ff_parse_item(&p, end, &list)) { ff_list_free(&list); free(text); return NULL; }
        ff_skip_ws(&p, end);
        cp = css_cp_at(p, end, NULL);
        if (cp == CSS_CP_EOF) break;
        DCHECK(cp == ',',
               "css-fonts-4 §2.1's `#` walk stopped at a code point that is neither the end of the value nor "
               "a comma. ff_parse_item returns only at one of those two, so this is the item parse and this "
               "loop disagreeing about where an item ends — which silently drops the rest of a family list");
        p++;                 /* U+002C COMMA is one byte, and css_cp_at answered it as one code point */
    }
    DCHECK(list.n > 0,
           "css-fonts-4 §2.1's `#` produced an EMPTY list from a value every item parse accepted. `#` is "
           "\"one or more\", so an empty list is not a `<'font-family'>` at all, and a caller would store a "
           "declaration whose value serializes to nothing — which CSSOM reads back as UNDECLARED");

    /* CSSOM §2.1 "Common Serializing Idioms"' SERIALIZE A COMMA-SEPARATED LIST: "concatenate all items of the
       list in list order while separating them by ", ", i.e., COMMA (U+002C) followed by a single SPACE". */
    for (i = 0; i < list.n; i++) {
        const FfItem *it = &list.v[i];

        DCHECK((it->generic == NULL) != (it->name == NULL),
               "a css-fonts-4 §2.1 list item is both a `<generic-family>` and a `<family-name>`, or neither. "
               "§2.1's value is `[ <family-name> | <generic-family> ]#` — exactly one alternative per item — "
               "and the two serialize by different rules, so an item answering both would be emitted under "
               "whichever branch happened to be tested first");
        if (i) ff_buf_add(&out, ", ", 2);
        if (it->generic) {
            /* The CANONICAL keyword out of §2.1.2's table and never the author's spelling, because a keyword
               is ASCII case-insensitive and serializes one way. */
            ff_buf_add(&out, it->generic, strlen(it->generic));
        } else if (ff_name_is_ident_sequence(it->name)) {
            /* CSSOM §2.1's SERIALIZE A WHITESPACE-SEPARATED LIST, which the name already IS: §2.1.1 joined its
               identifiers "by single spaces" and every part has just been tested as a bare ident. */
            ff_buf_add(&out, it->name, strlen(it->name));
        } else {
            char *s = css_serialize_string(it->name, strlen(it->name));

            CHECK(s != NULL, "cssom: CSSOM §2.1 \"Common Serializing Idioms\"' serialize a string answered "
                             "nothing for a `<family-name>`");
            ff_buf_add(&out, s, strlen(s));
            free(s);
        }
    }
    ff_list_free(&list);
    free(text);
    DCHECK(out.s != NULL,
           "css-fonts-4 §2.1's serializer produced NO TEXT from a non-empty list. Every arm above appends at "
           "least one code point, so an empty answer would be stored as an empty declaration — which CSSOM "
           "reads back as UNDECLARED and which the round-trip cannot re-parse");
    return out.s;
}
