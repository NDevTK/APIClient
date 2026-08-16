/* CSS Values and Units §5 — the `<length>` in CSS pixels. See css_length.h for why this is a component and why
   each unit group this engine cannot absolutize crashes with its OWN message. */
#include <ctype.h>
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
                  "css-sizing's `fit-content()`, or a `var()` substitution that produced one. css-values §10 "
                  "makes a math function a value in its "
                  "own right whose result is computed from operands in every unit this file knows and several "
                  "it crashes on, and lexbor parses one and serializes it back as TEXT, so it arrives here "
                  "looking like neither a dimension nor a keyword. BUILD the math-function grammar and its type "
                  "algebra: that is where `calc(100% - 2em)` splits into the PERCENTAGE half its caller "
                  "resolves against the containing block and the LENGTH half the two crashes below are about, "
                  "and it is also where css-variables' substitution has to have happened already");
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
              "models it, and Media Queries §4 already evaluates `(min-width: 40em)` against it — so what is "
              "missing here is not the value but the PLUMBING: a viewport is answered PER REALM (an iframe's is "
              "300 CSS pixels wide and the top-level traversable's is 1280), and this entry is reached through "
              "`css_resolved_value(element, property)`, which carries no JSContext. BUILD the realm through "
              "css_resolved_value into this component — the element's own document already knows it "
              "(document_active_realm_of) — and note that a value derived from the viewport inherits the "
              "viewport's CONCOLIC domain (viewport.h: the size is a PICKED environment fact and "
              "`viewport_env_value` is the one seam that mints it), so the resolved value stops being a plain "
              "`char *` at the same moment");
        return out;
    }
    DFAIL("a length-valued property's value is a DIMENSION in a unit CSS Values §5 does not define as a length "
          "at all — an angle, a time, a frequency or a resolution. A property whose grammar admits one of those "
          "is not a box-model length, so this is a caller asking the wrong component, or lexbor's own "
          "validation admitting a declaration its grammar rejects");
    return out;
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
