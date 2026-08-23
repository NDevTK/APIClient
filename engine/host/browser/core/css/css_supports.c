/* CSS Conditional Rules Module Level 3 §6 "Feature queries: the @supports rule" and §6.1 "Definition of
 * support". See css_supports.h for the grammar, for why validity and truth are two answers, for why
 * `<general-enclosed>` is FALSE here where a media query's is unknown, and for why this is concrete and not
 * concolic. */
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/css/syntax/token.h>
#include <lexbor/css/syntax/tokenizer.h>

#include "check.h"
#include "core/css/css_style_declaration.h"
#include "core/css/css_supports.h"

/* ONE CONDITION BEING READ. `base`/`len` are the span the offsets below index — every token lexbor hands back
   names its own span inside the buffer it was given, which is how a nested `( … )`'s contents are taken as raw
   source and handed to a fresh reader rather than re-serialized from tokens.
   THE RECURSION IS OVER A SPAN AND NOT OVER ONE CURSOR, and that is what makes §6's `<supports-in-parens>`
   implementable at all: its three arms are TRIED IN ORDER over the same text — `( <supports-condition> )`
   first, then `( <declaration> )`, then `<general-enclosed>` — which a forward-only tokenizer cannot back up
   over. A span can be read three times; a cursor can be read once.
   THE RECURSION IS THE AUTHOR'S OWN BRACKET NESTING, which is the same statement css_style_declaration.c makes
   about its rule walk and core/css/media_query.c's `parse_in_parens`/`parse_condition` pair makes about the
   grammar this one is a copy of. That component backtracks by COPYING its lexer struct, which is cheaper and
   is not available here: lexbor's tokenizer is not a value, so the alternatives are tried over a span instead
   and each level opens one. */
typedef struct {
    lxb_css_syntax_tokenizer_t *tkz;
    const char                 *base;
    size_t                      len;
} Sup;

static bool sup_open(Sup *p, const char *text, size_t len)
{
    DCHECK(text != NULL, "a `<supports-condition>` was read with no text — an EMPTY condition is a real span "
                         "that matches no production of §6's grammar, and the absence of one is a caller that "
                         "never asked the parse for it");
    p->base = text;
    p->len = len;
    p->tkz = lxb_css_syntax_tokenizer_create();
    CHECK(p->tkz != NULL, "cssom: the `<supports-condition>` tokenizer allocation failed");
    if (lxb_css_syntax_tokenizer_init(p->tkz) != LXB_STATUS_OK) {
        lxb_css_syntax_tokenizer_destroy(p->tkz);
        p->tkz = NULL;
        return false;
    }
    lxb_css_syntax_tokenizer_buffer_set(p->tkz, (const lxb_char_t *)text, len);
    return true;
}

static void sup_close(Sup *p)
{
    if (p->tkz) lxb_css_syntax_tokenizer_destroy(p->tkz);
    p->tkz = NULL;
}

/* The current token, never consumed. NULL is the tokenizer's own allocation failure, which every caller treats
   as "the grammar did not match" — a condition that could not be read is not a condition. */
static lxb_css_syntax_token_t *sup_peek(Sup *p) { return lxb_css_syntax_token(p->tkz); }

static void sup_take(Sup *p) { lxb_css_syntax_token_consume(p->tkz); }

static lxb_css_syntax_token_t *sup_peek_ws(Sup *p)
{
    lxb_css_syntax_token_t *t = sup_peek(p);

    while (t && t->type == LXB_CSS_SYNTAX_TOKEN_WHITESPACE) {
        sup_take(p);
        t = sup_peek(p);
    }
    return t;
}

/* WHERE THIS TOKEN STARTS in the span, as an offset — lexbor stamps every token with a pointer into the very
   buffer it was handed, so this is subtraction and not a second cursor that could drift. */
