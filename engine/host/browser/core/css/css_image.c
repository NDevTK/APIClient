/* CSS Images 3 §2 "Image Values: the <image> type" — see css_image.h for why it is a component of its own. */
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

/* THE LONGEST GROUP ANY ARM OF §3 ADMITS, which is css-images-3 §3.2.1 "radial-gradient() Syntax"'s prefix at
   its widest: `<radial-shape>` (1) plus `<radial-size>`'s `<length-percentage [0,∞]>{2}` (2) plus `at` (1)
   plus css-values-4 §8.3's four-component `<position>` (4). A group longer than that matches NO production of
   §3, so refusing it is the grammar's own answer and not a cap on anything. */
#define IMG_MAX_WORDS 8

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
   production of §3 is this long" rather than as a truncation. */
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
   declaration, while §9.1 "Numeric Functions" says a math function "never causes a declaration to become
   invalid" and is clamped later instead — so the sign is read off the literal and a `calc()` is admitted. */
static bool img_nonneg(const char *w, size_t n, bool percentage_too)
{
    if (n > 0 && w[0] == '-' && !css_math_is_lone_function(w, n)) return false;
    return img_length_predicate(w, n, percentage_too);
}

/* css-values-4 §7.1 "Angle Units: the <angle> type and deg, grad, rad, turn units" over a span: a dimension
   token whose unit is one of §7.1's four, or a math function whose §10.9 type is `<angle>`. The number's own
   syntax is CSS Syntax §4's, scanned here because the answer needed is where the unit STARTS. */
static bool img_is_angle(const char *w, size_t n)
{
    size_t i = 0;
    bool digits = false;

    if (css_math_is_lone_function(w, n)) return css_math_matches(w, n, CSS_MATH_PROD_ANGLE);
    if (i < n && (w[i] == '+' || w[i] == '-')) i++;
    while (i < n && isdigit((unsigned char)w[i])) { i++; digits = true; }
    if (i < n && w[i] == '.') {
        i++;
        while (i < n && isdigit((unsigned char)w[i])) { i++; digits = true; }
    }
    if (!digits) return false;
    if (i < n && (w[i] == 'e' || w[i] == 'E')) {
        size_t j = i + 1;
        bool exp = false;

        if (j < n && (w[j] == '+' || w[j] == '-')) j++;
        while (j < n && isdigit((unsigned char)w[j])) { j++; exp = true; }
        if (exp) i = j;
    }
    return i < n && css_angle_unit(w + i, n - i);
}

/* §3.1.1's `<zero>` arm — "the unit identifier may be omitted if the <angle> is zero". It is a SEPARATE
   production from `<angle>` in the spec's own grammar and not a widening of the unit set, which is css-values-4
   §7.1's own note: "for legacy reasons, some uses of <angle> allow a bare 0 to mean 0deg. This is not true in
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

/* §3.1.1's `<side-or-corner> = [left | right] || [top | bottom]`, over the components AFTER the `to`. */
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

/* §3.4.1 "Color Stop Lists": `<linear-color-stop> = <color> <length-percentage>?`. */
static bool img_color_stop(const char *g, size_t glen)
{
    const char *w[3];
    size_t wl[3];
    int n = img_words(g, glen, w, wl, 3);
    CssColor c;

    if (n < 1) return false;
    if (!css_color_parse(w[0], wl[0], &c)) return false;
    if (n == 1) return true;
    if (n == 2) return img_length_predicate(w[1], wl[1], true);
    if (img_length_predicate(w[1], wl[1], true) && img_length_predicate(w[2], wl[2], true))
        DFAIL("a colour stop carries TWO positions, which is css-images-4 §3.5.1 \"Color Stop Lists\"' "
              "`<color-stop-length> = <length-percentage>{1,2}` — the DOUBLE-POSITION stop that writes a hard "
              "colour band as one stop instead of two (`red 0% 50%, blue 50% 100%`). css-images-3 §3.4.1 "
              "\"Color Stop Lists\", which is the level css-backgrounds-3 references and which this component "
              "implements, gives `<linear-color-stop>` ONE optional position, so this value matches no arm "
              "here and refusing it would DROP a declaration that every user agent accepts — the page's whole "
              "`background` would read as undeclared with the initial value to show for it, which is the "
              "silent shape of this defect. BUILD css-images-4 §3.5.1's grammar: the second position is a "
              "third component of the same group and nothing else in this file changes");
    return false;
}

/* §3.4.1's `<linear-color-hint> = <length-percentage>` — the transition hint BETWEEN two stops. */
static bool img_color_hint(const char *g, size_t glen)
{
    const char *w[1];
    size_t wl[1];

    if (img_words(g, glen, w, wl, 1) != 1) return false;
    return img_length_predicate(w[0], wl[0], true);
}

/* §3.4.1's `<color-stop-list> = <linear-color-stop> , [ <linear-color-hint>? , <linear-color-stop> ]#?`, read
   as its own text reads: a hint never starts the list and never ends it, and two hints never abut, because
   every hint in the grammar sits INSIDE a group that is followed by a stop. TWO stops are the minimum for the
   same reason — the production's literal comma after the first `<linear-color-stop>` has to be followed by
   something, and a `linear-gradient(red)` is the value every user agent rejects. */
