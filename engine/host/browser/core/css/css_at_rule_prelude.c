/* CSS Cascade §2's, CSS Namespaces §2's and CSS Paged Media §4.3's at-rule preludes. See
 * css_at_rule_prelude.h for why this is a component, why it tokenizes with lexbor's own tokenizer, and which
 * spans stay raw source. */
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/css/syntax/token.h>
#include <lexbor/css/syntax/tokenizer.h>

#include "check.h"
#include "core/css/css_at_rule_prelude.h"
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
