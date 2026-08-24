/* HTML §4.8.4.3 "Processing model"'s SOURCE SET — see image_source_set.h for why this is a component of its
 * own, where the source size's units are answered, what an undecided set is, and what crashes. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/html/html.h>
#include <lexbor/css/syntax/token.h>
#include <lexbor/css/syntax/tokenizer.h>

#include "check.h"
#include "quickjs.h"
#include "solver/dom_cow.h"          /* the attribute a flow composed out of unknown input, beside its value */
#include "core/css/css_length.h"     /* §6's `<length>` UNIT set, to tell an unbuilt unit from an author error */
#include "core/css/css_math.h"       /* §4.8.4.2.2 admits the MATH FUNCTIONS as a `<source-size-value>` */
#include "core/css/media_query.h"    /* §4.8.4.3: "Other units must be interpreted the same as in Media Queries" */
#include "core/dom/element_view.h"   /* HTML's "being rendered", which is "has any associated CSS layout boxes" */
#include "core/frame/viewport.h"
#include "core/html/integer_microsyntax.h"
#include "core/html/number_microsyntax.h"
#include "core/html/image_source_set.h"
#include "core/layout/used_value.h"
#include "core/mime/mime_type.h"

/* ---- the pieces every step below is written over ----------------------------------------------------------- */

/* Infra §4.6 "Code points": "ASCII whitespace is U+0009 TAB, U+000A LF, U+000C FF, U+000D CR, or U+0020 SPACE."
   Spelled out rather than run through `strchr`, which reports the terminating NUL as a member of its set. */
static bool iss_is_ws(char c)
{
    return c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' ';
}

static char *iss_copy(const char *s, size_t n)
{
    char *out = malloc(n + 1);

    CHECK(out != NULL, "§4.8.4.3: OOM copying a piece of a source set's markup");
    if (n) memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

/* An attribute's value and its length, or NULL — Lexbor answers NULL for an attribute whose VALUE is absent, so
   `srcset=""` and no `srcset` at all are indistinguishable through this. That merge is exactly right here and
   nowhere else in this file: every string §4.8.4.3.9 hands to §4.8.4.3.8 defaults to THE EMPTY STRING when the
   attribute is absent, and §4.8.4.3.8's own tests are "is not an empty string" — so both answers are the same
   answer. The questions that ask about PRESENCE (`does child have a srcset attribute`, `does el use srcset or
   picture`) ask `lxb_dom_element_has_attribute` instead, which is the mistake core/html/html_script.c records
   making when it ran `<script src="">`'s child text as a program. */
static const char *iss_attr(lxb_dom_element_t *el, const char *name, size_t *out_n)
{
    size_t n = 0;
    const lxb_char_t *v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)name, strlen(name), &n);

    *out_n = v ? n : 0;
    return (const char *)v;
}

static bool iss_has_attr(lxb_dom_element_t *el, const char *name)
{
    return lxb_dom_element_has_attribute(el, (const lxb_char_t *)name, strlen(name));
}

/* An ASCII case-insensitive comparison against a lowercase literal — every keyword this file tests (`auto`,
   `lazy`) is one HTML or CSS defines that way. */
static bool iss_ascii_ci_eq(const char *s, size_t n, const char *lower)
{
    size_t i, m = strlen(lower);

    if (n != m) return false;
    for (i = 0; i < n; i++) {
        char c = s[i];

        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != lower[i]) return false;
    }
    return true;
}

static void iss_push(ImageSourceSet *s, char *url, bool has_w, double w, bool has_d, double d)
{
    ImageSource *grown = realloc(s->items, (size_t)(s->n + 1) * sizeof *grown);

    CHECK(grown != NULL, "§4.8.4.3: OOM extending a source set");
    s->items = grown;
    s->items[s->n].url = url;
    s->items[s->n].has_width = has_w;
    s->items[s->n].width = w;
    s->items[s->n].has_density = has_d;
    s->items[s->n].density = d;
    s->n++;
}

static void iss_clear_items(ImageSourceSet *s)
{
    int i;

    for (i = 0; i < s->n; i++) free(s->items[i].url);
    free(s->items);
    s->items = NULL;
    s->n = 0;
}

void image_source_set_release(JSContext *ctx, ImageSourceSet *s)
{
    DCHECK(s != NULL, "a source set was released with nothing to release");
    iss_clear_items(s);
    JS_FreeValue(ctx, s->undecided_url);
    s->undecided_url = JS_UNDEFINED;
    s->selected = -1;
}

/* THE SET BECOMES UNDECIDED, and the URL-bearing taint travels out with it. `taint` is BORROWED from the
   attribute's shadow, so it is DUPPED here: the set outlives the read by exactly one caller and a borrowed
   value that outlives its shadow is a reference nobody counted. The FIRST such attribute wins — a second one
   would be a second shape for the same undecidable answer, and the surface records shapes, not attempts. */
static void iss_undecided(JSContext *ctx, ImageSourceSet *s, JSValueConst taint)
{
    s->undecided = true;
    s->selected = -1;
    if (JS_IsUndefined(taint) || !JS_IsUndefined(s->undecided_url)) return;
    s->undecided_url = JS_DupValue(ctx, taint);
}

/* ---- §4.8.4.3.10 "Parsing a srcset attribute" --------------------------------------------------------------- */

/* THE DESCRIPTOR LIST ONE IMAGE CANDIDATE STRING PRODUCES. §4.8.4.3.10's own note is why it is a LIST and not a
   pair: "In order to be compatible with future additions, this algorithm supports multiple descriptors and
   descriptors with parens." */
typedef struct { char **v; int n, cap; } DescList;

static void desc_add(DescList *d, const char *s, size_t n)
{
    if (d->n == d->cap) {
        int cap = d->cap ? d->cap * 2 : 4;
        char **grown = realloc(d->v, (size_t)cap * sizeof *grown);

        CHECK(grown != NULL, "§4.8.4.3.10: OOM collecting an image candidate string's descriptors");
        d->v = grown;
        d->cap = cap;
    }
    d->v[d->n++] = iss_copy(s, n);
}

static void desc_free(DescList *d)
{
    int i;

    for (i = 0; i < d->n; i++) free(d->v[i]);
    free(d->v);
    d->v = NULL;
    d->n = d->cap = 0;
}

/* A GROWABLE "current descriptor", which the tokenizer below appends single characters to. */
typedef struct { char *s; size_t len, cap; } DescBuf;

static void descbuf_put(DescBuf *b, char c)
{
    if (b->len + 1 >= b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 16;
        char *grown = realloc(b->s, cap);

        CHECK(grown != NULL, "§4.8.4.3.10: OOM building a descriptor");
        b->s = grown;
        b->cap = cap;
    }
    b->s[b->len++] = c;
    b->s[b->len] = '\0';
}

static void descbuf_reset(DescBuf *b) { b->len = 0; if (b->s) b->s[0] = '\0'; }

/* HTML §2.3.4.2 "Non-negative integers": "A string is a valid non-negative integer if it consists of ONE OR
   MORE ASCII DIGITS." That is the PRODUCTION, which is what §4.8.4.3.10's descriptor dispatch tests — a
   different question from §2.3.4.2's RULES FOR PARSING, which skip whitespace, take a sign and stop wherever
   they stop. Both are asked of a `w` descriptor, in that order, and only the production rejects ` 12w`. */
