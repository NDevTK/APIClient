/* css-backgrounds-3 §2.10's `background` shorthand — see css_background_shorthand.h for why it is a component
   of its own. */
#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/css/css_background_shorthand.h"
#include "core/css/css_color.h"
#include "core/css/css_defaulting.h"
#include "core/css/css_image.h"
#include "core/css/css_length.h"
#include "core/css/css_math.h"
#include "core/css/css_position_value.h"

#define BG_N(a) ((unsigned)(sizeof(a) / sizeof((a)[0])))

/* The slots of §2.10's `<bg-layer>`, which ARE indices into the longhand list below and into every parallel
   array in this file. A second numbering is how a `<visual-box>` pair ends up assigned to the wrong axis. */
#define BG_IMAGE      0
#define BG_POSITION   1
#define BG_SIZE       2
#define BG_REPEAT     3
#define BG_ATTACHMENT 4
#define BG_ORIGIN     5
#define BG_CLIP       6
#define BG_COLOR      7

const char *const CSS_BACKGROUND_SHORTHAND_LONGHANDS[CSS_BACKGROUND_SHORTHAND_N] = {
    "background-image", "background-position", "background-size", "background-repeat",
    "background-attachment", "background-origin", "background-clip", "background-color",
};

/* EACH PROPERTY'S OWN `Initial:` LINE, IN THE FORM THIS FILE SERIALIZES IT — which is not always the form the
   line is written in, and the difference is a spec rule rather than a convenience.
     §2.3 background-image      -> none
     §2.6 background-position   -> `0% 0%`, already the two components css-values-4 §8.3.2 always writes
     §2.9 background-size       -> the line reads `auto`, and §2.9.1 "Serialization of background-size values"
                                   says the type's specified value "always serialize[s] as two values, even
                                   when the second value is auto", so its serialization is `auto auto`. The two
                                   are one value written two ways, and this array is the SERIALIZED face
                                   because that is the face §6.7.2's omission rule compares against.
     §2.4 background-repeat     -> repeat
     §2.5 background-attachment -> scroll
     §2.8 background-origin     -> padding-box
     §2.7 background-clip       -> border-box
     §2.2 background-color      -> transparent
   THE TWO `<visual-box>` INITIALS DIFFER, which is the one thing that makes §2.10's one-value form a rule
   rather than a shorthand for equality: `origin: padding-box, clip: border-box` is BOTH-INITIAL and writes
   nothing, while `origin: border-box, clip: border-box` is EQUAL and writes one value. A serializer that
   tested only equality would emit `padding-box border-box` for an undeclared background. */
static const char *const BG_INITIAL[CSS_BACKGROUND_SHORTHAND_N] = {
    "none", "0% 0%", "auto auto", "repeat", "scroll", "padding-box", "border-box", "transparent",
};

/* THE LONGEST `<final-bg-layer>` §2.10's grammar admits, component by component: `<bg-image>` (1) plus
   `<bg-position>` at css-backgrounds-3 §2.6's four (4) plus the `/` delim (1) plus `<bg-size>`'s `{2}` (2)
   plus `<repeat-style>`'s `{1,2}` (2) plus `<attachment>` (1) plus two `<visual-box>` (2) plus
   `<'background-color'>` (1) — fourteen. A layer longer than that matches NO arm, so refusing it is the
   grammar's own answer. */
#define BG_MAX_WORDS 16

static char *bg_strdup(const char *s)
{
    char *out = strdup(s);

    CHECK(out != NULL, "cssom: OOM expanding a `background` shorthand — a dropped component would read as "
                       "undeclared, which is the property's INITIAL value and a different declaration");
    return out;
}

static char *bg_dupn(const char *s, size_t n)
{
    char *out = malloc(n + 1);

    CHECK(out != NULL, "cssom: OOM copying a `background` component — a dropped one would read as undeclared");
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

/* A CSS keyword comparison over a span, and the canonical lower-case spelling CSSOM serializes back out. */
static bool bg_word_is(const char *w, size_t n, const char *kw)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (kw[i] == '\0' || (char)tolower((unsigned char)w[i]) != kw[i]) return false;
    return kw[n] == '\0';
}

