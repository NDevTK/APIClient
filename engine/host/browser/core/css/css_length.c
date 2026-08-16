/* CSS Values and Units §5 — the `<length>` in CSS pixels. See css_length.h for why this is a component and why
   each unit group this engine cannot absolutize crashes with its OWN message. */
#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/css/css_length.h"

static char *css_len_strdup(const char *s)
{
    char *out = strdup(s);
    CHECK(out != NULL, "cssom: OOM serializing a used length — a dropped one would read as undeclared");
    return out;
}

/* ---- the CSS pixel, and the environment fact it may derive from (see css_length.h) ------------------------ */

CssPx css_px(double px)
{
    CssPx out = { px, CSS_ENV_NONE, NULL };

    return out;
}

CssPx css_px_env(CssEnvFact fact, JSContext *realm, double px)
{
    CssPx out = { px, fact, realm };

    DCHECK(fact != CSS_ENV_NONE && realm != NULL,
           "a length was said to derive from an ENVIRONMENT FACT with no fact or no realm to name it — the two "
           "travel together because the mint at the JS boundary needs both (which viewport, and which of its "
           "dimensions), and a length carrying one without the other could not be turned into a domain");
    return out;
}

/* The one place two lengths' facts meet, so the one place the pair can be asserted. */
static CssPx css_px_combine(CssPx a, CssPx b, double px)
{
    CssPx out = { px, a.env, a.realm };

    if (a.env == CSS_ENV_NONE) { out.env = b.env; out.realm = b.realm; return out; }
    if (b.env == CSS_ENV_NONE) return out;
    DCHECK(a.env == b.env && a.realm == b.realm,
           "two lengths derived from DIFFERENT environment facts were combined into one. A concolic carries ONE "
           "source identity, so the result's domain would be a relation over a PAIR of facts — a `width: 50vh` "
           "inside a percentage-margined box is a function of both viewport axes at once — and keeping either "
           "one alone would report a narrowing the page never made. BUILD the multi-fact domain in "
           "solver/concolic.h, or resolve the second fact before it reaches this arithmetic");
    return out;
}

CssPx css_px_add(CssPx a, CssPx b)   { return css_px_combine(a, b, a.px + b.px); }
CssPx css_px_sub(CssPx a, CssPx b)   { return css_px_combine(a, b, a.px - b.px); }

CssPx css_px_scale(CssPx a, double k)
{
    CssPx out = a;

    out.px = a.px * k;
    return out;
}

/* THE LARGER EXAMPLE, CARRYING BOTH OPERANDS' FACT — which is why this is `css_px_combine` and not `a.px > b.px
   ? a : b`. Returning the winner whole would drop the loser's fact, and the loser is the arm the OTHER viewport
   takes: css-sizing §5's floor picks the declared length at 1280 and the four-term surround at 320, so a result
   that kept only the winner would report a length as environment-independent in exactly the world where it is
   not. Over-reporting the dependence forks a world the page might not have needed; under-reporting it deletes
   one it did, and CLAUDE.md's §Headless errs toward the first. */
CssPx css_px_max(CssPx a, CssPx b)
{
    return css_px_combine(a, b, a.px > b.px ? a.px : b.px);
}

/* CSS Values §5.2's ABSOLUTE UNITS, each as the spec's own definition of it in CSS pixels: "1in = 96px" is the
   anchor and the other five are exact fractions of the inch (1cm = 96/2.54, 1mm = 96/25.4, 1Q = 96/101.6,
   1pt = 96/72, 1pc = 16). Nothing here is a device measurement, which is why §Headless does not reach it. */
static const struct { const char *unit; double px; } CSS_ABSOLUTE[] = {
    { "px", 1.0 },
    { "in", 96.0 },
    { "cm", 96.0 / 2.54 },
    { "mm", 96.0 / 25.4 },
    { "q",  96.0 / 101.6 },
    { "pt", 96.0 / 72.0 },
    { "pc", 16.0 },
};

/* §5.1.1's FONT-RELATIVE units, listed so the crash can name the ONE component whose absence covers all of
   them — a computed `font-size`, which needs the cascade's inheritance step. */
static const char *const CSS_FONT_RELATIVE[] = {
    "em", "ex", "ch", "rem", "cap", "ic", "lh", "rlh",
};

/* §5.1.2's VIEWPORT-PERCENTAGE units. Separate, because what is missing for them is the REALM the viewport is
   answered per, not the value. */