static bool iss_is_valid_non_negative_integer(const char *s, size_t n)
{
    size_t i;

    if (n == 0) return false;
    for (i = 0; i < n; i++)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

/* THE NUMBER A DIGIT RUN DENOTES, WITH NO UPPER BOUND — §2.3.4.2's "the number that is represented in base ten
   by that string of digits", which names no maximum. core/html/integer_microsyntax.h hands the digits back for
   exactly this reason ("every bound in the platform belongs to the CONSUMER"), and this consumer's range is a
   `double`: a width descriptor is divided by the source size in §4.8.4.3.12 and never indexes anything. A run
   long enough to leave the double range becomes an infinity, which is the same thing §4.8.4.3.10's own note
   contemplates for a zero density ("the natural dimensions will be infinite"). */
static double iss_digits_value(const HtmlInteger *n)
{
    double v = 0.0;
    size_t i;

    DCHECK(n->digits != NULL && n->digits_len > 0,
           "§2.3.4.2's rules returned success with no digit run — core/html/integer_microsyntax.h documents at "
           "least one digit on success, so an empty run is that component disagreeing with its own contract");
    for (i = 0; i < n->digits_len; i++) v = v * 10.0 + (double)(n->digits[i] - '0');
    return v;
}

/* §4.8.4.3.10's DESCRIPTOR PARSER, over the descriptors one image candidate string produced. Returns whether
   the candidate is appended ("If error is still no, then append a new image source to candidates"). */
static bool iss_descriptor_parser(const DescList *d, bool *has_w, double *w, bool *has_d, double *dens)
{
    bool error = false;      /* "Let error be no." */
    bool have_h = false;     /* "Let future-compat-h be absent." */
    int i;

    *has_w = false;          /* "Let width be absent." */
    *has_d = false;          /* "Let density be absent." */
    *w = 0.0;
    *dens = 0.0;
    for (i = 0; i < d->n; i++) {
        const char *s = d->v[i];
        size_t n = strlen(s);
        char last = n ? s[n - 1] : '\0';
        bool fp_valid = false;
        double fp = 0.0;
        HtmlInteger iv;

        /* "If the descriptor consists of a VALID NON-NEGATIVE INTEGER followed by a U+0077 LATIN SMALL LETTER W
           character" — and the step this user agent does NOT take is the first one: "If the user agent does not
           support the sizes attribute, let error be yes." §4.8.4.3.11 is built, so it does. */
        if (n >= 2 && last == 'w' && iss_is_valid_non_negative_integer(s, n - 1)) {
            if (*has_w || *has_d) error = true;
            if (html_parse_non_negative_integer(s, n, &iv)) {
                double v = iss_digits_value(&iv);

                /* "If the result is 0, let error be yes. Otherwise, let width be the result." */
                if (v == 0.0) error = true;
                else { *has_w = true; *w = v; }
            } else {
                /* The release arm is written FIRST so the crash is not the only thing standing between this
                   state and a candidate appended with no width — a DCHECK is compiled out and an `if` body that
                   held only one would silently accept. */
                error = true;
                DFAIL("§2.3.4.2's RULES FOR PARSING NON-NEGATIVE INTEGERS returned an error for a run this "
                      "file has already decided matches §2.3.4.2's PRODUCTION — one or more ASCII digits, "
                      "which the rules collect after skipping whitespace and an optional sign that a valid "
                      "production cannot contain. The two answers cannot disagree, so this is the microsyntax "
                      "component and this dispatch describing two different grammars");
            }
            continue;
        }
        /* "If the descriptor consists of a VALID FLOATING-POINT NUMBER followed by a U+0078 LATIN SMALL LETTER
           X character" — §2.3.4.3's PRODUCTION, which core/html/number_microsyntax.h answers through `pvalid`
           and which is a different question from whether the rules return a number. */
        if (n >= 2 && last == 'x') {
            char *body = iss_copy(s, n - 1);

            html_parse_floating_point(body, n - 1, &fp, &fp_valid);
            free(body);
            if (fp_valid) {
                bool ok = html_parse_floating_point(s, n, &fp, NULL);

                if (*has_w || *has_d || have_h) error = true;
                /* "Apply the rules for parsing floating-point number values to the descriptor. If the result is
                   LESS THAN 0, let error be yes. Otherwise, let density be the result."
                   THE RULES CAN STILL RETURN AN ERROR HERE AND §4.8.4.3.10's "otherwise" HAS NO NUMBER TO TAKE.
                   §2.3.4.3's conversion step is "if rounded-value is 2^1024 or −2^1024, return an error", so
                   `1e999x` IS a valid floating-point number whose rules error — the ONLY way this arm is
                   reached without a number, which is asserted rather than assumed. A candidate whose descriptor
                   denotes no representable density is dropped: that is the same outcome as the negative arm,
                   and inventing an infinity for it would put a fabricated density into the choice. */
                if (!ok) error = true;
                else if (fp < 0.0) error = true;
                else { *has_d = true; *dens = fp; }
                continue;
            }
        }
        /* "If the descriptor consists of a valid non-negative integer followed by a U+0068 LATIN SMALL LETTER H
           character: This is a parse error." — kept because the step after the loop reads it. */
        if (n >= 2 && last == 'h' && iss_is_valid_non_negative_integer(s, n - 1)) {
            if (have_h || *has_d) error = true;
            if (!html_parse_non_negative_integer(s, n, &iv)) {
                error = true;
                DFAIL("§2.3.4.2's rules disagreed with its production for an `h` descriptor — see the identical "
                      "assert on the `w` arm above; one grammar cannot answer two ways");
            } else if (iss_digits_value(&iv) == 0.0) {
                error = true;
            } else {
                have_h = true;
            }
            continue;
        }
        error = true;                                            /* "Anything else: Let error be yes." */
    }
    /* "If future-compat-h is not absent and width is absent, let error be yes." */
    if (have_h && !*has_w) error = true;
    return !error;
}

/* §4.8.4.3.10 "Parsing a srcset attribute", transcribed. `input`/`len` is the attribute's value; the sources
   are APPENDED to `out`, which is what makes this usable both for the element's own srcset and for a
   `<source>`'s (§4.8.4.3.9 gives each its own fresh set). */
static void iss_parse_srcset(const char *input, size_t len, ImageSourceSet *out)
{
    size_t pos = 0;

    DCHECK(input != NULL || len == 0, "§4.8.4.3.10 was asked to parse a srcset attribute with no bytes and a "
                                      "non-zero length");
    for (;;) {
        size_t url_begin, url_end;
        DescList desc = { NULL, 0, 0 };
        DescBuf cur = { NULL, 0, 0 };
        bool has_w = false, has_d = false;
        double w = 0.0, d = 0.0;

        /* "SPLITTING LOOP: Collect a sequence of code points that are ASCII whitespace or U+002C COMMA
           characters from input given position. If any U+002C COMMA characters were collected, that is a parse
           error." A parse error is §4.8.4.3's "non-fatal mismatch between input and requirements" and changes
           nothing about the result, so nothing is recorded for it. */
        while (pos < len && (iss_is_ws(input[pos]) || input[pos] == ',')) pos++;
        /* "If position is past the end of input, return candidates." */
        if (pos >= len) return;

        /* "Collect a sequence of code points that are NOT ASCII whitespace from input given position, and let
           url be the result." */
        url_begin = pos;
        while (pos < len && !iss_is_ws(input[pos])) pos++;
        url_end = pos;
        DCHECK(url_end > url_begin,
               "§4.8.4.3.10 collected an EMPTY url — the splitting loop above stops on a code point that is "
               "neither ASCII whitespace nor a comma, so the very next collect takes at least that one");

        /* "If url ends with U+002C (,): Remove all trailing U+002C COMMA characters from url." — and the
           descriptor list stays empty, which is what makes `a.png,b.png` two candidates with no descriptors. */
        if (input[url_end - 1] == ',') {
            while (url_end > url_begin && input[url_end - 1] == ',') url_end--;
            DCHECK(url_end > url_begin,
                   "§4.8.4.3.10 removed every character of a url as a trailing comma — the splitting loop "
                   "consumed every leading comma, so the first code point of a url is never one");
        } else {
            /* "Otherwise: DESCRIPTOR TOKENIZER: Skip ASCII whitespace within input given position. Let current
               descriptor be the empty string. Let state be in descriptor." */
            enum { IN_DESCRIPTOR, IN_PARENS, AFTER_DESCRIPTOR } state = IN_DESCRIPTOR;

            while (pos < len && iss_is_ws(input[pos])) pos++;
            for (;;) {
                /* "Let c be the character at position. … For the purpose of this step, 'EOF' is a special
                   character representing that position is past the end of input." */
                bool eof = pos >= len;
                char c = eof ? '\0' : input[pos];

                if (state == IN_DESCRIPTOR) {
                    if (eof) {
                        if (cur.len) desc_add(&desc, cur.s, cur.len);
                        break;                                   /* jump to the descriptor parser */
                    }
                    if (iss_is_ws(c)) {
                        if (cur.len) { desc_add(&desc, cur.s, cur.len); descbuf_reset(&cur); }
                        state = AFTER_DESCRIPTOR;
                    } else if (c == ',') {
                        /* "Advance position to the next character in input. If current descriptor is not empty,
                           append current descriptor to descriptors. Jump to the step labeled descriptor
                           parser." — the advance is the ONE the comma arm performs for itself. */
                        pos++;
                        if (cur.len) desc_add(&desc, cur.s, cur.len);
                        break;
                    } else if (c == '(') {
                        descbuf_put(&cur, c);
                        state = IN_PARENS;
                    } else {
                        descbuf_put(&cur, c);
                    }
                } else if (state == IN_PARENS) {
                    if (eof) {
                        /* "EOF: Append current descriptor to descriptors." — UNCONDITIONALLY, unlike the in
                           descriptor arm above, which appends only a non-empty one. An unterminated `(` is
                           therefore a descriptor the parser below rejects rather than one that vanishes. */
                        desc_add(&desc, cur.s ? cur.s : "", cur.len);
                        break;
                    }
                    descbuf_put(&cur, c);
                    if (c == ')') state = IN_DESCRIPTOR;
                } else {
                    if (eof) break;
                    if (!iss_is_ws(c)) {
                        /* "Anything else: Set state to in descriptor. Set position to the PREVIOUS character in
                           input." — which the advance below then undoes, so this character is re-read in the
                           in descriptor state. */
                        state = IN_DESCRIPTOR;
                        pos--;
                    }
                }
                pos++;   /* "Advance position to the next character in input. Repeat this step." */
            }
        }

        /* "DESCRIPTOR PARSER" … "If error is still no, then append a new image source to candidates whose URL
           is url, associated with a width width if not absent and a pixel density density if not absent." */
        if (iss_descriptor_parser(&desc, &has_w, &w, &has_d, &d))
            iss_push(out, iss_copy(input + url_begin, url_end - url_begin), has_w, w, has_d, d);
        desc_free(&desc);
        free(cur.s);
        /* "Return to the step labeled splitting loop." */
    }
}

/* ---- §4.8.4.3.11 "Parsing a sizes attribute" ---------------------------------------------------------------- */

/* CSS SYNTAX'S COMPONENT VALUES, as SOURCE SPANS. §4.8.4.3.11's first step is "the result of parsing a
   COMMA-SEPARATED LIST OF COMPONENT VALUES from the value of element's sizes attribute", and its step 3.2 asks
   for "the LAST COMPONENT VALUE in unparsed size" — so the unit this algorithm is written over is the component
   value, and a byte scan cannot produce one: a comma inside `calc(min(1px, 2px))` does not separate, and a
   comma inside a `<string>` does not either.
   IT TOKENIZES WITH LEXBOR'S OWN TOKENIZER for core/css/css_at_rule_prelude.h's reason — a tokenizer is
   standalone, it decides strings and escapes, and this engine already has one. What is kept is only the SPAN,
   because every consumer below re-reads the source: a `<media-condition>` is handed to the media query grammar
   as text, and a `<source-size-value>` is one token this file re-tokenizes on its own. */
typedef struct {
    size_t                      begin, end;
    lxb_css_syntax_token_type_t lead;   /* the type of the component value's FIRST token */
} IssCv;

typedef struct { IssCv *v; int n, cap; } IssCvList;

static void cv_add(IssCvList *l, size_t begin, size_t end, lxb_css_syntax_token_type_t lead)
{
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 8;
        IssCv *grown = realloc(l->v, (size_t)cap * sizeof *grown);

        CHECK(grown != NULL, "§4.8.4.3.11: OOM collecting a sizes attribute's component values");
        l->v = grown;
        l->cap = cap;
    }
    l->v[l->n].begin = begin;
    l->v[l->n].end = end;
    l->v[l->n].lead = lead;
    l->n++;
}

