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
 * WHAT IS STILL SPECIFIED-VALUE-SHAPED, AND THE MECHANISM THAT ENDS IT. A property this file does not model
 * takes its computed value to BE its specified value, which is what the majority of CSS properties' "Computed
 * value:" line says and what the length-valued ones' does not. The gap is not a judgement call to be made per
 * property by hand — every property definition in every CSS spec carries that line, `@webref/css` publishes it
 * as a `computedValue` field, and engine/idlgen.mjs already reads that package's IDL sibling. Wiring the CSS
 * half in the same way turns this file's default arm from an assumption into an assertion. Until then the one
 * case that is CERTAINLY wrong crashes: a CSS-wide keyword (`inherit`/`initial`/`unset`/`revert`) as the
 * cascade's winner is CSS Cascade §7's DEFAULTING step, and this engine's cascade has no inheritance at all. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_COMPUTED_VALUE_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_COMPUTED_VALUE_H
#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"

/* THE COMPUTED VALUE of `name` on `el`, as text. OWNED: the caller frees. `name` must be one of the properties
   this component models — `css_computed_models` is that list, and asking for another one crashes. */
char *css_computed_value(lxb_dom_element_t *el, const char *name);

/* Does this component DERIVE `name`'s computed value from the cascade's specified value? */
bool css_computed_models(const char *name);

/* Is `value` one of CSS Cascade §7.3's CSS-WIDE KEYWORDS — `inherit`, `initial`, `unset`, `revert`,
   `revert-layer`? Each is the ENTIRE value of a declaration when present, so this is an equality and not a
   search, and each makes the value a product of §7's DEFAULTING step rather than of the declaration.
   EXPORTED because a SHORTHAND carrying one "sets all of its sub-properties to that keyword" (CSS Cascade
   §Shorthand Properties), which core/css/css_shorthand.h has to know BEFORE it tries the shorthand's own
   grammar: `border: initial` is not a `<line-width> || <line-style> || <color>`, and a keyword that reached
   that grammar would be classified as one of its terms. */
bool css_wide_keyword(const char *value);

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
