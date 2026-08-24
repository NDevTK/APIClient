/* CSS Properties and Values API 1 §5, the half that reads a CSS VALUE. See css_syntax_match.h for why it is a
 * component of its own, who asks, why a syntax component matches exactly one CSS component value, and why an
 * unbuilt type grammar crashes instead of answering "does not match". */
#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/css/syntax/token.h>
#include <lexbor/css/syntax/tokenizer.h>

#include "check.h"
#include "core/css/css_color.h"
#include "core/css/css_defaulting.h"
#include "core/css/css_dimension.h"
#include "core/css/css_length.h"
#include "core/css/css_math.h"
#include "core/css/css_syntax_match.h"

/* ONE VALUE BEING READ. `base`/`len` are the source every span below indexes — lexbor stamps each token with a
   pointer into the very buffer handed to the tokenizer, so a span is subtraction and not a second cursor. */
typedef struct {
    lxb_css_syntax_tokenizer_t *tkz;
    const char                 *base;
    size_t                      len;
} ValStream;

static char *val_copy(const char *s, size_t n)
{
    char *out = malloc(n + 1);

    CHECK(out != NULL, "cssom: OOM copying a piece of a custom property's value");
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

static bool val_open(ValStream *v, const char *value, size_t len)
{
    v->base = value;
    v->len = len;
    v->tkz = lxb_css_syntax_tokenizer_create();
    CHECK(v->tkz != NULL, "cssom: the custom property value tokenizer allocation failed");
    if (lxb_css_syntax_tokenizer_init(v->tkz) != LXB_STATUS_OK) {
        lxb_css_syntax_tokenizer_destroy(v->tkz);
        v->tkz = NULL;
        return false;
    }
    lxb_css_syntax_tokenizer_buffer_set(v->tkz, (const lxb_char_t *)value, len);
    return true;
}

static void val_close(ValStream *v)
{
    if (v->tkz) lxb_css_syntax_tokenizer_destroy(v->tkz);
    v->tkz = NULL;
}

/* The current token, never consumed. NULL is the tokenizer's own allocation failure, which every caller here
   treats as "this value matches nothing" — a value that could not be read did not parse. */
static lxb_css_syntax_token_t *val_peek(ValStream *v) { return lxb_css_syntax_token(v->tkz); }
static void val_take(ValStream *v) { lxb_css_syntax_token_consume(v->tkz); }

static lxb_css_syntax_token_t *val_peek_ws(ValStream *v)
{
    lxb_css_syntax_token_t *t = val_peek(v);

    while (t && t->type == LXB_CSS_SYNTAX_TOKEN_WHITESPACE) {
        val_take(v);
        t = val_peek(v);
    }
    return t;
}

static size_t val_at(const ValStream *v, lxb_css_syntax_token_t *t)
{
    const char *b = (const char *)lxb_css_syntax_token_base(t)->begin;

    DCHECK(b >= v->base && b <= v->base + v->len,
           "a CSS token names a span outside the value it was tokenized from — the tokenizer's buffer IS that "
           "string, so a pointer outside it means the buffer was set from something else");
    if (b < v->base || b > v->base + v->len) return v->len;
    return (size_t)(b - v->base);
}

static size_t val_end_of(const ValStream *v, lxb_css_syntax_token_t *t)
{
    size_t end = val_at(v, t) + lxb_css_syntax_token_base(t)->length;

    DCHECK(end <= v->len,
           "a CSS token's span runs past the end of the value it was tokenized from — lexbor sets a token's "
           "length as the distance the state machine advanced inside its own buffer, so a longer one is a "
           "length taken from a different buffer");
    return end < v->len ? end : v->len;
}

/* ---- one CSS component value ------------------------------------------------------------------------------ */

/* WHAT THE LEAD TOKEN OF A COMPONENT VALUE WAS, copied AT ONCE. Lexbor keeps a token's cooked string in a temp
   buffer only until the NEXT token is requested, so a pointer held across a consume is a dangling one — and
   consuming is exactly what the block walk below does. `text` is the IDENT's unescaped value, the FUNCTION's
   name or the DIMENSION's unit, which are the three the grammars ask about; every other type is decided by
   `type` alone. */
typedef struct {
    lxb_css_syntax_token_type_t type;
    char                       *text;      /* OWNED, NULL when this token kind carries none */
    double                      num;
    bool                        is_float;  /* CSS Syntax §4.3.13's type: FALSE is that algorithm's "integer" */
} ValLead;

static void val_lead_free(ValLead *lead) { free(lead->text); lead->text = NULL; }

/* THE LEAD TOKEN'S OWN TEXT, asserted and never defaulted. `val_lead_of` copies one for exactly three token
   types and every grammar below asks only after it has checked for one of them, so a NULL here is that copy
   having stopped happening — and a `?:` past it would turn "the snapshot no longer carries units" into "this
   value does not match", which is a hole no result could report. */
static const char *val_text(const ValLead *lead)
{
    DCHECK(lead->text != NULL,
           "a CSS token that carries text reached a grammar with none — the lead snapshot copies an IDENT's "
           "unescaped value, a FUNCTION's name and a DIMENSION's unit at once, before the next token overwrites "
           "the tokenizer's temp buffer, so an absent one is that copy having been skipped for a token type "
           "some grammar here asks about");
    return lead->text;
}

static void val_lead_of(ValLead *lead, lxb_css_syntax_token_t *t)
{
    const lxb_css_syntax_token_string_t *s;

    lead->type = t->type;
    lead->text = NULL;
    lead->num = 0.0;
    lead->is_float = false;
    switch (t->type) {
    case LXB_CSS_SYNTAX_TOKEN_IDENT:
        s = lxb_css_syntax_token_ident(t);
        lead->text = val_copy((const char *)s->data, s->length);
        return;
    case LXB_CSS_SYNTAX_TOKEN_FUNCTION:
        s = lxb_css_syntax_token_function(t);
        lead->text = val_copy((const char *)s->data, s->length);
        return;
    case LXB_CSS_SYNTAX_TOKEN_DIMENSION:
        s = lxb_css_syntax_token_dimension_string(t);
        lead->text = val_copy((const char *)s->data, s->length);
        lead->num = lxb_css_syntax_token_dimension(t)->num.num;
        lead->is_float = lxb_css_syntax_token_dimension(t)->num.is_float;
        return;
    case LXB_CSS_SYNTAX_TOKEN_NUMBER:
    case LXB_CSS_SYNTAX_TOKEN_PERCENTAGE:
        lead->num = lxb_css_syntax_token_number(t)->num;
        lead->is_float = lxb_css_syntax_token_number(t)->is_float;
        return;
    default:
        return;
    }
}

/* CSS Syntax 3 §5.5.8 "Consume a component value", whose whole body is a three-armed switch: "<{-token>
   <[-token> <(-token>: Consume a simple block from input and return the result. <function-token>: Consume a
   function from input and return the result. anything else: Consume a token from input and return the result."
   So everything except a block or a function is exactly ONE token, which is the invariant css_syntax_match.h
   rests on. */
static bool val_opens_block(lxb_css_syntax_token_type_t t, lxb_css_syntax_token_type_t *closer)
{
    switch (t) {
    case LXB_CSS_SYNTAX_TOKEN_FUNCTION:
    case LXB_CSS_SYNTAX_TOKEN_L_PARENTHESIS: *closer = LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS; return true;
    case LXB_CSS_SYNTAX_TOKEN_LS_BRACKET:    *closer = LXB_CSS_SYNTAX_TOKEN_RS_BRACKET;    return true;
    case LXB_CSS_SYNTAX_TOKEN_LC_BRACKET:    *closer = LXB_CSS_SYNTAX_TOKEN_RC_BRACKET;    return true;
    default: return false;
    }
}

/* The NESTING a block walk is inside. It grows rather than being sized, because a depth limit here would be a
   cap on a page's own value and §5.5.9's only stopping condition is the matching ending token or EOF. */
typedef struct { lxb_css_syntax_token_type_t *v; size_t n, cap; } ValNest;

static void val_nest_push(ValNest *k, lxb_css_syntax_token_type_t closer)
{
    if (k->n == k->cap) {
        size_t cap = k->cap ? k->cap * 2 : 8;
        lxb_css_syntax_token_type_t *grown = realloc(k->v, cap * sizeof *grown);

        CHECK(grown != NULL, "cssom: OOM tracking the nesting of a custom property value's blocks");
        k->v = grown;
        k->cap = cap;
    }
    k->v[k->n++] = closer;
}

/* Consume ONE component value, answering the span it covered. False at EOF, which is the caller's "there was
   nothing left to match". §5.5.9 "Consume a simple block" processes "<eof-token> / ending token: Discard a
   token from input. Return block." — the two arms are one, so an UNTERMINATED block is still a component
   value and it simply runs to the end of the value, leaving the type grammar to refuse it. §5.5.10 "Consume a
   function" ends the same way, which is why one walk serves both. */
static bool val_component_value(ValStream *v, ValLead *lead, size_t *begin, size_t *end)
{
    lxb_css_syntax_token_t *t = val_peek(v);
    lxb_css_syntax_token_type_t closer;
    ValNest nest = { NULL, 0, 0 };

    if (!t || t->type == LXB_CSS_SYNTAX_TOKEN__EOF) return false;
    DCHECK(t->type != LXB_CSS_SYNTAX_TOKEN_WHITESPACE,
           "a component value was consumed with the cursor on whitespace — the walk skips it before each one, "
           "so a whitespace lead is a caller that entered without skipping and would read an empty span");
    val_lead_of(lead, t);
    *begin = val_at(v, t);
    *end = val_end_of(v, t);
    if (val_opens_block(t->type, &closer)) val_nest_push(&nest, closer);
    val_take(v);
    while (nest.n > 0) {
        t = val_peek(v);
        if (!t || t->type == LXB_CSS_SYNTAX_TOKEN__EOF) { *end = v->len; break; }
        *end = val_end_of(v, t);
        if (t->type == nest.v[nest.n - 1]) nest.n--;
        else if (val_opens_block(t->type, &closer)) val_nest_push(&nest, closer);
        val_take(v);
    }
    free(nest.v);
    DCHECK(*end >= *begin, "a component value's span ends before it starts");
    return true;
}

/* ---- §5.1's types ----------------------------------------------------------------------------------------- */

/* An ASCII case-insensitive compare against a lower-case literal — which is what a FUNCTION name is compared
   with, CSS Syntax making a function name an ident sequence and CSS matching those ASCII case-insensitively. */
static bool val_name_is(const char *s, const char *lower)
{
    size_t i;

    for (i = 0; lower[i] != '\0'; i++) {
        char c = s[i];

        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != lower[i]) return false;
    }
    return s[i] == '\0';
}

