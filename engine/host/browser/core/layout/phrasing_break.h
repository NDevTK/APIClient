/* HTML §15.3.4 "Phrasing content": the two elements whose box breaks the line.
 *
 * The section's UA stylesheet declares them with property values no CSS module defines:
 *     br  { display-outside: newline; }              a forced line break
 *     wbr { display-outside: break-opportunity; }    a soft wrap opportunity
 * css-display-4 §2 "Box Layout Modes: the display property" has neither keyword, so the cascade cannot answer;
 * the engine implements the css-text-3 §5 "Line Breaking and Word Boundaries" behaviour the names stand for.
 *
 * The answer is HTML's, so it lives in one component that both walks over an inline formatting context's
 * children call: core/layout/line_box.c and core/layout/intrinsic_size.c. A walk that missed a `br` would
 * report the paragraph's whole width as the max-content size of css-sizing-3 §2.1 "Auto Box Sizes".
 *
 * It answers only what §15.3.4 declares about the element. Whether the element generates a box, is in flow,
 * and its computed `display` are cascade questions the caller has already asked. The namespace is part of the
 * declaration: the sheet opens `@namespace "http://www.w3.org/1999/xhtml";`, so an SVG `br` is not a break. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_PHRASING_BREAK_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_PHRASING_BREAK_H

#include <lexbor/dom/dom.h>

/* HTML §15.3.4's classification of one element. The values are named for the css-text-3 §5 behaviour rather
   than for the undefined `display-outside` keywords. */
typedef enum {
    PHRASING_BREAK_NONE = 0,     /* §15.3.4 declares neither value for this element */
    PHRASING_BREAK_FORCED,       /* `display-outside: newline` — css-text-3 §5's forced line break */
    PHRASING_BREAK_OPPORTUNITY,  /* `display-outside: break-opportunity` — §5's soft wrap opportunity */
} PhrasingBreak;

PhrasingBreak phrasing_break_of(lxb_dom_element_t *el);

#endif
