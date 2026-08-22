/* CSS COLOR — CSS Color Module Level 4's `<color>`: the PARSE, the conversion its callers ask for, and the
 * serializations a spec names.
 *
 * ONE PROBLEM: a string in, a colour out, and that colour back out as the string a spec names. It is its own
 * component because its callers are not one another's: HTML §4.10.5.1.14's colour well control sanitizes its
 * value through it, §4.10.5.1.14's constraint-validation clause decides SUFFERING FROM BAD INPUT by whether the
 * same parse returns failure, and the CSSOM and the canvas both reach for `<color>` on their own terms. Written
 * inside any one of them it would be that caller's private idea of what a colour is, and the first thing to go
 * wrong is the two disagreeing about which strings are colours at all.
 *
 * LEXBOR OWNS THE GRAMMAR, EXCEPT FOR THE ONE PRODUCTION IT DOES NOT HAVE. Lexbor has the real CSS tokenizer
 * and most of the real `<color>` production — hex, `rgb()`, `rgba()`, `hsl()`, `hsla()`, `hwb()`, `lab()`,
 * `lch()`, `oklab()`, `oklch()`, the named colours, the system colours, `transparent` and `currentcolor` —
 * reached through `lxb_css_property_state_color`, the state function its property registry runs for the `color`
 * property. What it does NOT have is §10.1's `color()` function: its own colour handler answers
 * `case LXB_CSS_VALUE_COLOR: default: *status = LXB_STATUS_OK; return false;`, so every `color(...)` string is
 * a parse failure there in either direction. That one production is therefore read HERE, over lexbor's own
 * tokenizer — the missing piece is the grammar rule, not the tokenization, so the escapes, comments, numbers
 * and percentages stay lexbor's. The two readers are DISJOINT by construction and asserted to be: a string
 * whose first token is the `color(` function goes only to this component's reader, everything else goes only to
 * lexbor's, and the reader asserts that lexbor still rejects what it accepted — the day that assert fires,
 * lexbor has grown the production and this component's copy of it is to be deleted.
 * What is NOT lexbor's either way is the numeric part — HSL and HWB are converted to sRGB by CSS Color 4 §7.1
 * and §8.1's own algorithms, the named colours by §6.1's own table, and Lab, LCH, Oklab and OkLCh by §11's
 * colour conversion in its own component (css_color_convert.c), all ported as data and arithmetic.
 *
 * A COLOUR KEEPS THE SPACE IT WAS WRITTEN IN, because §11 makes that observable: converting a colour to the
 * space it is already in is not a conversion at all, so its MISSING components (`none`) survive, while
 * converting between two different spaces replaces every missing component with zero. `color(srgb none 1 0)` in
 * a colour well whose serialization is sRGB therefore keeps its `none`, and the same colour in a Display P3
 * well does not. Resolving `none` at parse time would delete that distinction before any caller could ask.
 * THE ONE EXCEPTION IS THE LAB FAMILY, and it is a deliberate one: `lab()`, `lch()`, `oklab()` and `oklch()`
 * are converted to sRGB by the parse. Their conversion is §11's, performed early — which is sound precisely
 * because every destination this component can be asked for is one of §10's predefined spaces, all nine of
 * which differ from all four of theirs, so §11 would replace their missing components with zero at whatever
 * later moment the conversion happened. The day a caller needs a lab() colour serialized AS `lab()` (CSSOM's
 * computed-value serialization is the one that will), the four spaces join CssColorSpace and the conversion
 * moves to the point of use; until then the four have no representation here and no way to be asked for. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_COLOR_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_COLOR_H
#include <stdbool.h>
#include <stddef.h>

#include "core/css/css_color_convert.h"

/* A `<color>`: three components in `space`, an alpha, and which of the four are MISSING.
   The components are NOT clamped into the [0, 1] reference range, because a colour that came from lab(),
   lch(), oklab(), oklch() or a wider `color()` space can legitimately lie outside the destination's gamut and
   §11 preserves that: it gamut maps only when the destination "is a physical output color space, such as a
   display", which a used value is not. The clip is the SERIALIZATION's, and each caller's spec says where —
   HTML §4.10.5.1.14 step 4.2 rounds "into the range 0 to 255 inclusive", while §16.5's `color()` form keeps the
   out-of-gamut number, which is why HTML's own example of a Display P3 well's value is
   `color(display-p3 1.84 -0.19 0.72 / 0.6)`.
   `a` IS clamped to [0, 1] — CSS Color 4 §16.1.2: "<alpha-value>s which were specified outside the valid range
   are clamped at parse time".
   `missing` is CSS Color 4 §4.4's missing components, the `none` keyword: a missing component "behaves as a
   zero value" for every purpose except two — §13.2's interpolation, and its own serialization, which "represents
   that component as being the none keyword". The zero is therefore stored in `c`/`a` as well, so nothing that
   computes with a colour has to consult the bitmap; only §11's conversion and the serializations do. */
typedef struct {
    CssColorSpace space;
    double c[3];
    double a;
    unsigned missing;
} CssColor;

