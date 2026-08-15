/* CSS COLOR — CSS Color Module Level 4's `<color>`. See css_color.h for why this is its own component.
 *
 * THE PARSE IS LEXBOR'S, REACHED THROUGH A DECLARATION. Lexbor's `<color>` production is
 * `lxb_css_property_state_color`, the state function its property registry runs for the `color` property, and
 * the entry point it EXPOSES to that machinery is `lxb_css_declaration_list_parse`. So the input is parsed as
 * the value of a `color:` declaration and the `lxb_css_value_color_t` is read back out of the result. Three
 * facts about that result are then checked, and each rejects a string that is a DECLARATION BLOCK rather than a
 * `<color>`: the list holds exactly ONE declaration (`red;blue` is two), the declaration is not `!important`
 * (that is declaration syntax, not a colour), and nothing but CSS whitespace follows the value lexbor reported
 * consuming (`red;` ends a declaration where a `<color>` would have ended). Without them `input.value = 'red;'`
 * would sanitize to `#ff0000` where a browser gives `#000000`.
 *
 * THE ARITHMETIC IS THE SPEC'S. Lexbor hands back the parsed FORM — hex digits, rgb() components, an hsl()
 * triple — and converting those to sRGB is CSS Color 4's own §7.1 and §8.1 sample algorithms and §6.1's named
 * colour table, ported as arithmetic and data. The four spaces whose conversion is a matrix pipeline rather
 * than a formula — `lab()`, `lch()`, `oklab()` and `oklch()` — are §12's algorithm and live in their own
 * component, css_color_convert.c; this file's job for them is the part that IS about the parsed value: which
 * percentage reference range each component uses, §9.3/§9.4's parse-time clamping, and `none`.
 *
 * `currentcolor` AND THE `<system-color>`s ARE RESOLVED, NOT LOOKED UP. §4.5's parse algorithm resolves the
 * parsed colour to a USED colour against a context element, "or the initial values of the properties if not" —
 * and this entry has no context element to take (see css_color.h), so `currentcolor` is the initial value of
 * the `color` property, which §3.2 gives as CanvasText. The nineteen `<system-color>` keywords are read from
 * this UA's own colour theme in css_system_color.c. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/css/css.h>

#include "check.h"
#include "core/css/css_color.h"
#include "core/css/css_color_convert.h"
#include "core/css/css_system_color.h"

const CssColor CSS_COLOR_OPAQUE_BLACK = { 0.0, 0.0, 0.0, 1.0 };

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

/* ---- the numeric pieces of a parsed colour ------------------------------------------------------------------ */

static double css_clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

static double css_clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* An rgb() component: a `<number>` in the 0-255 reference range, a `<percentage>` in 0%-100%, or `none`.
   §16.2's "any missing values are converted to 0 if the chosen serialization form cannot represent the none
   keyword" is applied here, because the HTML-compatible hex form is the only one this component writes. */
