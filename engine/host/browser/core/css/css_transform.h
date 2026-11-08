/* CSS TRANSFORMS 1 — the two ELEMENT-LEVEL facts the module defines.
 *
 * WHICH EDITION'S NUMBERS THESE ARE. css-transforms-1's CR carries "§2 Terminology" and its current Editor's
 * Draft renders that heading with no number, so every section below it differs by one: `The transform
 * Property` is §4 in the CR and §3 in the ED. Every number in this component is the ED's, which is what the rest
 * of this tree cites, except "Terminology", cited as §2, the CR's number, because the ED offers none.
 *
 * WHY NEITHER FACT ALONE IS THE ANSWER. §2 Terminology defines a TRANSFORMED ELEMENT as "an element with a
 * computed value other than none for the transform property" and a TRANSFORMABLE ELEMENT by two categories;
 * §3's `Applies to:` line is "transformable elements". So `<span style="transform:rotate(45deg)">` has a
 * computed value and moves nothing, and a consumer asks both. Three of §2's rules — the stacking context, the
 * containing block and the fixed-background rule — are stated "For elements whose layout is governed by the
 * CSS box model" and read the first category alone, which is why it has its own predicate. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_TRANSFORM_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_TRANSFORM_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

/* §2 Terminology's two categories of transformable element, and the answer for an element in neither. */
typedef enum {
    CSS_TRANSFORMABLE_NONE = 0,
    /* "all elements whose layout is governed by the CSS box model except for non-replaced inline boxes,
       table-column boxes, and table-column-group boxes [CSS2]" */
    CSS_TRANSFORMABLE_CSS_BOX,
    /* "all SVG paint server elements, the clipPath element and SVG renderable elements with the exception of
       any descendant element of text content elements [SVG2]" */
    CSS_TRANSFORMABLE_SVG
} CssTransformableCategory;

/* Which of §2's categories `el` is in. An SVG element whose parent is an SVG element is SVG 2's to classify;
   every other element, the outermost `svg` included (a replaced element the CSS box model lays out), is the
   CSS box model's. */
CssTransformableCategory css_transform_category(lxb_dom_element_t *el);

/* §2 Terminology's TRANSFORMABLE ELEMENT, either category: §3's `Applies to:` line. */
bool css_transform_is_transformable(lxb_dom_element_t *el);

/* The first category alone: a transformable element "whose layout is governed by the CSS box model". */
bool css_transform_is_css_box_transformable(lxb_dom_element_t *el);

/* §2 Terminology's TRANSFORMED ELEMENT — "an element with a computed value other than none for the transform
   property", over core/css/css_computed_value.h's computed `transform`. */
bool css_transform_is_transformed(lxb_dom_element_t *el);

#endif