static size_t sup_at(const Sup *p, lxb_css_syntax_token_t *t)
{
    const char *b = (const char *)lxb_css_syntax_token_base(t)->begin;

    DCHECK(b >= p->base && b <= p->base + p->len,
           "a CSS token names a span outside the `<supports-condition>` it was tokenized from — the "
           "tokenizer's buffer IS that string, so a pointer outside it means the buffer was set from "
           "something else");
    if (b < p->base || b > p->base + p->len) return p->len;
    return (size_t)(b - p->base);
}

/* An IDENT token's spelling, ASCII case-insensitively — CSS Syntax makes every keyword in this grammar
   case-insensitive, so `NOT (display:flex)` is a negation. */
static bool sup_name_is(lxb_css_syntax_token_t *t, const char *lower)
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

/* CONSUME A BALANCED BLOCK whose opening token — `(`, `[`, `{`, or a FUNCTION token, which CSS Syntax says
 * opens a block just as `(` does — is the CURRENT token, and report the span STRICTLY INSIDE it.
 *
 * IT IS ALSO THE `<any-value>` CHECK, because those are one walk and not two. Media Queries Level 4 §3
 * "Syntax" builds `<general-enclosed>` out of `<any-value>`, and CSS Values names exactly what that production
 * refuses: a `<bad-string-token>`, a `<bad-url-token>`, and an unmatched `)`, `]` or `}`. A walk that has to
 * find the matching close already sees every one of those, so refusing them here is free and asking a second
 * pass for them would be a second chance to disagree about where the block ends.
 *
 * False is "this is not a balanced block", which makes the enclosing production fail and therefore makes the
 * whole at-rule invalid — §6's own answer for `@supports (display:flex` and for `@supports ([)`. */
static bool sup_block(Sup *p, size_t *pbegin, size_t *pend)
{
    lxb_css_syntax_token_t *t = sup_peek(p);
    int depth = 0;

    if (!t) return false;
    if (t->type != LXB_CSS_SYNTAX_TOKEN_L_PARENTHESIS && t->type != LXB_CSS_SYNTAX_TOKEN_FUNCTION)
        return false;
    depth = 1;
    sup_take(p);
    t = sup_peek(p);
    *pbegin = t ? sup_at(p, t) : p->len;
    for (;;) {
        if (!t) return false;
        switch (t->type) {
        case LXB_CSS_SYNTAX_TOKEN__EOF:
            return false;   /* the block never closed */
        case LXB_CSS_SYNTAX_TOKEN_BAD_STRING:
        case LXB_CSS_SYNTAX_TOKEN_BAD_URL:
            return false;   /* `<any-value>` admits neither */
        case LXB_CSS_SYNTAX_TOKEN_L_PARENTHESIS:
        case LXB_CSS_SYNTAX_TOKEN_FUNCTION:
        case LXB_CSS_SYNTAX_TOKEN_LS_BRACKET:
        case LXB_CSS_SYNTAX_TOKEN_LC_BRACKET:
            depth++;
            break;
        case LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS:
            depth--;
            if (depth == 0) {
                *pend = sup_at(p, t);
                sup_take(p);
                DCHECK(*pbegin <= *pend,
                       "a balanced `( … )` closed before it opened — the two offsets come from one forward "
                       "walk of one buffer, so a close standing before the first inner token means the "
                       "tokenizer handed back a span from a different buffer");
                return true;
            }
            break;
        /* A `]` or `}` that closes the block THIS walk is inside is an unmatched closer as far as
           `<any-value>` is concerned — the opener was a `(` and CSS Syntax closes it with `)` alone — so the
           nesting count is not decremented past a mismatch, it fails. Tracking WHICH bracket each level was
           opened by would be the same refusal reached one step later. */
        case LXB_CSS_SYNTAX_TOKEN_RS_BRACKET:
        case LXB_CSS_SYNTAX_TOKEN_RC_BRACKET:
            depth--;
            if (depth <= 0) return false;
            break;
        default:
            break;
        }
        sup_take(p);
        t = sup_peek(p);
    }
}