static const char *bg_keyword(const char *const *set, unsigned n, const char *w, size_t len)
{
    unsigned i;

    for (i = 0; i < n; i++)
        if (bg_word_is(w, len, set[i])) return set[i];
    return NULL;
}

/* ---- the two splitters --------------------------------------------------------------------------------- */

/* The next TOP-LEVEL comma group — one `<bg-layer>` of §2.10's `#`-list, or one item of a longhand's own
   `<X>#` list. A cursor rather than an array because the layer count is the declaration's and has no bound in
   the grammar; `*pos` past `len` marks exhausted, so a trailing comma yields a final EMPTY group rather than
   disappearing, and `background: red,` is the invalid declaration it should be. */
static bool bg_next_group(const char *text, size_t len, size_t *pos, const char **out, size_t *out_len)
{
    size_t i, start;
    int depth = 0;
    char quote = 0;

    if (*pos > len) return false;
    i = start = *pos;
    while (i < len) {
        char c = text[i];

        if (quote) {
            if (c == '\\' && i + 1 < len) i++;
            else if (c == quote) quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '(') {
            depth++;
        } else if (c == ')') {
            if (depth > 0) depth--;
        } else if (c == ',' && depth == 0) {
            break;
        }
        i++;
    }
    *out = text + start;
    *out_len = i - start;
    *pos = i + 1;
    return true;
}

/* The COMPONENT VALUES of one layer. A top-level `/` is CSS Syntax §4's DELIM token and therefore a component
   value of its own however it was spaced — `40%/10em` and `40% / 10em` are the same three components, and a
   splitter that only broke on whitespace would hand `40%/10em` to the position matcher as one word and refuse
   the declaration. -1 when there are more than `max`, which is BG_MAX_WORDS' own statement of the grammar. */
static int bg_words(const char *v, size_t n, const char **w, size_t *wl, int max)
{
    int cnt = 0;
    size_t i = 0;

    while (i < n) {
        size_t s;
        int depth = 0;
        char quote = 0;

        while (i < n && isspace((unsigned char)v[i])) i++;
        if (i >= n) break;
        if (v[i] == '/') {
            if (cnt == max) return -1;
            w[cnt] = v + i;
            wl[cnt] = 1;
            cnt++;
            i++;
            continue;
        }
        s = i;
        while (i < n) {
            char c = v[i];

            if (quote) {
                if (c == '\\' && i + 1 < n) i++;
                else if (c == quote) quote = 0;
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '(') {
                depth++;
            } else if (c == ')') {
                if (depth > 0) depth--;
            } else if (depth == 0 && (isspace((unsigned char)c) || c == '/')) {
                break;
            }
            i++;
        }
        if (cnt == max) return -1;
        w[cnt] = v + s;
        wl[cnt] = i - s;
        cnt++;
    }
    return cnt;
}

/* ---- the growable answer ------------------------------------------------------------------------------- */

typedef struct { char *s; size_t n, cap; } BgBuf;

static void bg_buf_init(BgBuf *b)
{
    b->cap = 64;
    b->n = 0;
    b->s = malloc(b->cap);
    CHECK(b->s != NULL, "cssom: OOM building a `background` value");
    b->s[0] = '\0';
}

static void bg_buf_add(BgBuf *b, const char *p, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        char *grown;

        while (b->n + n + 1 > b->cap) b->cap *= 2;
        grown = realloc(b->s, b->cap);
        CHECK(grown != NULL, "cssom: OOM growing a `background` value");
        b->s = grown;
    }
    memcpy(b->s + b->n, p, n);
    b->n += n;
    b->s[b->n] = '\0';
}

static void bg_buf_addz(BgBuf *b, const char *p) { bg_buf_add(b, p, strlen(p)); }

