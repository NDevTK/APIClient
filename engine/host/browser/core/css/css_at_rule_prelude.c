/* CSS Cascade §2's, CSS Namespaces §2's, CSS Paged Media §4.3's, CSS Animations §3's and CSS Properties and
 * Values API 1 §3's at-rule grammars. See css_at_rule_prelude.h for why this is a component, why it tokenizes
 * with lexbor's own tokenizer, which spans stay raw source, why a `<keyframe-block>`'s prelude is one of these,
 * and why `@property`'s DESCRIPTORS are here beside its prelude. */
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/css/syntax/token.h>
#include <lexbor/css/syntax/tokenizer.h>

#include "check.h"
#include "core/css/css_at_rule_prelude.h"
#include "core/css/css_defaulting.h"
#include "core/css/css_length.h"
#include "core/css/css_serialize.h"

/* ONE PRELUDE BEING READ. `base`/`len` are the source the offsets below index — every token lexbor hands back
   names its own span inside it, which is how a raw span (`supports(...)`'s contents, the media tail) is taken
   without re-serializing tokens that were never parsed into anything. */
typedef struct {
    lxb_css_syntax_tokenizer_t *tkz;
    const char                 *base;
    size_t                      len;
} Prelude;

static char *pre_copy(const char *s, size_t n)
{
    char *out = malloc(n + 1);

    CHECK(out != NULL, "cssom: OOM copying a piece of an at-rule prelude");
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

/* The current token, never consumed. NULL is the tokenizer's own allocation failure, which every caller treats
   as "the grammar did not match" — a prelude that could not be read is not a rule. */
static lxb_css_syntax_token_t *pre_peek(Prelude *p)
{
    return lxb_css_syntax_token(p->tkz);
}

static void pre_take(Prelude *p) { lxb_css_syntax_token_consume(p->tkz); }

static lxb_css_syntax_token_t *pre_peek_ws(Prelude *p)
{
    lxb_css_syntax_token_t *t = pre_peek(p);

    while (t && t->type == LXB_CSS_SYNTAX_TOKEN_WHITESPACE) {
        pre_take(p);
        t = pre_peek(p);
    }
    return t;
}

/* WHERE THIS TOKEN STARTS in the prelude, as an offset. Lexbor stamps every token with a pointer into the very
   buffer that was handed to the tokenizer, so this is subtraction and not a second cursor that could drift. */
static size_t pre_at(const Prelude *p, lxb_css_syntax_token_t *t)
{
    const char *b = (const char *)lxb_css_syntax_token_base(t)->begin;

    DCHECK(b >= p->base && b <= p->base + p->len,
           "a CSS token names a span outside the prelude it was tokenized from — the tokenizer's buffer IS "
           "that string, so a pointer outside it means the buffer was set from something else");
    if (b < p->base || b > p->base + p->len) return p->len;
    return (size_t)(b - p->base);
}

static size_t pre_end_of(const Prelude *p, lxb_css_syntax_token_t *t)
{
    return pre_at(p, t) + lxb_css_syntax_token_base(t)->length;
}

/* A token's own STRING, unescaped — lexbor cooks `\"` and `\41` before it hands the token over, which is the
   whole reason this file tokenizes rather than scans. Copied AT ONCE: the tokenizer keeps the cooked bytes in a
   temp buffer until the NEXT token is requested, so a pointer held across a `pre_take` is a dangling one. */
static char *pre_token_string(lxb_css_syntax_token_t *t)
{
    const lxb_css_syntax_token_string_t *s = lxb_css_syntax_token_string(t);

    return pre_copy((const char *)s->data, s->length);
}

static bool pre_name_is(lxb_css_syntax_token_t *t, const char *lower)
{
    const lxb_css_syntax_token_string_t *s = lxb_css_syntax_token_string(t);
    size_t i, n = strlen(lower);

    if (s->length != n) return false;
    for (i = 0; i < n; i++) {
        char c = (char)s->data[i];

        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != lower[i]) return false;
    }
    return true;
}

/* CSS Values' `<url>`, in the two shapes CSS Syntax gives it: a URL token (`url(a.css)`, whose contents the
   tokenizer already unescaped) and a `url(` FUNCTION token followed by a `<string>` and a `)`. A BAD-URL token
   is the tokenizer's own report that the construct is not a URL at all, so it fails here rather than becoming
   a rule with an unreadable location. The `<string>` arm is CSS Cascade §2's "if a <string> is provided, it
   must be interpreted as a <url> with the same value". OWNED, NULL when the next thing is not one. */
static char *pre_url_or_string(Prelude *p)
{
    lxb_css_syntax_token_t *t = pre_peek_ws(p);
    char *out;

    if (!t) return NULL;
    if (t->type == LXB_CSS_SYNTAX_TOKEN_URL || t->type == LXB_CSS_SYNTAX_TOKEN_STRING) {
        out = pre_token_string(t);
        pre_take(p);
        return out;
    }
    if (t->type != LXB_CSS_SYNTAX_TOKEN_FUNCTION || !pre_name_is(t, "url")) return NULL;
    pre_take(p);
    t = pre_peek_ws(p);
    if (!t || t->type != LXB_CSS_SYNTAX_TOKEN_STRING) return NULL;
    out = pre_token_string(t);
    pre_take(p);
    t = pre_peek_ws(p);
    if (!t || t->type != LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS) { free(out); return NULL; }
    pre_take(p);
    return out;
}