static const char *const CSS_VIEWPORT_RELATIVE[] = {
    "vw", "vh", "vi", "vb", "vmin", "vmax",
};

static bool css_len_in(const char *const *set, unsigned n, const char *unit)
{
    unsigned i;

    for (i = 0; i < n; i++)
        if (strcmp(set[i], unit) == 0) return true;
    return false;
}

/* THE DIMENSION'S UNIT, lowercased — CSS Syntax §4 makes a unit ASCII case-insensitive, and lexbor serializes
   the ident as the author wrote it, so `4PX` and `4px` arrive here as different bytes and are one value. */
#define CSS_LEN_UNIT_MAX 8

static bool css_len_unit_of(const char *s, char *out)
{
    size_t i;

    for (i = 0; s[i] != '\0'; i++) {
        if (i + 1 >= CSS_LEN_UNIT_MAX) return false;
        out[i] = (char)tolower((unsigned char)s[i]);
    }
    out[i] = '\0';
    return true;
}

CssLength css_length_parse(const char *value)
{
    CssLength out = { CSS_LENGTH_KEYWORD, 0.0, 0.0 };
    char unit[CSS_LEN_UNIT_MAX];
    const char *p = value;
    char *end = NULL;
    double num;
    unsigned i;

    DCHECK(value != NULL, "a CSS length was parsed from a NULL value — the cascade answers every property this "
                          "engine models, so a NULL here is a cascade that stopped early");
    while (*p != '\0' && isspace((unsigned char)*p)) p++;
    num = strtod(p, &end);
    /* NOT A NUMBER AT ALL. Either a KEYWORD — `auto`, `none`, `min-content`, `normal` — whose text every
       caller that cares compares for itself (inventing an enum of every keyword every length-valued property
       admits would be a second copy of lexbor's own grammar), or a FUNCTION, which is a different thing and is
       told apart by the one character that distinguishes them in CSS Syntax's own tokenizer. */
    if (end == p) {
        if (strchr(p, '(') != NULL)
            DFAIL("a length-valued property's value is a FUNCTION — `calc()`, `min()`, `max()`, `clamp()`, "
                  "css-sizing's `fit-content()`, a `var()` substitution that produced one, or a function that "
                  "is not a length at all and whose declaration the grammar should have DROPPED (`border-width: "
                  "rgb(1, 2, 3)` reaches here through css_length_is_length, which cannot tell them apart for "
                  "the same reason). css-values §10 "
                  "makes a math function a value in its "
                  "own right whose result is computed from operands in every unit this file knows and several "
                  "it crashes on, and lexbor parses one and serializes it back as TEXT, so it arrives here "
                  "looking like neither a dimension nor a keyword. BUILD the math-function grammar and its type "
                  "algebra: that is where `calc(100% - 2em)` splits into the PERCENTAGE half its caller "
                  "resolves against the containing block and the LENGTH half the two crashes below are about, "
                  "it is what tells a math function from `rgb()`, and it is also where css-variables' "
                  "substitution has to have happened already");
        return out;
    }
    while (*end != '\0' && isspace((unsigned char)*end)) end++;
    if (*end == '\0') {
        /* §5.1: "for zero lengths the unit identifier is optional". A unitless NON-zero is not a length in any
           property this engine models — it is `line-height: 1.5`, whose own <number> grammar is that property's
           and not this one's. */
        DCHECK(num == 0.0,
               "a length-valued property's value is a UNITLESS NON-ZERO NUMBER. CSS Values §5.1 permits the "
               "unit to be omitted only for zero, so this is either a property whose grammar admits a bare "
               "<number> (`line-height`, `zoom`) reaching a caller that asked for a length, or a serializer "
               "that dropped a unit");
        out.kind = CSS_LENGTH_ABSOLUTE;
        out.px = 0.0;
        return out;
    }
    if (*end == '%' && end[1] == '\0') {
        out.kind = CSS_LENGTH_PERCENTAGE;
        out.pct = num;
        return out;
    }
    if (!css_len_unit_of(end, unit)) {
        DFAIL("a length-valued property's value carries a DIMENSION whose unit is longer than any CSS unit "
              "identifier — CSS Values §5 names none longer than four characters, so this is a value that is "
              "not a dimension reaching the dimension arm");
        return out;
    }
    for (i = 0; i < sizeof(CSS_ABSOLUTE) / sizeof(CSS_ABSOLUTE[0]); i++) {
        if (strcmp(CSS_ABSOLUTE[i].unit, unit) != 0) continue;
        out.kind = CSS_LENGTH_ABSOLUTE;
        out.px = num * CSS_ABSOLUTE[i].px;
        return out;
    }
    if (css_len_in(CSS_FONT_RELATIVE, sizeof(CSS_FONT_RELATIVE) / sizeof(CSS_FONT_RELATIVE[0]), unit)) {
        DFAIL("a FONT-RELATIVE length (`em`, `ex`, `ch`, `rem`, …) reached CSS Values §5.1.1's absolutization. "
              "Each of them resolves against the element's own COMPUTED `font-size` (`rem` against the root "
              "element's), and this engine's cascade has NO INHERITANCE STEP — css_style_declaration.c resolves "
              "inline, then the author rules, then the UA sheet, then the property's initial value, and never "
              "takes a value from the parent — so `font-size` is an INHERITED property that no element has a "
              "computed value for. media_query.c resolves `em` against the INITIAL font size and is right to: "
              "Media Queries §4 evaluates a query before any element exists to have one, so there the initial "
              "value IS the spec's answer. Borrowing that number here would make it a stand-in for the missing "
              "chain. BUILD the cascade's defaulting and inheritance step (css_computed_value.c's CSS-wide-"
              "keyword DFAIL names the same one), then `font-size`'s own computed value, which is where the "
              "percentage-of-the-parent and the `smaller`/`larger` keywords are resolved");
        return out;
    }
    if (css_len_in(CSS_VIEWPORT_RELATIVE,
                   sizeof(CSS_VIEWPORT_RELATIVE) / sizeof(CSS_VIEWPORT_RELATIVE[0]), unit)) {
        DFAIL("a VIEWPORT-PERCENTAGE length (`vw`, `vh`, `vmin`, `vmax`) reached CSS Values §5.1.2's "
              "absolutization. The viewport this resolves against EXISTS in this engine — core/frame/viewport.h "
              "models it, Media Queries §4 already evaluates `(min-width: 40em)` against it, and a USED value "
              "derived from it now carries its domain the whole way to the page (CssPx above, minted at the JS "
              "boundary by viewport.h's initial-containing-block seam). What is missing is ONE LAYER UP: a "
              "viewport-percentage absolutizes at COMPUTED-VALUE time — `width`'s own `Computed value:` line is "
              "'the percentage as specified or the absolute length' — and css_computed_value.c's entry answers "
              "TEXT (`char *css_computed_value(element, property)`), which carries neither the realm the "
              "viewport is answered per (an iframe's is 300 CSS pixels wide and the top-level traversable's is "
              "1280) nor anywhere to put the fact. A `50vw` computed here would serialize to `640px` and every "
              "reader downstream would see a number the environment PICKED with nothing left to say so. BUILD "
              "the computed-value path over CssPx: the element's own document already knows the realm "
              "(document_active_realm_of), and the readers that want text are CSSOM's serializers, which is "
              "where the used value already turns back into one");
        return out;
    }
    DFAIL("a length-valued property's value is a DIMENSION in a unit CSS Values §5 does not define as a length "
          "at all — an angle, a time, a frequency or a resolution. A property whose grammar admits one of those "
          "is not a box-model length, so this is a caller asking the wrong component, or lexbor's own "
          "validation admitting a declaration its grammar rejects");
    return out;
}