static bool cv_opens_block(lxb_css_syntax_token_type_t t)
{
    return t == LXB_CSS_SYNTAX_TOKEN_FUNCTION || t == LXB_CSS_SYNTAX_TOKEN_L_PARENTHESIS ||
           t == LXB_CSS_SYNTAX_TOKEN_LS_BRACKET || t == LXB_CSS_SYNTAX_TOKEN_LC_BRACKET;
}

static lxb_css_syntax_token_type_t cv_mirror(lxb_css_syntax_token_type_t t)
{
    if (t == LXB_CSS_SYNTAX_TOKEN_LS_BRACKET) return LXB_CSS_SYNTAX_TOKEN_RS_BRACKET;
    if (t == LXB_CSS_SYNTAX_TOKEN_LC_BRACKET) return LXB_CSS_SYNTAX_TOKEN_RC_BRACKET;
    DCHECK(t == LXB_CSS_SYNTAX_TOKEN_FUNCTION || t == LXB_CSS_SYNTAX_TOKEN_L_PARENTHESIS,
           "CSS Syntax's consume a simple block was asked for the mirror of a token that opens none");
    return LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS;
}

/* THE TOP-LEVEL COMPONENT VALUES of `text`, in order. `false` is the tokenizer's own failure, which every
   caller reads as "this attribute produced no source size" — an attribute that could not be tokenized is not a
   valid source size list, which is §4.8.4.3.11's last step (`Return 100vw`) and not an error to report. */