/* §6's `<supports-in-parens>`, at the cursor. `*matches` is its boolean; false is "this is not one", which
   makes the enclosing `<supports-condition>` fail. */
static bool sup_in_parens(Sup *p, bool *matches);

/* §6's `<supports-condition>` over a WHOLE span — the entry, and the recursion's own step for a nested
   `( <supports-condition> )`. */
static bool sup_condition(const char *text, size_t len, bool *matches)
{
    Sup p = { NULL, NULL, 0 };
    lxb_css_syntax_token_t *t;
    bool acc = false, term = false;
    int op = 0;   /* 0 = no operator yet, 'a' = this chain is `and`, 'o' = `or` */

    if (!sup_open(&p, text, len)) return false;
    t = sup_peek_ws(&p);
    if (!t) goto fail;
    /* `not <supports-in-parens>` — the whole of the first alternative, which admits NO trailing `and`/`or`
       (§6's three alternatives are alternatives, and mixing takes a layer of parentheses). */
    if (t->type == LXB_CSS_SYNTAX_TOKEN_IDENT && sup_name_is(t, "not")) {
        sup_take(&p);
        if (!sup_in_parens(&p, &term)) goto fail;
        acc = !term;
        t = sup_peek_ws(&p);
        if (!t || t->type != LXB_CSS_SYNTAX_TOKEN__EOF) goto fail;
        sup_close(&p);
        *matches = acc;
        return true;
    }
    if (!sup_in_parens(&p, &acc)) goto fail;
    for (;;) {
        t = sup_peek_ws(&p);
        if (!t) goto fail;
        if (t->type == LXB_CSS_SYNTAX_TOKEN__EOF) break;
        if (t->type != LXB_CSS_SYNTAX_TOKEN_IDENT) goto fail;
        if (sup_name_is(t, "and")) {
            if (op == 'o') goto fail;   /* §6: `and` and `or` may not be mixed without parentheses */
            op = 'a';
        } else if (sup_name_is(t, "or")) {
            if (op == 'a') goto fail;
            op = 'o';
        } else {
            goto fail;
        }
        sup_take(&p);
        /* EVERY TERM IS PARSED, AND THEREFORE EVERY TERM IS EVALUATED. Short-circuiting an `or` whose first
           term is true would skip the PARSE of the rest, and the rest is what decides whether the rule exists
           at all — `(display:block) or (!!!)` is invalid and its contents are dropped, however true the first
           term was. The evaluation rides along because the parse has to happen anyway. */
        if (!sup_in_parens(&p, &term)) goto fail;
        acc = (op == 'a') ? (acc && term) : (acc || term);
    }
    sup_close(&p);
    *matches = acc;
    return true;

fail:
    sup_close(&p);
    return false;
}

static bool sup_in_parens(Sup *p, bool *matches)
{
    lxb_css_syntax_token_t *t = sup_peek_ws(p);
    size_t b = 0, e = 0;

    if (!t) return false;
    /* A FUNCTION TOKEN CAN ONLY BE `<general-enclosed>` HERE. §6's other two arms both begin with a `(`, and
       `supports(` inside a `@supports` prelude is not one of them — it is the forward-compatibility production,
       whose result §6's table states as FALSE. The block is still consumed and still has to be balanced: an
       unclosed one makes the whole rule invalid. */
    if (t->type == LXB_CSS_SYNTAX_TOKEN_FUNCTION) {
        if (!sup_block(p, &b, &e)) return false;
        *matches = false;
        return true;
    }
    if (t->type != LXB_CSS_SYNTAX_TOKEN_L_PARENTHESIS) return false;
    if (!sup_block(p, &b, &e)) return false;
    /* §6'S THREE ARMS, IN THE GRAMMAR'S OWN ORDER, over the one span the block just delimited.
       There is no ambiguity to resolve between the first two and the order is what proves it: a
       `<supports-condition>` begins with `not` or with a `(`, and a `<declaration>` begins with an IDENT, so
       `(display: flex)` cannot match the first arm and `((a:b) or (c:d))` cannot match the second. */
    if (sup_condition(p->base + b, e - b, matches)) return true;
    /* `<supports-feature>` = `<supports-decl>` = `( <declaration> )`, §6.1's definition of support applied to
       what is inside. A span that is not one declaration answers false here, which is the same answer
       `<general-enclosed>` carries — so the two arms need not be told apart to be answered. */
    *matches = css_supports_declaration(p->base + b, e - b);
    return true;
}