/* Is `unit` (already lowercased) one CSS Values §5 defines as a LENGTH unit — absolute, font-relative or
   viewport-percentage? All three tables, because §5's production admits all three and the two this engine
   cannot absolutize are a missing component rather than a syntax error. */
static bool css_len_unit_known(const char *unit)
{
    unsigned i;

    for (i = 0; i < sizeof(CSS_ABSOLUTE) / sizeof(CSS_ABSOLUTE[0]); i++)
        if (strcmp(CSS_ABSOLUTE[i].unit, unit) == 0) return true;
    return css_len_in(CSS_FONT_RELATIVE, sizeof(CSS_FONT_RELATIVE) / sizeof(CSS_FONT_RELATIVE[0]), unit) ||
           css_len_in(CSS_VIEWPORT_RELATIVE,
                      sizeof(CSS_VIEWPORT_RELATIVE) / sizeof(CSS_VIEWPORT_RELATIVE[0]), unit);
}

bool css_length_is_length(const char *value)
{
    char unit[CSS_LEN_UNIT_MAX];
    const char *p = value;
    char *end = NULL;

    DCHECK(value != NULL, "the `<length>` grammar was asked about a NULL value — a declaration carries a value "
                          "by the time lexbor has serialized it, and an absent one is a caller that lost it");
    while (*p != '\0' && isspace((unsigned char)*p)) p++;
    (void)strtod(p, &end);
    /* Not a number token at all: a FUNCTION is the one remaining production a `<length>` can take (see the
       header), and every other spelling — a keyword, an identifier, a string — is not one. */
    if (end == p) return strchr(p, '(') != NULL;
    while (*end != '\0' && isspace((unsigned char)*end)) end++;
    if (*end == '\0') return true;    /* §5.1's unitless zero; the parse above asserts on a unitless non-zero */
    if (*end == '%') return false;    /* a `<percentage>` is a sibling production, never a `<length>` */
    if (!css_len_unit_of(end, unit)) return false;
    return css_len_unit_known(unit);
}

