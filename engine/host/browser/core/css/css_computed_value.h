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
 *   declare (`transform` resolves to a matrix). A used value is what a LAYOUT produced. This engine gives
 *   geometry to exactly one box, the initial containing block (core/dom/element_view.h), so those properties
 *   have no resolved value here and `css_resolved_value` says so at the read instead of returning the cascade's
 *   text, which is a WRONG answer rather than a missing one and was what this file's absence used to produce.
 *   §9's two escapes are real branches and are taken: a `display: none` element's box-model lengths resolve to
 *   their COMPUTED values, and a statically positioned element's insets do too — so the common reads still
 *   answer.
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

/* THE COMPUTED VALUE of `name` on `el`, as text. OWNED: the caller frees. `name` must be one of the properties
   this component models — `css_computed_models` is that list, and asking for another one crashes. */
char *css_computed_value(lxb_dom_element_t *el, const char *name);

/* Does this component DERIVE `name`'s computed value from the cascade's specified value? */
bool css_computed_models(const char *name);

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

/* CSSOM §9's RESOLVED VALUE of `name` on `el` — what getComputedStyle() answers. OWNED. Any property name,
   including a custom one. */
char *css_resolved_value(lxb_dom_element_t *el, const char *name);

#endif