/* CSS Values and Units 4 §10's own first paragraph is why every numeric type below has a FUNCTION arm: "A math
   function represents a numeric value, one of: <length>, <frequency>, <angle>, <time>, <flex>, <resolution>,
   <percentage>, <number>, <integer> ...or the <length-percentage>/etc mixed types, and can be used wherever
   such a value would be valid." So a `calc()` IS a `<length>` for §5.1's purposes, and WHICH numeric type any
   given one is is §10.9 "Type Checking"'s question — core/css/css_math.h's, over the whole component value's
   source span rather than over the lead token, because a math function's type is a function of its operands.
   `rgb()` is a function too and is not a math function at all, so it simply does not match, which is what
   keeps a colour from being read as a number.
   THE CONTEXT IS §5.1's OWN SPLIT. `<length>` and `<length-percentage>` are two supported names, so the first
   is a context that does not allow percentages to be mixed and the second is one that resolves them against a
   length — which CSS Typed OM 1 §4.3.2 makes the difference between a null and a hinted percent hint, and is
   why the production is passed down rather than the answers being ORed together afterwards. */
static bool val_math_is(const ValLead *lead, const char *span, size_t span_len, CssMathProduction want)
{
    if (lead->type != LXB_CSS_SYNTAX_TOKEN_FUNCTION) return false;
    return css_math_matches(span, span_len, want);
}