double css_length_snap_line_width(double px)
{
    /* The DEVICE PIXEL every step of the algorithm is stated in. This engine models a `devicePixelRatio` of 1
       (core/frame/viewport.h says why that is a UA choice and not an unknown), so one device pixel is one CSS
       pixel and a whole number of CSS pixels is already snapped — at that ratio and at every other INTEGER
       one, which is what makes this arm independent of a value this entry cannot reach. */
    double whole = trunc(px);

    DCHECK(px == px && px * 0.0 == 0.0,
           "CSS Values §6's snap-as-a-border-width was handed a length that is not a FINITE number — every "
           "border width reaching it is an absolute length off a real declaration or one of §3.3's three "
           "keywords, so a NaN or an infinity is a derivation that lost an operand");
    if (px == whole) return px;
    DFAIL("CSS Values §6 SNAPS a border width to a whole number of DEVICE PIXELS — away from zero to one when "
          "the length is between zero and a single device pixel, towards zero otherwise — and this width is "
          "not a whole number of CSS pixels, so the answer depends on how many device pixels one CSS pixel is. "
          "core/frame/viewport.h MODELS that (`viewport_device_pixel_ratio`, a modelled 1.0) and it is a "
          "forkable environment SOURCE there, not a constant: `devicePixelRatio > 1` is the retina gate, and a "
          "computed border width derived from it carries that domain. Neither half is reachable from here, and "
          "for the SAME reason the viewport-percentage arm above gives: this entry arrives through "
          "css_computed_value.c's border-width rule, whose whole path is `char *`, so there is no realm to ask "
          "and nowhere to put the fact. BUILD the computed-value path over CssPx — and with it a `CssEnvFact` "
          "for the device pixel ratio, which is a SECOND fact beside the initial containing block's two "
          "dimensions and is exactly the case css_length.h says one source key cannot spell alone");
    return px;
}

char *css_length_serialize_px(double px)
{
    char num[64], out[72];
    int prec;

    DCHECK(px == px && px * 0.0 == 0.0,
           "a used length that is not a FINITE number reached CSSOM §6.7.2's serializer. Every length this "
           "engine reports is an absolute length off a real declaration or arithmetic over such lengths, so a "
           "NaN or an infinity is a derivation that lost an operand rather than a value to print");
    /* css-values §serializing a `<number>`: the SHORTEST form that round-trips. Printed at rising precision
       until strtod gives the value back, which is that rule stated directly rather than a fixed `%g` that
       would report `4.0000000000000004px` for one length and truncate the next. */
    for (prec = 1; prec <= 17; prec++) {
        snprintf(num, sizeof num, "%.*g", prec, px);
        if (strtod(num, NULL) == px) break;
    }
    DCHECK(prec <= 17, "a double did not round-trip through 17 significant digits, which is more than IEEE 754 "
                       "binary64 needs — the printer and the parser disagree");
    DCHECK(strchr(num, 'e') == NULL && strchr(num, 'E') == NULL,
           "CSSOM serialized a number in EXPONENT form, which CSS's number grammar has no notation for — a "
           "used length of this magnitude is a derivation that produced a coordinate no layout could hold");
    snprintf(out, sizeof out, "%spx", num);
    return css_len_strdup(out);
}