/* THE CONTENTS OF THE FUNCTION whose token is current, as RAW SOURCE, with the function and its closing
   parenthesis consumed. Depth is tracked over the two tokens that OPEN one — CSS Syntax's `(` and its
   `name(` — because `supports((display:flex) or (display:block))` closes three of them and the first `)` is
   not the function's. An unterminated function is a prelude the tokenizer ran off the end of, which is not a
   match. OWNED, NULL when it is not terminated. */
static char *pre_function_contents(Prelude *p)
{
    lxb_css_syntax_token_t *t = pre_peek(p);
    size_t begin, depth = 1;

    DCHECK(t != NULL && t->type == LXB_CSS_SYNTAX_TOKEN_FUNCTION,
           "an at-rule prelude's function contents were taken while the cursor was not on a function token");
    if (!t || t->type != LXB_CSS_SYNTAX_TOKEN_FUNCTION) return NULL;
    begin = pre_end_of(p, t);        /* just past `name(` */
    pre_take(p);
    for (;;) {
        t = pre_peek(p);
        if (!t || t->type == LXB_CSS_SYNTAX_TOKEN__EOF) return NULL;
        if (t->type == LXB_CSS_SYNTAX_TOKEN_FUNCTION ||
            t->type == LXB_CSS_SYNTAX_TOKEN_L_PARENTHESIS) depth++;
        else if (t->type == LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS && --depth == 0) {
            size_t end = pre_at(p, t);

            pre_take(p);
            DCHECK(end >= begin, "an at-rule prelude's function closed before it opened");
            return pre_copy(p->base + begin, end - begin);
        }
        pre_take(p);
    }
}

/* Trimmed of ASCII whitespace on both ends — a raw span runs from one token's start to another's and the
   parser that reads it next has no use for either edge. The test is spelled out rather than run through
   `strchr`, which reports the terminating NUL as a member of its set and would eat a U+0000 that CSS Syntax
   turns into U+FFFD rather than into whitespace. */
static bool pre_is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

static char *pre_span_trimmed(const Prelude *p, size_t begin, size_t end)
{
    while (begin < end && pre_is_ws(p->base[begin])) begin++;
    while (end > begin && pre_is_ws(p->base[end - 1])) end--;
    return pre_copy(p->base + begin, end - begin);
}

static bool pre_open(Prelude *p, const char *prelude, size_t len)
{
    DCHECK(prelude != NULL, "an at-rule prelude was read with no text — an EMPTY prelude is a real prelude "
                            "that matches no statement at-rule's grammar, and the absence of one is a caller "
                            "that never asked the parse for it");
    p->base = prelude;
    p->len = len;
    p->tkz = lxb_css_syntax_tokenizer_create();
    CHECK(p->tkz != NULL, "cssom: the at-rule prelude tokenizer allocation failed");
    if (lxb_css_syntax_tokenizer_init(p->tkz) != LXB_STATUS_OK) {
        lxb_css_syntax_tokenizer_destroy(p->tkz);
        p->tkz = NULL;
        return false;
    }
    lxb_css_syntax_tokenizer_buffer_set(p->tkz, (const lxb_char_t *)prelude, len);
    return true;
}

static void pre_close(Prelude *p)
{
    if (p->tkz) lxb_css_syntax_tokenizer_destroy(p->tkz);
    p->tkz = NULL;
}

void css_import_prelude_free(CssImportPrelude *p)
{
    free(p->href);
    free(p->layer_name);
    free(p->supports_text);
    free(p->media_text);
    p->href = p->layer_name = p->supports_text = p->media_text = NULL;
}

bool css_prelude_import(const char *prelude, size_t len, CssImportPrelude *out)
{
    Prelude p = { NULL, NULL, 0 };
    CssImportPrelude got = { NULL, NULL, NULL, NULL };
    lxb_css_syntax_token_t *t;

    DCHECK(out != NULL, "§6.4.4's prelude was parsed with nowhere to report its parts");
    if (!pre_open(&p, prelude, len)) return false;
    got.href = pre_url_or_string(&p);
    if (!got.href) goto fail;
    /* `[ layer | layer(<layer-name>) ]?` — the KEYWORD is the anonymous layer, which §6.4.4 answers as the
       empty string, and the FUNCTION carries a name. A `layer(` that never closes is not a match. */
    t = pre_peek_ws(&p);
    if (!t) goto fail;
    if (t->type == LXB_CSS_SYNTAX_TOKEN_IDENT && pre_name_is(t, "layer")) {
        got.layer_name = pre_copy("", 0);
        pre_take(&p);
        t = pre_peek_ws(&p);
    } else if (t->type == LXB_CSS_SYNTAX_TOKEN_FUNCTION && pre_name_is(t, "layer")) {
        got.layer_name = pre_function_contents(&p);
        if (!got.layer_name) goto fail;
        t = pre_peek_ws(&p);
    }
    if (!t) goto fail;
    if (t->type == LXB_CSS_SYNTAX_TOKEN_FUNCTION && pre_name_is(t, "supports")) {
        got.supports_text = pre_function_contents(&p);
        if (!got.supports_text) goto fail;
        t = pre_peek_ws(&p);
    }
    if (!t) goto fail;
    /* WHAT IS LEFT IS THE MEDIA QUERY LIST, taken as a raw span from where the cursor stands to the end of the
       prelude. It is not re-tokenized here: §4.4's create-a-MediaList runs MQ4 §3.1's own parse over it, which
       is what canonicalises it and what decides that an unparseable query is `not all`. */
    got.media_text = pre_span_trimmed(&p, pre_at(&p, t), len);
    pre_close(&p);
    *out = got;
    return true;

fail:
    css_import_prelude_free(&got);
    pre_close(&p);
    return false;
}