/* §5.1's `<number>`: "<number> values" — CSS Values §5.3's `<number-token>` production. */
static bool val_is_number(const ValLead *lead, const char *span, size_t span_len)
{
    if (lead->type == LXB_CSS_SYNTAX_TOKEN_FUNCTION)
        return val_math_is(lead, span, span_len, CSS_MATH_PROD_NUMBER);
    return lead->type == LXB_CSS_SYNTAX_TOKEN_NUMBER;
}

/* §5.1's `<integer>`: "Any valid <integer> value" — CSS Values §5.2, "when written literally, an integer is
   one or more decimal digits 0 through 9 and corresponds to a SUBSET of the <number-token> production", the
   subset CSS Syntax §4.3.13 "Consume a number" stamps with the "integer" type — that algorithm starts with
   "Let type be the string \"integer\"" and sets it to "number" at both the fraction step and the exponent
   step, so `1.0` and `1e3` are number tokens that are not `<integer>`s. */
static bool val_is_integer(const ValLead *lead, const char *span, size_t span_len)
{
    /* §10.9: "Additionally, math functions that resolve to <number> can be used in any place that only accepts
       <integer>; the value is rounded to the nearest integer as it resolves." So a math function needs no
       integer-valued operands to match this name, and `calc(1.5)` IS an `<integer>` here while the literal
       `1.5` is not — which is the spec's own asymmetry and not a looseness in this arm. */
    if (lead->type == LXB_CSS_SYNTAX_TOKEN_FUNCTION)
        return val_math_is(lead, span, span_len, CSS_MATH_PROD_INTEGER);
    return lead->type == LXB_CSS_SYNTAX_TOKEN_NUMBER && !lead->is_float;
}