static bool img_color_stop_list(const char *text, size_t len, size_t pos)
{
    unsigned stops = 0;
    bool hint = false;
    const char *g;
    size_t gl;

    while (img_next_group(text, len, &pos, &g, &gl)) {
        if (img_color_stop(g, gl)) {
            stops++;
            hint = false;
            continue;
        }
        if (!hint && stops >= 1 && img_color_hint(g, gl)) {
            hint = true;
            continue;
        }
        return false;
    }
    return stops >= 2 && !hint;
}

/* §3.1.1 "linear-gradient() syntax":
     <linear-gradient-syntax> = [ <angle> | <zero> | to <side-or-corner> ]? , <color-stop-list>
   The direction is OPTIONAL and shares the argument list's first comma group with nothing, so the whole of the
   ambiguity is whether that first group is a direction — and it cannot also be a stop, since neither an angle
   nor `to left` is a `<color>`. */
static bool img_linear(const char *a, size_t alen)
{
    const char *g, *w[3];
    size_t gl, wl[3], pos = 0, after;
    int n;

    if (!img_next_group(a, alen, &pos, &g, &gl)) return false;
    after = pos;
    n = img_words(g, gl, w, wl, 3);
    if (n > 0) {
        bool direction = false;

        if (n == 1) direction = img_is_angle(w[0], wl[0]) || img_is_zero(w[0], wl[0]);
        else if (img_word_is(w[0], wl[0], "to")) direction = img_side_or_corner(w + 1, wl + 1, n - 1);
        if (direction) return img_color_stop_list(a, alen, after);
    }
    return img_color_stop_list(a, alen, 0);
}

/* §3.2.1 "radial-gradient() Syntax"'s prefix:
     [ <radial-shape> || <radial-size> ]? [ at <position> ]?
     <radial-size> = <radial-extent> | <length [0,∞]> | <length-percentage [0,∞]>{2}
     <radial-extent> = closest-corner | closest-side | farthest-corner | farthest-side
     <radial-shape> = circle | ellipse
   The two-value `<radial-size>` is tried BEFORE the one-value one, which is the `{2}` multiplier read
   greedily: `10px 20px` is one size and not a size followed by an unmatched component. */
static bool img_radial_prefix(const char *g, size_t gl)
{
    static const char *const SHAPE[] = { "circle", "ellipse" };
    static const char *const EXTENT[] = {
        "closest-corner", "closest-side", "farthest-corner", "farthest-side"
    };
    const char *w[IMG_MAX_WORDS];
    size_t wl[IMG_MAX_WORDS];
    int n = img_words(g, gl, w, wl, IMG_MAX_WORDS), i = 0;
    bool shape = false, size = false;

    if (n <= 0) return false;
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
        if (took == 0) return false;
        i += (int)took;
    }
    return i == n;
}

static bool img_radial(const char *a, size_t alen)
{
    const char *g;
    size_t gl, pos = 0, after;

    if (!img_next_group(a, alen, &pos, &g, &gl)) return false;
    after = pos;
    if (img_radial_prefix(g, gl)) return img_color_stop_list(a, alen, after);
    return img_color_stop_list(a, alen, 0);
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
    /* §3 "Gradients": `<gradient> = <linear-gradient()> | <repeating-linear-gradient()> |
       <radial-gradient()> | <repeating-radial-gradient()>`. §3.3 "Repeating Gradients: the
       repeating-linear-gradient() and repeating-radial-gradient() notations" gives the two repeating forms the
       SAME syntax as the two they repeat — "the repeating-linear-gradient() and repeating-radial-gradient()
       functions take the same arguments as the linear-gradient() and radial-gradient() functions" — so the
       pair shares one branch rather than each carrying a copy of a grammar that cannot differ. */
    if (img_word_is(name, name_len, "linear-gradient") ||
        img_word_is(name, name_len, "repeating-linear-gradient"))
        return img_linear(args, args_len);
    if (img_word_is(name, name_len, "radial-gradient") ||
        img_word_is(name, name_len, "repeating-radial-gradient"))
        return img_radial(args, args_len);
    if (img_word_is(name, name_len, "conic-gradient") ||
        img_word_is(name, name_len, "repeating-conic-gradient"))
        DFAIL("a CONIC GRADIENT reached css-images-3 §2's `<image>`, and §3 \"Gradients\" at that level has "
              "four notations rather than six — the conic pair is css-images-4 §3.3 \"Conic Gradients: the "
              "conic-gradient() notation\" and its §3.4 \"Repeating Gradients: the repeating-linear-gradient(), "
              "repeating-radial-gradient(), and repeating-conic-gradient() notations\". Refusing it would DROP "
              "a declaration every user agent accepts, and the drop is silent: the whole `background` would "
              "read as undeclared. BUILD §3.3.1 \"conic-gradient() Syntax\" — `[ [ [ from [ <angle> | <zero> ] "
              "]? [ at <position> ]? ] || <color-interpolation-method> ]? , <angular-color-stop-list>` — whose "
              "three pieces are one call to css_position_value.h, one angle test this file already has, and "
              "§3.5.1's `<angular-color-stop-list>`, which is this file's stop list with `<angle-percentage>` "
              "where it reads `<length-percentage>`. `<color-interpolation-method>` is css-color-4's and is "
              "the one piece that is not already here");
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