bool css_prelude_namespace(const char *prelude, size_t len, char **pprefix, char **puri)
{
    Prelude p = { NULL, NULL, 0 };
    lxb_css_syntax_token_t *t;
    char *prefix = NULL, *uri = NULL;

    DCHECK(pprefix != NULL && puri != NULL,
           "§6.4.9's prelude was parsed with nowhere to report its prefix or its namespace");
    if (!pre_open(&p, prelude, len)) return false;
    t = pre_peek_ws(&p);
    if (!t) goto fail;
    /* `<namespace-prefix>?`. A `url(`-shaped token is the URL and never the prefix, which is why the prefix
       arm tests for a bare IDENT: `@namespace url(http://servo1)` declares the DEFAULT namespace. */
    if (t->type == LXB_CSS_SYNTAX_TOKEN_IDENT) {
        prefix = pre_token_string(t);
        pre_take(&p);
    } else {
        prefix = pre_copy("", 0);
    }
    uri = pre_url_or_string(&p);
    if (!uri) goto fail;
    /* CSS Namespaces §2's production ENDS there, and a prelude with anything after it matches no rule — CSS
       Syntax drops an at-rule whose grammar failed, which is why `@namespace a b c;` is not in `cssRules`. */
    t = pre_peek_ws(&p);
    if (!t || t->type != LXB_CSS_SYNTAX_TOKEN__EOF) goto fail;
    pre_close(&p);
    *pprefix = prefix;
    *puri = uri;
    return true;

fail:
    free(prefix);
    free(uri);
    pre_close(&p);
    return false;
}

/* ---- CSS Paged Media §4.3's `<page-selector-list>` ---------------------------------------------------------
 *
 * See css_at_rule_prelude.h for why the parse and the serialization are one step, why the grammar's
 * whitespace-sensitivity is what makes a tokenizer necessary here, and where each piece of the serialization
 * comes from. */

/* The serialization being built. It is its own buffer rather than one of the file's `char *` returns because a
   page selector list is a JOIN — the pieces are appended one at a time and the count is not known up front. */
typedef struct { char *s; size_t len, cap; } PBuf;

static void pbuf_add_n(PBuf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 32;
        char *grown;

        while (cap < b->len + n + 1) cap *= 2;
        grown = realloc(b->s, cap);
        CHECK(grown != NULL, "cssom: OOM serializing a page selector list");
        b->s = grown;
        b->cap = cap;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = '\0';
}

static void pbuf_add(PBuf *b, const char *s) { pbuf_add_n(b, s, strlen(s)); }

/* §4.3's `<pseudo-page> = : [ left | right | first | blank ]` — the four, and nothing else is one. The table
   is also the SERIALIZATION, which is why it is spelled in lowercase: a CSS keyword is ASCII case-insensitive,
   so `:First` matches this entry and is written back out as this entry. */
static const char *const PRE_PSEUDO_PAGES[] = { "left", "right", "first", "blank" };
#define PRE_PSEUDO_PAGE_N ((unsigned)(sizeof(PRE_PSEUDO_PAGES) / sizeof(PRE_PSEUDO_PAGES[0])))

/* ONE `<page-selector> = [ <ident-token>? <pseudo-page>* ]!`, appended to `out`. The `!` is what makes the
   return value meaningful: the group must produce at least one value, so an EMPTY page selector is not one —
   which is what refuses a trailing comma and a `@page ,` alike.
   EVERY PEEK HERE IS THE RAW ONE. `pre_peek_ws` would eat exactly the whitespace §4.3 forbids between these
   productions, so a whitespace token simply ends the selector and the caller then requires a comma or the end
   of the prelude where it stands. */
static bool pre_page_selector(Prelude *p, PBuf *out)
{
    lxb_css_syntax_token_t *t = pre_peek(p);
    bool produced = false;

    if (!t) return false;
    if (t->type == LXB_CSS_SYNTAX_TOKEN_IDENT) {
        const lxb_css_syntax_token_string_t *s = lxb_css_syntax_token_string(t);
        char *name = pre_copy((const char *)s->data, s->length), *id;

        /* §2.1's serialize an identifier, on the page NAME. Its case is the author's — a page name is an
           author-chosen ident, not a keyword — so only the escaping is this step's. */
        id = css_serialize_identifier(name, s->length);
        pre_take(p);
        pbuf_add(out, id);
        free(id);
        free(name);
        produced = true;
    }
    for (;;) {
        unsigned i;

        t = pre_peek(p);
        if (!t || t->type != LXB_CSS_SYNTAX_TOKEN_COLON) break;
        pre_take(p);
        t = pre_peek(p);
        if (!t || t->type != LXB_CSS_SYNTAX_TOKEN_IDENT) return false;
        for (i = 0; i < PRE_PSEUDO_PAGE_N; i++)
            if (pre_name_is(t, PRE_PSEUDO_PAGES[i])) break;
        if (i == PRE_PSEUDO_PAGE_N) return false;
        pre_take(p);
        pbuf_add(out, ":");
        pbuf_add(out, PRE_PSEUDO_PAGES[i]);
        produced = true;
    }
    return produced;
}

