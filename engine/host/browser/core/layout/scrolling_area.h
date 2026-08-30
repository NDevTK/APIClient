/* CSSOM VIEW §2 "Terminology" — THE SCROLLING AREA OF AN ELEMENT, which is the first geometry in this engine
 * that is a fact about a box AND ALL OF ITS DESCENDANTS rather than about one box.
 *
 * WHY IT IS A COMPONENT AND NOT ARITHMETIC INSIDE §6's `scrollWidth`. §2 states the scrolling area once and
 * three separate algorithms read it: §6's `scrollWidth`/`scrollHeight` step 7 returns its width and height,
 * §6's `scrollTop`/`scrollLeft` setter's last steps ask whether the element "has a scrolling box" and "has any
 * overflow", and §3.1's perform a scroll clamps a requested position into it. A second copy inside whichever
 * of those was written first is one rectangle with two derivations, free to disagree about which descendants
 * are in it.
 *
 * §2's TABLE IS FOUR ROWS AND ONE SHAPE. On each axis one side of the scrolling area is the element's OWN
 * padding edge and the other is the extreme over that padding edge and "the … margin edge of all of the
 * element's descendants' boxes, excluding boxes that have an ancestor of the element as their containing
 * block". Which side is which is the scrolling box's OVERFLOW DIRECTIONS, which §2 defines as "the block-end
 * and inline-end directions for that viewport or element" — css-writing-modes-4 §6.2 "Flow-relative
 * Directions" over the computed `writing-mode` and `direction`. So the WIDTH is a function of the inline-end
 * direction alone and the HEIGHT of the block-end direction alone, and the two never enter each other's
 * answer.
 *
 * THE EXCLUSION IS §10.1's CONTAINING BLOCK AND NOT A DEPTH TEST. A descendant whose containing block is an
 * ANCESTOR of this element is laid out against a rectangle this element does not own — an absolutely
 * positioned box whose nearest positioned ancestor is above it — so scrolling this element would not move it
 * and §2 leaves it out. A descendant whose containing block is this element, or any box inside it, is in.
 * core/layout/used_value.h answers which, per element, and crashes for the cases it does not decide, so this
 * component states the test and owns none of it.
 *
 * WHY IT IS NOT THE PADDING EDGE'S EXTENT, which is the one wrong answer that would pass every presence test.
 * core/layout/used_value.h computes the distance between two parallel edges of ONE box and CSSOM VIEW §6's
 * `clientWidth` reports it. §2's right edge is a right-most POSITION over this box and every descendant's, so
 * it needs each of those boxes PLACED — core/layout/flow_position.h — and an extent locates nothing. A
 * `scrollWidth` that answered the padding extent would equal `clientWidth` for every element in every
 * document, which is exactly the state a page tests for when it asks whether its content overflows.
 *
 * IT IS THE ELEMENT'S TABLE COLUMN AND NOT THE VIEWPORT'S. §2 gives the two different edges — the viewport's
 * are stated over the INITIAL CONTAINING BLOCK and over the viewport's descendants — and core/frame/viewport.h
 * owns that one. Two columns, two derivations, one term.
 *
 * A `CssPx` FOR core/layout/used_value.h's REASON: every operand is a used value or a placed coordinate whose
 * chain bottoms out in the initial containing block, so the extent carries the viewport's environment fact and
 * `el.scrollWidth > el.clientWidth` is a comparison the environment can answer either way. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_SCROLLING_AREA_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_SCROLLING_AREA_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"

/* THE WIDTH (`vertical` false) OR HEIGHT of `el`'s scrolling area, in CSS pixels — the distance between the
   two edges §2's table gives that axis.
   THE CALLER HAS ALREADY ESTABLISHED THAT THE ELEMENT HAS AN ASSOCIATED BOX (core/dom/element_view.h's one
   predicate), which is §6's own step 6 and is what makes the element's padding edge and its position exist at
   all. Every box this component cannot place or measure crashes in the component that owns that half, naming
   its own section; there is no fallback extent. */
CssPx scrolling_area_extent_px(lxb_dom_element_t *el, bool vertical);

/* §2's OVERFLOW DIRECTIONS for `el`'s scrolling box, reduced to the one bit every consumer needs: does the
   ENDING edge of the scrolling area sit at the LARGER coordinate on this axis. "A scrolling box of a viewport
   or element has two overflow directions, which are the block-end and inline-end directions for that viewport
   or element", derived through css-writing-modes-4 §6.2 "Flow-relative Directions" over the computed
   `writing-mode` and `direction`.
   IT IS EXPORTED BECAUSE §2's TABLE IS NOT ITS ONLY READER. CSSOM VIEW §6.1 "Element Scrolling Members"' scroll
   an element to x,y states its clamp as four rows over the same fact — "if box has rightward overflow
   direction … if box has leftward overflow direction" — and §6.1's determine the scroll-into-view position
   needs it to know which physical edge is a scrolling box's BEGINNING edge. A second derivation beside this one
   is one direction with two answers, free to disagree about `direction: rtl` in exactly the document where it
   matters. */
bool scrolling_area_ending_edge_at_higher_coordinate(lxb_dom_element_t *el, bool vertical);

#endif