static bool iss_component_values(const char *text, size_t len, IssCvList *out)
{
    lxb_css_syntax_tokenizer_t *tkz = lxb_css_syntax_tokenizer_create();
    lxb_css_syntax_token_type_t *stack = NULL;
    int depth = 0, cap = 0;
    size_t begin = 0;
    lxb_css_syntax_token_type_t lead = LXB_CSS_SYNTAX_TOKEN_UNDEF;
    bool ok = true;

    CHECK(tkz != NULL, "§4.8.4.3.11: the sizes attribute tokenizer allocation failed");
    if (lxb_css_syntax_tokenizer_init(tkz) != LXB_STATUS_OK) {
        lxb_css_syntax_tokenizer_destroy(tkz);
        return false;
    }
    lxb_css_syntax_tokenizer_buffer_set(tkz, (const lxb_char_t *)text, len);
    for (;;) {
        lxb_css_syntax_token_t *t = lxb_css_syntax_token(tkz);
        const lxb_char_t *b;
        size_t at, end;

        if (!t) { ok = false; break; }
        if (t->type == LXB_CSS_SYNTAX_TOKEN__EOF) break;
        /* CSS Syntax §4 consumes comments in the tokenizer and emits no token for them; lexbor's own header
           marks this type "not in specification", so it is dropped here rather than counted as a component
           value it is not. */
        if (t->type == LXB_CSS_SYNTAX_TOKEN_COMMENT) { lxb_css_syntax_token_consume(tkz); continue; }
        b = lxb_css_syntax_token_base(t)->begin;
        DCHECK(b >= (const lxb_char_t *)text && b <= (const lxb_char_t *)text + len,
               "a CSS token names a span outside the sizes attribute it was tokenized from — the tokenizer's "
               "buffer IS that string, so a pointer outside it means the buffer was set from something else");
        at = (size_t)(b - (const lxb_char_t *)text);
        end = at + lxb_css_syntax_token_base(t)->length;
        if (end > len) end = len;

        if (depth == 0) {
            begin = at;
            lead = t->type;
            if (!cv_opens_block(t->type)) {
                cv_add(out, at, end, t->type);
                lxb_css_syntax_token_consume(tkz);
                continue;
            }
        }
        if (cv_opens_block(t->type)) {
            if (depth == cap) {
                int grow = cap ? cap * 2 : 8;
                lxb_css_syntax_token_type_t *s2 = realloc(stack, (size_t)grow * sizeof *s2);

                CHECK(s2 != NULL, "§4.8.4.3.11: OOM tracking a sizes attribute's nested blocks");
                stack = s2;
                cap = grow;
            }
            stack[depth++] = cv_mirror(t->type);
        } else if (depth > 0 && t->type == stack[depth - 1]) {
            depth--;
            if (depth == 0) {
                cv_add(out, begin, end, lead);
                lxb_css_syntax_token_consume(tkz);
                continue;
            }
        }
        lxb_css_syntax_token_consume(tkz);
    }
    /* CSS Syntax's consume a simple block ends at EOF as well as at the mirror, so an unterminated block is a
       component value that runs to the end of the input rather than one that is discarded. */
    if (ok && depth > 0) cv_add(out, begin, len, lead);
    free(stack);
    lxb_css_syntax_tokenizer_destroy(tkz);
    return ok;
}

/* §4.8.4.3's ONE SENTENCE about units, as a function so the math-function arm and the bare-dimension arm ask
   it once: a source size's units other than the viewport-relative ones "must be interpreted THE SAME AS IN
   MEDIA QUERIES". So core/css/media_query.h's table is the answer and this file carries no second copy of it.
   FALSE for a unit that is not a `<length>` at all; a unit that IS one and that table cannot absolutize is a
   MISSING COMPONENT and crashes here, because reporting it as a refusal would turn one into a page's own parse
   error. */
static bool iss_length_px(JSContext *ctx, double n, const char *unit, size_t unit_len, double *px)
{
    if (media_query_length_px(ctx, n, unit, unit_len, px)) return true;
    if (css_length_is_length_unit(unit, unit_len))
        DFAIL("a `sizes` attribute's `<source-size-value>` is a `<length>` in a unit core/css/media_query.c "
              "cannot absolutize. HTML §4.8.4.3 'Processing model' is explicit that a source size's units "
              "other than the viewport-relative ones 'must be interpreted the same as in Media Queries', "
              "so this is that component's unit table and not a second one — and css-values-4 §6.1.1 "
              "'Font-relative Lengths' and §6.1.2 'Viewport-relative Lengths' are the two families it "
              "stops short of (the ten font METRICS beyond `em`/`rem`/`ex`/`ch`, and the `sv*`/`lv*`/`dv*` "
              "and `vi`/`vb` viewport families). Reporting one as an author's mistake would turn a missing "
              "component into a page's own parse error. Build it there, beside the table this asks");
    return false;
}

/* css-values-4 §10.10.1's canonical-unit step for a `<length>` leaf inside a math function, over the same
   table. It never answers a stand-in: a unit this engine cannot absolutize crashes above, in the one place
   that message belongs. */
static CssPx iss_math_length(void *ctx, double n, const char *unit, size_t unit_len)
{
    double v = 0.0;

    DCHECK(css_length_is_length_unit(unit, unit_len),
           "core/css/css_math.c asked this `sizes` attribute's resolver to absolutize a unit that is not one of "
           "css-values-4 §6's. That callback's contract is that it is called only for a unit "
           "`css_length_is_length_unit` has already admitted — every other family is converted inside that "
           "component by an exact ratio — so one arriving here is that contract having changed");
    (void)iss_length_px((JSContext *)ctx, n, unit, unit_len, &v);
    return css_px(v);
}

/* §4.8.4.3.11 step 3.2's "a valid non-negative `<source-size-value>`", over ONE component value's source span.
   `<source-size-value> = <length> | auto` (§4.8.4.2.2 "Sizes attributes"), and "A `<source-size-value>` that is
   a `<length>` must not be negative, and must not use CSS functions other than the math functions."
   `*is_auto` is the keyword arm, which carries no number; `*px` is the length in CSS pixels otherwise. */
