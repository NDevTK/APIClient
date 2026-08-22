/* CSS COLOR — CSS Color Module Level 4's `<color>`. See css_color.h for why this is its own component.
 *
 * MOST OF THE PARSE IS LEXBOR'S, REACHED THROUGH A DECLARATION. Lexbor's `<color>` production is
 * `lxb_css_property_state_color`, the state function its property registry runs for the `color` property, and
 * the entry point it EXPOSES to that machinery is `lxb_css_declaration_list_parse`. So the input is parsed as
 * the value of a `color:` declaration and the `lxb_css_value_color_t` is read back out of the result. Three
 * facts about that result are then checked, and each rejects a string that is a DECLARATION BLOCK rather than a
 * `<color>`: the list holds exactly ONE declaration (`red;blue` is two), the declaration is not `!important`
 * (that is declaration syntax, not a colour), and nothing but CSS whitespace follows the value lexbor reported
 * consuming (`red;` ends a declaration where a `<color>` would have ended). Without them `input.value = 'red;'`
 * would sanitize to `#ff0000` where a browser gives `#000000`.
 *
 * THE ONE PRODUCTION LEXBOR DOES NOT HAVE IS §10.1's `color()`, and it is read HERE over lexbor's own
 * TOKENIZER. Lexbor's colour handler answers `case LXB_CSS_VALUE_COLOR: default: return false` for it, so what
 * is missing is the grammar rule and nothing else: the escapes, comments, numbers and percentages are still
 * tokenized by `lxb_css_syntax_tokenizer_t`, and only the production — a colour space keyword, three
 * `<number> | <percentage> | none` components and an optional slash-separated alpha — is ported here. The two
 * readers are disjoint by construction (a string whose first token is the `color(` function reaches only this
 * one) and the seam is asserted from both sides: the reader checks that the function name it matched is the
 * very value id lexbor declines, and that lexbor still rejects every string this reader was given.
 *
 * THE ARITHMETIC IS THE SPEC'S. Lexbor hands back the parsed FORM — hex digits, rgb() components, an hsl()
 * triple — and converting those to sRGB is CSS Color 4's own §7.1 and §8.1 sample algorithms and §6.1's named
 * colour table, ported as arithmetic and data. The four spaces whose conversion is a matrix pipeline rather
 * than a formula — `lab()`, `lch()`, `oklab()` and `oklch()` — are §11's algorithm and live in their own
 * component, css_color_convert.c; this file's job for them is the part that IS about the parsed value: which
 * percentage reference range each component uses, §9.3/§9.4's parse-time clamping, and `none`.
 *
 * `currentcolor` AND THE `<system-color>`s ARE RESOLVED, NOT LOOKED UP. §4.5's parse algorithm resolves the
 * parsed colour to a USED colour against a context element, "or the initial values of the properties if not" —
 * and this entry has no context element to take (see css_color.h), so `currentcolor` is the initial value of
 * the `color` property, which §3.2 gives as CanvasText. The nineteen `<system-color>` keywords are read from
 * this UA's own colour theme in css_system_color.c. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/css/css.h>

#include "check.h"
#include "core/css/css_color.h"
#include "core/css/css_color_convert.h"
#include "core/css/css_system_color.h"

const CssColor CSS_COLOR_OPAQUE_BLACK = { CSS_COLOR_SPACE_SRGB, { 0.0, 0.0, 0.0 }, 1.0, 0u };

/* ---- CSS Color 4 §6.1's NAMED COLORS -------------------------------------------------------------------------
 *
 * The standard's own table, as data. Lexbor's value registry knows these names — it has to, to parse them — but
 * it keeps only the NAME, because serializing a named colour is echoing the keyword back. The numeric
 * equivalents are what a colour resolved to sRGB needs, and §6.1 is the one place they are defined.
 * SORTED BY NAME, so the lookup is a binary search and a mis-ordered edit is caught by the DCHECK at the miss. */
typedef struct { const char *name; unsigned rgb; } CssNamedColor;

static const CssNamedColor CSS_NAMED_COLORS[] = {
    { "aliceblue", 0xf0f8ff }, { "antiquewhite", 0xfaebd7 }, { "aqua", 0x00ffff },
    { "aquamarine", 0x7fffd4 }, { "azure", 0xf0ffff }, { "beige", 0xf5f5dc },
    { "bisque", 0xffe4c4 }, { "black", 0x000000 }, { "blanchedalmond", 0xffebcd },
    { "blue", 0x0000ff }, { "blueviolet", 0x8a2be2 }, { "brown", 0xa52a2a },
    { "burlywood", 0xdeb887 }, { "cadetblue", 0x5f9ea0 }, { "chartreuse", 0x7fff00 },
    { "chocolate", 0xd2691e }, { "coral", 0xff7f50 }, { "cornflowerblue", 0x6495ed },
    { "cornsilk", 0xfff8dc }, { "crimson", 0xdc143c }, { "cyan", 0x00ffff },
    { "darkblue", 0x00008b }, { "darkcyan", 0x008b8b }, { "darkgoldenrod", 0xb8860b },
    { "darkgray", 0xa9a9a9 }, { "darkgreen", 0x006400 }, { "darkgrey", 0xa9a9a9 },
    { "darkkhaki", 0xbdb76b }, { "darkmagenta", 0x8b008b }, { "darkolivegreen", 0x556b2f },
    { "darkorange", 0xff8c00 }, { "darkorchid", 0x9932cc }, { "darkred", 0x8b0000 },
    { "darksalmon", 0xe9967a }, { "darkseagreen", 0x8fbc8f }, { "darkslateblue", 0x483d8b },
    { "darkslategray", 0x2f4f4f }, { "darkslategrey", 0x2f4f4f }, { "darkturquoise", 0x00ced1 },
    { "darkviolet", 0x9400d3 }, { "deeppink", 0xff1493 }, { "deepskyblue", 0x00bfff },
    { "dimgray", 0x696969 }, { "dimgrey", 0x696969 }, { "dodgerblue", 0x1e90ff },
    { "firebrick", 0xb22222 }, { "floralwhite", 0xfffaf0 }, { "forestgreen", 0x228b22 },
    { "fuchsia", 0xff00ff }, { "gainsboro", 0xdcdcdc }, { "ghostwhite", 0xf8f8ff },
    { "gold", 0xffd700 }, { "goldenrod", 0xdaa520 }, { "gray", 0x808080 },
    { "green", 0x008000 }, { "greenyellow", 0xadff2f }, { "grey", 0x808080 },
    { "honeydew", 0xf0fff0 }, { "hotpink", 0xff69b4 }, { "indianred", 0xcd5c5c },
    { "indigo", 0x4b0082 }, { "ivory", 0xfffff0 }, { "khaki", 0xf0e68c },
    { "lavender", 0xe6e6fa }, { "lavenderblush", 0xfff0f5 }, { "lawngreen", 0x7cfc00 },
    { "lemonchiffon", 0xfffacd }, { "lightblue", 0xadd8e6 }, { "lightcoral", 0xf08080 },
    { "lightcyan", 0xe0ffff }, { "lightgoldenrodyellow", 0xfafad2 }, { "lightgray", 0xd3d3d3 },
    { "lightgreen", 0x90ee90 }, { "lightgrey", 0xd3d3d3 }, { "lightpink", 0xffb6c1 },
    { "lightsalmon", 0xffa07a }, { "lightseagreen", 0x20b2aa }, { "lightskyblue", 0x87cefa },
    { "lightslategray", 0x778899 }, { "lightslategrey", 0x778899 }, { "lightsteelblue", 0xb0c4de },
    { "lightyellow", 0xffffe0 }, { "lime", 0x00ff00 }, { "limegreen", 0x32cd32 },
    { "linen", 0xfaf0e6 }, { "magenta", 0xff00ff }, { "maroon", 0x800000 },
    { "mediumaquamarine", 0x66cdaa }, { "mediumblue", 0x0000cd }, { "mediumorchid", 0xba55d3 },
    { "mediumpurple", 0x9370db }, { "mediumseagreen", 0x3cb371 }, { "mediumslateblue", 0x7b68ee },
    { "mediumspringgreen", 0x00fa9a }, { "mediumturquoise", 0x48d1cc }, { "mediumvioletred", 0xc71585 },
    { "midnightblue", 0x191970 }, { "mintcream", 0xf5fffa }, { "mistyrose", 0xffe4e1 },
    { "moccasin", 0xffe4b5 }, { "navajowhite", 0xffdead }, { "navy", 0x000080 },
    { "oldlace", 0xfdf5e6 }, { "olive", 0x808000 }, { "olivedrab", 0x6b8e23 },
    { "orange", 0xffa500 }, { "orangered", 0xff4500 }, { "orchid", 0xda70d6 },
    { "palegoldenrod", 0xeee8aa }, { "palegreen", 0x98fb98 }, { "paleturquoise", 0xafeeee },
    { "palevioletred", 0xdb7093 }, { "papayawhip", 0xffefd5 }, { "peachpuff", 0xffdab9 },
    { "peru", 0xcd853f }, { "pink", 0xffc0cb }, { "plum", 0xdda0dd },
    { "powderblue", 0xb0e0e6 }, { "purple", 0x800080 }, { "rebeccapurple", 0x663399 },
    { "red", 0xff0000 }, { "rosybrown", 0xbc8f8f }, { "royalblue", 0x4169e1 },
    { "saddlebrown", 0x8b4513 }, { "salmon", 0xfa8072 }, { "sandybrown", 0xf4a460 },
    { "seagreen", 0x2e8b57 }, { "seashell", 0xfff5ee }, { "sienna", 0xa0522d },
    { "silver", 0xc0c0c0 }, { "skyblue", 0x87ceeb }, { "slateblue", 0x6a5acd },
    { "slategray", 0x708090 }, { "slategrey", 0x708090 }, { "snow", 0xfffafa },
    { "springgreen", 0x00ff7f }, { "steelblue", 0x4682b4 }, { "tan", 0xd2b48c },
    { "teal", 0x008080 }, { "thistle", 0xd8bfd8 }, { "tomato", 0xff6347 },
    { "turquoise", 0x40e0d0 }, { "violet", 0xee82ee }, { "wheat", 0xf5deb3 },
    { "white", 0xffffff }, { "whitesmoke", 0xf5f5f5 }, { "yellow", 0xffff00 },
    { "yellowgreen", 0x9acd32 },
};

