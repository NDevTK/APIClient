/* CSS Images 3 §2 "Image Values: the <image> type"'s `<url> | <gradient>`, with `<gradient>` read at
   css-images-4 §3 "Gradients" — see css_image.h for why it is a component of its own, and for why the two
   levels sit side by side in it. */
#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/css/css_color.h"
#include "core/css/css_dimension.h"
#include "core/css/css_image.h"
#include "core/css/css_length.h"
#include "core/css/css_math.h"
#include "core/css/css_position_value.h"

#define IMG_N(a) (sizeof(a) / sizeof((a)[0]))

/* THE LONGEST PREFIX GROUP ANY ARM OF §3 ADMITS, which is css-images-4 §3.2.1 "Adding
   <color-interpolation-method>"'s radial prefix at its widest: `<radial-shape>` (1) plus `<radial-size>`'s
   `<length-percentage [0,∞]>{2}` (2) plus `at` (1) plus css-values-4 §8.3's four-component `<position>` (4)
   plus `<color-interpolation-method>`'s own widest form, `in oklch longer hue` (4). A group longer than that
   matches NO production of css-images-4 §3 "Gradients", so refusing it is the grammar's own answer and not a cap on anything.
   IT WAS 8 AND THAT WAS LEVEL 3'S ANSWER. The bound is DERIVED rather than chosen, so every term §3 adds
   moves it: level 4 hangs a `<color-interpolation-method>` off all three families, and the four components
   that production can reach are the whole of the difference. A bound left at the old figure would not have
   reported anything — `img_words` answers -1 for a longer group, every caller reads that as "no production is
   this long", and `radial-gradient(circle 10px 20px at left 10px top 20px in oklch longer hue, red, blue)`
   would have been refused as if its grammar said so. */
#define IMG_MAX_WORDS 12

/* A CSS keyword comparison over a span. CSS Syntax §4 makes an ident and a function name ASCII
   case-insensitive, and a declaration lexbor's registry does not type arrives as the author's own bytes. */
static bool img_word_is(const char *w, size_t n, const char *kw)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (kw[i] == '\0' || (char)tolower((unsigned char)w[i]) != kw[i]) return false;
    return kw[n] == '\0';
}

static bool img_keyword(const char *const *set, unsigned n, const char *w, size_t len)
{
    unsigned i;

    for (i = 0; i < n; i++)
        if (img_word_is(w, len, set[i])) return true;
    return false;
}

static void img_trim(const char **s, size_t *n)
{
    while (*n > 0 && isspace((unsigned char)(*s)[0])) { (*s)++; (*n)--; }
    while (*n > 0 && isspace((unsigned char)(*s)[*n - 1])) (*n)--;
}

/* THE NEXT TOP-LEVEL COMMA GROUP of a function's arguments, as a CURSOR rather than an array — a
   `<color-stop-list>` has no length limit in its grammar, so materialising every group would need a bound and
   a bound here would refuse a long gradient, which is a valid declaration this component would then drop.
   DEPTH AND QUOTES ARE BOTH COUNTED because a comma inside either is not a separator: `rgb(1, 2, 3)` is ONE
   `<linear-color-stop>` (CSS Syntax §4's function token) and `url("a,b")` is one `<url>`. */
static bool img_next_group(const char *text, size_t len, size_t *pos, const char **out, size_t *out_len)
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
    /* Past the end MARKS EXHAUSTED, which is what makes a trailing comma produce a final EMPTY group rather
       than disappearing: `linear-gradient(red,)` is an invalid stop list and must be told from `(red)`. */
    *pos = i + 1;
    return true;
}

/* The COMPONENT VALUES of one group. -1 when there are more than `max`, which every caller reads as "no
   production of css-images-4 §3 "Gradients" is this long" rather than as a truncation. */