#define CSS_COLOR_MISSING_COMPONENT(i)  (1u << (unsigned)(i))
#define CSS_COLOR_MISSING_ALPHA         (1u << 3)

/* CSS Color 4's "opaque black" — `rgb(0 0 0 / 100%)`, the colour §4.10.5.1.14 falls to when the parse fails. */
extern const CssColor CSS_COLOR_OPAQUE_BLACK;

/* CSS Color 4 §4.5's "PARSE A CSS `<color>` VALUE", given a string and NO context element: parse the input as a
   `<color>`, and resolve it to a used colour. Answers false on failure — which is the answer HTML's colour well
   control turns into opaque black and its constraint validation turns into SUFFERING FROM BAD INPUT.
   The optional context element is not a parameter because no caller has one to pass: §4.10.5.1.14 calls this
   algorithm with the string alone, so `currentcolor` and the system colours resolve against the initial values
   of the properties rather than against an element — `currentcolor` to the `color` property's initial value,
   CanvasText, and each `<system-color>` to this UA's own theme in css_system_color.h. */
bool css_color_parse(const char *text, size_t len, CssColor *out);

/* CSS Color 4 §11's CONVERTING COLORS, in place. Converting a colour to the space it is already in is NOT a
   conversion — §11 defines the algorithm only "where src and dest are different" — so it is an identity here,
   which is what keeps a missing component missing. Any other destination replaces every missing COMPONENT with
   zero (§11 step 2) and runs the pipeline in css_color_convert.c. The ALPHA is carried across untouched,
   missing or not: §11's steps are the source and destination colour spaces' transfer functions, matrices and
   white points, and alpha belongs to none of them. */
void css_color_convert(CssColor *c, CssColorSpace dest);

/* HTML §4.10.5.1.14's `serialize a color well control color` step 4.2: "Round each of color's components so
   they are in the range 0 to 255, inclusive. Components are to be rounded towards +∞" — the 8-bit quantization
   the Limited sRGB state's name refers to, applied to the three components and not to the alpha. This is also
   the one place the sRGB gamut clip happens, and the only place it is allowed to: a lab() or oklch() colour
   outside the gamut arrives with a component below 0 or above 1, and "in the range 0 to 255 inclusive" is what
   §4.10.5.1.14 does about it.
   A missing component stays missing: rounding it is a computation on its value, which §4.4 says "behaves as a
   zero value", and §4.4's other sentence — that a serialized missing component is still the `none` keyword —
   is unaffected by having computed with the zero. */
void css_color_quantize_8bit(CssColor *c);

/* CSS Color 4 §16.2.1's HTML-COMPATIBLE SERIALIZATION of an sRGB value: "#" followed by the two-digit lowercase
   hexadecimal representations of the red, green and blue components. Writes seven characters and a NUL.
   The colour must already be sRGB and already through §4.10.5.1.14 step 4.2's rounding — both are asserted,
   because this form has no way to express a component this step has not already put in 0..255.
   §16.2.1 states the form only for an alpha of 1, so an alpha other than 1 is a caller that skipped the step
   that makes it 1. A MISSING component is written as its zero: §16.2's "any missing values are converted to 0
   if the chosen serialization form cannot represent the none keyword", and this form cannot. */
void css_color_serialize_html(const CssColor *c, char out[8]);

/* The longest string §16.5's form can produce, plus its NUL: the longest colour space name, four components
   each of which is a `<number>` serialized in base ten with no exponent — so up to DBL_MAX's 309 integer
   digits, a sign, a point and six decimals — and the separators. */
#define CSS_COLOR_FUNCTION_MAX 1400

/* CSS Color 4 §16.5's SERIALIZATION OF THE `color()` FUNCTION: "color(", the ASCII-lowercase colour space name,
   the three components separated by single spaces, and — only when the alpha is not 1 — " / " and the alpha.
   Every number is base ten with the "." decimal separator, six decimal places, trailing fractional zeroes
   omitted along with a then-empty decimal point, and rounded towards +∞ rather than truncated. Six is the
   figure §16.1.2 states for the alpha, and it satisfies §16.5's own minimum round-trip precision for every one
   of the predefined spaces (the widest of which, the XYZ spaces, asks for 16 bits). A missing component is
   written as the `none` keyword, which is what this form exists to be able to do.
   Returns the length written, not counting the NUL. */
size_t css_color_serialize_function(const CssColor *c, char out[CSS_COLOR_FUNCTION_MAX]);

/* CSS Color 4 §16.2's SERIALIZATION OF AN sRGB VALUE — the LEGACY comma form, `rgb(255, 0, 0)` or
   `rgba(255, 0, 0, 0.5)`, which is what a COMPUTED or USED colour serializes as and what a page comparing
   against getComputedStyle reads. §16.2.2 routes a colour with a MISSING component to the `color(srgb ...)`
   form instead, because the comma form cannot write `none`, so the answer is not always an `rgb()` and the
   buffer is the same one that form needs. The colour must already be sRGB (§11's conversion is the caller's).
   Returns the length written, not counting the NUL. */
size_t css_color_serialize_srgb(const CssColor *c, char out[CSS_COLOR_FUNCTION_MAX]);

#endif