/* The §6.1 entry for a keyword, or NULL. The keyword comes from lexbor's own value registry, which stores it
   ASCII-lowercased and canonical, so this is a plain comparison and not a case-folding one. */
static const CssNamedColor *css_named_color(const char *name)
{
    size_t lo = 0, hi = sizeof CSS_NAMED_COLORS / sizeof CSS_NAMED_COLORS[0];

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(name, CSS_NAMED_COLORS[mid].name);

        if (c == 0) return &CSS_NAMED_COLORS[mid];
        if (c < 0) hi = mid;
        else lo = mid + 1;
    }
    return NULL;
}

/* ---- CSS Color 4 §10's COLOUR SPACE KEYWORDS -----------------------------------------------------------------
 *
 * ONE TABLE, BOTH DIRECTIONS. `color()`'s grammar names ten keywords for nine spaces, and §16.5 serializes a
 * space back "with ASCII lowercase letters for ... the color space name" — so the keyword-to-space map and the
 * space-to-keyword map are the same fact and are read out of one place. The FIRST row for a space is the name
 * §16.5 writes, which is why `xyz-d65` precedes the `xyz` that §10.9 defines as the same space: the two
 * keywords are one space, and a serialization has to pick the one that says which white it is. */
typedef struct { const char *name; CssColorSpace space; } CssColorSpaceKeyword;

static const CssColorSpaceKeyword CSS_COLOR_SPACE_KEYWORDS[] = {
    { "srgb",              CSS_COLOR_SPACE_SRGB },
    { "srgb-linear",       CSS_COLOR_SPACE_SRGB_LINEAR },
    { "display-p3",        CSS_COLOR_SPACE_DISPLAY_P3 },
    { "display-p3-linear", CSS_COLOR_SPACE_DISPLAY_P3_LINEAR },
    { "a98-rgb",           CSS_COLOR_SPACE_A98_RGB },
    { "prophoto-rgb",      CSS_COLOR_SPACE_PROPHOTO_RGB },
    { "rec2020",           CSS_COLOR_SPACE_REC2020 },
    { "xyz-d50",           CSS_COLOR_SPACE_XYZ_D50 },
    { "xyz-d65",           CSS_COLOR_SPACE_XYZ_D65 },
    { "xyz",               CSS_COLOR_SPACE_XYZ_D65 },
};

/* A CSS keyword is ASCII case-insensitive — §16.5's own example serializes `color(dIsPlAy-P3 ...)` — and the
   token's text is whatever the author wrote, so the comparison folds rather than the table. */
static bool css_ascii_ci_eq(const char *a, size_t alen, const char *b)
{
    size_t i;

    for (i = 0; i < alen; i++) {
        char c = a[i];

        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (b[i] == '\0' || c != b[i]) return false;
    }
    return b[alen] == '\0';
}

static bool css_color_space_by_name(const char *name, size_t len, CssColorSpace *out)
{
    size_t i;

    for (i = 0; i < sizeof CSS_COLOR_SPACE_KEYWORDS / sizeof CSS_COLOR_SPACE_KEYWORDS[0]; i++) {
        if (css_ascii_ci_eq(name, len, CSS_COLOR_SPACE_KEYWORDS[i].name)) {
            *out = CSS_COLOR_SPACE_KEYWORDS[i].space;
            return true;
        }
    }
    /* §10.1: "If the <ident> names a non-existent color space ... this argument represents an invalid color."
       The grammar's own <predefined-rgb> and <xyz-space> productions are CLOSED keyword lists, so an ident
       outside them does not match the production at all and the parse fails — and for every caller of this
       component the two readings agree anyway, because §10.1's invalid colour has a used value of opaque black
       and that is exactly what a failed parse gives HTML §4.10.5.1.14. A CSS Color 5 <dashed-ident> custom
       space is the same answer for the same reason: there is no @color-profile rule in this engine for one to
       have been defined by, so every dashed-ident names a non-existent colour space. */
    return false;
}

static const char *css_color_space_name(CssColorSpace space)
{
    size_t i;

    for (i = 0; i < sizeof CSS_COLOR_SPACE_KEYWORDS / sizeof CSS_COLOR_SPACE_KEYWORDS[0]; i++) {
        if (CSS_COLOR_SPACE_KEYWORDS[i].space == space) return CSS_COLOR_SPACE_KEYWORDS[i].name;
    }
    DFAIL("a colour space has no keyword in CSS Color 4 §10's table — every member of CssColorSpace is a space "
          "the color() grammar names, so a member without a name is one that was added to the enum and not to "
          "the keyword table it is parsed and serialized through");
    return "srgb";
}

/* ---- the numeric pieces of a parsed colour ------------------------------------------------------------------ */

static double css_clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

static double css_clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* An rgb() component: a `<number>` in the 0-255 reference range, a `<percentage>` in 0%-100%, or `none`.
   A `none` is reported to the caller as a MISSING COMPONENT rather than resolved here: rgb() is an sRGB value,
   so a caller that serializes it in the sRGB space is not converting it, and §11 only replaces a missing
   component with zero when it converts. The zero is stored anyway — §4.4's "a missing component behaves as a
   zero value" — so nothing downstream has to consult the flag to compute with the colour. */