static double css_rgb_component(const lxb_css_value_number_percentage_t *np)
{
    switch (np->type) {
    case LXB_CSS_VALUE_NONE:        return 0.0;
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
static double css_alpha(const lxb_css_value_number_percentage_t *np)
{
    switch (np->type) {
    case LXB_CSS_VALUE__UNDEF:      return 1.0;
    case LXB_CSS_VALUE_NONE:        return 0.0;
    case LXB_CSS_VALUE__NUMBER:     return css_clamp01(np->u.number.num);
    case LXB_CSS_VALUE__PERCENTAGE: return css_clamp01(np->u.percentage.num / 100.0);
    default:
        DFAIL("an alpha component came back from lexbor as neither a number, a percentage, `none` nor absent");
        return 1.0;
    }
}

/* An hsl()/hwb() percentage argument, in its own 0-100 reference range. Clamped there: CSS Color 4 states the
   parse-time clamping its §7.1 sample code then assumes has already happened. */
static double css_percentage(const lxb_css_value_percentage_type_t *pt)
{
    if (pt->type == LXB_CSS_VALUE_NONE) return 0.0;
    DCHECK(pt->type == LXB_CSS_VALUE__PERCENTAGE,
           "an hsl() or hwb() argument came back from lexbor as neither a percentage nor `none`");
    return css_clamp(pt->percentage.num, 0.0, 100.0);
}

/* A `<hue>` in DEGREES. CSS Values 4 defines the four angle units against the degree, and `none` is a missing
   component, which §16.2 resolves to 0 for the form this component serializes. The value is NOT normalised into
   [0, 360): §7.1's own algorithm takes the hue modulo 12 after dividing by 30, which is where the wrap belongs. */
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
   A `none` component is 0 here for the reason §4.4 gives, which is stronger than the serialization rule the
   rgb() reader cites: "for all other purposes, a missing component behaves as a zero value ... This includes
   rendering the color directly, CONVERTING IT TO ANOTHER COLOR SPACE", and a conversion is what happens next. */
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

/* ---- a parsed `<color>`, RESOLVED TO sRGB -------------------------------------------------------------------- */

/* The three components a §12 conversion produced, paired with the alpha this file resolved. They are written
   through UNCLAMPED, which is the one thing that separates this from the hsl()/hwb() writers above: those two
   spaces cannot leave the sRGB gamut and their clamp is only guarding float error, while Lab, LCH, Oklab and
   OkLCh all can and routinely do. `true` is returned so a case can be one line — every caller of this is a
   `<color>` that resolved, and the false answer belongs to the CSS-wide keywords alone. */
static bool css_color_from_srgb(const double rgb[3], double alpha, CssColor *out)
{
    out->r = rgb[0];
    out->g = rgb[1];
    out->b = rgb[2];
    out->a = alpha;
    return true;
}

/* Answers false when the value lexbor produced is not a `<color>` at all — which is the CSS-wide keywords, the
   only other thing `lxb_css_property_state_color` writes into this record. `initial` is a valid value of the
   `color` PROPERTY and is not a colour, so `<input type=color value=initial>` is opaque black in a browser. */
static bool css_color_to_srgb(const lxb_css_value_color_t *c, CssColor *out)
{
    const lxb_css_data_t *kw;
    const CssNamedColor *named;
    double rgb[3];

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
            out->r = c->u.hex.rgba.r * 17 / 255.0;
            out->g = c->u.hex.rgba.g * 17 / 255.0;
            out->b = c->u.hex.rgba.b * 17 / 255.0;
            out->a = 1.0;
            return true;
        case LXB_CSS_PROPERTY_COLOR_HEX_TYPE_4:
            out->r = c->u.hex.rgba.r * 17 / 255.0;
            out->g = c->u.hex.rgba.g * 17 / 255.0;
            out->b = c->u.hex.rgba.b * 17 / 255.0;
            out->a = c->u.hex.rgba.a * 17 / 255.0;
            return true;
        case LXB_CSS_PROPERTY_COLOR_HEX_TYPE_6:
            out->r = c->u.hex.rgba.r / 255.0;
            out->g = c->u.hex.rgba.g / 255.0;
            out->b = c->u.hex.rgba.b / 255.0;
            out->a = 1.0;
            return true;
        case LXB_CSS_PROPERTY_COLOR_HEX_TYPE_8:
            out->r = c->u.hex.rgba.r / 255.0;
            out->g = c->u.hex.rgba.g / 255.0;
            out->b = c->u.hex.rgba.b / 255.0;
            out->a = c->u.hex.rgba.a / 255.0;
            return true;
        }
        DFAIL("a hex colour carried a digit count that is not three, four, six or eight — those are the four "
              "<hex-color> forms CSS Color 4 defines");
        return false;

    case LXB_CSS_VALUE_TRANSPARENT:
        /* CSS Color 4: "transparent black", the same colour as opaque black but fully transparent. */
        out->r = out->g = out->b = 0.0;
        out->a = 0.0;
        return true;

    case LXB_CSS_VALUE_RGB: case LXB_CSS_VALUE_RGBA:
        out->r = css_rgb_component(&c->u.rgb.r);
        out->g = css_rgb_component(&c->u.rgb.g);
        out->b = css_rgb_component(&c->u.rgb.b);
        out->a = css_alpha(&c->u.rgb.a);
        return true;

    case LXB_CSS_VALUE_HSL: case LXB_CSS_VALUE_HSLA:
        css_hsl_to_srgb(css_hue_degrees(&c->u.hsl.h), css_percentage(&c->u.hsl.s),
                        css_percentage(&c->u.hsl.l), rgb);
        out->r = css_clamp01(rgb[0]);
        out->g = css_clamp01(rgb[1]);
        out->b = css_clamp01(rgb[2]);
        out->a = css_alpha(&c->u.hsl.a);
        return true;

    case LXB_CSS_VALUE_HWB:
        /* The whiteness and blackness ride the same record as hsl()'s saturation and lightness. */
        css_hwb_to_srgb(css_hue_degrees(&c->u.hwb.h), css_percentage(&c->u.hwb.s),
                        css_percentage(&c->u.hwb.l), rgb);
        out->r = css_clamp01(rgb[0]);
        out->g = css_clamp01(rgb[1]);
        out->b = css_clamp01(rgb[2]);
        out->a = css_alpha(&c->u.hwb.a);
        return true;

    /* THE FOUR SPACES §12 CONVERTS. The lab and lch records are shared by their OK counterparts — lexbor
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
        return css_color_from_srgb(rgb, css_alpha(&c->u.lab.alpha), out);

    case LXB_CSS_VALUE_OKLAB:
        /* §9.4: L is 0% = 0.0, 100% = 1.0, clamped to that range; a and b are -100% = -0.4, 100% = 0.4. */
        css_oklab_to_srgb(css_clamp(css_lab_component(&c->u.lab.l, 1.0), 0.0, 1.0),
                          css_lab_component(&c->u.lab.a, 0.4),
                          css_lab_component(&c->u.lab.b, 0.4), rgb);
        return css_color_from_srgb(rgb, css_alpha(&c->u.lab.alpha), out);

    case LXB_CSS_VALUE_LCH:
        /* §9.3: L as in lab(); C is 0% = 0, 100% = 150, clamped below at 0; H is a <hue>, and 0deg points
           along the positive a axis rather than at red — which is the conversion's business, not this one's. */
        css_lch_to_srgb(css_clamp(css_lab_component(&c->u.lch.l, 100.0), 0.0, 100.0),
                        css_clamp_chroma(css_lab_component(&c->u.lch.c, 150.0)),
                        css_hue_degrees(&c->u.lch.h), rgb);
        return css_color_from_srgb(rgb, css_alpha(&c->u.lch.a), out);

    case LXB_CSS_VALUE_OKLCH:
        /* §9.4: L as in oklab(); C is 0% = 0.0, 100% = 0.4, clamped below at 0. */
        css_oklch_to_srgb(css_clamp(css_lab_component(&c->u.lch.l, 1.0), 0.0, 1.0),
                          css_clamp_chroma(css_lab_component(&c->u.lch.c, 0.4)),
                          css_hue_degrees(&c->u.lch.h), rgb);
        return css_color_from_srgb(rgb, css_alpha(&c->u.lch.a), out);

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

    default:
        break;
    }

    kw = lxb_css_value_by_id(c->type);
    DCHECK(kw != NULL, "a parsed <color> carried a value id lexbor's own registry does not know");
    named = kw ? css_named_color((const char *)kw->name) : NULL;
    if (named) {
        out->r = ((named->rgb >> 16) & 0xff) / 255.0;
        out->g = ((named->rgb >> 8) & 0xff) / 255.0;
        out->b = (named->rgb & 0xff) / 255.0;
        out->a = 1.0;
        return true;
    }
    DFAIL("a <color> lexbor's grammar accepted is neither one of the forms above nor a keyword in CSS Color 4 "
          "§6.1's named-colour table — the grammar has gained a member the table and the cases above have not, "
          "so add it to whichever of the two it belongs to");
    return false;
}