/* ---- §2.10's TERM GRAMMARS ------------------------------------------------------------------------------
 *
 * Each matcher answers how many component values it CONSUMED (0 for no match) and writes the term's own
 * serialized specified value. The keyword sets below are DISJOINT — no `<repeat-style>` word is an
 * `<attachment>` word is a `<visual-box>` word is one of css-values-4 §8.3's position keywords, and none of
 * them is a CSS named colour — which is what lets a `||` whose terms are order-free be partitioned by looking
 * at one component at a time. */

/* §2.4 "Tiling Images: the background-repeat property": `<repeat-style> = repeat-x | repeat-y |
   [repeat | space | round | no-repeat]{1,2}`. The two single-only keywords are their own arm and take no
   second component. */
static const char *const BG_REPEAT_SINGLE[] = { "repeat-x", "repeat-y" };
static const char *const BG_REPEAT_AXIS[] = { "repeat", "space", "round", "no-repeat" };
/* §2.5 "Affixing Images: the background-attachment property": `<attachment> = scroll | fixed | local`. */
static const char *const BG_ATTACHMENT_KEYWORDS[] = { "scroll", "fixed", "local" };
/* css-box-4 §2.3 "Box-edge Keywords": `<visual-box> = content-box | padding-box | border-box`, which is what
   §2.7 "Painting Area: the background-clip property" and §2.8 "Positioning Area: the background-origin
   property" both name. */
static const char *const BG_VISUAL_BOX_KEYWORDS[] = { "content-box", "padding-box", "border-box" };
/* §2.9 "Sizing Images: the background-size property"'s two whole-value keywords. */
static const char *const BG_SIZE_WHOLE[] = { "cover", "contain" };

static unsigned bg_match_repeat(const char *const *w, const size_t *wl, unsigned n, char **out)
{
    const char *kw;

    if (n == 0) return 0;
    kw = bg_keyword(BG_REPEAT_SINGLE, BG_N(BG_REPEAT_SINGLE), w[0], wl[0]);
    if (kw) { *out = bg_strdup(kw); return 1; }
    kw = bg_keyword(BG_REPEAT_AXIS, BG_N(BG_REPEAT_AXIS), w[0], wl[0]);
    if (!kw) return 0;
    if (n >= 2) {
        const char *second = bg_keyword(BG_REPEAT_AXIS, BG_N(BG_REPEAT_AXIS), w[1], wl[1]);

        if (second) {
            BgBuf b;

            bg_buf_init(&b);
            bg_buf_addz(&b, kw);
            bg_buf_addz(&b, " ");
            bg_buf_addz(&b, second);
            *out = b.s;
            return 2;
        }
    }
    *out = bg_strdup(kw);
    return 1;
}

static unsigned bg_match_attachment(const char *const *w, const size_t *wl, unsigned n, char **out)
{
    const char *kw;

    if (n == 0) return 0;
    kw = bg_keyword(BG_ATTACHMENT_KEYWORDS, BG_N(BG_ATTACHMENT_KEYWORDS), w[0], wl[0]);
    if (!kw) return 0;
    *out = bg_strdup(kw);
    return 1;
}

static unsigned bg_match_visual_box(const char *const *w, const size_t *wl, unsigned n, char **out)
{
    const char *kw;

    if (n == 0) return 0;
    kw = bg_keyword(BG_VISUAL_BOX_KEYWORDS, BG_N(BG_VISUAL_BOX_KEYWORDS), w[0], wl[0]);
    if (!kw) return 0;
    *out = bg_strdup(kw);
    return 1;
}

/* css-backgrounds-3 §2.6's `<bg-position>`, which is css-values-4 §8.3's `<position>` plus §2.6's own
   three-value form — the one caller in CSS that passes `true` there. */
static unsigned bg_match_position(const char *const *w, const size_t *wl, unsigned n, char **out)
{
    CssPositionValue p;
    unsigned took = css_position_match(w, wl, n, true, &p);

    if (took == 0) return 0;
    *out = css_position_serialize(&p);
    return took;
}