static double css_rgb_component(const lxb_css_value_number_percentage_t *np, bool *missing)
{
    *missing = false;
    switch (np->type) {
    case LXB_CSS_VALUE_NONE:        *missing = true; return 0.0;
    case LXB_CSS_VALUE__NUMBER:     return css_clamp01(np->u.number.num / 255.0);
    case LXB_CSS_VALUE__PERCENTAGE: return css_clamp01(np->u.percentage.num / 100.0);
    default:
        DFAIL("an rgb() component came back from lexbor as neither a number, a percentage nor `none` — those "
              "are the three the <color> grammar's own component production accepts");
        return 0.0;
    }
}

/* An `<alpha-value>`: a `<number>` in 0-1, a `<percentage>`, `none`, or ABSENT — and absent is the interesting
   one, because CSS Color 4 makes an omitted alpha "an implicit value of 1 (fully opaque)" and lexbor leaves the
   field at the zeroed type its property allocator produced. */
static double css_alpha(const lxb_css_value_number_percentage_t *np, bool *missing)
{
    *missing = false;
    switch (np->type) {
    case LXB_CSS_VALUE__UNDEF:      return 1.0;
    case LXB_CSS_VALUE_NONE:        *missing = true; return 0.0;
    case LXB_CSS_VALUE__NUMBER:     return css_clamp01(np->u.number.num);
    case LXB_CSS_VALUE__PERCENTAGE: return css_clamp01(np->u.percentage.num / 100.0);
    default:
        DFAIL("an alpha component came back from lexbor as neither a number, a percentage, `none` nor absent");
        return 1.0;
    }
}

/* An hsl()/hwb() percentage argument, in its own 0-100 reference range. Clamped there: CSS Color 4 states the
   parse-time clamping its §7.1 sample code then assumes has already happened. A `none` is a zero and NOT a
   missing component of the answer: what is missing is the saturation or the whiteness, and once §7.1 has run
   there is no such component left to be missing — the three the caller receives are red, green and blue. */
static double css_percentage(const lxb_css_value_percentage_type_t *pt)
{
    if (pt->type == LXB_CSS_VALUE_NONE) return 0.0;
    DCHECK(pt->type == LXB_CSS_VALUE__PERCENTAGE,
           "an hsl() or hwb() argument came back from lexbor as neither a percentage nor `none`");
    return css_clamp(pt->percentage.num, 0.0, 100.0);
}

/* A `<hue>` in DEGREES. CSS Values 4 defines the four angle units against the degree, and `none` is a missing
   component, which resolves to 0 for the same reason the saturation above does. The value is NOT normalised
   into [0, 360): §7.1's own algorithm takes the hue modulo 12 after dividing by 30, which is where the wrap
   belongs. */
static double css_hue_degrees(const lxb_css_value_hue_t *h)
{
    double n;

    if (h->type == LXB_CSS_VALUE_NONE) return 0.0;
    if (h->type == LXB_CSS_VALUE__NUMBER) return h->u.number.num;
    DCHECK(h->type == LXB_CSS_VALUE__ANGLE,
           "a <hue> came back from lexbor as neither a number, an angle nor `none`");
    n = h->u.angle.num;
    switch (h->u.angle.unit) {
    case LXB_CSS_UNIT_DEG:  return n;
    case LXB_CSS_UNIT_GRAD: return n * 360.0 / 400.0;
    case LXB_CSS_UNIT_RAD:  return n * 180.0 / M_PI;
    case LXB_CSS_UNIT_TURN: return n * 360.0;
    default:
        DFAIL("a <hue> carried an angle unit that is not deg, grad, rad or turn — CSS Values 4 defines four");
        return 0.0;
    }
}

/* A lab()/lch()/oklab()/oklch() component, in the NUMBER its own space measures it in. Each of those functions
   accepts `<percentage> | <number> | none` for its L, a, b and C, and each declares its own PERCENT REFERENCE
   RANGE, so the only thing that differs between the eight of them is what 100% means — `pct100` — and a
   percentage is that fraction of it. The ranges are symmetric where the component is signed, which is why the
   a and b axes need no second parameter: §9.3 gives lab's as "-100% = -125, 100% = 125" and §9.4 gives
   oklab's as "-100% = -0.4, 100% = 0.4", and -100% × pct100 / 100 is exactly the negative endpoint.
   A `none` component is 0 here for the reason §11 gives, which is stronger than the reason the rgb() reader
   keeps its own missing components: the four Lab spaces are CONVERTED by this parse (see css_color.h), and
   §11's step 2 replaces every missing component with zero before it converts. */
static double css_lab_component(const lxb_css_value_number_percentage_t *np, double pct100)
{
    switch (np->type) {
    case LXB_CSS_VALUE_NONE:        return 0.0;
    case LXB_CSS_VALUE__NUMBER:     return np->u.number.num;
    case LXB_CSS_VALUE__PERCENTAGE: return np->u.percentage.num * pct100 / 100.0;
    default:
        DFAIL("a lab(), lch(), oklab() or oklch() component came back from lexbor as neither a number, a "
              "percentage nor `none` — those are the three each of their component productions accepts");
        return 0.0;
    }
}

/* §9.3 and §9.4's parse-time clamp of a chroma: "If the provided value is negative, it is clamped to 0 at
   parsed-value time." There is no upper bound to clamp against — the chroma is "theoretically unbounded". */
static double css_clamp_chroma(double v) { return v < 0.0 ? 0.0 : v; }

/* CSS Color 4 §7.1's CONVERTING HSL COLORS TO sRGB, its own algorithm. `hue` is in degrees and `sat` and
   `light` in the 0-100 reference range; the results are the sRGB components, in gamut in [0, 1]. The modulo is
   C's fmod because the algorithm's is JavaScript's `%`, and both truncate toward zero — a floored modulo would
   answer differently for a negative hue. */
static void css_hsl_to_srgb(double hue, double sat, double light, double *rgb)
{
    static const double N[3] = { 0.0, 8.0, 4.0 };
    double a;
    int i;

    sat /= 100.0;
    light /= 100.0;
    a = sat * (light < 1.0 - light ? light : 1.0 - light);
    for (i = 0; i < 3; i++) {
        double k = fmod(N[i] + hue / 30.0, 12.0);
        double m = k - 3.0 < 9.0 - k ? k - 3.0 : 9.0 - k;

        if (m > 1.0) m = 1.0;
        if (m < -1.0) m = -1.0;
        rgb[i] = light - a * m;
    }
}

/* CSS Color 4 §8.1's CONVERTING HWB COLORS TO sRGB, its own algorithm: a whiteness and blackness summing to
   100% or more is an achromatic colour whose three components are white / (white + black), and otherwise the
   fully-saturated hue is scaled into what is left between them. */
static void css_hwb_to_srgb(double hue, double white, double black, double *rgb)
{
    int i;

    white /= 100.0;
    black /= 100.0;
    if (white + black >= 1.0) {
        rgb[0] = rgb[1] = rgb[2] = white / (white + black);
        return;
    }
    css_hsl_to_srgb(hue, 100.0, 50.0, rgb);
    for (i = 0; i < 3; i++) {
        rgb[i] *= 1.0 - white - black;
        rgb[i] += white;
    }
}

/* ---- a parsed `<color>`, RESOLVED TO A USED COLOUR ----------------------------------------------------------- */

/* An sRGB colour with no missing components — every form below that is neither rgb() nor a `color()` function
   is one of those: a hex colour, a named colour, `transparent`, a system colour, and the four Lab spaces once
   §11 has converted them (which is where their missing components became zeroes). */
static bool css_color_srgb(const double rgb[3], double alpha, CssColor *out)
{
    out->space = CSS_COLOR_SPACE_SRGB;
    out->c[0] = rgb[0];
    out->c[1] = rgb[1];
    out->c[2] = rgb[2];
    out->a = alpha;
    out->missing = 0u;
    return true;
}

/* Answers false when the value lexbor produced is not a `<color>` at all — which is the CSS-wide keywords, the
   only other thing `lxb_css_property_state_color` writes into this record. `initial` is a valid value of the
   `color` PROPERTY and is not a colour, so `<input type=color value=initial>` is opaque black in a browser. */
