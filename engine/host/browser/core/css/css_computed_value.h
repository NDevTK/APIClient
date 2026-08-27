/* CSS Cascade §Computed Value — the C entry — and CSSOM §9's RESOLVED VALUE, which is a different thing.
 *
 * TWO ANSWERS, AND CONFLATING THEM IS A FIDELITY BUG WITH NO SYMPTOM. The cascade in css_style_declaration.c
 * produces the SPECIFIED value: the declaration that won, as text. Two different consumers want two different
 * things from it and neither wants that:
 *
 *   A C COMPONENT asking a spec question wants the COMPUTED VALUE. Every one of CSSOM VIEW §6's algorithms
 *   reads computed values and nothing else — "potentially scrollable" reads the computed `overflow-x`/
 *   `overflow-y` of the body and of its parent, `clientTop` reads the computed `border-top-width`,
 *   `getClientRects`' constraints read the computed `display` and `transform`. `css_computed_value` is that
 *   entry, and it is EXACT: it names the properties whose computed value it derives, it asserts that the
 *   shorthands which can set them are all expanded (css_shorthand.h), and it CRASHES for a property it does
 *   not model rather than handing back a specified value wearing the word "computed".
 *
 *   getComputedStyle() wants the RESOLVED VALUE, which CSSOM §9 defines as the computed value for MOST
 *   properties and the USED value for a named set — the box-model lengths (`width`, `height`, the margins and
 *   paddings), the insets of a positioned box, the colors, `line-height`, and the special cases other specs
 *   declare (`transform` resolves to a matrix). A used value is what a LAYOUT produced, and CSS 2.1 §10 is
 *   that layout: core/layout/used_value.h owns it and computes every arm of §10 that needs no INTRINSIC SIZE —
 *   §10.1's containing block and §10.3.3's constraint equation included — crashing by SECTION for the arms
 *   that need the box's own content measured with a real font. What is NOT a layout question is §9's OTHER conjunct — whether
 *   the property APPLIES to the element at all — and core/css/css_property_applies.h answers it from the
 *   property's own `Applies to:` line, which is why `getComputedStyle(span).width` reports the computed `auto`
 *   that every user agent reports rather than reaching for a box.
 *   §9's escapes are real branches and are taken: a `display: none` element's box-model lengths resolve to
 *   their COMPUTED values, a statically positioned element's insets do too, and so does every property that
 *   does not apply to the element.
 *
 * A COMPUTED VALUE IS NOT TEXT, AND WHICH OF THE TWO ENTRIES ANSWERS IS THE PROPERTY'S OWN `Computed value:`
 * LINE. Half the properties this file models have a line that says "specified keyword" — `display`, `float`,
 * `position`, `box-sizing`, `border-*-style`, and css-overflow's one computed-value rule — and a keyword IS its
 * text, so `css_computed_value` answers a `char *` for those and asserts it was not asked for a length. The
 * other half's line is "the percentage as specified or THE ABSOLUTE LENGTH", and an absolute length is where
 * CSS 2.1 §4.3.2's absolutization happens: `50vw` becomes a number here, out of the INITIAL CONTAINING BLOCK
 * (css-values §6.1.2), and a `border-*-width` becomes a whole number of DEVICE PIXELS here (css-backgrounds-3
 * §3.3's "snapped as a border width"). Both of those rectangles are PICKED environment facts core/frame/
 * viewport.h models, so the answer is a `CssLength` whose absolute arm is a `CssPx` — the example plus the fact
 * it derives from — and `css_computed_length` is that entry. Serializing it to text first is what the `char *`
 * path did, and it dropped the fact on the floor: `getComputedStyle(el).width` compared against 768 is the same
 * responsive gate as `innerWidth < 768`, and `devicePixelRatio > 1` is the same retina gate whether a page
 * asks the member or measures a border.
 *
 * WHAT IS STILL SPECIFIED-VALUE-SHAPED, AND THE MECHANISM THAT ENDS IT. A property this file does not model
 * takes its computed value to BE its specified value, which is what the majority of CSS properties' "Computed
 * value:" line says and what the length-valued ones' does not. The gap is not a judgement call to be made per
 * property by hand — every property definition in every CSS spec carries that line, `@webref/css` publishes it
 * as a `computedValue` field, and engine/idlgen.mjs already reads that package's IDL sibling. Wiring the CSS
 * half in the same way turns this file's default arm from an assumption into an assertion.
 *
 * BOTH ENTRIES RUN CSS Cascade §7's DEFAULTING FIRST, and that step is core/css/css_defaulting.h's. What the
 * cascade answers is the value of the declaration that WON, which for almost every property on almost every
 * element is no declaration at all; §7 is what turns that into the property's initial value or into the value
 * the PARENT computed, and §7.3's CSS-wide keywords are what let a declaration ask for either by name. The
 * fetch is performed HERE rather than there because the inherited value is the parent's COMPUTED value and
 * this file is what computes one — and because it comes in two shapes: `char *` for a keyword-valued property,
 * `CssLength` for a length-valued one, whose absolute arm carries the environment fact it derives from and
 * would lose it if it were handed down the tree as text. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_COMPUTED_VALUE_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_COMPUTED_VALUE_H
#include <stdbool.h>
#include <stdint.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_defaulting.h"
#include "core/css/css_length.h"
#include "quickjs.h"

/* THE COMPUTED VALUE of a KEYWORD-VALUED property on `el`, as text. OWNED: the caller frees. `name` must be one
   of the properties this component models and must NOT be one whose computed value is a length — the two
   entries are split by the property's own `Computed value:` line (see the header above), and each crashes when
   asked the other's question rather than answering it in a shape that cannot carry the answer. */
