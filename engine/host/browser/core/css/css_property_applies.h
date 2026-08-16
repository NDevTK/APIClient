/* EVERY CSS PROPERTY DEFINITION CARRIES AN "Applies to:" LINE, and CSSOM §9 READS IT — this is that line.
 *
 * §9 does not say "the resolved value of `width` is the used value". It says: "If THE PROPERTY APPLIES TO THE
 * ELEMENT and the resolved value of the display property is not none or contents, then the resolved value is
 * the used value. Otherwise the resolved value is the computed value." That is a CONJUNCTION, and only its
 * second half had ever been asked here — so `getComputedStyle(span).width` was routed to a used value that
 * does not exist, when CSS 2.1 §10.2's own "Applies to: all elements but non-replaced inline elements" makes
 * the answer the COMPUTED value, `auto`, in every user agent. The first conjunct is not a way of excluding
 * elements from a layout that is not written yet: it is a REAL ANSWER for a large share of the elements on a
 * page, and getting it right is what makes the layout's remaining scope small and honest.
 *
 * IT IS DECIDED FROM THE COMPUTED `display`, which is the only thing an "Applies to:" line ever names. The
 * lines are quoted at their sites and are the properties' own, from the specs that define them:
 *   `width`   — "all elements but non-replaced inline elements, table rows, and row groups"      (CSS 2.1 §10.2)
 *   `height`  — "all elements but non-replaced inline elements, table columns, and column groups"(CSS 2.1 §10.5)
 *   margins   — "all elements except elements with table display types other than table-caption,
 *                table and inline-table"                                                          (CSS 2.1 §8.3)
 *   paddings  — "all elements except table-row-group, table-header-group, table-footer-group,
 *                table-row, table-column-group and table-column"                                  (CSS 2.1 §8.4)
 *   insets    — "positioned elements"                                                             (CSS 2.1 §9.3.2)
 * The table display types those lines name are css-display §2.4's `<display-internal>` keywords, so the
 * membership test is over the computed value core/css/css_computed_value.h already derives — including its
 * BLOCKIFICATION, which is why a floated `<span>` answers as a block box here and not as an inline one.
 *
 * THE INSETS' LINE IS §9'S OTHER CONJUNCT, WHICH IS WHY IT IS HERE AND NOT AT THE READ. §9's inset row says
 * "if the property applies to A POSITIONED ELEMENT" — the applies-to line for `top`/`left`/`bottom`/`right` IS
 * "positioned elements", so the two are ONE fact. css_computed_value.c asked it for itself, which made the
 * position of `position` in the resolved-value algorithm a thing two files had to agree about.
 *
 * WHAT IT REFUSES TO GUESS. "Non-replaced inline" is the only line above whose answer needs a fact that is not
 * `display`, and CSS 2.1 §3.1's definition of a REPLACED element ("an element whose content is outside the
 * scope of the CSS formatting model") is not a list. css-display Appendix B publishes the list of HTML
 * elements that "aren't rendered purely by CSS box concepts", and that list is what this component tests
 * against — but membership in it is not the same question, so an INLINE element on it CRASHES here rather than
 * being answered either way. Both answers are reachable and they differ: a non-replaced inline `<span>`
 * resolves `width` to the computed `auto`, while an inline `<img>` resolves it to a used value CSS 2.1 §10.3.2
 * derives from an INTRINSIC WIDTH this engine has no image decoder to ask for. Answering "not replaced" for
 * both would report `auto` for an image that is 200 pixels wide. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_PROPERTY_APPLIES_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_PROPERTY_APPLIES_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

/* Does `name`'s "Applies to:" line cover `el`? Asked of the properties CSSOM §9 asks it of; a property whose
   line this component has not recorded crashes rather than being waved through, for css_computed_value.h's
   reason — a default here is a used value computed for a box that never had the property. */
bool css_property_applies(lxb_dom_element_t *el, const char *name);

/* Is `el` one of the elements css-display Appendix B lists as not rendered purely by CSS box concepts? The
   question CSS 2.1 §3.1's "replaced element" is asked THROUGH, and deliberately not the same question — see
   the header. Exported because CSS 2.1 §10.3's box-type split asks it too (§10.3.2 and §10.3.4 are the
   replaced arms of the used-width algorithm). */
bool css_element_may_be_replaced(lxb_dom_element_t *el);

#endif