char *css_prelude_page_selectors(const char *prelude, size_t len)
{
    Prelude p = { NULL, NULL, 0 };
    PBuf out = { NULL, 0, 0 };
    lxb_css_syntax_token_t *t;

    if (!pre_open(&p, prelude, len)) return NULL;
    t = pre_peek_ws(&p);
    if (!t) goto fail;
    /* `<page-selector-list>?` — `@page { }` declares NO list, which is the empty one and a real answer. The
       loop is entered only when there is something to read, and it is left only at the END of the prelude, so
       a trailing comma reaches the selector parse with nothing in front of it and the group's `!` refuses it. */
    if (t->type != LXB_CSS_SYNTAX_TOKEN__EOF) {
        for (;;) {
            if (!pre_page_selector(&p, &out)) goto fail;
            t = pre_peek_ws(&p);
            if (!t) goto fail;
            if (t->type == LXB_CSS_SYNTAX_TOKEN__EOF) break;
            /* The `#` multiplier, whose commas MAY carry whitespace — it is only INSIDE a selector that §4.3
               forbids it. */
            if (t->type != LXB_CSS_SYNTAX_TOKEN_COMMA) goto fail;
            pre_take(&p);
            if (!pre_peek_ws(&p)) goto fail;
            pbuf_add(&out, ", ");
        }
    }
    pre_close(&p);
    return out.s ? out.s : pre_copy("", 0);

fail:
    free(out.s);
    pre_close(&p);
    return NULL;
}

/* ---- CSS Animations §3's `<keyframes-name>` and `<keyframe-selector>#` -------------------------------------
 *
 * See css_at_rule_prelude.h for why both live here, what each refuses, and why only the second one serializes.
 */

/* An ASCII case-insensitive equality against a lowercase literal, which is what a CSS KEYWORD comparison is.
   `pre_name_is` is the same test against a TOKEN; this one is against a name already copied out of one, which
   is the shape the serialization side has. */
static bool pre_keyword_is(const char *v, const char *lower)
{
    size_t i;

    for (i = 0; lower[i]; i++) {
        char c = v[i];

        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != lower[i]) return false;
    }
    return v[i] == '\0';
}

bool css_prelude_keyframes_name_excluded(const char *name)
{
    DCHECK(name != NULL, "a `<keyframes-name>` exclusion was asked about with no name");
    /* CSS Values §4.2's `<custom-ident>` set is READ FROM THE ONE PLACE THAT HOLDS IT (core/css/css_defaulting.h)
       and never restated: a second copy here could disagree about `revert-layer`, and that test already folds
       ASCII case as §4.2 requires. `none` is CSS Animations §3's own addition on top of it — the clause §4.2
       reserves for a specification using the production ("must specify clearly what other keywords are
       excluded"), which is why it is spelled here and the rest is not. */
    return css_custom_ident_excluded(name) || pre_keyword_is(name, "none");
}

char *css_prelude_keyframes_name(const char *prelude, size_t len)
{
    Prelude p = { NULL, NULL, 0 };
    lxb_css_syntax_token_t *t;
    char *name = NULL;

    if (!pre_open(&p, prelude, len)) return NULL;
    t = pre_peek_ws(&p);
    if (!t) goto fail;
    if (t->type == LXB_CSS_SYNTAX_TOKEN_IDENT) {
        name = pre_token_string(t);
        /* The `<custom-ident>` arm, and the only arm the exclusions apply to. */
        if (css_prelude_keyframes_name_excluded(name)) goto fail;
        pre_take(&p);
    } else if (t->type == LXB_CSS_SYNTAX_TOKEN_STRING) {
        name = pre_token_string(t);
        /* "The <string> additionally excludes the empty string (but allows the string "none" and other
           excluded keywords)" — so this arm tests LENGTH and never the keyword list. */
        if (!*name) goto fail;
        pre_take(&p);
    } else {
        goto fail;
    }
    /* The production is one value, so a prelude with anything after it matches no `@keyframes` at all. */
    t = pre_peek_ws(&p);
    if (!t || t->type != LXB_CSS_SYNTAX_TOKEN__EOF) goto fail;
    pre_close(&p);
    return name;

fail:
    free(name);
    pre_close(&p);
    return NULL;
}

/* ONE `<keyframe-selector> = from | to | <percentage [0,100]>`, as the percentage it denotes. `*pct` is the
   number `keyText` reports; false when the token in front of the cursor is not one of the three, which
   includes a `<number>` (§3's note: "the percentage unit specifier must be used ... Therefore, 0 is an invalid
   keyframe selector") and a percentage outside the range (§3: "Values less than 0% or higher than 100% are
   invalid and cause their <keyframe-block> to be ignored"). */
static bool pre_keyframe_selector(Prelude *p, double *pct)
{
    lxb_css_syntax_token_t *t = pre_peek_ws(p);

    if (!t) return false;
    if (t->type == LXB_CSS_SYNTAX_TOKEN_IDENT) {
        if (pre_name_is(t, "from")) *pct = 0.0;
        else if (pre_name_is(t, "to")) *pct = 100.0;
        else return false;
        pre_take(p);
        return true;
    }
    if (t->type != LXB_CSS_SYNTAX_TOKEN_PERCENTAGE) return false;
    *pct = lxb_css_syntax_token_percentage(t)->num;
    if (!(*pct >= 0.0 && *pct <= 100.0)) return false;   /* written so a NaN is refused by the same test */
    pre_take(p);
    return true;
}