bool css_supports_condition(const char *text, size_t len, bool *matches)
{
    DCHECK(matches != NULL, "a `<supports-condition>` was evaluated with nowhere to report its result — §6's "
                            "terms each carry a boolean and the caller is what acts on it");
    return sup_condition(text, len, matches);
}

bool css_supports_declaration(const char *decl, size_t len)
{
    Sup p = { NULL, NULL, 0 };
    lxb_css_syntax_token_t *t;
    int depth = 0;
    char *serialized;
    bool one = true;

    DCHECK(decl != NULL, "§6.1's definition of support was asked about no declaration at all");
    /* ONE `<declaration>`, WHICH IS WHAT THE SEMICOLON DECIDES. CSS Syntax's `<declaration>` is a single
       property and value with no terminator in it, so a top-level `;` means the parenthesized text is two
       declarations (or one and a fragment) and is therefore `<general-enclosed>` rather than
       `<supports-decl>`. It has to be refused HERE, before the serializer below, because that serializer
       parses a declaration BLOCK: handed `display:flex; color:red` it returns both, and "something survived"
       would then read as support for a construct §6's own at-supports-038 requires to be false. A `;` inside a
       function or a block is part of a value and is not this. */
    if (!sup_open(&p, decl, len)) return false;
    for (t = sup_peek(&p); t; sup_take(&p), t = sup_peek(&p)) {
        if (t->type == LXB_CSS_SYNTAX_TOKEN__EOF) break;
        if (t->type == LXB_CSS_SYNTAX_TOKEN_BAD_STRING || t->type == LXB_CSS_SYNTAX_TOKEN_BAD_URL) {
            one = false;
            break;
        }
        if (t->type == LXB_CSS_SYNTAX_TOKEN_L_PARENTHESIS || t->type == LXB_CSS_SYNTAX_TOKEN_FUNCTION ||
            t->type == LXB_CSS_SYNTAX_TOKEN_LS_BRACKET || t->type == LXB_CSS_SYNTAX_TOKEN_LC_BRACKET) {
            depth++;
        } else if (t->type == LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS ||
                   t->type == LXB_CSS_SYNTAX_TOKEN_RS_BRACKET ||
                   t->type == LXB_CSS_SYNTAX_TOKEN_RC_BRACKET) {
            /* An UNMATCHED closer, which `<any-value>` refuses outright. Reaching one through the
               `<supports-decl>` arm is impossible — that span is the inside of a block `sup_block` already
               balanced — but this entry is also §7.5's, whose argument comes straight from the page. */
            if (--depth < 0) { one = false; break; }
        } else if (t->type == LXB_CSS_SYNTAX_TOKEN_SEMICOLON && depth == 0) {
            one = false;
            break;
        }
    }
    if (!t) one = false;
    sup_close(&p);
    if (!one) return false;
    /* §6.1: "A CSS processor is considered to support a declaration ... if it ACCEPTS that declaration (rather
       than discarding it as a parse error) WITHIN A STYLE RULE." So the context is the unrestricted one — a
       style rule's — and not §4.3's page context or CSS Animations §3's keyframe block, whose restrictions are
       about where a declaration is WRITTEN and not about whether this processor implements it. What comes back
       is NULL when nothing parsed, which IS the discarding §6.1 names. */
    serialized = cssom_serialize_declarations(decl, len, CSSOM_BLOCK_UNRESTRICTED);
    if (!serialized) return false;
    free(serialized);
    return true;
}