static int img_words(const char *v, size_t n, const char **w, size_t *wl, int max)
{
    int cnt = 0;
    size_t i = 0;

    while (i < n) {
        size_t s;
        int depth = 0;
        char quote = 0;

        while (i < n && isspace((unsigned char)v[i])) i++;
        if (i >= n) break;
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
            } else if (depth == 0 && isspace((unsigned char)c)) {
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

/* CSS Syntax §4's FUNCTION TOKEN, split into its name and its arguments. False for a component value that is
   not one, which for §2's purposes is the whole answer: `<image>` has no keyword arm and no bare-ident arm. */
static bool img_function(const char *text, size_t len, const char **name, size_t *name_len,
                         const char **args, size_t *args_len)
{
    size_t i;

    if (len < 3 || text[len - 1] != ')') return false;
    for (i = 0; i < len; i++)
        if (text[i] == '(') break;
    if (i == 0 || i > len - 2) return false;
    *name = text;
    *name_len = i;
    *args = text + i + 1;
    *args_len = len - i - 2;
    return true;
}

/* css-values-4 §5.6 "Mixing Percentages and Dimensions"'s `<length-percentage>` and §6's `<length>` over a
   SPAN. core/css/css_length.h publishes both over a NUL-terminated value because every other caller has one. */
static bool img_length_predicate(const char *w, size_t n, bool percentage_too)
{
    char *probe = malloc(n + 1);
    bool ok;

    CHECK(probe != NULL, "cssom: OOM testing a gradient component — a dropped one would make a valid "
                         "declaration invalid, which reads as no background at all");
    memcpy(probe, w, n);
    probe[n] = '\0';
    ok = percentage_too ? css_length_is_length_percentage(probe) : css_length_is_length(probe);
    free(probe);
    return ok;
}

/* The `[0,∞]` RANGE that §3.2.1 writes on both of `<radial-size>`'s numeric arms. css-values-4 §5.1 "Range
   Restrictions and Range Definition Notation" makes a negative LITERAL outside the range a dropped
   declaration, while §9.1 "Numeric Functions" says a math function "never cause[s] a declaration to become
   invalid" and is clamped later instead — so the sign is read off the literal and a `calc()` is admitted. The
   bracket is the standard's plural subject adapted to this sentence's singular one: §9.1 writes "numeric
   functions returning out-of-range values never cause a declaration to become invalid". */
static bool img_nonneg(const char *w, size_t n, bool percentage_too)
{
    if (n > 0 && w[0] == '-' && !css_math_is_lone_function(w, n)) return false;
    return img_length_predicate(w, n, percentage_too);
}

/* CSS Syntax 3 §4.3.3 "Consume a numeric token"'s NUMBER, as the OFFSET ITS UNIT WOULD START AT. Zero means
   the component value does not begin with a number at all, which is unambiguous because a number carries at
   least one digit and so can never end at zero. It is read HERE rather than taken from a dimension parser
   because what every caller below needs is exactly that boundary: which unit follows is a different question
   per production, and `css_length.h`/`css_dimension.h` each answer it for their own unit table. */
static size_t img_number_end(const char *w, size_t n)
{
    size_t i = 0;
    bool digits = false;

    if (i < n && (w[i] == '+' || w[i] == '-')) i++;
    while (i < n && isdigit((unsigned char)w[i])) { i++; digits = true; }
    if (i < n && w[i] == '.') {
        i++;
        while (i < n && isdigit((unsigned char)w[i])) { i++; digits = true; }
    }
    if (!digits) return 0;
    if (i < n && (w[i] == 'e' || w[i] == 'E')) {
        size_t j = i + 1;
        bool exp = false;

        if (j < n && (w[j] == '+' || w[j] == '-')) j++;
        while (j < n && isdigit((unsigned char)w[j])) { j++; exp = true; }
        if (exp) i = j;
    }
    return i;
}

/* css-values-4 §7.1 "Angle Units: the <angle> type and deg, grad, rad, turn units" over a span: a dimension
   token whose unit is one of §7.1's four, or a math function whose §10.9 type is `<angle>`. */
static bool img_is_angle(const char *w, size_t n)
{
    size_t u;

    if (css_math_is_lone_function(w, n)) return css_math_matches(w, n, CSS_MATH_PROD_ANGLE);
    u = img_number_end(w, n);
    return u > 0 && u < n && css_angle_unit(w + u, n - u);
}

/* css-values-4 §5.6 "Mixing Percentages and Dimensions"'s `<angle-percentage>` — "Equivalent to
   [ <angle> | <percentage> ]" — which is what css-images-4 §3.5.1's angular positions are written over.
   THE MATH ARM ASKS ONE PRODUCTION AND NOT TWO, and the reason is the CALCULATION CONTEXT rather than a value
   that falls between the disjuncts. `calc(25% + 10deg)` matches the FIRST one: CSS Typed OM 1 §4.3.2 makes
   matching context-relative — "If the context in which the value is used allows <percentage> values, and
   those percentages are resolved against another type, then for the type to be considered matching it must
   either have a null percent hint, or the percent hint must match the other type" — and here it does. What a
   caller asking `<angle>` and then `<percentage>` would get is not a near miss but a FAILURE type: css-values-4
   §10.9's `<percentage>` terminal reads §10.9.1's context to type the `25%` at all, an `<angle>`-only
   context types it "percent", and percent+angle has no consistent hint. So the production IS the context, and
   asking the halves separately asks about a world this position is not in — dropping a declaration
   css-images-4 admits. The literal arm below needs no such care: a `<percentage>` TOKEN is one either way. */
static bool img_is_angle_percentage(const char *w, size_t n)
{
    size_t u;

    if (css_math_is_lone_function(w, n)) return css_math_matches(w, n, CSS_MATH_PROD_ANGLE_PERCENTAGE);
    u = img_number_end(w, n);
    if (u == 0 || u >= n) return false;
    if (n - u == 1 && w[u] == '%') return true;
    return css_angle_unit(w + u, n - u);
}

/* The `<zero>` arm that css-images-4 §3.1 "Linear Gradients: the linear-gradient() notation" and its §3.3.1
   "conic-gradient() Syntax" each write beside `<angle>`, and whose sentence §3.3.1 states: "The unit
   identifier may be omitted if the <angle> is zero." It is a SEPARATE production from `<angle>` in the
   grammar's own text and not a widening of the unit set, which is css-values-4 §7.1's own note: "for legacy reasons, some uses of <angle> allow a bare 0 to mean 0deg. This is not true in
   general". */
static bool img_is_zero(const char *w, size_t n)
{
    size_t i = 0;
    bool digit = false;

    if (n == 0) return false;
    if (w[0] == '+' || w[0] == '-') i = 1;
    for (; i < n; i++) {
        if (w[i] == '0') digit = true;
        else if (w[i] != '.') return false;
    }
    return digit;
}

/* WHICH TYPE A STOP LIST'S POSITIONS ARE, as a parameter rather than as a second copy of the list. This is
   css-images-4 §3.5.1's own structural claim made structural here — its note reads "are exactly identical in
   structure, they just differ on whether they accept" lengths or angles "for specifying the position of the
   stops and hints" — so the list, the stop and the hint are ONE implementation each and the day a
   double-position stop is edited there is no second list to disagree with it. */
typedef bool (*ImgPositionFn)(const char *w, size_t n);

/* `<color-stop-list>`'s positions: css-values-4 §5.6's `<length-percentage>`. */
static bool img_pos_length(const char *w, size_t n)
{
    return img_length_predicate(w, n, true);
}

/* `<angular-color-stop-list>`'s positions. BOTH of css-images-4 §3.5.1's angular productions carry the SAME pair of arms —
   `<angular-color-hint> = <angle-percentage> | <zero>` and `<color-stop-angle> = [ <angle-percentage> |
   <zero> ]{1,2}` — so the `<zero>` arm belongs to the position type rather than to either one of them. It is
   not decoration: `conic-gradient(red 0, blue)` writes a bare zero, which is no `<angle>` at all. */
static bool img_pos_angle(const char *w, size_t n)
{
    return img_is_angle_percentage(w, n) || img_is_zero(w, n);
}

/* css-images-4 §3.1 "Linear Gradients: the linear-gradient() notation"'s
   `<side-or-corner> = [left | right] || [top | bottom]`, over the components AFTER the `to`. */
static bool img_side_or_corner(const char *const *w, const size_t *wl, int n)
{
    static const char *const H[] = { "left", "right" };
    static const char *const V[] = { "top", "bottom" };
    bool h = false, v = false;
    int i;

    if (n < 1 || n > 2) return false;
    for (i = 0; i < n; i++) {
        if (!h && img_keyword(H, IMG_N(H), w[i], wl[i])) { h = true; continue; }
        if (!v && img_keyword(V, IMG_N(V), w[i], wl[i])) { v = true; continue; }
        return false;
    }
    return true;
}

/* css-images-4 §3.5.1 "Color Stop Lists":
     <linear-color-stop>  = <color> <color-stop-length>?
     <color-stop-length>  = <length-percentage>{1,2}
     <angular-color-stop> = <color> <color-stop-angle>?
     <color-stop-angle>   = [ <angle-percentage> | <zero> ]{1,2}
   ONE FUNCTION FOR BOTH, because the two differ in nothing but `pos` — see ImgPositionFn.
   THE `{1,2}` IS THIS FUNCTION'S DELTA FROM LEVEL 3, whose §3.4.1 "Color Stop Lists" writes
   `<linear-color-stop> = <color> <length-percentage>?` and admits ONE position. A second position is the
   DOUBLE-POSITION stop that writes a hard colour band as one stop instead of two, and css-images-4 §3.5.1 states its
   meaning rather than leaving it to a renderer: "A color stop with two positions is equivalent to specifying
   two color stops with the same color, one for each position."
   NEITHER POSITION IS RANGE-CHECKED AND THE PAIR IS NOT ORDERED, which is the grammar's own answer and not a
   laxity here. css-images-4 §3.5.1 writes its positions with no `[0,∞]` bracket and says in its own words that they "can be
   specified anywhere on the gradient line"; a second position BELOW the first is resolved at used-value time
   by css-images-4 §3.5.3 "Color Stop Fixup" and is never a parse error. */
static bool img_color_stop(const char *g, size_t glen, ImgPositionFn pos)
{
    const char *w[3];
    size_t wl[3];
    int n = img_words(g, glen, w, wl, 3), i;
    CssColor c;

    /* -1 IS "MORE THAN THREE", which `n < 1` takes: `<color>` is ONE component value however many commas its
       function token carries, so four components match no reading of `<color> <color-stop-length>?`. */
    if (n < 1) return false;
    if (!css_color_parse(w[0], wl[0], &c)) return false;
    for (i = 1; i < n; i++)
        if (!pos(w[i], wl[i])) return false;
    return true;
}

/* css-images-4 §3.5.1's `<linear-color-hint>` / `<angular-color-hint>` — the transition hint BETWEEN two stops, which is
   ONE position in either family. */
static bool img_color_hint(const char *g, size_t glen, ImgPositionFn pos)
{
    const char *w[1];
    size_t wl[1];

    if (img_words(g, glen, w, wl, 1) != 1) return false;
    return pos(w[0], wl[0]);
}

/* css-images-4 §3.5.1's `<color-stop-list> = <linear-color-stop> , [ <linear-color-hint>? , <linear-color-stop> ]#?` and
   its `<angular-color-stop-list>`, which the section writes with the same shape over the angular pair. Read as
   its own text reads: a hint never starts the list and never ends it, and two hints never abut, because every
   hint in the grammar sits INSIDE a group that is followed by a stop. TWO stops are the minimum for the same
   reason — the production's literal comma after the first stop has to be followed by something, and a
   `linear-gradient(red)` is the value every user agent rejects. */
static bool img_color_stop_list(const char *text, size_t len, size_t pos, ImgPositionFn posfn)
{
    unsigned stops = 0;
    bool hint = false;
    const char *g;
    size_t gl;

    while (img_next_group(text, len, &pos, &g, &gl)) {
        if (img_color_stop(g, gl, posfn)) {
            stops++;
            hint = false;
            continue;
        }
        if (!hint && stops >= 1 && img_color_hint(g, gl, posfn)) {
            hint = true;
            continue;
        }
        return false;
    }
    return stops >= 2 && !hint;
}

/* ---- css-images-4 §3 "Gradients"'s THREE FAMILIES, WHICH ARE ONE SHAPE -----------------------------------------------------
 *
 * css-images-4 writes the three separately and they differ in exactly two places:
 *   <linear-gradient-syntax> =
 *     [ [ <angle> | <zero> | to <side-or-corner> ] || <color-interpolation-method> ]? , <color-stop-list>
 *   <radial-gradient-syntax> =
 *     [ [ [ <radial-shape> || <radial-size> ]? [ at <position> ]? ] || <color-interpolation-method> ]? ,
 *     <color-stop-list>
 *   <conic-gradient-syntax> =
 *     [ [ [ from [ <angle> | <zero> ] ]? [ at <position> ]? ] || <color-interpolation-method> ]? ,
 *     <angular-color-stop-list>
 * WHICH PREFIX, and WHICH POSITION TYPE the stop list is written over. So the `||`, the optionality of the
 * whole prefix group and the stop list are read ONCE here and each family supplies those two — a copy per
 * family is what disagrees about `in oklch` the day one of them is edited, and css-images-4 §3.2.1's own title is
 * "Adding <color-interpolation-method>", which is that edit having already happened once upstream.
 *
 * A PREFIX ANSWERS A COUNT AND NOT A BOOLEAN, and -1 is a third answer that neither of those carries. Zero is
 * `this term is ABSENT`, which the `||` must be free to accept because two of the three prefixes are written
 * entirely out of optional pieces. -1 is `this term is PRESENT AND MALFORMED` — a `to` with no
 * `<side-or-corner>`, an `at` with no `<position>` — and it must not be read as absence, or the caller retries
 * the group as a colour stop and the refusal arrives from the wrong production. */
typedef int (*ImgPrefixFn)(const char *const *w, const size_t *wl, int n);

/* css-images-4 §3.1 "Linear Gradients: the linear-gradient() notation"'s
   `[ <angle> | <zero> | to <side-or-corner> ]`. `to` STARTS this production, so a `to` whose remainder
   is not a `<side-or-corner>` is -1 rather than 0.
   THE TWO-COMPONENT `<side-or-corner>` IS TRIED FIRST, which is `||` read greedily: `to left top` is one
   corner and not a side followed by a component no arm admits. Trying it the other way round would leave
   `top` for the `<color-interpolation-method>` arm, which does not begin with it, and the whole gradient
   would be refused. */
static int img_linear_direction(const char *const *w, const size_t *wl, int n)
{
    if (n >= 1 && (img_is_angle(w[0], wl[0]) || img_is_zero(w[0], wl[0]))) return 1;
    if (n >= 1 && img_word_is(w[0], wl[0], "to")) {
        if (n >= 3 && img_side_or_corner(w + 1, wl + 1, 2)) return 3;
        if (n >= 2 && img_side_or_corner(w + 1, wl + 1, 1)) return 2;
        return -1;
    }
    return 0;
}

/* css-images-4 §3.2.1 "Adding <color-interpolation-method>"'s
   `[ [ <radial-shape> || <radial-size> ]? [ at <position> ]? ]`:
     <radial-size> = <radial-extent> | <length [0,∞]> | <length-percentage [0,∞]>{2}
     <radial-extent> = closest-corner | closest-side | farthest-corner | farthest-side
     <radial-shape> = circle | ellipse
   The two-value `<radial-size>` is tried BEFORE the one-value one, which is the `{2}` multiplier read
   greedily: `10px 20px` is one size and not a size followed by an unmatched component. */
static int img_radial_shape_size_at(const char *const *w, const size_t *wl, int n)
{
    static const char *const SHAPE[] = { "circle", "ellipse" };
    static const char *const EXTENT[] = {
        "closest-corner", "closest-side", "farthest-corner", "farthest-side"
    };
    bool shape = false, size = false;
    int i = 0;

    while (i < n) {
        if (!shape && img_keyword(SHAPE, IMG_N(SHAPE), w[i], wl[i])) { shape = true; i++; continue; }
        if (!size) {
            if (img_keyword(EXTENT, IMG_N(EXTENT), w[i], wl[i])) { size = true; i++; continue; }
            if (i + 1 < n && img_nonneg(w[i], wl[i], true) && img_nonneg(w[i + 1], wl[i + 1], true)) {
                size = true;
                i += 2;
                continue;
            }
            if (img_nonneg(w[i], wl[i], false)) { size = true; i++; continue; }
        }
        break;
    }
    if (i < n && img_word_is(w[i], wl[i], "at")) {
        CssPositionValue p;
        unsigned took;

        i++;
        /* THREE-VALUE IS REFUSED HERE, and css-values-4 §8.3's own Note is the reason: the three-value form
           "creates parsing ambiguities when combined with other length or percentage components in a property
           value", so only css-backgrounds-3 §2.6's `<bg-position>` admits it. `at` is followed by
           `<position>`, the type, and by nothing wider. */
        took = css_position_match(w + i, wl + i, (unsigned)(n - i), false, &p);
        if (took == 0) return -1;
        i += (int)took;
    }
    return i;
}

/* css-images-4 §3.3.1 "conic-gradient() Syntax"'s `[ [ from [ <angle> | <zero> ] ]? [ at <position> ]? ]`. The two are a
   SEQUENCE and not a `||`, so `at 50% 50% from 45deg` matches no reading of the production — which is the one
   place this prefix differs in shape from the radial one it otherwise resembles. */
static int img_conic_from_at(const char *const *w, const size_t *wl, int n)
{
    CssPositionValue p;
    unsigned took;
    int i = 0;

    if (i < n && img_word_is(w[i], wl[i], "from")) {
        if (i + 1 >= n || !(img_is_angle(w[i + 1], wl[i + 1]) || img_is_zero(w[i + 1], wl[i + 1]))) return -1;
        i += 2;
    }
    if (i < n && img_word_is(w[i], wl[i], "at")) {
        i++;
        took = css_position_match(w + i, wl + i, (unsigned)(n - i), false, &p);
        if (took == 0) return -1;
        i += (int)took;
    }
    return i;
}

/* `[ <family prefix> || <color-interpolation-method> ]` over ONE comma group. `||` is "one or more, in any
   order", so the method is tried at BOTH ends and the group must be consumed WHOLE — `i == n` is what makes
   this an ordered-alternatives read of a `||` rather than a scan that tolerates a leftover.
   `i == n` ALSO CARRIES THE "ONE OR MORE": a group of `n >= 1` components that is entirely consumed cannot
   have taken zero terms, so no separate emptiness test is owed. An EMPTY group is refused by `n <= 0`, which
   is `linear-gradient(, red, blue)` — a leading comma with no prefix in front of it. */
static bool img_gradient_prefix(const char *g, size_t gl, ImgPrefixFn prefix)
{
    const char *w[IMG_MAX_WORDS];
    size_t wl[IMG_MAX_WORDS];
    int n = img_words(g, gl, w, wl, IMG_MAX_WORDS), i, took;
    unsigned method;

    if (n <= 0) return false;
    method = css_color_interpolation_method_match(w, wl, (unsigned)n);
    i = (int)method;
    took = prefix(w + i, wl + i, n - i);
    if (took < 0) return false;
    i += took;
    if (method == 0 && i < n)
        i += (int)css_color_interpolation_method_match(w + i, wl + i, (unsigned)(n - i));
    return i == n;
}

/* `[ <prefix> || <color-interpolation-method> ]? , <stop list>`. The prefix group is OPTIONAL and shares the
   argument list's first comma group with nothing, so the whole of the ambiguity is whether that first group is
   a prefix — and it cannot also be a stop, since no arm of any prefix is a `<color>`. */
static bool img_gradient(const char *a, size_t alen, ImgPrefixFn prefix, ImgPositionFn pos)
{
    const char *g;
    size_t gl, cursor = 0, after;

    if (!img_next_group(a, alen, &cursor, &g, &gl)) return false;
    after = cursor;
    if (img_gradient_prefix(g, gl, prefix)) return img_color_stop_list(a, alen, after, pos);
    return img_color_stop_list(a, alen, 0, pos);
}

/* css-values-4 §4.5 "Resource Locators: the <url> type":
     <url> = <url()> | <src()>
     <url()> = url( <string> <url-modifier>* ) | <url-token>
     <src()> = src( <string> <url-modifier>* )
   THE `url()` ARM IS ACCEPTED WHOLE and that is the grammar rather than a shortcut: §4.5.3 "URL Modifiers"
   says "this specification does not define any <url-modifier>s" and makes one "either an <ident> or a
   functional notation", so every balanced argument list matches; and the unquoted form is a `<url-token>`,
   which CSS Syntax 3 §4.3.6 "Consume a url token" has already decided by the time a declaration's text exists
   — a `url(` holding an unescaped paren, quote or space never arrives as one component value.
   `src()` HAS NO url-token ARM, so its first argument must be a `<string>`, or the `var()` §4.5's own example
   uses ("background: src(var(--foo))") which substitutes before this grammar is applied. */
static bool img_url(const char *name, size_t name_len, const char *args, size_t args_len)
{
    if (img_word_is(name, name_len, "url")) return true;
    if (!img_word_is(name, name_len, "src")) return false;
    img_trim(&args, &args_len);
    if (args_len == 0) return false;
    if (args[0] == '"' || args[0] == '\'') return true;
    return args[args_len - 1] == ')' && memchr(args, '(', args_len) != NULL;
}

bool css_image_is_image(const char *text, size_t len)
{
    const char *name, *args;
    size_t name_len, args_len;

    DCHECK(text != NULL, "css-images-3 §2's <image> was asked about a NULL component value");
    img_trim(&text, &len);
    if (!img_function(text, len, &name, &name_len, &args, &args_len)) return false;
    if (img_url(name, name_len, args, args_len)) return true;
    /* css-images-4 §3 "Gradients" has SIX notations, and §3.4 "Repeating Gradients: the
       repeating-linear-gradient(), repeating-radial-gradient(), and repeating-conic-gradient() notations"
       gives each repeating form the syntax of the one it repeats — "These notations take the same values and
       are interpreted the same as their respective non-repeating siblings defined previously", which it then
       writes out as `<repeating-conic-gradient()> = repeating-conic-gradient( [ <conic-gradient-syntax> ] )`
       and the two like it. So each pair shares one branch rather than carrying a copy of a grammar that
       cannot differ.
       THAT QUOTATION USED TO BE A PARAPHRASE IN QUOTATION MARKS — "the repeating-linear-gradient() and
       repeating-radial-gradient() functions take the same arguments as the linear-gradient() and
       radial-gradient() functions", which is not a sentence either level contains: `take the same arguments
       as` occurs ZERO times in css-images-3 and in css-images-4, and the eight words that did match were the
       SECTION TITLE rather than any prose. The reasoning above it was right and only its evidence was
       invented, which is the pairing that survives review longest — the argument checks out, so nobody reads
       the quotation. */
    if (img_word_is(name, name_len, "linear-gradient") ||
        img_word_is(name, name_len, "repeating-linear-gradient"))
        return img_gradient(args, args_len, img_linear_direction, img_pos_length);
    if (img_word_is(name, name_len, "radial-gradient") ||
        img_word_is(name, name_len, "repeating-radial-gradient"))
        return img_gradient(args, args_len, img_radial_shape_size_at, img_pos_length);
    if (img_word_is(name, name_len, "conic-gradient") ||
        img_word_is(name, name_len, "repeating-conic-gradient"))
        return img_gradient(args, args_len, img_conic_from_at, img_pos_angle);
    if (img_word_is(name, name_len, "image") || img_word_is(name, name_len, "image-set") ||
        img_word_is(name, name_len, "cross-fade") || img_word_is(name, name_len, "element"))
        DFAIL("a component value names one of css-images-4 §2 \"2D Image Values: the <image> type\"'s FOUR "
              "extra arms — `<image()>`, `<image-set()>`, `<cross-fade()>` or `<element()>` — which "
              "css-images-3 §2's `<image> = <url> | <gradient>` does not have. Each is a real production with "
              "its own section in css-images-4, and each is a value a page can legitimately write, so "
              "refusing it here DROPS a valid declaration rather than reporting a gap. BUILD the arm the "
              "crash names; `image-set()` is the one a real page reaches first, since it is how a bundle "
              "ships a 2x asset");
    return false;
}