/* One component of `<bg-size>`'s `[ <length-percentage [0,∞]> | auto ]{1,2}` arm. css-values-4 §5.1 "Range
   Restrictions and Range Definition Notation" makes a negative LITERAL a dropped declaration while §9.1
   "Numeric Functions" says a math function "never cause[s] a declaration to become invalid" and is clamped at
   used-value time instead, so the sign is read off the literal and a `calc()` is admitted. */
static bool bg_size_component(const char *w, size_t n)
{
    char *probe;
    bool ok;

    if (bg_word_is(w, n, "auto")) return true;
    if (n > 0 && w[0] == '-' && !css_math_is_lone_function(w, n)) return false;
    probe = bg_dupn(w, n);
    ok = css_length_is_length_percentage(probe);
    free(probe);
    return ok;
}

/* §2.9's `<bg-size> = [ <length-percentage [0,∞]> | auto ]{1,2} | cover | contain`, serialized by §2.9.1 —
   "always serialize as two values, even when the second value is auto". That sentence is about the `{1,2}`
   arm and not about `cover`/`contain`, which are whole values with no second half to write. */
static unsigned bg_match_size(const char *const *w, const size_t *wl, unsigned n, char **out)
{
    const char *kw;
    BgBuf b;

    if (n == 0) return 0;
    kw = bg_keyword(BG_SIZE_WHOLE, BG_N(BG_SIZE_WHOLE), w[0], wl[0]);
    if (kw) { *out = bg_strdup(kw); return 1; }
    if (!bg_size_component(w[0], wl[0])) return 0;
    bg_buf_init(&b);
    bg_buf_add(&b, w[0], wl[0]);
    bg_buf_addz(&b, " ");
    if (n >= 2 && bg_size_component(w[1], wl[1])) {
        bg_buf_add(&b, w[1], wl[1]);
        *out = b.s;
        return 2;
    }
    bg_buf_addz(&b, "auto");
    *out = b.s;
    return 1;
}

/* §2.3 "Image Sources: the background-image property"'s `<bg-image> = <image> | none`. The value is copied
   VERBATIM for the same reason a `<color>` is one term along: this is the SPECIFIED value, and which of
   `url(a.png)` / `url("a.png")` was written is what CSSOM §6.7.2 "Serializing CSS Values" serializes back. */
static unsigned bg_match_image(const char *const *w, const size_t *wl, unsigned n, char **out)
{
    if (n == 0) return 0;
    if (bg_word_is(w[0], wl[0], "none")) { *out = bg_strdup("none"); return 1; }
    if (!css_image_is_image(w[0], wl[0])) return 0;
    *out = bg_dupn(w[0], wl[0]);
    return 1;
}

/* §2.2 "Base Color: the background-color property"'s `<color>`, through core/css/css_color.h — the agent's ONE
   `<color>` parse, the same one the colour well and the canvas go through. */
static unsigned bg_match_color(const char *const *w, const size_t *wl, unsigned n, char **out)
{
    CssColor c;

    if (n == 0) return 0;
    if (!css_color_parse(w[0], wl[0], &c)) return 0;
    *out = bg_dupn(w[0], wl[0]);
    return 1;
}

/* ---- one layer ------------------------------------------------------------------------------------------ */

typedef struct { char *v[CSS_BACKGROUND_SHORTHAND_N]; } BgLayer;

static void bg_layer_free(BgLayer *l)
{
    unsigned i;

    for (i = 0; i < CSS_BACKGROUND_SHORTHAND_N; i++) {
        free(l->v[i]);
        l->v[i] = NULL;
    }
}

/* §2.10's `<bg-layer>` (or `<final-bg-layer>`), partitioned one component value at a time.
   §2.10's OWN SENTENCE IS WHY AN UNSET SLOT STAYS NULL RATHER THAN GETTING ITS INITIAL HERE: "for each layer
   the shorthand first sets the corresponding value of each of [the seven] to that property's initial value,
   then assigns any explicit values specified for this layer". The initial is therefore a property of the
   LAYER and is filled by the caller out of BG_INITIAL; keeping the slot NULL is what lets the serialization
   direction tell "the author wrote the initial" from "the author wrote nothing", which §6.7.2's omission rule
   does not need to distinguish but §2.10's non-final-layer colour check does.
   THE POSITION IS TRIED FIRST AT EVERY COMPONENT, because it is the only multi-component term and css-values-4
   §8.3.1 "Parsing <position>" makes it greedy; every other term is one or two keywords whose sets are
   disjoint from each other and from a position's, so their order among themselves cannot matter. */