static bool css_color_to_used(const lxb_css_value_color_t *c, CssColor *out)
{
    const lxb_css_data_t *kw;
    const CssNamedColor *named;
    double rgb[3];
    bool missing;
    int i;

    switch (c->type) {
    case LXB_CSS_VALUE_INITIAL: case LXB_CSS_VALUE_INHERIT:
    case LXB_CSS_VALUE_UNSET:   case LXB_CSS_VALUE_REVERT:
        return false;

    case LXB_CSS_VALUE_HEX:
        /* Lexbor stores a three- or four-digit hex as its NIBBLES and a six- or eight-digit one as its bytes,
           which is exactly the distinction CSS Color 4 draws when it says the shorter forms are "the longer
           form with each digit DUPLICATED" — duplicating a hex digit is multiplying the nibble by 17. The
           three-digit form's alpha is the one field lexbor fills as a byte rather than a nibble. */
        switch (c->u.hex.type) {
        case LXB_CSS_PROPERTY_COLOR_HEX_TYPE_3:
            rgb[0] = c->u.hex.rgba.r * 17 / 255.0;
            rgb[1] = c->u.hex.rgba.g * 17 / 255.0;
            rgb[2] = c->u.hex.rgba.b * 17 / 255.0;
            return css_color_srgb(rgb, 1.0, out);
        case LXB_CSS_PROPERTY_COLOR_HEX_TYPE_4:
            rgb[0] = c->u.hex.rgba.r * 17 / 255.0;
            rgb[1] = c->u.hex.rgba.g * 17 / 255.0;
            rgb[2] = c->u.hex.rgba.b * 17 / 255.0;
            return css_color_srgb(rgb, c->u.hex.rgba.a * 17 / 255.0, out);
        case LXB_CSS_PROPERTY_COLOR_HEX_TYPE_6:
            rgb[0] = c->u.hex.rgba.r / 255.0;
            rgb[1] = c->u.hex.rgba.g / 255.0;
            rgb[2] = c->u.hex.rgba.b / 255.0;
            return css_color_srgb(rgb, 1.0, out);
        case LXB_CSS_PROPERTY_COLOR_HEX_TYPE_8:
            rgb[0] = c->u.hex.rgba.r / 255.0;
            rgb[1] = c->u.hex.rgba.g / 255.0;
            rgb[2] = c->u.hex.rgba.b / 255.0;
            return css_color_srgb(rgb, c->u.hex.rgba.a / 255.0, out);
        }
        DFAIL("a hex colour carried a digit count that is not three, four, six or eight — those are the four "
              "<hex-color> forms CSS Color 4 defines");
        return false;

    case LXB_CSS_VALUE_TRANSPARENT:
        /* CSS Color 4: "transparent black", the same colour as opaque black but fully transparent. */
        rgb[0] = rgb[1] = rgb[2] = 0.0;
        return css_color_srgb(rgb, 0.0, out);

    case LXB_CSS_VALUE_RGB: case LXB_CSS_VALUE_RGBA:
        out->space = CSS_COLOR_SPACE_SRGB;
        out->missing = 0u;
        out->c[0] = css_rgb_component(&c->u.rgb.r, &missing);
        if (missing) out->missing |= CSS_COLOR_MISSING_COMPONENT(0);
        out->c[1] = css_rgb_component(&c->u.rgb.g, &missing);
        if (missing) out->missing |= CSS_COLOR_MISSING_COMPONENT(1);
        out->c[2] = css_rgb_component(&c->u.rgb.b, &missing);
        if (missing) out->missing |= CSS_COLOR_MISSING_COMPONENT(2);
        out->a = css_alpha(&c->u.rgb.a, &missing);
        if (missing) out->missing |= CSS_COLOR_MISSING_ALPHA;
        return true;

    case LXB_CSS_VALUE_HSL: case LXB_CSS_VALUE_HSLA:
        css_hsl_to_srgb(css_hue_degrees(&c->u.hsl.h), css_percentage(&c->u.hsl.s),
                        css_percentage(&c->u.hsl.l), rgb);
        for (i = 0; i < 3; i++) rgb[i] = css_clamp01(rgb[i]);
        css_color_srgb(rgb, css_alpha(&c->u.hsl.a, &missing), out);
        if (missing) out->missing |= CSS_COLOR_MISSING_ALPHA;
        return true;

    case LXB_CSS_VALUE_HWB:
        /* The whiteness and blackness ride the same record as hsl()'s saturation and lightness. */
        css_hwb_to_srgb(css_hue_degrees(&c->u.hwb.h), css_percentage(&c->u.hwb.s),
                        css_percentage(&c->u.hwb.l), rgb);
        for (i = 0; i < 3; i++) rgb[i] = css_clamp01(rgb[i]);
        css_color_srgb(rgb, css_alpha(&c->u.hwb.a, &missing), out);
        if (missing) out->missing |= CSS_COLOR_MISSING_ALPHA;
        return true;

    /* THE FOUR SPACES §11 CONVERTS. The lab and lch records are shared by their OK counterparts — lexbor
       parses `oklab()` into `u.lab` and `oklch()` into `u.lch` — so what separates the pairs here is entirely
       the percentage reference range and the parse-time clamp each function declares, which is why the four
       cases are written out rather than folded. The result is EXTENDED sRGB and is deliberately not clamped:
       see css_color_convert.h for why the gamut clip belongs to the serialization and not to this step. */
    case LXB_CSS_VALUE_LAB:
        /* §9.3: L is 0% = 0.0, 100% = 100.0, clamped to that range at parsed-value time; a and b are
           -100% = -125, 100% = 125, signed and unbounded. */
        css_lab_to_srgb(css_clamp(css_lab_component(&c->u.lab.l, 100.0), 0.0, 100.0),
                        css_lab_component(&c->u.lab.a, 125.0),
                        css_lab_component(&c->u.lab.b, 125.0), rgb);
        css_color_srgb(rgb, css_alpha(&c->u.lab.alpha, &missing), out);
        if (missing) out->missing |= CSS_COLOR_MISSING_ALPHA;
        return true;

    case LXB_CSS_VALUE_OKLAB:
        /* §9.4: L is 0% = 0.0, 100% = 1.0, clamped to that range; a and b are -100% = -0.4, 100% = 0.4. */
        css_oklab_to_srgb(css_clamp(css_lab_component(&c->u.lab.l, 1.0), 0.0, 1.0),
                          css_lab_component(&c->u.lab.a, 0.4),
                          css_lab_component(&c->u.lab.b, 0.4), rgb);
        css_color_srgb(rgb, css_alpha(&c->u.lab.alpha, &missing), out);
        if (missing) out->missing |= CSS_COLOR_MISSING_ALPHA;
        return true;

    case LXB_CSS_VALUE_LCH:
        /* §9.3: L as in lab(); C is 0% = 0, 100% = 150, clamped below at 0; H is a <hue>, and 0deg points
           along the positive a axis rather than at red — which is the conversion's business, not this one's. */
        css_lch_to_srgb(css_clamp(css_lab_component(&c->u.lch.l, 100.0), 0.0, 100.0),
                        css_clamp_chroma(css_lab_component(&c->u.lch.c, 150.0)),
                        css_hue_degrees(&c->u.lch.h), rgb);
        css_color_srgb(rgb, css_alpha(&c->u.lch.a, &missing), out);
        if (missing) out->missing |= CSS_COLOR_MISSING_ALPHA;
        return true;

    case LXB_CSS_VALUE_OKLCH:
        /* §9.4: L as in oklab(); C is 0% = 0.0, 100% = 0.4, clamped below at 0. */
        css_oklch_to_srgb(css_clamp(css_lab_component(&c->u.lch.l, 1.0), 0.0, 1.0),
                          css_clamp_chroma(css_lab_component(&c->u.lch.c, 0.4)),
                          css_hue_degrees(&c->u.lch.h), rgb);
        css_color_srgb(rgb, css_alpha(&c->u.lch.a, &missing), out);
        if (missing) out->missing |= CSS_COLOR_MISSING_ALPHA;
        return true;

    /* §4.5 STEP 2's USED-COLOUR RESOLUTION, for the two kinds of value that need one. This entry point has no
       context element to resolve against — §4.5 says "use element if it was passed, or the initial values of
       the properties if not" — so `currentcolor` is the initial value of the `color` property, which §3.2
       gives as CanvasText, and each `<system-color>` is §15.5's "corresponding color in its color space" read
       from this UA's own theme. The keyword carries no alpha of its own in this grammar, so §15.5's "paired
       with the specified alpha component" pairs it with the fully-opaque default the theme already answers. */
    case LXB_CSS_VALUE_CURRENTCOLOR:
        *out = css_system_color(CSS_SYSTEM_COLOR_CANVASTEXT);
        return true;

    case LXB_CSS_VALUE_CANVAS:           *out = css_system_color(CSS_SYSTEM_COLOR_CANVAS);           return true;
    case LXB_CSS_VALUE_CANVASTEXT:       *out = css_system_color(CSS_SYSTEM_COLOR_CANVASTEXT);       return true;
    case LXB_CSS_VALUE_LINKTEXT:         *out = css_system_color(CSS_SYSTEM_COLOR_LINKTEXT);         return true;
    case LXB_CSS_VALUE_VISITEDTEXT:      *out = css_system_color(CSS_SYSTEM_COLOR_VISITEDTEXT);      return true;
    case LXB_CSS_VALUE_ACTIVETEXT:       *out = css_system_color(CSS_SYSTEM_COLOR_ACTIVETEXT);       return true;
    case LXB_CSS_VALUE_BUTTONFACE:       *out = css_system_color(CSS_SYSTEM_COLOR_BUTTONFACE);       return true;
    case LXB_CSS_VALUE_BUTTONTEXT:       *out = css_system_color(CSS_SYSTEM_COLOR_BUTTONTEXT);       return true;
    case LXB_CSS_VALUE_BUTTONBORDER:     *out = css_system_color(CSS_SYSTEM_COLOR_BUTTONBORDER);     return true;
    case LXB_CSS_VALUE_FIELD:            *out = css_system_color(CSS_SYSTEM_COLOR_FIELD);            return true;
    case LXB_CSS_VALUE_FIELDTEXT:        *out = css_system_color(CSS_SYSTEM_COLOR_FIELDTEXT);        return true;
    case LXB_CSS_VALUE_HIGHLIGHT:        *out = css_system_color(CSS_SYSTEM_COLOR_HIGHLIGHT);        return true;
    case LXB_CSS_VALUE_HIGHLIGHTTEXT:    *out = css_system_color(CSS_SYSTEM_COLOR_HIGHLIGHTTEXT);    return true;
    case LXB_CSS_VALUE_SELECTEDITEM:     *out = css_system_color(CSS_SYSTEM_COLOR_SELECTEDITEM);     return true;
    case LXB_CSS_VALUE_SELECTEDITEMTEXT: *out = css_system_color(CSS_SYSTEM_COLOR_SELECTEDITEMTEXT); return true;
    case LXB_CSS_VALUE_MARK:             *out = css_system_color(CSS_SYSTEM_COLOR_MARK);             return true;
    case LXB_CSS_VALUE_MARKTEXT:         *out = css_system_color(CSS_SYSTEM_COLOR_MARKTEXT);         return true;
    case LXB_CSS_VALUE_GRAYTEXT:         *out = css_system_color(CSS_SYSTEM_COLOR_GRAYTEXT);         return true;
    case LXB_CSS_VALUE_ACCENTCOLOR:      *out = css_system_color(CSS_SYSTEM_COLOR_ACCENTCOLOR);      return true;
    case LXB_CSS_VALUE_ACCENTCOLORTEXT:  *out = css_system_color(CSS_SYSTEM_COLOR_ACCENTCOLORTEXT);  return true;

    case LXB_CSS_VALUE_COLOR:
        DFAIL("lexbor's <color> grammar returned a parsed `color()` function — its own colour handler answers "
              "`case LXB_CSS_VALUE_COLOR: default: return false` for that token, so this component reads the "
              "production itself over lexbor's tokenizer. Lexbor has grown the production: delete "
              "css_color_parse_function and read the value out of this record instead");
        return false;

    default:
        break;
    }

    kw = lxb_css_value_by_id(c->type);
    DCHECK(kw != NULL, "a parsed <color> carried a value id lexbor's own registry does not know");
    named = kw ? css_named_color((const char *)kw->name) : NULL;
    if (named) {
        rgb[0] = ((named->rgb >> 16) & 0xff) / 255.0;
        rgb[1] = ((named->rgb >> 8) & 0xff) / 255.0;
        rgb[2] = (named->rgb & 0xff) / 255.0;
        return css_color_srgb(rgb, 1.0, out);
    }
    DFAIL("a <color> lexbor's grammar accepted is neither one of the forms above nor a keyword in CSS Color 4 "
          "§6.1's named-colour table — the grammar has gained a member the table and the cases above have not, "
          "so add it to whichever of the two it belongs to");
    return false;
}