char *css_prelude_keyframe_selectors(const char *prelude, size_t len)
{
    Prelude p = { NULL, NULL, 0 };
    PBuf out = { NULL, 0, 0 };
    lxb_css_syntax_token_t *t;

    if (!pre_open(&p, prelude, len)) return NULL;
    for (;;) {
        double pct = 0.0;
        char *one;

        if (!pre_keyframe_selector(&p, &pct)) goto fail;
        one = css_length_serialize_pct(pct);
        pbuf_add(&out, one);
        free(one);
        t = pre_peek_ws(&p);
        if (!t) goto fail;
        if (t->type == LXB_CSS_SYNTAX_TOKEN__EOF) break;
        if (t->type != LXB_CSS_SYNTAX_TOKEN_COMMA) goto fail;
        pre_take(&p);
        pbuf_add(&out, ", ");
    }
    pre_close(&p);
    DCHECK(out.s != NULL && out.s[0] != '\0',
           "a keyframe selector list parsed to the EMPTY string — the `#` multiplier has no zero-length arm, "
           "so the loop above cannot leave without having appended at least one percentage");
    return out.s;

fail:
    free(out.s);
    pre_close(&p);
    return NULL;
}

/* ---- CSS Cascade §6.4.2's `<layer-name>`, as §6.4.4's two at-rules take it ---------------------------------
 *
 * See css_at_rule_prelude.h for why one entry serves both `@layer` grammars, why an empty list is an answer
 * rather than a failure, where the whitespace is significant, and why each name crosses serialized. */

/* ONE NAME JOINING A LIST OF THEM. The growth is shared by the two at-rules that declare a `#`-multiplied list
   of names (`@layer`'s `<layer-name>#` and `@property`'s `<custom-property-name>#`), which is why it takes the
   two fields rather than either list type — the lists are two different FACTS and this is one allocation. */
static void pre_names_push(char ***pv, unsigned *pn, char *one)   /* CONSUMES one */
{
    char **grown = realloc(*pv, (size_t)(*pn + 1) * sizeof(*grown));

    CHECK(grown != NULL, "cssom: OOM collecting an at-rule's declared names");
    *pv = grown;
    (*pv)[(*pn)++] = one;
}

static void pre_layer_names_push(CssLayerNames *out, char *one)   /* CONSUMES one */
{
    pre_names_push(&out->v, &out->n, one);
}

void css_layer_names_free(CssLayerNames *p)
{
    unsigned i;

    DCHECK(p != NULL, "an `@layer` at-rule's layer names were freed through no list");
    for (i = 0; i < p->n; i++) free(p->v[i]);
    free(p->v);
    p->v = NULL;
    p->n = 0;
}

/* ONE `<layer-name> = <ident> [ '.' <ident> ]*`, appended to `out` ALREADY SERIALIZED.
   EVERY PEEK HERE IS THE RAW ONE, for the reason `pre_page_selector`'s are: `pre_peek_ws` would eat exactly the
   whitespace §6.4.2 forbids between the segments, so a whitespace token simply ends the name and the caller
   then requires a comma or the end of the prelude where it stands. */
static bool pre_layer_name(Prelude *p, PBuf *out)
{
    for (;;) {
        lxb_css_syntax_token_t *t = pre_peek(p);
        const lxb_css_syntax_token_string_t *s;
        char *raw, *id;

        if (!t || t->type != LXB_CSS_SYNTAX_TOKEN_IDENT) return false;
        s = lxb_css_syntax_token_string(t);
        raw = pre_copy((const char *)s->data, s->length);
        /* §6.4.2: "The CSS-wide keywords are reserved for future use, and cause the rule to be INVALID AT
           PARSE TIME if used as an <ident> in the <layer-name>." The token's own value is what is tested, so
           `@layer \69 nherit` is refused too — an escape is spelling and not a different identifier. */
        if (css_wide_keyword(raw)) { free(raw); return false; }
        id = css_serialize_identifier(raw, s->length);
        free(raw);
        pre_take(p);
        pbuf_add(out, id);
        free(id);
        /* `[ '.' <ident> ]*`. CSS Syntax tokenizes a period that does not start a number as a DELIM, so this
           is the whole of the separator test — and a `.` followed by anything but an ident (`a.`, `a.5b`) ends
           the loop with the cursor on it, which the caller then refuses as neither a comma nor the end. */
        t = pre_peek(p);
        if (!t || t->type != LXB_CSS_SYNTAX_TOKEN_DELIM || lxb_css_syntax_token_delim_char(t) != '.')
            return true;
        pre_take(p);
        pbuf_add(out, ".");
    }
}

bool css_prelude_layer_names(const char *prelude, size_t len, CssLayerNames *out)
{
    Prelude p = { NULL, NULL, 0 };
    CssLayerNames got = { NULL, 0 };
    lxb_css_syntax_token_t *t;

    DCHECK(out != NULL, "an `@layer` at-rule's prelude was parsed with nowhere to report its layer names");
    if (!pre_open(&p, prelude, len)) return false;
    t = pre_peek_ws(&p);
    if (!t) goto fail;
    /* The EMPTY prelude is `@layer { }`'s ANONYMOUS layer — no names at all, which is a real answer and the one
       thing the two callers disagree about. The loop is entered only when there is something to read, and it is
       left only at the END of the prelude, so a trailing comma reaches `pre_layer_name` with the EOF token in
       front of it and is refused there. */
    if (t->type != LXB_CSS_SYNTAX_TOKEN__EOF) {
        for (;;) {
            PBuf one = { NULL, 0, 0 };

            if (!pre_layer_name(&p, &one)) { free(one.s); goto fail; }
            DCHECK(one.s != NULL && one.s[0] != '\0',
                   "a `<layer-name>` parsed to nothing — the production's first term is an `<ident>` and the "
                   "parse above answers false without one, so a match has appended at least one segment");
            pre_layer_names_push(&got, one.s);
            t = pre_peek_ws(&p);
            if (!t) goto fail;
            if (t->type == LXB_CSS_SYNTAX_TOKEN__EOF) break;
            /* The `#` multiplier, whose commas MAY carry whitespace — it is only INSIDE a name that §6.4.2
               forbids it. */
            if (t->type != LXB_CSS_SYNTAX_TOKEN_COMMA) goto fail;
            pre_take(&p);
            if (!pre_peek_ws(&p)) goto fail;
        }
    }
    pre_close(&p);
    *out = got;
    return true;

fail:
    css_layer_names_free(&got);
    pre_close(&p);
    return false;
}

