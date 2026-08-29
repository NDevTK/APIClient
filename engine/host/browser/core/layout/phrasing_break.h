/* HTML §15.3.4 "Phrasing content" — THE TWO ELEMENTS WHOSE BOX BREAKS THE LINE, which is a fact no CSS module
 * can be asked for and which every walk over an inline formatting context's children needs.
 *
 * §15.3.4's UA stylesheet declares exactly two of them, and it declares them with a property value:
 *     br  { display-outside: newline; }              a FORCED LINE BREAK
 *     wbr { display-outside: break-opportunity; }    a SOFT WRAP OPPORTUNITY
 * and NO CSS MODULE DEFINES EITHER KEYWORD. css-display-4 §2 "Box Layout Modes: the display property" has the
 * term `display-outside` and contains neither `newline` nor `break-opportunity` anywhere in it, so there is no
 * `Computed value:` line to derive, nothing for core/css/css_computed_value.c to answer, and no cascade query
 * that could reach the declaration. What a user agent implements is the css-text-3 §5 "Line Breaking and Word
 * Boundaries" behaviour those two names stand for, and this component is where that fact enters the engine.
 *
 * IT IS A COMPONENT FOR THE REASON core/layout/replaced_element.h IS ONE, and it is the same reason twice.
 * CSS declines to say which elements are replaced and HTML §15.4 answers it; CSS declines to define these two
 * keywords and HTML §15.3.4 declares them. In both cases the answer is HTML's, so a predicate living inside
 * one layout walk would be the one fact answered from two places — and there are two walks over exactly this
 * child list, core/layout/line_box.c's (which measures CSS 2.2 §10.8 over the line boxes) and
 * core/layout/intrinsic_size.c's (which measures css-sizing-3 §2.1's two sizes over the same run). A `br`
 * invisible to the second is not a crash there but a WRONG NUMBER: §2.1's max-content size is stated over
 * content "formatted without breaking lines OTHER THAN WHERE EXPLICIT LINE BREAKS OCCUR" (CSS 2.2 §10.3.5), so
 * a run whose forced break the walk never saw reports a paragraph's whole width as the width of its longest
 * line.
 *
 * THE ANSWER IS THE ELEMENT'S OWN, NOT ITS BOX'S, and the two questions the consumers then ask are theirs.
 * Whether the element generates a box at all (`display: none`), whether it is in flow, and what its computed
 * `display` is are all cascade questions the caller has already asked by the time it gets here — this answers
 * only what §15.3.4 declares, so a caller that reaches it with an out-of-flow element has skipped a step of its
 * own walk rather than been given a wrong answer.
 *
 * THE NAMESPACE IS PART OF THE DECLARATION and is checked rather than assumed: §15.3.4's sheet opens with
 * `@namespace "http://www.w3.org/1999/xhtml";`, so an SVG element whose local name happens to be `br` is not a
 * line break, and a document that puts one there gets the ordinary inline box its own rules give it. A test on
 * the local name alone would answer for a tree the sheet does not select. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_PHRASING_BREAK_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_PHRASING_BREAK_H

#include <lexbor/dom/dom.h>

/* HTML §15.3.4's classification of ONE element. The two named values are the two `display-outside` values the
   section declares, spelled here as what css-text-3 §5 makes them rather than as the keywords, because the
   keywords are the half no specification defines and the behaviour is the half every consumer needs. */
typedef enum {
    PHRASING_BREAK_NONE = 0,     /* §15.3.4 declares neither value for this element */
    PHRASING_BREAK_FORCED,       /* `display-outside: newline` — css-text-3 §5's FORCED LINE BREAK */
    PHRASING_BREAK_OPPORTUNITY,  /* `display-outside: break-opportunity` — §5's SOFT WRAP OPPORTUNITY */
} PhrasingBreak;

PhrasingBreak phrasing_break_of(lxb_dom_element_t *el);

#endif