/* ---- the lexbor-driven reader --------------------------------------------------------------------------- */

static bool css_is_whitespace(char c)
{
    /* CSS Syntax's whitespace: newline (a CR or FF is preprocessed into an LF), tab, space. */
    return c == '\n' || c == '\r' || c == '\f' || c == '\t' || c == ' ';
}

static bool css_color_parse_lexbor(const char *text, size_t len, CssColor *out)
{
    static const char PREFIX[] = "color:";
    const size_t plen = sizeof PREFIX - 1;
    lxb_css_parser_t *parser;
    lxb_css_memory_t *mem;
    lxb_css_rule_declaration_list_t *list;
    char *buf;
    bool ok = false;

    buf = malloc(plen + len + 1);
    CHECK(buf != NULL, "css color: OOM building the declaration a <color> is parsed as");
    memcpy(buf, PREFIX, plen);
    if (len) memcpy(buf + plen, text, len);
    buf[plen + len] = 0;

    /* A PARSER PER PARSE, and it is not an oversight that no module static holds one. A parser kept across
       calls is state the flow machinery would have to swap, and an arena kept with it accumulates every colour
       this engine ever parsed — the same reason cssom's declaration reads take a fresh arena each time. */
    parser = lxb_css_parser_create();
    CHECK(parser != NULL, "css color: the CSS parser allocation failed");
    CHECK(lxb_css_parser_init(parser, NULL) == LXB_STATUS_OK, "css color: the CSS parser could not initialise");
    mem = lxb_css_memory_create();
    CHECK(mem != NULL, "css color: the CSS arena allocation failed");
    CHECK(lxb_css_memory_init(mem, 64) == LXB_STATUS_OK, "css color: the CSS arena could not initialise");

    lxb_css_parser_memory_set(parser, mem);
    list = lxb_css_declaration_list_parse(parser, (const lxb_char_t *)buf, plen + len);
    lxb_css_parser_memory_set(parser, NULL);

    if (list != NULL && list->count == 1 && list->first->type == LXB_CSS_RULE_DECLARATION) {
        lxb_css_rule_declaration_t *d = lxb_css_rule_declaration(list->first);
        size_t i;
        bool tail_is_ws = true;

        /* WHAT LEXBOR CONSUMED, checked against what it was given. A declaration ends at a `;` and a `<color>`
           does not, so a value the parser reported ending before the end of the input had declaration syntax
           after it and is not a colour. Trailing whitespace is not that — CSS discards it around a value — so
           the tail is examined rather than merely measured. */
        for (i = d->offset.value_end; i < plen + len; i++) {
            if (!css_is_whitespace(buf[i])) { tail_is_ws = false; break; }
        }
        /* A FAILED declaration is kept in the list with its type rewritten to lexbor's `__undef`, which is why
           the type is asked rather than the list being empty-checked. */
        if (d->type == LXB_CSS_PROPERTY_COLOR && !d->important && tail_is_ws)
            ok = css_color_to_used((const lxb_css_value_color_t *)d->u.color, out);
    }

    lxb_css_memory_destroy(mem, true);
    lxb_css_parser_destroy(parser, true);
    free(buf);
    return ok;
}