/* §5.1's `<percentage>`: "Any valid <percentage> value" — CSS Values §5.5's `<percentage-token>`. */
static bool val_is_percentage(const ValLead *lead, const char *span, size_t span_len)
{
    if (lead->type == LXB_CSS_SYNTAX_TOKEN_FUNCTION)
        return val_math_is(lead, span, span_len, CSS_MATH_PROD_PERCENTAGE);
    return lead->type == LXB_CSS_SYNTAX_TOKEN_PERCENTAGE;
}

/* §5.1's `<length>`: "Any valid <length> value" — CSS Values §6, a dimension in one of its units, plus that
   section's unitless zero: "for zero lengths the unit identifier is optional (i.e. can be syntactically
   represented as the <number> 0)". The unit set is core/css/css_length.h's, which is the same set the
   computed-value path absolutizes from. */
static bool val_is_length(const ValLead *lead, const char *span, size_t span_len)
{
    if (lead->type == LXB_CSS_SYNTAX_TOKEN_FUNCTION)
        return val_math_is(lead, span, span_len, CSS_MATH_PROD_LENGTH);
    if (lead->type == LXB_CSS_SYNTAX_TOKEN_NUMBER) return lead->num == 0.0;
    if (lead->type != LXB_CSS_SYNTAX_TOKEN_DIMENSION) return false;
    return css_length_is_length_unit(val_text(lead), strlen(val_text(lead)));
}

/* §5.1's `<length-percentage>`: "Any valid <length> or <percentage> value, any valid <calc()> expression
   combining <length> and <percentage> components." The third clause is ONE question and not the OR of the
   other two: `calc(1px + 50%)` matches neither `<length>` nor `<percentage>` on its own — CSS Typed OM 1
   §4.3.2 gives it «["length" → 1]» with a percent hint of "length", which `<length>` refuses for exactly the
   reason §4.3.2 states — and it is the value the third clause exists for. */
static bool val_is_length_percentage(const ValLead *lead, const char *span, size_t span_len)
{
    if (lead->type == LXB_CSS_SYNTAX_TOKEN_FUNCTION)
        return val_math_is(lead, span, span_len, CSS_MATH_PROD_LENGTH_PERCENTAGE);
    return val_is_length(lead, span, span_len) || val_is_percentage(lead, span, span_len);
}