char *css_computed_value(lxb_dom_element_t *el, const char *name);

/* THE COMPUTED VALUE of a LENGTH-VALUED property on `el` — CSS 2.1 §8.3, §8.4, §10.2 and §10.5's one line,
   "the percentage as specified or the absolute length", plus css-backgrounds-3 §3.3's snapped border widths.
   Every relative unit is ABSOLUTIZED here (that is what makes this the computed value rather than the specified
   one), against the realm the ELEMENT'S OWN DOCUMENT is the active document of — never the running realm, since
   an iframe's initial containing block is 300 CSS pixels wide and its parent's is 1280. The absolute arm is a
   `CssPx` and carries the environment fact it derives from; the percentage and keyword arms carry the number
   and the text, as specified. Nothing is owned: the keyword rides the struct. */
CssLength css_computed_length(lxb_dom_element_t *el, const char *name);

/* Does this component DERIVE `name`'s computed value from the cascade's specified value? */
bool css_computed_models(const char *name);

/* Is it a LENGTH — so `css_computed_length` is the entry and `css_computed_value` is not? */
bool css_computed_models_length(const char *name);

/* The element's BOX PARENT's `display` — the nearest ancestor element that GENERATES a box, which is not the
   parent element when a `display: contents` ancestor sits between them. OWNED, or NULL at the root. Exported
   because it is the question "is this box a flex or grid ITEM", and TWO algorithms ask it: CSS Display §2.7's
   blockification here, and CSS 2.1 §10.3's box-type split in core/layout/used_value.c — a flex item's size is
   its container's algorithm and not §10's, so a used value derived from §10 for one would be an answer from
   the wrong section. `n` must be an element's node. */
char *css_box_parent_display(const lxb_dom_node_t *n);

/* CSSOM §9's own split, per property. */
typedef enum {
    CSS_RESOLVED_COMPUTED = 0,      /* "Any other property": the resolved value is the computed value */
    CSS_RESOLVED_USED,              /* the colors, box-shadow: the resolved value is the used value */
    CSS_RESOLVED_USED_IF_RENDERED,  /* width/height/margin/padding: used, unless display is none or contents */
    CSS_RESOLVED_USED_IF_POSITIONED,/* the insets: used, unless the element is not positioned */
    CSS_RESOLVED_LINE_HEIGHT,       /* `normal` if the computed value is normal, the used value otherwise */
    CSS_RESOLVED_TRANSFORM          /* css-transforms §3.2: a <transform-list> resolves to one matrix() */
} CssResolvedKind;

CssResolvedKind css_resolved_kind(const char *name);

/* CSSOM §9's RESOLVED VALUE of `name` on `el` — what getComputedStyle() answers. Any property name, including
   a custom one; a name no cascade layer answers is the EMPTY STRING, which is §6.6.1's own answer for a
   property that is not set and not a hole where a value would be.
   IT IS A `JSValue` AND NOT A `char *`, AND THAT IS THE POINT OF THE ENTRY RATHER THAN A DETAIL OF IT. §9 makes
   the resolved value of the box-model lengths the USED value, CSS 2.1 §10.1 makes the base case of every used
   width the INITIAL CONTAINING BLOCK, and core/frame/viewport.h makes the ICB's dimensions a PICKED
   environment fact — so the string this returns for `width` may be the example of a CONCOLIC whose domain is
   the viewport's, and `parseInt(getComputedStyle(el).width) < 768` forks the same two worlds `innerWidth < 768`
   does. A `char *` could carry the number and not the domain, and the arm behind that gate would be deleted
   with nothing to say so. `ctx` is the CALLER's realm — the string is created in it — while the realm the ICB
   is answered per is the ELEMENT's document's, which core/layout/used_value.c reads for itself. */
JSValue css_resolved_value(JSContext *ctx, lxb_dom_element_t *el, const char *name);