static bool iss_source_size_value(JSContext *ctx, const char *text, size_t len, bool *is_auto, double *px)
{
    lxb_css_syntax_tokenizer_t *tkz = lxb_css_syntax_tokenizer_create();
    lxb_css_syntax_token_t *t;
    bool ok = false;

    *is_auto = false;
    *px = 0.0;
    CHECK(tkz != NULL, "§4.8.4.3.11: the source size value tokenizer allocation failed");
    if (lxb_css_syntax_tokenizer_init(tkz) != LXB_STATUS_OK) {
        lxb_css_syntax_tokenizer_destroy(tkz);
        return false;
    }
    lxb_css_syntax_tokenizer_buffer_set(tkz, (const lxb_char_t *)text, len);
    t = lxb_css_syntax_token(tkz);
    if (!t) { lxb_css_syntax_tokenizer_destroy(tkz); return false; }
    switch (t->type) {
    case LXB_CSS_SYNTAX_TOKEN_FUNCTION: {
        /* §4.8.4.2.2 admits the MATH FUNCTIONS here and nothing else — "must not use CSS functions other than
           the math functions" — and css-values-4 §10 makes one a value of whichever numeric type its operands
           give it, so `calc(100vw - 2em)` IS a `<length>`. The production asked for is `<length>` and NOT
           `<length-percentage>`, because §4.8.4.2.2 says so in its own words: "Percentages are not allowed in a
           <source-size-value>, to avoid confusion about what it would be relative to." That one parameter is
           the whole of the rule — a `calc(50%)` types as a `<percentage>` under it and simply does not match,
           so there is no second check to keep in step with the first. */
        CssMathResolver res;
        CssMathValue v;

        res.length_px = iss_math_length;
        res.ctx = ctx;
        res.realm = ctx;
        if (css_math_eval(text, len, &res, CSS_MATH_PROD_LENGTH, &v)) {
            /* THE FACT SET IS EMPTY HERE AND THAT IS media_query.h's LAYERING, not a drop. That header states
               it for `media_query_length_px`: "The answer is a plain `double` — the modelled EXAMPLE — ... the
               C side resolves and the concolic is minted at the JS boundary." A viewport-derived `sizes` value
               therefore reaches the page through `img.currentSrc`, which is where the source identity belongs.
               ASSERTED rather than assumed, so the day that table starts answering an environment fact this
               arm is where the obligation to carry it surfaces. */
            DCHECK(v.num.env == CSS_ENV_NONE && !v.pct_term,
                   "a `sizes` attribute's math function resolved to a length carrying an environment fact or a "
                   "surviving `<percentage>`. Neither is reachable through this resolver: core/css/"
                   "media_query.h answers a plain modelled number by design, and HTML §4.8.4.2.2 'Sizes "
                   "attributes' forbids a percentage here, which css_math_eval enforces by being asked for a "
                   "`<length>` rather than a `<length-percentage>`. So this is one of those two having changed "
                   "— and if it is the first, this arm now owes the page the source identity it is carrying");
            /* "A `<source-size-value>` that is a `<length>` MUST NOT BE NEGATIVE" — and §4.8.4.3.11 step 3.2
               asks for a valid NON-NEGATIVE one, so a negative length is a parse error and not a size. */
            if (v.num.px >= 0.0) { *px = v.num.px; ok = true; }
        }
        break;
    }
    case LXB_CSS_SYNTAX_TOKEN_IDENT: {
        const lxb_css_syntax_token_string_t *s = lxb_css_syntax_token_string(t);

        /* §4.8.4.2.2: "The keyword `auto` is a width that is computed in parse a sizes attribute." A CSS
           keyword is ASCII case-insensitive, and §4.8.4.2.2 spells that out for this one where it constrains
           where in the list it may appear. */
        if (iss_ascii_ci_eq((const char *)s->data, s->length, "auto")) { *is_auto = true; ok = true; }
        break;
    }
    case LXB_CSS_SYNTAX_TOKEN_DIMENSION: {
        const lxb_css_syntax_token_string_t *u = lxb_css_syntax_token_dimension_string(t);
        double n = lxb_css_syntax_token_number(t)->num;
        double v = 0.0;

        if (iss_length_px(ctx, n, (const char *)u->data, u->length, &v)) {
            /* "A `<source-size-value>` that is a `<length>` MUST NOT BE NEGATIVE" — and §4.8.4.3.11 step 3.2
               asks for a valid NON-NEGATIVE one, so a negative length is a parse error and not a size. */
            if (v >= 0.0) { *px = v; ok = true; }
        }
        break;
    }
    case LXB_CSS_SYNTAX_TOKEN_NUMBER:
        /* css-values-4 §6 permits the unit to be omitted for ZERO and for nothing else, which is the whole of
           what a bare `<number>` can be here. `media_query_length_px` owns that rule too. */
        if (media_query_length_px(ctx, lxb_css_syntax_token_number(t)->num, "", 0, px)) ok = true;
        break;
    default:
        /* §4.8.4.2.2: "PERCENTAGES ARE NOT ALLOWED in a `<source-size-value>`, to avoid confusion about what it
           would be relative to." Every other token is not a `<length>` either. */
        break;
    }
    if (ok) {
        /* A component value is ONE token or one block, and this span was cut at a component value boundary, so
           there must be nothing after it. A second token means the walk that produced the span and this
           re-tokenization disagree about where one ends. */
        lxb_css_syntax_token_consume(tkz);
        t = lxb_css_syntax_token(tkz);
        DCHECK(t != NULL && t->type == LXB_CSS_SYNTAX_TOKEN__EOF,
               "a `<source-size-value>` span held more than one component value — the span was cut by "
               "iss_component_values at a top-level boundary, so a second token here means those two walks "
               "read the same bytes as different component values");
    }
    lxb_css_syntax_tokenizer_destroy(tkz);
    return ok;
}

/* §4.8.3 "The img element": "An img element ALLOWS AUTO-SIZES if: its `loading` attribute is in the Lazy state,
   and its `sizes` attribute's value is 'auto' (ASCII case-insensitive), or starts with 'auto,' (ASCII
   case-insensitive)." The `loading` attribute is §2.5.7's enumerated lazy loading attribute, whose keywords are
   `lazy` and `eager` with Eager the missing-value and invalid-value default — so the Lazy state is the one
   spelling and every other value, including an absent attribute, is Eager. */
static bool iss_allows_auto_sizes(lxb_dom_element_t *img)
{
    size_t n = 0;
    const char *v;

    if (!img) return false;
    v = iss_attr(img, "loading", &n);
    if (!v || !iss_ascii_ci_eq(v, n, "lazy")) return false;
    v = iss_attr(img, "sizes", &n);
    if (!v) return false;
    if (iss_ascii_ci_eq(v, n, "auto")) return true;
    return n >= 5 && iss_ascii_ci_eq(v, 5, "auto,");
}

/* §4.8.4.3.11 "Parsing a sizes attribute", answering the source size in CSS pixels.
   IT TAKES THE VALUE AND NOT THE ELEMENT, because the standard reaches it two ways and only one of them has an
   element to read: §4.8.4.3.9 step 5.7 says "Parse CHILD's sizes attribute with img" while §4.8.4.3.8 step 3
   says "parsing SIZES with img" over a string its own caller already took off an element. `img` is the second
   argument in both — the `img` element or null — and is what step 3.3's auto-sizes branch is about. */