/* §5.1's `<angle>`: "Any valid <angle> value" — CSS Values §7.1, a dimension in `deg`, `grad`, `rad` or
   `turn`. A BARE ZERO IS NOT ONE: §7.1's note says the legacy bare-0 spelling "is not true in general, however,
   and will not occur in future uses of the <angle> type", and a syntax component is a new use. */
static bool val_is_angle(const ValLead *lead, const char *span, size_t span_len)
{
    if (lead->type == LXB_CSS_SYNTAX_TOKEN_FUNCTION)
        return val_math_is(lead, span, span_len, CSS_MATH_PROD_ANGLE);
    if (lead->type != LXB_CSS_SYNTAX_TOKEN_DIMENSION) return false;
    return css_angle_unit(val_text(lead), strlen(val_text(lead)));
}

/* §5.1's `<time>`: "Any valid <time> value" — CSS Values §7.2, a dimension in `s` or `ms`. */
static bool val_is_time(const ValLead *lead, const char *span, size_t span_len)
{
    if (lead->type == LXB_CSS_SYNTAX_TOKEN_FUNCTION)
        return val_math_is(lead, span, span_len, CSS_MATH_PROD_TIME);
    if (lead->type != LXB_CSS_SYNTAX_TOKEN_DIMENSION) return false;
    return css_time_unit(val_text(lead), strlen(val_text(lead)));
}

/* §5.1's `<resolution>`: "Any valid <resolution> value" — CSS Values §7.4, a dimension in `dpi`, `dpcm`,
   `dppx` or `x`. NEGATIVE IS NOT VALID and that is a grammar refusal rather than a clamp: §7.4 states "the
   allowed range of <resolution> values always excludes negative values, in addition to any explicit ranges
   that might be specified", and §5.1 "Range Restrictions and Range Definition Notation" says what a value
   outside the allowed range is — "If the value is outside the allowed range, then unless otherwise specified,
   the declaration is invalid and must be ignored." */
static bool val_is_resolution(const ValLead *lead, const char *span, size_t span_len)
{
    if (lead->type == LXB_CSS_SYNTAX_TOKEN_FUNCTION)
        /* The range restriction is NOT re-asked of a math function: css-values-4 §10.12 "Range Checking" says
           the clamping "is, for math functions, only performed on the results of a TOP-LEVEL calculation", and
           a top-level result outside the range is clamped rather than making the value invalid — which is why
           §10.9's type question is the whole of the grammar question here. */
        return val_math_is(lead, span, span_len, CSS_MATH_PROD_RESOLUTION);
    if (lead->type != LXB_CSS_SYNTAX_TOKEN_DIMENSION) return false;
    if (!css_resolution_unit(val_text(lead), strlen(val_text(lead)))) return false;
    return lead->num >= 0.0;
}

/* §5.1's `<string>`: "Any valid <string> value" — CSS Values §4.4's `<string-token>`. A BAD-STRING token is the
   tokenizer's own report that the construct never closed, so it is not one. */
static bool val_is_string(const ValLead *lead)
{
    return lead->type == LXB_CSS_SYNTAX_TOKEN_STRING;
}

/* §5.1's `<custom-ident>`: "Any valid <custom-ident> value" — CSS Values §4.2, any CSS identifier except the
   CSS-wide keywords and the reserved `default`, "excluded in all ASCII case permutations". The exclusion set is
   core/css/css_defaulting.h's, which is the same one §5.4.3 refuses a syntax component's own name against. */
static bool val_is_custom_ident(const ValLead *lead)
{
    if (lead->type != LXB_CSS_SYNTAX_TOKEN_IDENT) return false;
    return !css_custom_ident_excluded(val_text(lead));
}