static bool bg_layer_parse(const char *text, size_t len, bool final, BgLayer *out)
{
    const char *w[BG_MAX_WORDS];
    size_t wl[BG_MAX_WORDS];
    int n = bg_words(text, len, w, wl, BG_MAX_WORDS);
    unsigned i = 0, rest;
    char *v = NULL;
    unsigned took;

    memset(out, 0, sizeof *out);
    if (n <= 0) return false;
    while (i < (unsigned)n) {
        rest = (unsigned)n - i;
        if (!out->v[BG_POSITION] && (took = bg_match_position(w + i, wl + i, rest, &v)) != 0) {
            out->v[BG_POSITION] = v;
            i += took;
            /* `[ / <bg-size> ]?` — the ONE infix in the grammar, and it is anchored to the position: a `/`
               anywhere else matches no term and drops the declaration, which is what the fall-through below
               does with it. */
            if (i < (unsigned)n && wl[i] == 1 && w[i][0] == '/') {
                i++;
                took = bg_match_size(w + i, wl + i, (unsigned)n - i, &v);
                if (took == 0) { bg_layer_free(out); return false; }
                out->v[BG_SIZE] = v;
                i += took;
            }
            continue;
        }
        if (!out->v[BG_REPEAT] && (took = bg_match_repeat(w + i, wl + i, rest, &v)) != 0) {
            out->v[BG_REPEAT] = v;
            i += took;
            continue;
        }
        if (!out->v[BG_ATTACHMENT] && (took = bg_match_attachment(w + i, wl + i, rest, &v)) != 0) {
            out->v[BG_ATTACHMENT] = v;
            i += took;
            continue;
        }
        if (!out->v[BG_CLIP] && (took = bg_match_visual_box(w + i, wl + i, rest, &v)) != 0) {
            /* §2.10: "if two values are present, then the first sets background-origin and the second
               background-clip" — so the FIRST fills the origin slot and the second the clip slot, and the
               one-value case is resolved after the whole layer is read. */
            out->v[out->v[BG_ORIGIN] ? BG_CLIP : BG_ORIGIN] = v;
            i += took;
            continue;
        }
        /* THE IMAGE IS ASKED BEFORE THE COLOUR, which is §2.10's own term order and is also the ordering that
           cannot go wrong: `<bg-image>`'s `none` keyword and its functions are decidable without consulting
           `<color>`, while `<color>`'s grammar is the widest in the layer — a colour parse asked first would
           be the one place a keyword belonging to another term could be silently absorbed. */
        if (!out->v[BG_IMAGE] && (took = bg_match_image(w + i, wl + i, rest, &v)) != 0) {
            out->v[BG_IMAGE] = v;
            i += took;
            continue;
        }
        /* §2.10's Note: "a color is permitted in <final-bg-layer>, but not in <bg-layer>". A colour in an
           earlier layer therefore matches no term at all and drops the declaration, which is the spec's own
           answer and not an extra rule. */
        if (final && !out->v[BG_COLOR] && (took = bg_match_color(w + i, wl + i, rest, &v)) != 0) {
            out->v[BG_COLOR] = v;
            i += took;
            continue;
        }
        bg_layer_free(out);
        return false;
    }
    /* §2.10: "if one <visual-box> value is present then it sets both background-origin and background-clip to
       that value." */
    if (out->v[BG_ORIGIN] && !out->v[BG_CLIP]) out->v[BG_CLIP] = bg_strdup(out->v[BG_ORIGIN]);
    return true;
}