static double iss_parse_sizes(JSContext *ctx, const char *text, size_t len, lxb_dom_element_t *img)
{
    IssCvList cvs = { NULL, 0, 0 };
    int start = 0, i;
    /* Step 4's answer unless some entry returns first, so it is written before the walk rather than after it —
       there is no arm here that leaves it unwritten, and a `double` nobody assigned is exactly the plausible
       datum this engine refuses to produce. */
    double answer;

    if (!text) { text = ""; len = 0; }
    /* Step 1: "Let unparsed sizes list be the result of parsing a comma-separated list of component values from
       the value of element's sizes attribute (OR THE EMPTY STRING, IF THE ATTRIBUTE IS ABSENT)." */
    if (!iss_component_values(text, len, &cvs)) goto hundred_vw;
    /* Step 3: "For each unparsed size in unparsed sizes list" — the groups the top-level commas cut. */
    for (i = 0; i <= cvs.n; i++) {
        int last;
        bool is_auto = false, ok;
        double size = 0.0;
        size_t rem_begin, rem_end;

        if (i < cvs.n && cvs.v[i].lead != LXB_CSS_SYNTAX_TOKEN_COMMA) continue;
        last = i - 1;                                    /* the group is [start, last] inclusive */

        /* Step 3.1: "Remove all consecutive `<whitespace-token>`s from the END of unparsed size. If unparsed
           size is now empty, then that is a parse error; continue." */
        while (last >= start && cvs.v[last].lead == LXB_CSS_SYNTAX_TOKEN_WHITESPACE) last--;
        if (last < start) { start = i + 1; continue; }

        /* Step 3.2: "If the LAST COMPONENT VALUE in unparsed size is a valid non-negative
           `<source-size-value>`, then set size to its value and REMOVE the component value from unparsed size.
           Otherwise, there is a parse error; continue." */
        ok = iss_source_size_value(ctx, text + cvs.v[last].begin, cvs.v[last].end - cvs.v[last].begin,
                                   &is_auto, &size);
        if (!ok) { start = i + 1; continue; }
        last--;

        /* Step 3.3: "If size is auto, and img is not null, and img is BEING RENDERED, and img ALLOWS
           AUTO-SIZES, then set size to the CONCRETE OBJECT SIZE WIDTH of img, in CSS pixels. If size is still
           auto, then it will be ignored."
           CSS Images 3 §4.5 "Sizing Objects: the object-fit property" is what that width IS under the initial
           `fill`: "The replaced content is sized to fill the element's CONTENT BOX: the object's concrete
           object size is the element's USED WIDTH AND HEIGHT". So the question is asked of the component that
           owns used values, and whichever arm of CSS 2.1 §10 it cannot compute crashes there rather than being
           guessed at here.
           IT IS THE CONTENT EXTENT AND NOT `used_value_px`, which is the half of §4.5's sentence that names a
           BOX. The two differ under `box-sizing: border-box`, where css-sizing §5 makes the value `width`
           EXPOSES the border box's — and `* { box-sizing: border-box }` is on most of the modern web, so an
           img with padding would otherwise select its source against a box the image is not drawn into. */
        if (is_auto && img && element_view_has_box(lxb_dom_interface_node(img)) && iss_allows_auto_sizes(img)) {
            size = used_value_content_px(img, false).px;
            is_auto = false;
        }

        /* Step 3.4: "Remove all consecutive `<whitespace-token>`s from the end of unparsed size. If unparsed
           size is now empty: if this was not the last item in unparsed sizes list, that is a parse error; if
           size is not auto, then return size. Otherwise, continue." */
        while (last >= start && cvs.v[last].lead == LXB_CSS_SYNTAX_TOKEN_WHITESPACE) last--;
        if (last < start) {
            if (!is_auto) { answer = size; goto done; }
            start = i + 1;
            continue;
        }

        /* Step 3.5: "Parse the remaining component values in unparsed size as a `<media-condition>`. If it does
           not parse correctly, or it does parse correctly but the `<media-condition>` evaluates to FALSE,
           continue." */
        rem_begin = cvs.v[start].begin;
        rem_end = cvs.v[last].end;
        DCHECK(rem_end >= rem_begin && rem_end <= len,
               "§4.8.4.3.11's remaining component values name a span outside the sizes attribute — every offset "
               "here came from one walk over one buffer, so a span outside it is those offsets disagreeing");
        {
            char *cond_text = iss_copy(text + rem_begin, rem_end - rem_begin);
            MediaQuerySet *cond = media_query_parse_condition(cond_text);
            bool matched = cond != NULL && media_query_matches_now(ctx, cond);

            media_query_free(cond);
            free(cond_text);
            if (!matched) { start = i + 1; continue; }
        }

        /* Step 3.6: "If size is not auto, then return size. Otherwise, continue." */
        if (!is_auto) { answer = size; goto done; }
        start = i + 1;
    }

hundred_vw:
    /* Step 4: "Return 100vw." — css-values-4 §6.1.2 makes `vw` a hundredth of the INITIAL CONTAINING BLOCK's
       width, and CSS 2.1 §10.1 makes that the viewport's, which is core/frame/viewport.h's modelled rectangle
       for THIS document. It is answered as the EXAMPLE for the reason image_source_set.h states: the number
       is consumed by a C division and a C comparison. */
    answer = viewport_width(ctx);
done:
    free(cvs.v);
    DCHECK(!(answer < 0.0),
           "§4.8.4.3.11 returned a NEGATIVE source size — every arm that can return one has already refused a "
           "negative `<source-size-value>` per §4.8.4.2.2, and the fallback is a viewport width");
    return answer;
}

/* ---- §4.8.4.3.12 "Normalizing the source densities" --------------------------------------------------------- */

static void iss_normalize(ImageSourceSet *s)
{
    int i;

    /* "Let source size be source set's source size. For each image source in source set: If the image source
       has a pixel density descriptor, continue to the next image source. Otherwise, if the image source has a
       width descriptor, REPLACE the width descriptor with a pixel density descriptor with a value of the width
       descriptor value DIVIDED BY SOURCE SIZE and a unit of x. … Otherwise, give the image source a pixel
       density descriptor of 1x."
       A SOURCE SIZE OF 0 IS NOT A SPECIAL CASE — §4.8.4.3.12 says so itself ("If the source size is 0, then the
       density would be infinity, which results in the natural dimensions being 0 by 0"), and IEEE division
       answers exactly that. Clamping it would be a bound on a value the standard defines. */
    for (i = 0; i < s->n; i++) {
        if (s->items[i].has_density) continue;
        if (s->items[i].has_width) {
            s->items[i].density = s->items[i].width / s->source_size;
            s->items[i].has_width = false;
        } else {
            s->items[i].density = 1.0;
        }
        s->items[i].has_density = true;
    }
    for (i = 0; i < s->n; i++)
        DCHECK(s->items[i].has_density,
               "§4.8.4.3.12 left an image source without a pixel density descriptor — 'normalizing a source "
               "set gives EVERY image source a pixel density descriptor' is the whole of what this algorithm "
               "is, and every consumer below reads that descriptor and nothing else");
}

/* ---- §4.8.4.3.8 "Creating a source set from attributes" ----------------------------------------------------- */

/* "When asked to CREATE A SOURCE SET given a string default source, a string srcset, a string sizes, and an
   element or null img."
   The three taints travel beside the three strings because the question "is this string readable" belongs to
   the string and not to the element — a `<source>`'s srcset and an `img`'s reach this from different elements.
   Each is consulted ONLY on the arm that uses its string, which is what keeps a tainted `src` that step 4 never
   appends from making a perfectly readable set undecided. */
static void iss_create_source_set(JSContext *ctx, ImageSourceSet *out,
                                  const char *default_source, size_t default_n, JSValueConst default_taint,
                                  const char *srcset, size_t srcset_n, JSValueConst srcset_taint,
                                  const char *sizes, size_t sizes_n, JSValueConst sizes_taint,
                                  lxb_dom_element_t *img)
{
    bool has_1x = false, has_width = false;
    int i;

    /* Step 1: "Let source set be an empty source set." — `out` arrives empty; the walk asserts it. */
    DCHECK(out->n == 0, "§4.8.4.3.8 was handed a source set that already holds image sources — step 1 is 'let "
                        "source set be an EMPTY source set', and §4.8.4.3.9 gives every element a fresh one");

    /* Step 2: "If srcset is not an empty string, then set source set to the result of parsing srcset." */
    if (srcset_n) {
        if (!JS_IsUndefined(srcset_taint)) { iss_undecided(ctx, out, srcset_taint); return; }
        iss_parse_srcset(srcset, srcset_n, out);
    }

    /* Step 3: "Set source set's source size to the result of parsing sizes with img." — ALWAYS, including for
       an absent attribute, whose empty string §4.8.4.3.11 answers with `100vw`. */
    out->source_size = iss_parse_sizes(ctx, sizes, sizes_n, img);

    /* Step 4: "If default source is not the empty string and source set does not contain an image source with a
       PIXEL DENSITY DESCRIPTOR VALUE OF 1, and NO image source with a WIDTH DESCRIPTOR, append default source
       to source set." */
    for (i = 0; i < out->n; i++) {
        if (out->items[i].has_density && out->items[i].density == 1.0) has_1x = true;
        if (out->items[i].has_width) has_width = true;
    }
    if (default_n && !has_1x && !has_width) {
        if (!JS_IsUndefined(default_taint)) { iss_undecided(ctx, out, default_taint); return; }
        iss_push(out, iss_copy(default_source, default_n), false, 0.0, false, 0.0);
    }

    /* THE SOURCE SIZE IS ONLY EVER READ BY THE STEP BELOW, so an unreadable `sizes` matters exactly when some
       image source carries a WIDTH descriptor and not otherwise — a `srcset="a 1x, b 2x"` decides the same set
       whatever the sizes attribute says. Asked here rather than at the read for that reason. */
    if (has_width && !JS_IsUndefined(sizes_taint)) { iss_undecided(ctx, out, sizes_taint); return; }

    /* Step 5: "Normalize the source densities of source set." */
    iss_normalize(out);
}

/* ---- §4.8.4.3.9 "Updating the source set" ------------------------------------------------------------------- */