/* CSS Values and Units 4 §4.5 "Resource Locators: the <url> type" —
     <url> = <url()> | <src()>
     <url()> = url( <string> <url-modifier>* ) | <url-token>
     <src()> = src( <string> <url-modifier>* )
   and §4.5.3 "URL Modifiers": "A <url-modifier> is either an <ident> or a functional notation."
   THE UNQUOTED SPELLING IS THE TOKENIZER'S ANSWER, not a second grammar: §4.5 says `url(` without quotes "is
   specially-parsed as a <url-token>", which is CSS Syntax §4.3.6's own state, so a URL token IS this
   production's first arm and a BAD-URL token is that state's own report that it is not. That is also why the
   unquoted spelling accepts no modifier here — there is no interior left to read.
   The interior is re-tokenized from the span rather than read during the walk above, because a component value
   is consumed as a unit and only this one type looks inside it. */
static bool val_is_url(const ValLead *lead, const char *span, size_t span_len)
{
    ValStream in;
    lxb_css_syntax_token_t *t;
    bool ok = false;

    if (lead->type == LXB_CSS_SYNTAX_TOKEN_URL) return true;
    if (lead->type != LXB_CSS_SYNTAX_TOKEN_FUNCTION) return false;
    if (!val_name_is(val_text(lead), "url") && !val_name_is(val_text(lead), "src")) return false;
    if (!val_open(&in, span, span_len)) return false;
    t = val_peek(&in);                                        /* the `url(` / `src(` function token itself */
    if (!t || t->type != LXB_CSS_SYNTAX_TOKEN_FUNCTION) goto done;
    val_take(&in);
    t = val_peek_ws(&in);
    if (!t || t->type != LXB_CSS_SYNTAX_TOKEN_STRING) goto done;   /* `<string>`, required by both arms */
    val_take(&in);
    for (;;) {
        t = val_peek_ws(&in);
        if (!t) goto done;
        if (t->type == LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS) { val_take(&in); break; }
        /* §4.5.3's `<url-modifier>`: an `<ident>`, or a functional notation — which is consumed whole, so a
           modifier's own parentheses do not close the url(). */
        if (t->type == LXB_CSS_SYNTAX_TOKEN_IDENT) { val_take(&in); continue; }
        if (t->type == LXB_CSS_SYNTAX_TOKEN_FUNCTION) {
            ValLead sub = { LXB_CSS_SYNTAX_TOKEN_UNDEF, NULL, 0.0, false };
            size_t b, e;

            if (!val_component_value(&in, &sub, &b, &e)) { val_lead_free(&sub); goto done; }
            val_lead_free(&sub);
            continue;
        }
        goto done;
    }
    t = val_peek_ws(&in);
    ok = t != NULL && t->type == LXB_CSS_SYNTAX_TOKEN__EOF;

done:
    val_close(&in);
    return ok;
}

/* §5.1's `<color>`: "Any valid <color> value" — CSS Color 4's production, answered by the component that owns
   it, over the span the value occupies. It takes TEXT and not a token because a colour is `#fff`, an ident, or
   a functional notation whose interior is its own grammar. */
static bool val_is_color(const char *span, size_t span_len)
{
    CssColor c;

    return css_color_parse(span, span_len, &c);
}

/* Does ONE component value match ONE §5.1 supported name. `type` carries its angle brackets for a data type
   name and is the bare ident for §5.1's ident row, which is exactly how §5.4.3 built it. */
