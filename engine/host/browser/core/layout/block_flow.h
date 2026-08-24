/* CSS 2 §9.4.1 "Block formatting contexts" and CSS 2.1 §8.3.1 "Collapsing margins" — THE BOX TREE, which is
 * the one walk two members of this directory were each waiting on separately.
 *
 * ONE SUBPROBLEM, TWO ANSWERS, WHICH IS WHY THIS IS ONE COMPONENT AND NOT TWO. core/layout/flow_position.c's
 * non-root crash and core/layout/used_value.c's `height: auto` crash both named the SAME missing thing in the
 * same words — "the distance from its top content edge to the bottom margin edge of the last in-flow child"
 * (§10.6.3) and "boxes are laid out one after the other, vertically, beginning at the top of a containing
 * block" (§9.4.1) — because a box's y IS the running offset the height walk already computes and the height IS
 * where that offset ends up. Building them apart would be two walks over one child list that can disagree
 * about where a margin collapsed, so there is one walk and it answers both.
 *
 * §8.3.1 IS THE ALGORITHM, NOT A CORRECTION TO IT. "The vertical distance between two sibling boxes is
 * determined by the margin properties" is §9.4.1's whole placement rule, and the very next sentence is
 * "vertical margins between adjacent block-level boxes in a block formatting context collapse" — so a walk
 * that adds margins and then subtracts a collapse is a walk that had the rule wrong. What travels through the
 * walk is therefore not a number but §8.3.1's ADJOINING RUN: the set of margins that have met without a line
 * box, clearance, padding or border between them, whose collapsed width is "the maximum of the collapsing
 * margins' widths" with, "in the case of negative margins, the maximum of the absolute values of the negative
 * adjoining margins deducted from the maximum of the positive adjoining margins". That reduction is two
 * running maxima, so a run is two lengths and a flag and it merges associatively — which is what lets the
 * SAME structure carry a run upward out of a child, downward out of a child, and through a child that
 * collapses through entirely.
 *
 * A RUN LEAVING THE BOX IS THE ONLY DIFFERENCE BETWEEN §10.6.3 AND §10.6.7, AND IT IS THE SPEC'S OWN.
 * §8.3.1's third and first adjoining pairs make a box's top margin adjoin its FIRST in-flow child's, and its
 * bottom margin adjoin its LAST in-flow child's — but only when nothing separates them, which is "no line
 * boxes, no clearance, no padding and no border", and only when the box does not establish a new block
 * formatting context (§8.3.1's own note: "margins of elements that establish new block formatting contexts …
 * do not collapse with their in-flow children"). When the run DOES escape, the child's top border edge is at
 * the box's own top content edge and the margin contributes nothing INSIDE the box — which is exactly
 * §10.6.3's "the bottom border edge of the last in-flow child whose top margin doesn't collapse with the
 * element's bottom margin" read from the other end. When it does NOT escape, the same walk measures from the
 * child's top MARGIN edge to the last child's bottom MARGIN edge, which is §10.6.7's rule verbatim. So the two
 * sections are one walk under two boolean flags, and the flags are a fact about the box (§9.4.1's list of
 * what establishes a formatting context, §8.3.1's list of what separates two margins), never a mode this
 * component picks.
 *
 * WHAT IS DELIBERATELY NOT BUILT, AND WHY EACH IS A CRASH AND NOT A ZERO. The smallest box model that answers
 * CSSOM VIEW §6 and §7 for the documents this engine parses is BLOCK FLOW: a block container whose in-flow
 * children are all block-level, over the used values core/layout/used_value.h already computes. Everything
 * else names its own section and aborts, because a zero from an unimplemented box passes a presence test and
 * is indistinguishable from a real one:
 *   - INLINE-LEVEL CONTENT of any kind — a non-collapsing text run, an `inline`, `inline-block`,
 *     `inline-flex` or `inline-grid` child — is §9.4.2's inline formatting context, whose line boxes need the
 *     text measured with a real font, and §9.2.1.1's anonymous block boxes around it. That is a different
 *     component and this one must not guess at its height.
 *   - A FLOAT in the formatting context is §9.5's own placement, and it is not enough to note that §10.6.3
 *     ignores floats: §9.5.2's `clear` on a LATER sibling introduces CLEARANCE, which §8.3.1 makes
 *     non-adjoining, so one float invalidates every collapse below it.
 *   - A TABLE box is §17.5's two algorithms and a FLEX or GRID container is its own spec's; neither's height
 *     is §10.6.3's walk, and a container whose children this walk placed would be a wrong number rather than
 *     an absent one.
 *   - An OUT-OF-FLOW child is not a gap at all: §10.6.3 states outright that "absolutely positioned boxes are
 *     ignored", so skipping one is the rule running, and the box's own position is §9.3.2's over a static
 *     position that this walk is what will one day provide.
 *
 * NOTHING IS STORED, FOR used_value.h's REASON, RESTATED BECAUSE THIS COMPONENT IS WHERE IT WOULD FIRST BE
 * TEMPTING TO BREAK IT. A layout is per-flow state — two flows with different DOMs have different boxes — so a
 * cached box tree is shared state the COW delta does not swap, and a stale one is a geometry from another
 * flow's document. Every answer here is DERIVED PER READ from the running flow's own tree and its own cascade,
 * which makes it per-flow by construction with no capture to write. The cost is that a subtree is walked once
 * per question asked of it; the day that is the bottleneck the cache is per-flow state and needs
 * solver/dom_cow.h's capture at its accessor, exactly as a browser component's own C record does.
 *
 * A HEIGHT IS A `CssPx` FOR used_value.h's REASON AND THE PROPAGATION IS FREE. Every operand of the walk is
 * already a used value carrying the set of environment facts it derives from — a percentage margin resolves
 * against the containing block's WIDTH (§8.3, and "note that this is true for 'margin-top' and 'margin-bottom'
 * as well"), which bottoms out in the initial containing block; a border width carries the device pixel ratio.
 * So an auto height computed here is a joint function of whatever its children's margins and borders were
 * functions of, and css_length.h's arithmetic unions those sets without this file deciding anything. A box
 * whose whole subtree is author-pinned comes out with the empty set and is CONCRETE, which is the correct
 * answer and not a lost domain: viewport.h's test is whether the model PICKED a value out of a range the
 * environment leaves free, and a stack of declared heights is not that. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_BLOCK_FLOW_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_BLOCK_FLOW_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"

/* CSS 2.1 §9.2.1's BLOCK CONTAINER BOX, decided from a computed `display` and from nothing else — the box
   §9.4.1's formatting context is stated over, and the box §10.1's second case looks for when it walks for a
   containing block. It is exported because BOTH of those callers ask it and the list is not "everything
   block-level": an `inline-block` is a block container and is not block-level, and a TABLE box is block-level
   and is not a block container (§17.4 makes the CELL and the CAPTION the block containers inside one). */