static lxb_dom_element_t *iss_first_element_child(lxb_dom_node_t *parent)
{
    lxb_dom_node_t *n;

    for (n = parent ? parent->first_child : NULL; n; n = n->next)
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) return lxb_dom_interface_element(n);
    return NULL;
}

static lxb_dom_element_t *iss_next_element_sibling(lxb_dom_element_t *el)
{
    lxb_dom_node_t *n;

    for (n = lxb_dom_interface_node(el)->next; n; n = n->next)
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) return lxb_dom_interface_element(n);
    return NULL;
}

static bool iss_is_tag(const lxb_dom_element_t *el, lxb_tag_id_t tag)
{
    const lxb_dom_node_t *n = lxb_dom_interface_node((lxb_dom_element_t *)el);

    return n->type == LXB_DOM_NODE_TYPE_ELEMENT && lxb_html_tree_node_is(n, tag);
}

/* §4.8.4.3.9 step 8's "If child has a `type` attribute, and its value is an UNKNOWN OR UNSUPPORTED MIME TYPE,
 * continue to the next child."
 *
 * WHICH IMAGE TYPES THIS USER AGENT SUPPORTS, AND WHY THE ANSWER IS ALL OF THEM. §4.8.4.3 "Processing model"
 * ends "This specification does not specify which image types are to be supported", so the set is a UA choice
 * and not a fact to discover. core/html/html_image.h already made the corresponding one for the element as a
 * whole: this agent HAS no decoder, and it deliberately does not take §4.8.4.3.5's "cannot support images"
 * exit, because doing so would issue no request and fire no event — deleting both an endpoint the page named
 * and the auto-firing sink an `onerror` is. The same reasoning decides this step. A `<source type="image/avif">`
 * whose branch is refused here loses its address from the surface entirely and hands the selection to a
 * fallback the page did not choose; what this agent actually does with every image resource — fetch it, and
 * land the reply in the not-a-supported-file-format arm — is identical for every image type, so no image type
 * is more supported than another.
 * WHAT IS STILL REFUSED, and it is the half the step is really about: a type this engine cannot PARSE is
 * §4.8.4.3.9's "unknown", and a type that is not an image type at all is §4.8.4.3's own "user agents must not
 * support non-image resources with the img element". */
static bool iss_type_supported(const char *type, size_t len)
{
    MimeType m;
    bool ok;

    /* MIME Sniffing §4.4's failure leaves the record "initialised-and-empty … so the caller frees it either
       way" (core/mime/mime_type.h) — which is why the free is unconditional and the parse's answer is a
       separate `bool` rather than a state read back off the record. */
    ok = mime_type_parse(&m, type, len) && mime_type_is_image(&m);
    mime_type_free(&m);
    return ok;
}

/* §4.8.4.3.9 step 5, for `child` == `el` — "Set el's source set to the result of creating a source set given
   default source, srcset, sizes, and img", with the four strings §4.8.4.3.9 reads off an `img`. */
static void iss_set_from_img(JSContext *ctx, lxb_dom_element_t *el, ImageSourceSet *out)
{
    const char *srcset, *sizes, *src;
    size_t srcset_n = 0, sizes_n = 0, src_n = 0;

    /* "Let default source be the empty string. Let srcset be the empty string. Let sizes be the empty string.
       If el is an img element that has a srcset attribute, then set srcset to that attribute's value. If el is
       an img element that has a sizes attribute, then set sizes to that attribute's value. If el is an img
       element that has a src attribute, then set default source to that attribute's value." */
    srcset = iss_attr(el, "srcset", &srcset_n);
    sizes = iss_attr(el, "sizes", &sizes_n);
    src = iss_attr(el, "src", &src_n);
    iss_create_source_set(ctx, out,
                          src, src_n, dom_cow_attr_taint(el, "src"),
                          srcset, srcset_n, dom_cow_attr_taint(el, "srcset"),
                          sizes, sizes_n, dom_cow_attr_taint(el, "sizes"),
                          el);
}

/* §4.8.4.3.9 "Updating the source set" for an `img` element. */
static void iss_update_source_set(JSContext *ctx, lxb_dom_element_t *el, ImageSourceSet *out)
{
    lxb_dom_node_t *parent = lxb_dom_interface_node(el)->parent;
    lxb_dom_element_t *child;

    /* Steps 1-3: "Set el's source set to an empty source set. Let elements be « el ». If el is an img element
       whose PARENT NODE IS A PICTURE ELEMENT, then replace the contents of elements with el's parent node's
       CHILD ELEMENTS, retaining relative order." */
    if (!parent || parent->type != LXB_DOM_NODE_TYPE_ELEMENT ||
        !lxb_html_tree_node_is(parent, LXB_TAG_PICTURE)) {
        iss_set_from_img(ctx, el, out);
        return;
    }

    /* Step 5: "For each child in elements" — and §4.8.4.3.9's own note is what makes the walk stop at `el`:
       "Each img element independently considers its PREVIOUS SIBLING source elements plus the img element
       itself for selecting an image source, ignoring any other (invalid) elements, including other img
       elements in the same picture element, or source elements that are FOLLOWING SIBLINGS." */
    for (child = iss_first_element_child(parent); child; child = iss_next_element_sibling(child)) {
        ImageSourceSet cand;
        const char *srcset, *sizes, *media, *type;
        size_t srcset_n = 0, sizes_n = 0, media_n = 0, type_n = 0;
        JSValue taint;

        /* Step 5.1: "If child is el: … Set el's source set … Return." */
        if (child == el) { iss_set_from_img(ctx, el, out); return; }
        /* Step 5.2: "If child is not a source element, then continue." */
        if (!iss_is_tag(child, LXB_TAG_SOURCE)) continue;
        /* Step 5.3: "If child does not have a srcset attribute, continue to the next child." */
        if (!iss_has_attr(child, "srcset")) continue;

        /* Step 5.4: "PARSE CHILD'S SRCSET ATTRIBUTE and let source set be the returned source set." */
        taint = dom_cow_attr_taint(child, "srcset");
        if (!JS_IsUndefined(taint)) { iss_undecided(ctx, out, taint); return; }
        memset(&cand, 0, sizeof cand);
        cand.selected = -1;
        cand.undecided_url = JS_UNDEFINED;
        srcset = iss_attr(child, "srcset", &srcset_n);
        iss_parse_srcset(srcset ? srcset : "", srcset_n, &cand);

        /* Step 5.5: "If source set has ZERO IMAGE SOURCES, continue to the next child." */
        if (cand.n == 0) { iss_clear_items(&cand); continue; }

        /* Step 5.6: "If child has a MEDIA attribute, and its value DOES NOT MATCH THE ENVIRONMENT, continue to
           the next child." §4.8.2's `media` attribute is a valid media query list, so it is the LIST grammar
           and not the bare `<media-condition>` a sizes entry carries. The evaluation goes through
           core/css/media_query.h's non-forking read: C cannot fork, so it takes the arm THIS FLOW has already
           committed to and falls back to the modelled environment where the flow has committed to neither. */
        if (iss_has_attr(child, "media")) {
            MediaQuerySet *set;
            bool matched;

            taint = dom_cow_attr_taint(child, "media");
            if (!JS_IsUndefined(taint)) { iss_clear_items(&cand); iss_undecided(ctx, out, taint); return; }
            media = iss_attr(child, "media", &media_n);
            {
                char *text = iss_copy(media ? media : "", media_n);

                set = media_query_parse(text);
                free(text);
            }
            matched = media_query_matches_now(ctx, set);
            media_query_free(set);
            if (!matched) { iss_clear_items(&cand); continue; }
        }

        /* Step 5.7: "PARSE CHILD'S SIZES ATTRIBUTE with img, and let source set's source size be the returned
           value." */
        taint = dom_cow_attr_taint(child, "sizes");
        sizes = iss_attr(child, "sizes", &sizes_n);
        cand.source_size = iss_parse_sizes(ctx, sizes, sizes_n, el);

        /* Step 5.8: "If child has a TYPE attribute, and its value is an unknown or unsupported MIME type,
           continue to the next child." */
        if (iss_has_attr(child, "type")) {
            JSValue ttaint = dom_cow_attr_taint(child, "type");

            if (!JS_IsUndefined(ttaint)) { iss_clear_items(&cand); iss_undecided(ctx, out, ttaint); return; }
            type = iss_attr(child, "type", &type_n);
            if (!type || !iss_type_supported(type, type_n)) { iss_clear_items(&cand); continue; }
        }

        /* Step 5.9: "If child has WIDTH OR HEIGHT attributes, set el's DIMENSION ATTRIBUTE SOURCE to child.
           Otherwise, set el's dimension attribute source to el."
           NOT STORED, and image_source_set.h says why: it is a pure function of this walk, its only consumers
           are §4.8.3's `width` and `height` — which core/html/html_image.c declares ABSENT because their first
           branch is the element's rendered size — and a stored copy of a derived fact is a second thing to
           keep in step. The day those members exist they take it from here. */

        /* THE SOURCE SIZE'S ONE CONSUMER AGAIN — see the same test in create-a-source-set. */
        if (!JS_IsUndefined(taint)) {
            int i;
            bool has_width = false;

            for (i = 0; i < cand.n; i++) if (cand.items[i].has_width) has_width = true;
            if (has_width) { iss_clear_items(&cand); iss_undecided(ctx, out, taint); return; }
        }

        /* Steps 5.10-5.12: "Normalize the source densities of source set. Set el's source set to source set.
           Return." The candidate set's ITEMS move to `out` and nothing else does, which holds only while a
           `<source>`'s own set can carry no undecidability of its own — every attribute that could make it
           undecided is tested above and returns through `out`, never through `cand`. */
        DCHECK(JS_IsUndefined(cand.undecided_url) && !cand.undecided,
               "§4.8.4.3.9's per-`source` candidate set became undecided inside the srcset parse — every "
               "undecidable attribute is tested at its read and reports through the element's own set, so a "
               "flag here is a value the move below would drop on the floor");
        iss_normalize(&cand);
        out->items = cand.items;
        out->n = cand.n;
        out->source_size = cand.source_size;
        return;
    }
    /* An `img` is one of its parent's child elements, so the walk above always reaches it — a walk that ran off
       the end means the element this was asked about is not in the tree it was asked about. */
    DFAIL("§4.8.4.3.9's walk over a `picture` element's child elements ran past the `img` it was updating. "
          "Step 3 replaces `elements` with the img's own PARENT NODE's child elements, so the img is one of "
          "them by construction — reaching the end means the child list and the parent pointer disagree");
}