static bool val_type_matches(const char *type, const ValLead *lead, const char *span, size_t span_len)
{
    DCHECK(type != NULL && type[0] != '\0', "a syntax component with no name reached the value match");
    if (type[0] != '<') {
        /* §5.1's ident row: "Any sequence which starts an identifier, can be consumed as a name, and matches
           the <custom-ident> production" accepts "That identifier". §5.1's own note makes the comparison
           codepoint-wise — "specifying an ident like Red means that the precise value Red is accepted; red,
           RED, and any other casing variants are not matched by this" — so this is `strcmp` and never a folded
           compare. Both sides are UNESCAPED (lexbor cooks the value's ident, §5.4.3 unescaped the name), which
           is what makes at-property-cssom.html's `--escape-syntax` match its own `I\ dent`. */
        DCHECK(!css_custom_ident_excluded(type),
               "a syntax component's ident name is a keyword CSS Values §4.2 excludes from `<custom-ident>` — "
               "§5.4.3 refuses exactly those when it builds the component, so one reaching here is a "
               "definition that was not built by that algorithm");
        return lead->type == LXB_CSS_SYNTAX_TOKEN_IDENT && strcmp(val_text(lead), type) == 0;
    }
    if (strcmp(type, "<length>") == 0)             return val_is_length(lead, span, span_len);
    if (strcmp(type, "<number>") == 0)             return val_is_number(lead, span, span_len);
    if (strcmp(type, "<percentage>") == 0)         return val_is_percentage(lead, span, span_len);
    if (strcmp(type, "<length-percentage>") == 0)  return val_is_length_percentage(lead, span, span_len);
    if (strcmp(type, "<string>") == 0)             return val_is_string(lead);
    if (strcmp(type, "<color>") == 0)              return val_is_color(span, span_len);
    if (strcmp(type, "<url>") == 0)                return val_is_url(lead, span, span_len);
    if (strcmp(type, "<integer>") == 0)            return val_is_integer(lead, span, span_len);
    if (strcmp(type, "<angle>") == 0)              return val_is_angle(lead, span, span_len);
    if (strcmp(type, "<time>") == 0)               return val_is_time(lead, span, span_len);
    if (strcmp(type, "<resolution>") == 0)         return val_is_resolution(lead, span, span_len);
    if (strcmp(type, "<custom-ident>") == 0)       return val_is_custom_ident(lead);
    if (strcmp(type, "<image>") == 0)
        DFAIL("a registered custom property's syntax is `<image>` and this engine has no `<image>` grammar. "
              "CSS Images 4 §2 '2D Image Values: the <image> type' defines it as `<url> | <image()> | "
              "<image-set()> | <cross-fade()> | <element()> | <gradient>`, and its `<gradient>` half is §3 "
              "'Gradients' — §3.1's linear, §3.2's radial, §3.3's conic and §3.4's three repeating forms, over "
              "§3.5's colour stop lists, `<angle>`, `<length-percentage>` and `<position>`. BUILD it as "
              "core/css/css_image.c — a component in its own right, because `background-image` and the "
              "computed-value path need the same production — and route this name to it");
    DCHECK(strcmp(type, "<transform-list>") != 0,
           "§5.1's PRE-MULTIPLIED `<transform-list>` reached the per-type match. It is unrolled to "
           "`<transform-function>` with §5.2's space multiplier one level up, where the multiplier belongs, so "
           "one arriving here is that unrolling having been skipped and a list being asked to match as a single "
           "value");
    if (strcmp(type, "<transform-function>") == 0)
        DFAIL("a registered custom property's syntax names a transform and this engine has no "
              "`<transform-function>` grammar. CSS Transforms 1 §7.1 '2D Transform Functions' defines "
              "matrix(<number>#{6}), translate/translateX/translateY over `<length-percentage>`, "
              "scale/scaleX/scaleY over `<number>`, and rotate/skew/skewX/skewY over `[<angle>|<zero>]`; CSS "
              "Transforms 2 §12.2 '3D Transform Functions' adds matrix3d, translate3d/translateZ, "
              "scale3d/scaleZ, rotate3d/rotateX/rotateY/rotateZ and perspective. Every argument type in both "
              "lists is one this file already decides, and `<zero>` is CSS Values §5.3's literal number 0. "
              "BUILD it as core/css/css_transform.c and route both names to it; `<transform-list>` is §5.1's "
              "pre-multiplied name for `<transform-function>+` and is already unrolled to that here");
    DFAIL("a syntax component names a type that is not one of CSS Properties and Values API 1 §5.1's fifteen "
          "supported names. §5.4.4 only ever produces a name it has already checked against that list, so a "
          "name reaching here is that list having grown in the parser and not in the matcher");
    return false;
}

/* ---- §5.2's multipliers over §5.3's ordered components ---------------------------------------------------- */