bool block_flow_display_is_block_container(const char *display);

/* CSS 2.1 §9.2.2.1 "Anonymous inline boxes"'s WHITE-SPACE RULE for one TEXT child of a block container:
   "White space content that would subsequently be collapsed away according to the 'white-space' property does
   not generate any anonymous inline boxes." FALSE is that sentence — a run this element's computed
   `white-space` collapses away, which is most of the character data in a pretty-printed document — and the
   two ways a text run DOES generate a box both CRASH naming §9.4.2, because a line box needs the text
   measured with a real font and this engine has none.
   IT IS EXPORTED BECAUSE EVERY WALK OVER A BLOCK CONTAINER'S CHILDREN MUST ASK IT. The height walk below is
   one; CSSOM VIEW §2's scrolling area (core/layout/scrolling_area.h) is another, and it asks for a reason the
   height walk cannot cover — a box with a DECLARED height never reaches the walk at all, so its own text
   would be invisible to a caller that only measured heights, and a text run that overflows a declared-height
   box is exactly what `scrollHeight` is asked about. A second copy of §9.2.2.1 would be one rule with two
   answers about whether a page's white space is content. */
bool block_flow_text_child_generates_box(lxb_dom_element_t *parent, const lxb_dom_node_t *text);

/* CSS 2.1 §10.6.3's (and, for a box that establishes a block formatting context, §10.6.7's) CONTENT-BASED
   HEIGHT of `el`'s box, in CSS pixels — the used value of a `height` that computed to `auto`.
   THE CALLER HAS ALREADY ESTABLISHED that `height` is `auto` and that the box is one §10.6.3 or §10.6.6
   covers; core/layout/used_value.c's `uv_size` is that caller and it has classified the box type first. Every
   case this component does not lay out crashes naming its own section — see the header. */
CssPx block_flow_auto_height(lxb_dom_element_t *el);

/* CSS 2 §9.4.1's VERTICAL PLACEMENT of `el`: the distance from the TOP CONTENT EDGE of `el`'s containing block
   to `el`'s TOP BORDER EDGE, in CSS pixels. It is stated against the containing block's CONTENT edge because
   that is the rectangle §10.1 gives the box and the one §10.6.3 measures its height from, so the caller adds
   exactly the containing block's own origin plus its top border and padding and nothing else.
   `el` MUST BE AN IN-FLOW BLOCK-LEVEL BOX whose containing block is §10.1's second case — core/layout/
   flow_position.c is the caller and it has taken every other positioning scheme out through its own section
   first. An element the walk over its containing block's children never places crashes rather than answering
   a coordinate no box has. */
CssPx block_flow_child_top(lxb_dom_element_t *el);

#endif