void css_layer_name_segments(const char *name, CssLayerNames *out)
{
    CssLayerNames got = { NULL, 0 };
    size_t i = 0, start = 0;

    DCHECK(name != NULL && out != NULL,
           "a `<layer-name>` was split with no name, or with nowhere to report its segments");
    DCHECK(name != NULL && name[0] != '\0',
           "an EMPTY `<layer-name>` was split into segments. The production's first term is an `<ident>` and "
           "the parse above answers false without one, so no rule in this build stores an empty name — "
           "§6.4.2.1's ANONYMOUS layer is the ABSENCE of a name, reported as such at the caller, and a layer "
           "named by the empty string would unify with every other empty one instead of being unique");
    /* THE ESCAPE IS WHAT MAKES THIS A SCAN AND NOT A `strchr` LOOP. Every segment came out of
       `css_serialize_identifier` above, which writes a period inside a segment as `\.` (a period is not one of
       §2.1's escape-as-code-point cases, so it takes the escape-a-character arm) and a backslash as `\\`. So a
       BACKSLASH consumes exactly the next byte — which is what tells `a\.b`, ONE segment, from `a.b`, two, and
       what keeps `a\\` followed by `.b` reading as two. Nothing else in a serialized identifier can be a bare
       period, so a period this scan reaches is always §6.4.2's separator. */
    for (;;) {
        if (name[i] == '\\' && name[i + 1] != '\0') { i += 2; continue; }
        if (name[i] != '.' && name[i] != '\0') { i++; continue; }
        pre_layer_names_push(&got, pre_copy(name + start, i - start));
        if (name[i] == '\0') break;
        start = ++i;
    }
    *out = got;
}

/* ---- CSS Conditional 5 §5.4's `@container` — its `<container-condition>#` prelude -------------------------
 *
 * See css_at_rule_prelude.h for the grammar, for why the name crosses serialized while the query crosses raw,
 * and for why a group this build cannot read is the `<general-enclosed>` arm rather than a parse failure. */

void css_container_conditions_free(CssContainerConditions *p)
{
    unsigned i;

    DCHECK(p != NULL, "an `@container` at-rule's conditions were freed through no list");
    for (i = 0; i < p->n; i++) {
        free(p->v[i].name);
        free(p->v[i].query);
    }
    free(p->v);
    p->v = NULL;
    p->n = 0;
}

static void pre_container_push(CssContainerConditions *c, char *name, char *query)
{
    CssContainerCondition *grown = realloc(c->v, (size_t)(c->n + 1) * sizeof *grown);

    CHECK(grown != NULL, "cssom: OOM extending an `@container` rule's condition list");
    c->v = grown;
    c->v[c->n].name = name;
    c->v[c->n].query = query;
    c->n++;
}

/* IS `name` A KEYWORD A `<container-name>` CANNOT BE — CSS Values §4.2's exclusions from `<custom-ident>` (the
   CSS-wide keywords and the reserved `default`), read from the one place that holds them, plus §5.4's own
   addition on top: "the keywords none, and, not, and or are excluded from the <custom-ident> above". That is
   the clause §4.2 reserves for a specification using the production, which is why these four are spelled here
   and the rest is not — the same arrangement `css_prelude_keyframes_name_excluded` has for its one keyword.
   ONE OF THE FOUR IS ALSO WHAT KEEPS THE TWO OPTIONAL TERMS APART. `not` is the only bare ident a
   `<container-query>` can begin with, so excluding it from the name is what makes `@container not (width >
   0px)` a NAMELESS condition rather than a condition named `not` followed by a query that cannot parse — and
   the caller must therefore treat an excluded ident as "not a name" and read on, never as a failure. */
static bool pre_container_name_excluded(const char *name)
{
    DCHECK(name != NULL, "a `<container-name>` exclusion was asked about with no name");
    return css_custom_ident_excluded(name) || pre_keyword_is(name, "none") || pre_keyword_is(name, "and") ||
           pre_keyword_is(name, "not") || pre_keyword_is(name, "or");
}

/* ONE BALANCED GROUP, consumed — the whole of `<query-in-parens>` at the token level, for the reason the header
   gives: `<general-enclosed>` admits any `( <any-value>? )` and any `<function-token> <any-value>? )`, so every
   arm of that production is "a group that opens and closes" and nothing inside one decides whether the RULE is
   valid. Depth counts the two tokens that OPEN a group — CSS Syntax's `(` and its `name(` — because
   `((a) or (b))` closes three and the first `)` is not the outer one's. False at EOF: an unterminated group is
   a prelude the tokenizer ran off the end of, which matches no production. */
static bool pre_query_in_parens(Prelude *p)
{
    lxb_css_syntax_token_t *t = pre_peek_ws(p);
    size_t depth = 1;

    if (!t) return false;
    if (t->type != LXB_CSS_SYNTAX_TOKEN_L_PARENTHESIS && t->type != LXB_CSS_SYNTAX_TOKEN_FUNCTION) return false;
    pre_take(p);
    for (;;) {
        t = pre_peek(p);
        if (!t || t->type == LXB_CSS_SYNTAX_TOKEN__EOF) return false;
        if (t->type == LXB_CSS_SYNTAX_TOKEN_FUNCTION || t->type == LXB_CSS_SYNTAX_TOKEN_L_PARENTHESIS) depth++;
        else if (t->type == LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS && --depth == 0) { pre_take(p); return true; }
        pre_take(p);
    }
}