/* Does the whole value match ONE syntax component, multiplier included. */
static bool val_component_matches(const CssSyntaxComponent *c, const char *value, size_t len)
{
    const char *type = c->name;
    CssSyntaxMultiplier mult = c->multiplier;
    ValStream v;
    unsigned matched = 0;
    bool ok = false;

    /* §5.1: "<transform-list> is a pre-multiplied data type name equivalent to <transform-function>+". §5.4.3
       returns a pre-multiplied component BEFORE it looks for a multiplier, so the multiplication is part of
       what the NAME means and is unrolled here — which is also why §5.2 forbids writing one beside it. */
    if (strcmp(type, "<transform-list>") == 0) {
        DCHECK(mult == CSS_SYNTAX_MULT_NONE,
               "a pre-multiplied data type name carries a multiplier of its own — §5.4.3 returns the component "
               "before the multiplier is consumed and §5.2 excludes exactly these names from being multiplied, "
               "so `<transform-list>+` is not a syntax string and cannot have become a component");
        type = "<transform-function>";
        mult = CSS_SYNTAX_MULT_SPACE;
    }
    if (!val_open(&v, value, len)) return false;
    for (;;) {
        ValLead lead = { LXB_CSS_SYNTAX_TOKEN_UNDEF, NULL, 0.0, false };
        lxb_css_syntax_token_t *t = val_peek_ws(&v);
        size_t begin = 0, end = 0;
        bool one;

        if (!t) goto done;
        if (t->type == LXB_CSS_SYNTAX_TOKEN__EOF) break;
        if (matched > 0) {
            /* An unmultiplied component is ONE component value and nothing after it. */
            if (mult == CSS_SYNTAX_MULT_NONE) goto done;
            if (mult == CSS_SYNTAX_MULT_COMMA) {
                /* §5.2's `#`: "Indicates a comma-separated list." A separator with nothing after it is not a
                   shorter list, it is no list — `<color>#` does not accept `red,`. */
                if (t->type != LXB_CSS_SYNTAX_TOKEN_COMMA) goto done;
                val_take(&v);
                t = val_peek_ws(&v);
                if (!t || t->type == LXB_CSS_SYNTAX_TOKEN__EOF) goto done;
            }
            /* §5.2's `+`: "Indicates a space-separated list." The whitespace is already consumed, so the next
               component value simply begins here; a COMMA in that position is a component value of its own and
               is refused by the type below, which is what keeps `<length>+` from accepting `1px, 2px`. */
        }
        if (!val_component_value(&v, &lead, &begin, &end)) { val_lead_free(&lead); goto done; }
        one = val_type_matches(type, &lead, value + begin, end - begin);
        val_lead_free(&lead);
        if (!one) goto done;
        matched++;
    }
    /* Neither multiplier has a zero-length arm and an unmultiplied component is exactly one, so an empty value
       matches nothing — which is the answer §3.3 wants for `initial-value:` with nothing behind it. */
    ok = matched > 0;

done:
    val_close(&v);
    return ok;
}

bool css_property_syntax_matches(const CssSyntaxDefinition *d, const char *value, size_t len)
{
    size_t i;

    DCHECK(d != NULL && value != NULL, "a value was matched against no syntax definition, or a definition "
                                       "against no value");
    DCHECK(!d->universal,
           "§5.4.1's UNIVERSAL syntax definition reached the component matcher. §4.1 parses a value against it "
           "as `<declaration-value>?` and against every other definition `according to syntax definition` — "
           "two different productions, and the universal one has no components to ask, so answering it here "
           "could only ever be an accident of an empty list");
    DCHECK(d->n > 0,
           "a non-universal syntax definition holds no syntax components — §5.4.2 runs step 5 at least once "
           "before it can return one, so an empty list is a definition that was not built by that algorithm");
    /* §5.3: "When a syntax definition with multiple syntax components is used to parse a CSS value, the syntax
       components are matched in the order specified." — the note spells out why the order is the answer and not
       a detail: "given the syntax string `red | <color>`, matching the value red against it will parse as an
       identifier, while matching the value blue will parse as a <color>". */
    for (i = 0; i < d->n; i++)
        if (val_component_matches(&d->v[i], value, len)) return true;
    return false;
}