static int bg_longhand_index(const char *longhand)
{
    unsigned i;

    if (!longhand) return -1;
    for (i = 0; i < CSS_BACKGROUND_SHORTHAND_N; i++)
        if (strcmp(CSS_BACKGROUND_SHORTHAND_LONGHANDS[i], longhand) == 0) return (int)i;
    return -1;
}

/* ---- the forward direction ------------------------------------------------------------------------------ */

char *css_background_shorthand_component(const char *value, const char *longhand)
{
    int idx = bg_longhand_index(longhand);
    size_t len, pos;
    const char *g;
    size_t gl;
    unsigned total = 0, k = 0;
    BgBuf b;
    char *color = NULL;

    DCHECK(value != NULL, "a `background` declaration was expanded with no value");
    if (idx < 0) return NULL;
    /* CSS Cascade §Shorthand Properties: "if a shorthand is specified as one of the CSS-wide keywords, it sets
       all of its sub-properties to that keyword" — the ENTIRE value, ahead of §2.10's own grammar, in which
       `inherit` is no term at all. §7's DEFAULTING step is what resolves it. */
    if (css_wide_keyword(value)) return bg_strdup(value);
    len = strlen(value);
    for (pos = 0; bg_next_group(value, len, &pos, &g, &gl);) total++;
    if (total == 0) return NULL;
    bg_buf_init(&b);
    for (pos = 0; bg_next_group(value, len, &pos, &g, &gl);) {
        BgLayer layer;

        k++;
        if (!bg_layer_parse(g, gl, k == total, &layer)) {
            free(b.s);
            free(color);
            return NULL;
        }
        if (idx == BG_COLOR) {
            /* §2.10: "finally background-color is set to the specified color, if any, else set to its initial
               value" — ONE value for the whole declaration rather than one per layer, which is why this slot
               is not part of the comma-joined list below. */
            if (layer.v[BG_COLOR]) {
                free(color);
                color = bg_strdup(layer.v[BG_COLOR]);
            }
        } else {
            if (k > 1) bg_buf_addz(&b, ", ");
            bg_buf_addz(&b, layer.v[idx] ? layer.v[idx] : BG_INITIAL[idx]);
        }
        bg_layer_free(&layer);
    }
    if (idx == BG_COLOR) {
        free(b.s);
        return color ? color : bg_strdup(BG_INITIAL[BG_COLOR]);
    }
    return b.s;
}

bool css_background_shorthand_validates_longhand(const char *longhand)
{
    int idx = bg_longhand_index(longhand);

    /* `background-color` is the one §2.10 names that lexbor's property registry DOES carry, so its parser has
       already validated and canonically serialized the declaration and a second `<color>` grammar here would
       be one question with two answers. */
    return idx >= 0 && idx != BG_COLOR;
}

