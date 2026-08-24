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
 * ONE LINE NEEDS A FACT THAT IS NOT `display`, AND IT IS ANSWERED ELSEWHERE ON PURPOSE. "Non-replaced inline"
 * is that line, and CSS 2.1 §3.1's definition of a REPLACED element ("an element whose content is outside the
 * scope of the CSS formatting model") is not a list — CSS declines to say which elements those are, and HTML
 * §15.4 "Replaced elements" is where the answer lives. core/layout/replaced_element.h owns it, and this file
 * ASKS: an inline element's size properties apply exactly when it is replaced.
 * WHAT STOOD HERE SAID THIS COMPONENT REFUSES TO GUESS AND CRASHES INSTEAD, and named css-display Appendix B's
 * list of elements that "aren't rendered purely by CSS box concepts" as the superset it tested. That crash is
 * gone and so is the list, because both were wrong rather than merely incomplete: Appendix B's list is drawn
 * up for `display: contents` and carries elements §15.4 does not call replaceable at all (`br`, `select`,
 * `textarea`, `meter`, `progress`), while the fact the line actually needs CHANGES UNDER THE RUNNING FLOW —
 * §15.4.2's rules make an `img` replaced while its request is outstanding and NON-replaced once it breaks with
 * an `alt` present. Both answers are common and they differ: a non-replaced inline `<span>` and a broken
 * `<img alt="…">` resolve `width` to the computed `auto`, while a loading `<img>` resolves it to CSS 2.1
 * §10.3.2's used width. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_PROPERTY_APPLIES_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_PROPERTY_APPLIES_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

/* Does `name`'s "Applies to:" line cover `el`? Asked of the properties CSSOM §9 asks it of; a property whose
   line this component has not recorded crashes rather than being waved through, for css_computed_value.h's
   reason — a default here is a used value computed for a box that never had the property. */
bool css_property_applies(lxb_dom_element_t *el, const char *name);

#endif