/* §5.4's `<container-query>`, CONSUMED, leaving the cursor on the token that ends it (a comma or the EOF, which
   is the caller's to check — the `not` arm takes ONE `<query-in-parens>` and admits nothing after it, so a
   trailing `and (x)` has to be refused where the whole condition's terminator is decided rather than here).
   THE MIXING RULE IS THE LOOP'S ONE PIECE OF STATE. §5.4's three alternatives contain no production admitting
   both `and` and `or`, so the first combinator seen fixes which the rest must be — `(a) and (b) or (c)` is not
   a query, and `(a) and ((b) or (c))` is, which is what "without a layer of parentheses" means. */
static bool pre_container_query(Prelude *p)
{
    lxb_css_syntax_token_t *t = pre_peek_ws(p);
    char op = 0;

    if (!t) return false;
    /* `not <query-in-parens>` — matched as an IDENT, which is what enforces the space: CSS Syntax tokenizes
       `not(x)` as ONE function token, so it falls to the `<query-in-parens>` arm below and is a
       `<general-enclosed>` that evaluates to unknown rather than a negation. */
    if (t->type == LXB_CSS_SYNTAX_TOKEN_IDENT && pre_name_is(t, "not")) {
        pre_take(p);
        return pre_query_in_parens(p);
    }
    if (!pre_query_in_parens(p)) return false;
    for (;;) {
        t = pre_peek_ws(p);
        if (!t) return false;
        if (t->type == LXB_CSS_SYNTAX_TOKEN__EOF || t->type == LXB_CSS_SYNTAX_TOKEN_COMMA) return true;
        if (t->type != LXB_CSS_SYNTAX_TOKEN_IDENT) return false;
        if (pre_name_is(t, "and")) { if (op == 'o') return false; op = 'a'; }
        else if (pre_name_is(t, "or")) { if (op == 'a') return false; op = 'o'; }
        else return false;
        pre_take(p);
        if (!pre_query_in_parens(p)) return false;
    }
}

/* ONE `<container-condition> = [ <container-name>? <container-query>? ]!`, both halves OWNED and never NULL on
   a match — §9.1's dictionary has no absent member, only an empty string. */
static bool pre_container_condition(Prelude *p, CssContainerCondition *out)
{
    lxb_css_syntax_token_t *t = pre_peek_ws(p);
    char *name = NULL, *query = NULL;
    size_t qbegin;

    if (!t) return false;
    /* `<container-name>?`. A bare IDENT here is the name UNLESS the production excludes it, and an excluded
       one is NOT REFUSED HERE — it is simply not a name, and the parse falls through to `<container-query>?`
       with the cursor still on it. That distinction is the whole reason the exclusion test decides this rather
       than a lookahead: `@container not (width > 0px)` is a NAMELESS condition whose query begins with the
       `not` arm, and refusing the condition on meeting `not` would drop a rule §5.4 admits. The other excluded
       keywords cannot begin a query either, so they fall through and are refused one line down by the grammar
       that really excludes them rather than by a second copy of the rule here — `@container none { }` and
       `@container and (width > 0px) { }` are at-rules whose grammar failed. */
    if (t->type == LXB_CSS_SYNTAX_TOKEN_IDENT) {
        const lxb_css_syntax_token_string_t *s = lxb_css_syntax_token_string(t);
        char *raw = pre_copy((const char *)s->data, s->length);

        if (!pre_container_name_excluded(raw)) {
            name = css_serialize_identifier(raw, s->length);
            pre_take(p);
        }
        free(raw);
    }
    /* `<container-query>?`. Its RAW SPAN runs from the first token of the query to wherever the condition
       ends, trimmed — see the header for why nothing re-serializes it. */
    t = pre_peek_ws(p);
    if (!t) { free(name); return false; }
    if (t->type != LXB_CSS_SYNTAX_TOKEN__EOF && t->type != LXB_CSS_SYNTAX_TOKEN_COMMA) {
        qbegin = pre_at(p, t);
        if (!pre_container_query(p)) { free(name); return false; }
        t = pre_peek(p);
        if (!t) { free(name); return false; }
        query = pre_span_trimmed(p, qbegin, pre_at(p, t));
        /* The `not` arm returns without checking what follows it, and the loop arm returns only ON the
           terminator, so this is where BOTH are held to the same rule — `not (a) and (b)` ends here with the
           cursor on an IDENT and is refused, which is §5.4's grammar and not an extra restriction. */
        t = pre_peek_ws(p);
        if (!t || (t->type != LXB_CSS_SYNTAX_TOKEN__EOF && t->type != LXB_CSS_SYNTAX_TOKEN_COMMA)) {
            free(name);
            free(query);
            return false;
        }
    }
    /* The `!` — the group is not optional even though both of its terms are. */
    if (!name && !query) return false;
    out->name = name ? name : pre_copy("", 0);
    out->query = query ? query : pre_copy("", 0);
    return true;
}