/* THE SEAM, from lexbor's side. Only a DCHECK calls this, and only after this component's own reader has
   claimed the string: the two grammars must not both accept one input, and the day lexbor's does, its answer
   is the one to use and the reader below is the code to delete. */
static bool css_color_lexbor_accepts(const char *text, size_t len)
{
    CssColor scratch;

    return css_color_parse_lexbor(text, len, &scratch);
}

/* ---- §10.1's `color()`, the production lexbor does not have ------------------------------------------------
 *
 * color() = color( <colorspace-params> [ / [ <alpha-value> | none ] ]? )
 * <colorspace-params> = [ <predefined-rgb-params> | <xyz-params> ]
 * <predefined-rgb-params> = <predefined-rgb> [ <number> | <percentage> | none ]{3}
 * <predefined-rgb> = srgb | srgb-linear | display-p3 | display-p3-linear | a98-rgb | prophoto-rgb | rec2020
 * <xyz-params> = <xyz-space> [ <number> | <percentage> | none ]{3}
 * <xyz-space> = xyz | xyz-d50 | xyz-d65
 *
 * Every space in the grammar shares one percent reference range — §10.2 through §10.9 each state "0% = 0.0,
 * 100% = 1.0" for their three components — so a `<percentage>` is one hundredth of itself and nothing here
 * needs to know which space it is reading. Components are NOT clamped: "An out of gamut color has component
 * values less than 0 or 0%, or greater than 1 or 100%. These are not invalid, and are retained." The alpha IS
 * clamped, per §16.1.2's "<alpha-value>s which were specified outside the valid range are clamped at parse
 * time". A comma anywhere is a parse failure and needs no case of its own: "Using commas inside this function
 * is an error", and no step below accepts one. */
typedef enum {
    CSS_COLOR_FN_NOT_A_COLOR_FUNCTION,   /* the input does not begin with the `color(` function token */
    CSS_COLOR_FN_FAILURE,                /* it does, and it is not a valid color() */
    CSS_COLOR_FN_PARSED
} CssColorFnResult;

/* The next token, whitespace skipped, still UNCONSUMED — and NULL is never a parse answer. Lexbor's tokenizer
   returns NULL only where it failed to allocate (its own state functions set no other status), so a NULL here
   is the allocation floor and not a colour that failed to parse. */
static const lxb_css_syntax_token_t *css_color_peek(lxb_css_syntax_tokenizer_t *tkz)
{
    const lxb_css_syntax_token_t *t = lxb_css_syntax_token_wo_ws(tkz);

    CHECK(t != NULL, "css color: OOM tokenizing a color() function");
    return t;
}

/* A `<number> | <percentage> | none` component of `color()`, or false for anything else. */
static bool css_color_fn_component(const lxb_css_syntax_token_t *t, double *value, bool *missing)
{
    *missing = false;
    switch (t->type) {
    case LXB_CSS_SYNTAX_TOKEN_NUMBER:
        *value = lxb_css_syntax_token_number(t)->num;
        return true;
    case LXB_CSS_SYNTAX_TOKEN_PERCENTAGE:
        *value = lxb_css_syntax_token_percentage(t)->num / 100.0;
        return true;
    case LXB_CSS_SYNTAX_TOKEN_IDENT:
        if (css_ascii_ci_eq((const char *)lxb_css_syntax_token_ident(t)->data,
                            lxb_css_syntax_token_ident(t)->length, "none")) {
            *value = 0.0;              /* §4.4: "a missing component behaves as a zero value" */
            *missing = true;
            return true;
        }
        return false;
    default:
        return false;
    }
}

static CssColorFnResult css_color_parse_function(const char *text, size_t len, CssColor *out)
{
    lxb_css_syntax_tokenizer_t *tkz;
    const lxb_css_syntax_token_t *t;
    CssColor c = { CSS_COLOR_SPACE_SRGB, { 0.0, 0.0, 0.0 }, 1.0, 0u };
    CssColorFnResult res = CSS_COLOR_FN_FAILURE;
    double value;
    bool missing;
    int i;

    tkz = lxb_css_syntax_tokenizer_create();
    CHECK(tkz != NULL, "css color: the CSS tokenizer allocation failed");
    CHECK(lxb_css_syntax_tokenizer_init(tkz) == LXB_STATUS_OK,
          "css color: the CSS tokenizer could not initialise");
    lxb_css_syntax_tokenizer_buffer_set(tkz, (const lxb_char_t *)text, len);

    /* A STRING TOKEN'S TEXT LIVES UNTIL THE NEXT TOKEN IS PRODUCED — lexbor decodes it into the tokenizer's
       one temporary buffer and copies it into the arena only when the following token arrives — so every
       comparison below reads the token it was handed BEFORE asking for another. That is the same discipline
       lexbor's own property state functions keep. */
    t = css_color_peek(tkz);
    if (t->type != LXB_CSS_SYNTAX_TOKEN_FUNCTION
        || !css_ascii_ci_eq((const char *)lxb_css_syntax_token_function(t)->data,
                            lxb_css_syntax_token_function(t)->length, "color"))
    {
        res = CSS_COLOR_FN_NOT_A_COLOR_FUNCTION;
        goto done;
    }
    /* THE SEAM, from this side: the token claimed here is the one lexbor's own colour handler declines by id.
       If lexbor ever resolves this name to something else, its grammar and this reader are no longer talking
       about the same production. */
    DCHECK(lxb_css_value_by_name(lxb_css_syntax_token_function(t)->data,
                                 lxb_css_syntax_token_function(t)->length) == LXB_CSS_VALUE_COLOR,
           "lexbor's value registry no longer maps the `color` function name to LXB_CSS_VALUE_COLOR — that is "
           "the id its <color> handler declines, and this component reads that production on the strength of "
           "it, so the two are no longer describing the same token");
    lxb_css_syntax_token_consume(tkz);

    t = css_color_peek(tkz);
    if (t->type != LXB_CSS_SYNTAX_TOKEN_IDENT) goto done;
    if (!css_color_space_by_name((const char *)lxb_css_syntax_token_ident(t)->data,
                                 lxb_css_syntax_token_ident(t)->length, &c.space)) goto done;
    lxb_css_syntax_token_consume(tkz);

    for (i = 0; i < 3; i++) {
        t = css_color_peek(tkz);
        if (!css_color_fn_component(t, &value, &missing)) goto done;
        c.c[i] = value;
        if (missing) c.missing |= CSS_COLOR_MISSING_COMPONENT(i);
        lxb_css_syntax_token_consume(tkz);
    }

    t = css_color_peek(tkz);
    if (t->type == LXB_CSS_SYNTAX_TOKEN_DELIM && lxb_css_syntax_token_delim_char(t) == '/') {
        lxb_css_syntax_token_consume(tkz);
        t = css_color_peek(tkz);
        if (!css_color_fn_component(t, &value, &missing)) goto done;
        c.a = css_clamp01(value);
        if (missing) c.missing |= CSS_COLOR_MISSING_ALPHA;
        lxb_css_syntax_token_consume(tkz);
        t = css_color_peek(tkz);
    }

    if (t->type != LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS) goto done;
    lxb_css_syntax_token_consume(tkz);

    /* §4.5 parses the WHOLE input as a `<color>`, so anything after the closing parenthesis that is not
       whitespace makes this not a colour — the same rule the lexbor-driven reader enforces on its own tail. */
    t = css_color_peek(tkz);
    if (t->type != LXB_CSS_SYNTAX_TOKEN__EOF) goto done;

    *out = c;
    res = CSS_COLOR_FN_PARSED;

done:
    lxb_css_syntax_tokenizer_destroy(tkz);
    return res;
}

