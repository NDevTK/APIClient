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

#endif