bool css_prelude_container_conditions(const char *prelude, size_t len, CssContainerConditions *out)
{
    Prelude p = { NULL, NULL, 0 };
    CssContainerConditions got = { NULL, 0 };
    lxb_css_syntax_token_t *t;

    DCHECK(out != NULL, "an `@container` at-rule's prelude was parsed with nowhere to report its conditions");
    if (!pre_open(&p, prelude, len)) return false;
    for (;;) {
        CssContainerCondition one = { NULL, NULL };

        if (!pre_container_condition(&p, &one)) goto fail;
        pre_container_push(&got, one.name, one.query);
        t = pre_peek_ws(&p);
        if (!t) goto fail;
        if (t->type == LXB_CSS_SYNTAX_TOKEN__EOF) break;
        /* The `#` multiplier. A trailing comma re-enters the condition parse with EOF in front of it, where
           the `!` refuses it — `@container a, ` is not a rule. */
        if (t->type != LXB_CSS_SYNTAX_TOKEN_COMMA) goto fail;
        pre_take(&p);
    }
    pre_close(&p);
    DCHECK(got.n > 0, "an `@container` prelude matched the grammar and produced no conditions — the `#` "
                      "multiplier has no zero-length arm and the loop above appends before it can break");
    *out = got;
    return true;

fail:
    css_container_conditions_free(&got);
    pre_close(&p);
    return false;
}

/* ---- CSS Properties and Values API 1 §3's `@property` — its prelude AND its descriptors --------------------
 *
 * See css_at_rule_prelude.h for why one at-rule's grammar stays in one file even when half of it is a body,
 * what a `<custom-property-name>` is, and why a name crosses unescaped and case-sensitive. */

void css_property_names_free(CssPropertyNames *p)
{
    unsigned i;

    DCHECK(p != NULL, "an `@property` at-rule's custom property names were freed through no list");
    for (i = 0; i < p->n; i++) free(p->v[i]);
    free(p->v);
    p->v = NULL;
    p->n = 0;
}

bool css_prelude_property_names(const char *prelude, size_t len, CssPropertyNames *out)
{
    Prelude p = { NULL, NULL, 0 };
    CssPropertyNames got = { NULL, 0 };
    lxb_css_syntax_token_t *t;

    DCHECK(out != NULL, "§3's prelude was parsed with nowhere to report the custom property names it declares");
    if (!pre_open(&p, prelude, len)) return false;
    for (;;) {
        char *one;

        /* A `<dashed-ident>` IS AN IDENT TOKEN and there is no other shape it can arrive in: CSS Syntax §4.3.9
           starts an ident sequence on a U+002D followed by another U+002D, so `--foo` tokenizes whole. */
        t = pre_peek_ws(&p);
        if (!t || t->type != LXB_CSS_SYNTAX_TOKEN_IDENT) goto fail;
        one = pre_token_string(t);
        /* CSS Variables §2's two restrictions, and both are on the token's VALUE rather than on its spelling:
           the name must start with two dashes, and `--` alone is "reserved for future use by CSS" and is not a
           custom property name. `\2d\2d x` is therefore `--x` and is accepted, which is the same reading that
           makes `@layer \69 nherit` refused. */
        if (one[0] != '-' || one[1] != '-' || one[2] == '\0') { free(one); goto fail; }
        pre_take(&p);
        pre_names_push(&got.v, &got.n, one);
        t = pre_peek_ws(&p);
        if (!t) goto fail;
        if (t->type == LXB_CSS_SYNTAX_TOKEN__EOF) break;
        /* The `#` multiplier, whose commas may carry whitespace. A TRAILING comma reaches the top of the loop
           with the EOF token in front of it and is refused there, so `@property --a, { }` is not a rule. */
        if (t->type != LXB_CSS_SYNTAX_TOKEN_COMMA) goto fail;
        pre_take(&p);
    }
    DCHECK(got.n >= 1, "an `@property` prelude matched `<custom-property-name>#` with no name in it — the `#` "
                       "multiplier has no zero-length arm and the loop above cannot leave without pushing one");
    pre_close(&p);
    *out = got;
    return true;

fail:
    css_property_names_free(&got);
    pre_close(&p);
    return false;
}

char *css_property_descriptor_syntax(const char *value, size_t len)
{
    Prelude p = { NULL, NULL, 0 };
    lxb_css_syntax_token_t *t;
    char *out = NULL;

    if (!pre_open(&p, value, len)) return NULL;
    t = pre_peek_ws(&p);
    if (!t || t->type != LXB_CSS_SYNTAX_TOKEN_STRING) goto fail;
    /* The token's own string, which the tokenizer has already unescaped — the whole reason this file tokenizes
       rather than scanning for quotes. */
    out = pre_token_string(t);
    pre_take(&p);
    /* `Value: <string>` is ONE value, so a descriptor with anything after it does not match §3.1's grammar. */
    t = pre_peek_ws(&p);
    if (!t || t->type != LXB_CSS_SYNTAX_TOKEN__EOF) goto fail;
    pre_close(&p);
    return out;

fail:
    free(out);
    pre_close(&p);
    return NULL;
}

bool css_property_descriptor_inherits(const char *value, size_t len, bool *pinherits)
{
    Prelude p = { NULL, NULL, 0 };
    lxb_css_syntax_token_t *t;
    bool got = false, ok = false;

    DCHECK(pinherits != NULL, "§3.2's `inherits` descriptor was read with nowhere to report the flag it sets");
    if (!pre_open(&p, value, len)) return false;
    t = pre_peek_ws(&p);
    if (!t || t->type != LXB_CSS_SYNTAX_TOKEN_IDENT) goto done;
    if (pre_name_is(t, "true")) got = true;
    else if (pre_name_is(t, "false")) got = false;
    else goto done;
    pre_take(&p);
    t = pre_peek_ws(&p);
    if (!t || t->type != LXB_CSS_SYNTAX_TOKEN__EOF) goto done;
    ok = true;

done:
    pre_close(&p);
    if (ok) *pinherits = got;
    return ok;
}