/* ---- §4.8.4.3.7 "Selecting an image source" ----------------------------------------------------------------- */

/* "To SELECT AN IMAGE SOURCE FROM A SOURCE SET given a source set sourceSet: If an entry b in sourceSet has the
   SAME associated pixel density descriptor as an EARLIER entry a in sourceSet, then remove entry b. Repeat this
   step until none of the entries in sourceSet have the same associated pixel density descriptor as an earlier
   entry. In an IMPLEMENTATION-DEFINED MANNER, choose one image source from sourceSet. Let selectedSource be
   this choice. Return selectedSource and its associated pixel density."
 *
 * THE DEDUPLICATION IS THE SPEC'S AND IS PERFORMED AS AN INDEX FILTER rather than by deleting entries: the
 * caller records EVERY image source in the set on the @H surface (image_source_set.h says why), so removing a
 * duplicate from the list would delete an address the bundle ships in order to answer a question about which
 * one to fetch. A duplicate is skipped by the CHOICE, which is the whole of what removing it decides.
 *
 * THE CHOICE IS THE UA's, AND THIS ONE IS STATED RATHER THAN LEFT IMPLICIT: the SMALLEST density that is at
 * least the output device's, and the LARGEST density otherwise. That is an image at least as dense as the
 * display when the author shipped one, and the densest available when they did not, which is what every
 * shipping browser does and what the `2x` in an author's `srcset` means. The device pixel ratio is
 * core/frame/viewport.h's modelled 1.0 and is read as the EXAMPLE, for the reason stated in this file's
 * header — the comparison below is a C `if`. */
static int iss_choose(JSContext *ctx, const ImageSourceSet *s)
{
    double dpr = viewport_device_pixel_ratio(ctx);
    int best = -1, largest = -1, i, j;

    DCHECK(s->n > 0, "§4.8.4.3.7's choice was asked of an EMPTY source set — step 2 answers a null URL for one "
                     "and never reaches step 3");
    for (i = 0; i < s->n; i++) {
        bool dup = false;

        DCHECK(s->items[i].has_density,
               "§4.8.4.3.7 compared an image source with no pixel density descriptor — §4.8.4.3.12 gives every "
               "source one and runs before every path that reaches here");
        for (j = 0; j < i; j++)
            if (s->items[j].density == s->items[i].density) dup = true;
        if (dup) continue;                                     /* the spec's own removal of a later duplicate */
        if (largest < 0 || s->items[i].density > s->items[largest].density) largest = i;
        if (s->items[i].density >= dpr && (best < 0 || s->items[i].density < s->items[best].density)) best = i;
    }
    DCHECK(largest >= 0, "§4.8.4.3.7 found no surviving entry in a non-empty source set — the first entry can "
                         "never duplicate an earlier one, so at least it survives");
    return best >= 0 ? best : largest;
}

void image_source_set_select(JSContext *ctx, lxb_dom_element_t *el, ImageSourceSet *out)
{
    DCHECK(out != NULL, "§4.8.4.3.7 was asked to select an image source with nowhere to report it");
    DCHECK(el != NULL, "§4.8.4.3.7 was asked to select an image source for no element");
    memset(out, 0, sizeof *out);
    out->selected = -1;
    out->undecided_url = JS_UNDEFINED;
    DCHECK(iss_is_tag(el, LXB_TAG_IMG),
           "§4.8.4.3.7 was asked to select an image source for an element that is not an `img`. §4.8.4.3.9 is "
           "written over 'a given img or link element' and the link half reads `imagesrcset`/`imagesizes`/"
           "`href` instead — a different set of attribute names with a different consumer, which is a caller "
           "this component does not have yet rather than one it can serve by accident");

    /* Step 1: "Update the source set for el." */
    iss_update_source_set(ctx, el, out);
    /* Step 2: "If el's source set is EMPTY, return NULL as the URL and undefined as the pixel density." — and
       an UNDECIDED set answers the same way, because a choice made out of a value nobody can read is not one. */
    if (out->undecided || out->n == 0) {
        DCHECK(out->selected == -1,
               "a source set that is empty or undecided named a selected image source — both answers are "
               "§4.8.4.3.7 step 2's null URL, and an index here would send a fetch to a source nobody chose");
        return;
    }
    /* Step 3: "Return the result of SELECTING AN IMAGE FROM el's source set." */
    out->selected = iss_choose(ctx, out);
    DCHECK(out->selected >= 0 && out->selected < out->n,
           "§4.8.4.3.7's choice named an image source outside the set it chose from");
}