/* ---- the two entry points ------------------------------------------------------------------------------------ */

static bool css_is_whitespace(char c)
{
    /* CSS Syntax's whitespace: newline (a CR or FF is preprocessed into an LF), tab, space. */
    return c == '\n' || c == '\r' || c == '\f' || c == '\t' || c == ' ';
}

bool css_color_parse(const char *text, size_t len, CssColor *out)
{
    static const char PREFIX[] = "color:";
    const size_t plen = sizeof PREFIX - 1;
    lxb_css_parser_t *parser;
    lxb_css_memory_t *mem;
    lxb_css_rule_declaration_list_t *list;
    char *buf;
    bool ok = false;

    DCHECK(out != NULL, "css color: a parse was asked for with nowhere to put the result");
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
            ok = css_color_to_srgb((const lxb_css_value_color_t *)d->u.color, out);
    }

    lxb_css_memory_destroy(mem, true);
    lxb_css_parser_destroy(parser, true);
    free(buf);
    return ok;
}

/* HTML §4.10.5.1.14 step 4.2's rounding: into the range 0 to 255 inclusive, to the nearest integer with a tie
   going towards +infinity — CSS Values 4's rule, which is `floor(x + 0.5)` and NOT C's `round`, whose tie goes
   away from zero and therefore answers -1.5 differently. This step is ALSO where the gamut clip happens, and
   the only place it is allowed to: a lab() or oklch() colour outside the sRGB gamut arrives here with a real
   component below 0 or above 1, and "in the range 0 to 255 inclusive" is what §4.10.5.1.14 does about it.
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

void css_color_serialize_html(const CssColor *c, char out[8])
{
    static const char HEX[] = "0123456789abcdef";
    int v[3], i;

    DCHECK(c->a == 1.0, "CSS Color 4 §16.2.1 states the HTML-compatible serialization only for an alpha of 1, "
                        "and a caller reaching it with any other alpha skipped the step that makes it 1 — for "
                        "HTML §4.10.5.1.14 that is `serialize a color well control color` step 3");
    v[0] = css_round_255(c->r);
    v[1] = css_round_255(c->g);
    v[2] = css_round_255(c->b);
    out[0] = '#';
    for (i = 0; i < 3; i++) {
        out[1 + i * 2] = HEX[(v[i] >> 4) & 0xf];
        out[2 + i * 2] = HEX[v[i] & 0xf];
    }
    out[7] = 0;
}