char *css_background_shorthand_longhand_value(const char *longhand, const char *value)
{
    int idx = bg_longhand_index(longhand);
    size_t len, pos;
    const char *g;
    size_t gl;
    unsigned k = 0;
    BgBuf b;

    DCHECK(value != NULL, "a `background` longhand declaration was validated with no value");
    DCHECK(css_background_shorthand_validates_longhand(longhand),
           "a longhand this component does not own the grammar of was routed through it — the predicate and "
           "this function are one list and have come apart, and the failure is silent in the direction that "
           "matters: `background-color`, which lexbor DOES type, would have its own parser's answer replaced");
    if (css_wide_keyword(value)) return bg_strdup(value);
    len = strlen(value);
    bg_buf_init(&b);
    /* Each of the seven is a `<X>#` — a comma-separated list of the SAME term the layer partition matches, so
       one grammar answers both spellings and the `background-repeat: repeat-x, no-repeat` a page writes
       directly is validated by the sentence that validates the one inside a `background`. */
    for (pos = 0; bg_next_group(value, len, &pos, &g, &gl);) {
        const char *w[BG_MAX_WORDS];
        size_t wl[BG_MAX_WORDS];
        int n = bg_words(g, gl, w, wl, BG_MAX_WORDS);
        char *v = NULL;
        unsigned took = 0;

        if (n <= 0) { free(b.s); return NULL; }
        switch (idx) {
        case BG_IMAGE:      took = bg_match_image(w, wl, (unsigned)n, &v); break;
        case BG_POSITION:   took = bg_match_position(w, wl, (unsigned)n, &v); break;
        case BG_SIZE:       took = bg_match_size(w, wl, (unsigned)n, &v); break;
        case BG_REPEAT:     took = bg_match_repeat(w, wl, (unsigned)n, &v); break;
        case BG_ATTACHMENT: took = bg_match_attachment(w, wl, (unsigned)n, &v); break;
        default:
            DCHECK(idx == BG_ORIGIN || idx == BG_CLIP,
                   "a `background` longhand index reached the longhand grammar with no arm — the switch and "
                   "the slot constants are one list and have come apart");
            took = bg_match_visual_box(w, wl, (unsigned)n, &v);
            break;
        }
        /* THE TERM MUST CONSUME THE WHOLE GROUP. A longhand's list item is exactly one value of its type, so a
           trailing component is not a second item — it is an invalid declaration, and `background-repeat:
           repeat scroll` has to be dropped rather than read as `repeat`. */
        if (took == 0 || took != (unsigned)n) {
            free(v);
            free(b.s);
            return NULL;
        }
        if (k > 0) bg_buf_addz(&b, ", ");
        bg_buf_addz(&b, v);
        free(v);
        k++;
    }
    if (k == 0) { free(b.s); return NULL; }
    return b.s;
}

/* ---- CSSOM §6.7.2's REVERSE DIRECTION -------------------------------------------------------------------- */

/* The `k`-th comma group of a longhand's list, NUL-terminated and trimmed, or NULL when the list is shorter.
   OWNED. */
static char *bg_group_at(const char *text, unsigned k, unsigned *count)
{
    size_t len = strlen(text), pos;
    const char *g;
    size_t gl;
    unsigned i = 0;
    char *out = NULL;

    for (pos = 0; bg_next_group(text, len, &pos, &g, &gl);) {
        if (i == k) {
            while (gl > 0 && isspace((unsigned char)g[0])) { g++; gl--; }
            while (gl > 0 && isspace((unsigned char)g[gl - 1])) gl--;
            out = bg_dupn(g, gl);
        }
        i++;
    }
    if (count) *count = i;
    return out;
}