/* ---- the entry points ------------------------------------------------------------------------------------ */

bool css_color_parse(const char *text, size_t len, CssColor *out)
{
    CssColorFnResult r;

    DCHECK(out != NULL, "css color: a parse was asked for with nowhere to put the result");
    r = css_color_parse_function(text, len, out);
    if (r != CSS_COLOR_FN_NOT_A_COLOR_FUNCTION) {
        DCHECK(!css_color_lexbor_accepts(text, len),
               "lexbor's <color> grammar has GAINED the color() production this component reads itself — the "
               "two now both accept one string and only one of them can own it. Delete css_color_parse_function "
               "and the routing below, and read the parsed value out of lexbor's record");
        return r == CSS_COLOR_FN_PARSED;
    }
    return css_color_parse_lexbor(text, len, out);
}

void css_color_convert(CssColor *c, CssColorSpace dest)
{
    double xyz[3];
    int i;

    DCHECK(c != NULL, "css color: a conversion was asked for with no colour");
    for (i = 0; i < 3; i++) {
        DCHECK(!(c->missing & CSS_COLOR_MISSING_COMPONENT(i)) || c->c[i] == 0.0,
               "a missing component carries a value other than zero — §11 step 2's `replace any missing "
               "component with zero` is a no-op in this component ONLY because §4.4's zero is stored beside "
               "the flag at every point a colour is built, and one of those points stored something else");
    }
    /* §11 defines the conversion "where src and dest are different". Where they are the same there is nothing
       to convert, and that is not a shortcut: it is the whole reason a missing component can survive to a
       serialization at all. */
    if (c->space == dest) return;

    /* §11 step 2: "Replace any missing component with zero." The zero is already in place — §4.4 keeps every
       missing component's value at zero — so what this does is forget that they were missing, which is what
       makes the difference observable in the serialization. The ALPHA's flag is NOT cleared: the pipeline
       below is the two spaces' transfer functions, matrices and white points, and alpha is in none of them. */
    c->missing &= ~(CSS_COLOR_MISSING_COMPONENT(0) | CSS_COLOR_MISSING_COMPONENT(1)
                    | CSS_COLOR_MISSING_COMPONENT(2));
    css_color_space_to_xyz_d65(c->space, c->c, xyz);
    css_color_space_from_xyz_d65(dest, xyz, c->c);
    c->space = dest;
}

/* HTML §4.10.5.1.14 step 4.2's rounding: into the range 0 to 255 inclusive, to the nearest integer with a tie
   going towards +infinity — CSS Values 4's rule, which is `floor(x + 0.5)` and NOT C's `round`, whose tie goes
   away from zero and therefore answers -1.5 differently.
   A NaN is the one input the two comparisons below cannot place — both are false for it and the cast is then
   undefined — so it is asserted rather than silently becoming whatever the cast produced. */
static int css_round_255(double v)
{
    double n;

    DCHECK(!isnan(v), "a NaN colour component reached HTML §4.10.5.1.14 step 4.2's rounding — the range check "
                      "below cannot place it and the conversion that produced it should have asserted first");
    n = floor(v * 255.0 + 0.5);
    if (n < 0.0) return 0;
    if (n > 255.0) return 255;
    return (int)n;
}

void css_color_quantize_8bit(CssColor *c)
{
    int i;

    DCHECK(c != NULL, "css color: an 8-bit quantization was asked for with no colour");
    DCHECK(c->space == CSS_COLOR_SPACE_SRGB,
           "HTML §4.10.5.1.14 step 4.2's rounding reached a colour that is not sRGB — step 4.1 converts to "
           "'srgb' first and the two steps are in that order because `the range 0 to 255` is the Limited sRGB "
           "state's 8 bits per component, so its caller ran them the other way round or skipped 4.1");
    for (i = 0; i < 3; i++)
        c->c[i] = css_round_255(c->c[i]) / 255.0;
}

void css_color_serialize_html(const CssColor *c, char out[8])
{
    static const char HEX[] = "0123456789abcdef";
    int v[3], i;

    DCHECK(c->space == CSS_COLOR_SPACE_SRGB,
           "CSS Color 4 §16.2.1's HTML-compatible serialization is defined for an sRGB value, and the colour "
           "reaching it is in another colour space — HTML §4.10.5.1.14 step 4.1 converts to 'srgb' before it "
           "chooses this form, so its caller skipped the conversion");
    DCHECK(c->a == 1.0, "CSS Color 4 §16.2.1 states the HTML-compatible serialization only for an alpha of 1, "
                        "and a caller reaching it with any other alpha skipped the step that makes it 1 — for "
                        "HTML §4.10.5.1.14 that is `serialize a color well control color` step 3");
    for (i = 0; i < 3; i++) {
        DCHECK(c->c[i] >= 0.0 && c->c[i] <= 1.0
               && fabs(c->c[i] * 255.0 - floor(c->c[i] * 255.0 + 0.5)) < 1e-9,
               "a component that is not one of the 256 this form can write reached §16.2.1's serialization — "
               "HTML §4.10.5.1.14 step 4.2 rounds every component into 0 to 255 before step 6 serializes, so "
               "its caller skipped css_color_quantize_8bit");
        v[i] = css_round_255(c->c[i]);
    }
    out[0] = '#';
    for (i = 0; i < 3; i++) {
        out[1 + i * 2] = HEX[(v[i] >> 4) & 0xf];
        out[2 + i * 2] = HEX[v[i] & 0xf];
    }
    out[7] = 0;
}

/* §16.5's and §16.1.2's `<number>`: base ten, "." as the decimal separator, a leading zero that "must not be
   omitted", six decimal places with trailing fractional zeroes omitted along with a then-empty decimal point,
   and "rounded towards +∞, not truncated".
 *
 * THE ROUNDING IS DONE ON THE DECIMAL EXPANSION, not by scaling the double, because both of the obvious
 * shortcuts get the tie wrong and the tie is reachable. `printf("%.6f")` rounds a tie to EVEN, which answers
 * 0.007812 for the exactly-representable 0.0078125 where §16.5 asks for 0.007813; multiplying by a million
 * first rounds twice, and the second rounding can invent a tie the value did not have. So the value is printed
 * with far more decimals than the grid — enough that no double can be closer to a midpoint than the printed
 * digits can show — and the string is then rounded at the sixth decimal, with the midpoint going to the
 * GREATER of the two candidates, which is what "towards +∞" means for a negative component too. */