/* css-inline-3 §5.1 "Line Spacing: the line-height property" — THE THIRD SHAPE A COMPUTED VALUE COMES IN, and
 * the reason this property gets an entry of its own rather than a row in either list above. §5.1's
 * `Computed value:` line is "the specified keyword, A NUMBER, or a computed <length> value", which is a UNION:
 * `css_computed_length` answers a `CssLength` and has no number, and `css_computed_value` answers text and
 * would drop the environment fact a length carries. Both refuse this property by name and say so.
 * THE THREE ARMS ARE THREE DIFFERENT ANSWERS AND §5.1 STATES EACH SEPARATELY. `normal` is "determine the
 * preferred line height automatically based on the metrics of the used font" and computes to the keyword. A
 * `<number>` is "the preferred line height … multiplied by the element's font-size" and "the computed value is
 * the same as the specified value" — so it stays a NUMBER, which is what makes it inherit differently: §5.1's
 * own example notes that a number "will lead to different line heights if descendants have different font
 * sizes" while a length or a percentage "inherit as absolute lengths, which will not be influenced by the font
 * size on descendants". A `<percentage>` is "this percentage of the element's computed font-size" and §5.1
 * says outright that the computed value IS that length, which is why there is no percentage arm here.
 * WHICH FONT SIZE THE NUMBER MULTIPLIES IS SETTLED BY ANOTHER MODULE, and the two specs have to be read
 * together. §5.1 says "the element's used font-size"; css-fonts-4 §2.6 "Relative sizing: the font-size-adjust
 * property" says the used and computed font sizes differ only through `font-size-adjust` and then states this
 * case explicitly — "since numeric values of line-height refer to the COMPUTED size of font-size,
 * font-size-adjust does not affect the used value of line-height". So the multiplicand is the computed font
 * size, normatively, and this engine needs no assumption about a property it does not model. */
typedef enum {
    CSS_LINE_HEIGHT_NORMAL = 0,  /* §5.1's `normal`, which computes to the keyword */
    CSS_LINE_HEIGHT_NUMBER,      /* §5.1's `<number [0,∞]>`, whose computed value is the number itself */
    CSS_LINE_HEIGHT_LENGTH       /* §5.1's `<length-percentage [0,∞]>`, which computes to an absolute length */
} CssLineHeightKind;

typedef struct {
    CssLineHeightKind kind;
    double            number;    /* CSS_LINE_HEIGHT_NUMBER only */
    CssPx             px;        /* CSS_LINE_HEIGHT_LENGTH only — a length, so it carries its facts */
} CssLineHeight;

/* §5.1's computed value of `line-height` on `el`, with CSS Cascade §7's defaulting applied. §7.2's inherited
   value is taken WHOLE — the parent's own answer, in this same shape — because serializing it would turn a
   number into text that reads like a length and would drop a length's environment fact, which is the same
   reason `css_computed_length` inherits a `CssLength` rather than a string. */
CssLineHeight css_computed_line_height(lxb_dom_element_t *el);

/* THE THREE TERMS CSS 2.2 §10.8 "Line height calculations: the 'line-height' and 'vertical-align' properties"
 * AND §10.8.1 "Leading and half-leading" ARE ARITHMETIC OVER, each for ONE element.
 *
 * §10.8's step 1 takes an inline box's height to be "their 'line-height'", and §10.8.1 splits the leading
 * `L = 'line-height' - AD` in half around `A` and `D` — so a line box needs the USED `line-height` and the two
 * font metrics APART. `css_computed_line_height` above answers the COMPUTED value, which is §5.1's union of a
 * keyword, a number and a length; none of those three is a distance, and turning them into one is what these
 * entries do. The first is also CSSOM §9's `CSS_RESOLVED_LINE_HEIGHT` used value, which is why there is one
 * conversion and not one per reader: a page comparing `getComputedStyle(el).lineHeight` against a measured box
 * must not be able to read two different numbers for one element.
 *
 * THE TWO METRICS ARE ANSWERED PER ELEMENT AND NOT PER FACE because §10.8.1 states them that way — "the A and
 * D of the element's first available font", "for a given font AT A GIVEN SIZE" — so each is the picked ratio
 * core/css/font_metrics.h owns times THIS element's computed `font-size`, resolved in THIS element's
 * document's realm. Both of those are this file's own derivations, which is why the product is formed here;
 * see the definitions for why font_metrics.c must not form it instead. */
CssPx css_used_line_height_px(lxb_dom_element_t *el);
CssPx css_font_ascent_px(lxb_dom_element_t *el);
CssPx css_font_descent_px(lxb_dom_element_t *el);

/* css-values-4 §6.1.1 "Font-relative Lengths…"'s ADVANCE MEASURE of one Unicode scalar value on `el` — "its
   advance width or height, whichever is in the inline axis of the element" — at that element's own computed
   `font-size`, in CSS pixels. It is the third product formed out of the same two operands as the two entries
   above, and it exists here for the same reason: the FACE's ratio is core/css/font_metrics.h's and the ELEMENT
   is this file's, so a layout component that took them apart would have to remember to multiply and could take
   the size from one element and the orientation from another.
   IT MEASURES THE GLYPH THAT GETS DRAWN, .notdef included — `font_metrics_advance_measure_em`, NOT
   `font_metrics_typical_advance_measure_em`, which is §6.1.1's `ch`/`ic` answer and a different question. See
   the definition, and font_metrics.h, for what answering either with the other reports.
   A VERTICAL WRITING MODE CRASHES in the direction resolution this shares with `ch` and `ic`, which is where
   css-writing-modes-4 §5.1's `text-orientation` is named as the row to add. */
CssPx css_font_advance_measure_px(lxb_dom_element_t *el, uint32_t codepoint);

#endif
