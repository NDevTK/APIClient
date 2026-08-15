/* CSS COLOR — CSS Color Module Level 4's `<color>`: the PARSE, and the serialization HTML asks of it.
 *
 * ONE PROBLEM: a string in, a colour out, and that colour back out as the string a spec names. It is its own
 * component because its callers are not one another's: HTML §4.10.5.1.14's colour well control sanitizes its
 * value through it, §4.10.5.1.14's constraint-validation clause decides SUFFERING FROM BAD INPUT by whether the
 * same parse returns failure, and the CSSOM and the canvas both reach for `<color>` on their own terms. Written
 * inside any one of them it would be that caller's private idea of what a colour is, and the first thing to go
 * wrong is the two disagreeing about which strings are colours at all.
 *
 * LEXBOR OWNS THE GRAMMAR. It has the real CSS tokenizer and the real `<color>` production — hex, `rgb()`,
 * `rgba()`, `hsl()`, `hsla()`, `hwb()`, `lab()`, `lch()`, `oklab()`, `oklch()`, the named colours, the system
 * colours, `transparent` and `currentcolor` — reached through `lxb_css_property_state_color`, the state
 * function its property registry runs for the `color` property. So this file parses a DECLARATION whose value
 * is the input and reads the `lxb_css_value_color_t` back out: the entry lexbor exposes is the declaration
 * parser, and going through it is what makes the grammar lexbor's rather than a second one written here.
 * What is NOT lexbor's is the numeric part — HSL and HWB are converted to sRGB by CSS Color 4 §7.1 and §8.1's
 * own algorithms, the named colours by §6.1's own table, and Lab, LCH, Oklab and OkLCh by §12's colour
 * conversion in its own component (css_color_convert.c), all ported as data and arithmetic, not re-parsed.
 *
 * A COLOUR IS RESOLVED TO sRGB, because that is where every caller's serialization starts: §4.10.5.1.14
 * converts to 'srgb' before rounding, and §16.2.1's HTML-compatible form is defined only for sRGB values. A
 * `none` component (CSS Color 4's missing components) is resolved to 0 HERE, which is both §4.4's rule for
 * converting a colour to another colour space and §16.2's rule for a serialization form that cannot represent
 * `none` — the only form this component performs. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_COLOR_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_COLOR_H
#include <stdbool.h>
#include <stddef.h>

/* A `<color>` in the EXTENDED sRGB colour space. `a` is in [0, 1] — CSS Color 4 clamps alpha at parse time —
   but `r`, `g` and `b` are NOT clamped into the [0, 1] reference range, because a colour that came from lab(),
   lch(), oklab() or oklch() can legitimately lie outside the sRGB gamut and §12 preserves that: it gamut maps
   only when the destination "is a physical output color space, such as a display", which a used value is not.
   The clip is the SERIALIZATION's, and each caller's spec says where — HTML §4.10.5.1.14 step 4.2 rounds "into
   the range 0 to 255 inclusive", while §16.5's `color()` form keeps the out-of-gamut number. */
typedef struct {
    double r, g, b;
    double a;
} CssColor;

/* CSS Color 4's "opaque black" — `rgb(0 0 0 / 100%)`, the colour §4.10.5.1.14 falls to when the parse fails. */
extern const CssColor CSS_COLOR_OPAQUE_BLACK;

/* CSS Color 4's "PARSE A CSS `<color>` VALUE", given a string and NO context element: parse the input as a
   `<color>`, and resolve it to a used colour. Answers false on failure — which is the answer HTML's colour well
   control turns into opaque black and its constraint validation turns into SUFFERING FROM BAD INPUT.
   The optional context element is not a parameter because no caller has one to pass: §4.10.5.1.14 calls this
   algorithm with the string alone, so `currentcolor` and the system colours resolve against the initial values
   of the properties rather than against an element — `currentcolor` to the `color` property's initial value,
   CanvasText, and each `<system-color>` to this UA's own theme in css_system_color.h. */
bool css_color_parse(const char *text, size_t len, CssColor *out);

/* CSS Color 4 §16.2.1's HTML-COMPATIBLE SERIALIZATION of an sRGB value: "#" followed by the two-digit lowercase
   hexadecimal representations of the red, green and blue components. Writes seven characters and a NUL.
   The components are rounded into 0..255 the way HTML §4.10.5.1.14 step 4.2 requires — to the nearest integer,
   ties towards +infinity, which is css-values-4's rounding rule. §16.2.1 states the form only for an alpha of
   1, so an alpha other than 1 is a caller that skipped the step that makes it 1. */
void css_color_serialize_html(const CssColor *c, char out[8]);

#endif