char *css_background_shorthand_value(const char *const *values)
{
    unsigned layers = 0, i, k;
    BgBuf b;

    DCHECK(values != NULL, "a `background` serialization was asked with no value list");
    for (i = 0; i < CSS_BACKGROUND_SHORTHAND_N; i++)
        DCHECK(values[i] != NULL,
               "a `background` serialization was asked with a longhand the block does not declare. §6.6's own "
               "loop establishes that every longhand of a shorthand is present BEFORE it asks for the "
               "shorthand's value, so a NULL here is a caller that skipped that step and the answer would be "
               "a layer built out of one property's list and another's absence");
    for (i = 0; i < CSS_BACKGROUND_SHORTHAND_N; i++)
        DCHECK(!css_wide_keyword(values[i]),
               "a `background` serialization was handed a longhand carrying a CSS-wide keyword. A keyword is a "
               "value of the WHOLE shorthand, so whether the list can be written as one is a question about "
               "EVERY shorthand and is answered once, in css_shorthand.c's own step, before this kind is "
               "dispatched to — a second answer here would be a second rule for the mixture");
    /* §2.10 states ONE layer count for the whole declaration, so a block whose seven list-valued longhands
       disagree about it is one no `background` declaration could have produced — §6.7.2's "the shorthand
       cannot exactly represent the values of all the properties in list". */
    for (i = 0; i < BG_COLOR; i++) {
        unsigned count = 0;
        char *first = bg_group_at(values[i], 0, &count);

        free(first);
        if (i == 0) layers = count;
        else if (count != layers) return NULL;
    }
    if (layers == 0) return NULL;
    bg_buf_init(&b);
    for (k = 0; k < layers; k++) {
        char *t[CSS_BACKGROUND_SHORTHAND_N];
        bool wrote = false, need_size, boxes_initial;

        for (i = 0; i < BG_COLOR; i++) t[i] = bg_group_at(values[i], k, NULL);
        /* The colour rides the FINAL layer, which is the half of §2.10 that is not a list. */
        t[BG_COLOR] = bg_strdup(k + 1 == layers ? values[BG_COLOR] : BG_INITIAL[BG_COLOR]);
        for (i = 0; i < CSS_BACKGROUND_SHORTHAND_N; i++)
            CHECK(t[i] != NULL, "cssom: a `background` layer lost a longhand value the layer count said was "
                                "there — the two readings of one list disagree");
        if (k > 0) bg_buf_addz(&b, ", ");
        /* §6.7.2's two syntactic rules, in order: the `||`'s terms in the property's own canonical order, and
           every term that can be omitted without changing the meaning omitted — which for a `||` over
           properties with initial values is exactly the terms holding theirs. */
        if (strcmp(t[BG_IMAGE], BG_INITIAL[BG_IMAGE]) != 0) {
            bg_buf_addz(&b, t[BG_IMAGE]);
            wrote = true;
        }
        /* A NON-INITIAL SIZE FORCES ITS POSITION TO BE WRITTEN even when the position is initial, because
           `<bg-size>` has no spelling of its own in the shorthand: §2.10 puts it behind `<bg-position> [ /
           <bg-size> ]?`, so `background-size: cover` with an initial position can only be written
           `0% 0% / cover`. Omitting the position there would produce a `/ cover` that matches no arm. */
        need_size = strcmp(t[BG_SIZE], BG_INITIAL[BG_SIZE]) != 0;
        if (need_size || strcmp(t[BG_POSITION], BG_INITIAL[BG_POSITION]) != 0) {
            if (wrote) bg_buf_addz(&b, " ");
            bg_buf_addz(&b, t[BG_POSITION]);
            wrote = true;
            if (need_size) {
                bg_buf_addz(&b, " / ");
                bg_buf_addz(&b, t[BG_SIZE]);
            }
        }
        if (strcmp(t[BG_REPEAT], BG_INITIAL[BG_REPEAT]) != 0) {
            if (wrote) bg_buf_addz(&b, " ");
            bg_buf_addz(&b, t[BG_REPEAT]);
            wrote = true;
        }
        if (strcmp(t[BG_ATTACHMENT], BG_INITIAL[BG_ATTACHMENT]) != 0) {
            if (wrote) bg_buf_addz(&b, " ");
            bg_buf_addz(&b, t[BG_ATTACHMENT]);
            wrote = true;
        }
        boxes_initial = strcmp(t[BG_ORIGIN], BG_INITIAL[BG_ORIGIN]) == 0 &&
                        strcmp(t[BG_CLIP], BG_INITIAL[BG_CLIP]) == 0;
        if (!boxes_initial) {
            if (wrote) bg_buf_addz(&b, " ");
            bg_buf_addz(&b, t[BG_ORIGIN]);
            if (strcmp(t[BG_ORIGIN], t[BG_CLIP]) != 0) {
                bg_buf_addz(&b, " ");
                bg_buf_addz(&b, t[BG_CLIP]);
            }
            wrote = true;
        }
        if (k + 1 == layers && strcmp(t[BG_COLOR], BG_INITIAL[BG_COLOR]) != 0) {
            if (wrote) bg_buf_addz(&b, " ");
            bg_buf_addz(&b, t[BG_COLOR]);
            wrote = true;
        }
        /* `<bg-layer>` is a `||`, and a `||` requires at least one of its terms — so an all-initial layer has
           no empty spelling and is written as the `background-image` initial, which is the term that expands
           back to exactly this layer. */
        if (!wrote) bg_buf_addz(&b, BG_INITIAL[BG_IMAGE]);
        for (i = 0; i < CSS_BACKGROUND_SHORTHAND_N; i++) free(t[i]);
    }
    return b.s;
}