static size_t css_color_write_number_at(double v, size_t decimals, char *out, size_t cap)
{
    char buf[384];
    int n;
    size_t dot, first, tail, i, len;
    bool up = false;

    DCHECK(isfinite(v), "a non-finite component reached CSS Color 4 §16.5's serialization — §16.5 allows an "
                        "implementation-defined limit for values approaching infinity and this component does "
                        "not impose one, so build that limit where the components are computed");
    DCHECK(decimals >= 1 && decimals <= 20,
           "a decimal grid outside the two CSS Color 4 states — §16.1.2 and §16.5's six places, and §16.1.1's "
           "two for a LEGACY alpha — reached this rounding, whose thirty printed decimals are what bound it");
    n = snprintf(buf, sizeof buf, "%.30f", v);
    CHECK(n > 0 && (size_t)n + 2 <= sizeof buf,
          "css color: a <number>'s decimal expansion did not fit the buffer §16.5's serialization prints it in");
    len = (size_t)n;

    first = buf[0] == '-' ? 1u : 0u;
    for (dot = first; dot < len && buf[dot] != '.'; dot++) { }
    DCHECK(dot < len && len > dot + 30,
           "the decimal expansion of a <number> came back from snprintf without the thirty decimal places it "
           "was asked for — the rounding below indexes them by position");

    /* Round at the LAST decimal of the grid: the digits beyond it decide, and an exact midpoint goes upwards. */
    tail = dot + decimals + 1;
    if (buf[tail] > '5') {
        up = true;
    }
    else if (buf[tail] == '5') {
        for (i = tail + 1; i < len && buf[i] == '0'; i++) { }
        up = i < len ? true : (first == 0);   /* an exact midpoint: towards +∞, so up in magnitude only if + */
    }

    if (up) {
        i = dot + decimals;
        for (;;) {
            if (buf[i] == '.') { i--; continue; }
            if (buf[i] < '9') { buf[i]++; break; }
            buf[i] = '0';
            if (i == first) {
                /* Every digit carried — 9.9999996 becomes 10.000000 — so the number grew a place. */
                memmove(buf + first + 1, buf + first, len - first + 1);
                buf[first] = '1';
                dot++;
                len++;
                break;
            }
            i--;
        }
    }

    len = dot + decimals + 1;
    while (len > 0 && buf[len - 1] == '0') len--;
    if (len > 0 && buf[len - 1] == '.') len--;
    buf[len] = 0;

    CHECK(len + 1 <= cap, "css color: §16.5's serialization ran out of room for a component");
    memcpy(out, buf, len + 1);
    return len;
}

/* §16.1.2 and §16.5's grid: "the serialized value must contain six decimal places (unless trailing zeroes have
   been removed)". */
static size_t css_color_write_number(double v, char *out, size_t cap)
{
    return css_color_write_number_at(v, 6, out, cap);
}

/* CSS Color 4 §16.2's SERIALIZATION OF AN sRGB VALUE, which is the form a COMPUTED or USED colour takes: "for
 * the computed and used value, the corresponding sRGB value is used", so a `red` that was a keyword in the
 * declared value is `rgb(255, 0, 0)` here, and `transparent` is `rgba(0, 0, 0, 0)`.
 * §16.2.2 STATES IT AS TWO FORMS AND THE MISSING COMPONENTS DECIDE WHICH. With none missing it is the LEGACY
 * comma form — `rgb()` when the clamped alpha is exactly 1 and `rgba()` otherwise, function name in ASCII
 * lowercase, the three components as `<number>`s in [0, 255] "regardless of the bit depth with which they are
 * stored", commas each followed by exactly one ASCII space, including the one before the alpha ("not slash").
 * With at least one component missing the legacy form "cannot represent none", so §16.2.2 sends the value to
 * the `color(srgb ...)` form instead — which is `css_color_serialize_function` and is why this delegates to it
 * rather than converting the `none` to a zero. That delegation is the whole reason both live in one file.
 * THE ALPHA IS §16.1.1's LEGACY ALPHA and not §16.1.2's: "the serialized value must contain at least two
 * decimal places (unless trailing zeroes have been removed)", which is the precision that round-trips an
 * integer percentage, rounded towards +∞ like every other number here.
 * Returns the length written, not counting the NUL. */
size_t css_color_serialize_srgb(const CssColor *c, char out[CSS_COLOR_FUNCTION_MAX])
{
    int v[3], i, n;
    size_t len;

    DCHECK(c != NULL, "css color: an sRGB serialization was asked for with no colour");
    DCHECK(c->space == CSS_COLOR_SPACE_SRGB,
           "CSS Color 4 §16.2's serialization is stated for an sRGB value and the colour reaching it is in "
           "another colour space — §11's conversion is what puts a computed colour in sRGB, and the caller "
           "that chose this form over the one for its own space skipped it");
    /* §16.2.2's second branch, verbatim in its condition: "if the value has at least one missing color
       component, the serialization form is chosen to preserve those components as the none keyword". */
    if (c->missing != 0u) return css_color_serialize_function(c, out);
    DCHECK(c->a >= 0.0 && c->a <= 1.0,
           "an alpha outside [0, 1] reached §16.2's serialization — CSS Color 4 §16.1.2 states that an "
           "<alpha-value> outside the range is clamped at parse time, so it was not");
    for (i = 0; i < 3; i++) v[i] = css_round_255(c->c[i]);
    n = snprintf(out, CSS_COLOR_FUNCTION_MAX, "%s(%d, %d, %d", c->a == 1.0 ? "rgb" : "rgba",
                 v[0], v[1], v[2]);
    CHECK(n > 0 && (size_t)n < CSS_COLOR_FUNCTION_MAX, "css color: §16.2's components did not fit");
    len = (size_t)n;
    /* "if the alpha is exactly 1, the rgb() form is used, with an implicit alpha; otherwise, the rgba() form is
       used, with an explicit alpha value." */
    if (c->a != 1.0) {
        CHECK(len + 2 < CSS_COLOR_FUNCTION_MAX, "css color: §16.2's alpha separator did not fit");
        out[len++] = ',';
        out[len++] = ' ';
        len += css_color_write_number_at(c->a, 2, out + len, CSS_COLOR_FUNCTION_MAX - len);
    }
    CHECK(len + 2 <= CSS_COLOR_FUNCTION_MAX, "css color: §16.2's closing parenthesis did not fit");
    out[len++] = ')';
    out[len] = 0;
    return len;
}

size_t css_color_serialize_function(const CssColor *c, char out[CSS_COLOR_FUNCTION_MAX])
{
    static const char NONE[] = "none";
    size_t len = 0;
    int i, n;

    DCHECK(c != NULL, "css color: a color() serialization was asked for with no colour");
    n = snprintf(out, CSS_COLOR_FUNCTION_MAX, "color(%s", css_color_space_name(c->space));
    CHECK(n > 0 && (size_t)n < CSS_COLOR_FUNCTION_MAX, "css color: §16.5's function name did not fit");
    len = (size_t)n;

    for (i = 0; i < 3; i++) {
        CHECK(len + 1 < CSS_COLOR_FUNCTION_MAX, "css color: §16.5's component separator did not fit");
        out[len++] = ' ';
        if (c->missing & CSS_COLOR_MISSING_COMPONENT(i)) {
            CHECK(len + sizeof NONE <= CSS_COLOR_FUNCTION_MAX, "css color: §16's `none` keyword did not fit");
            memcpy(out + len, NONE, sizeof NONE);
            len += sizeof NONE - 1;
        }
        else {
            len += css_color_write_number(c->c[i], out + len, CSS_COLOR_FUNCTION_MAX - len);
        }
    }

    /* §16.1: "If, after clamping to the range [0, 1] the alpha is 1, it is omitted from the serialization; an
       implicit value of 1 (fully opaque) is the default." A MISSING alpha is not 1 and is not omitted — §16
       serializes a missing component of a non-legacy form as the `none` keyword. */
    if ((c->missing & CSS_COLOR_MISSING_ALPHA) || c->a != 1.0) {
        CHECK(len + 3 < CSS_COLOR_FUNCTION_MAX, "css color: §16.5's alpha separator did not fit");
        out[len++] = ' ';
        out[len++] = '/';
        out[len++] = ' ';
        if (c->missing & CSS_COLOR_MISSING_ALPHA) {
            CHECK(len + sizeof NONE <= CSS_COLOR_FUNCTION_MAX, "css color: §16's `none` keyword did not fit");
            memcpy(out + len, NONE, sizeof NONE);
            len += sizeof NONE - 1;
        }
        else {
            DCHECK(c->a >= 0.0 && c->a <= 1.0,
                   "an alpha outside [0, 1] reached §16.5's serialization — CSS Color 4 §16.1.2 states that an "
                   "<alpha-value> outside the range is clamped at parse time, so it was not");
            len += css_color_write_number(c->a, out + len, CSS_COLOR_FUNCTION_MAX - len);
        }
    }

    CHECK(len + 2 <= CSS_COLOR_FUNCTION_MAX, "css color: §16.5's closing parenthesis did not fit");
    out[len++] = ')';
    out[len] = 0;
    return len;
}
