/* CSS 2.1 §10 — VISUAL FORMATTING MODEL DETAILS, which is where a box's USED VALUES come from, and the first
 * real box geometry in this engine.
 *
 * THE USED VALUE IS A THIRD ANSWER AND THE CASCADE PRODUCES NEITHER OF THE FIRST TWO. css_computed_value.h
 * states the split it owns — the cascade's SPECIFIED value, and the COMPUTED value a spec algorithm reads. CSS
 * Cascade's third stage is the USED value: "the values used are the same as the computed values, with 'auto'
 * replaced by some suitable value, and percentages calculated based on the containing block, but there are
 * exceptions" (§10.3's own opening). That sentence is this component's whole contract, and the exceptions are
 * §10.3.1 through §10.3.10 and §10.6.1 through §10.6.7, one per BOX TYPE.
 *
 * WHY THE BOX TYPE COMES FIRST AND IS NOT A DETAIL. §10.3 does not give `width` one algorithm; it gives ten,
 * selected by whether the box is inline or block-level, replaced or not, floating, absolutely positioned or in
 * normal flow. A used value computed without asking is not approximately right, it is an answer from the wrong
 * algorithm: a floated box's `width: auto` is a SHRINK-TO-FIT (§10.3.5) and a block-level one's follows from a
 * constraint equation (§10.3.3), and the two are not close. So `uv_box_kind` is the first thing every entry
 * below does, and a box type whose section is unbuilt CRASHES naming that section rather than borrowing the
 * neighbouring one's answer.
 *
 * WHAT THIS COMPONENT COMPUTES TODAY, AND WHY THAT SET AND NOT A LARGER ONE. The set is every arm of §10 that
 * needs no INTRINSIC SIZE — no measurement of the box's own content — plus the box edge stated over them and
 * the containing block they are all stated against. The one thing missing from all of it is therefore one
 * thing: a shrink-to-fit width needs the box's text measured with a real font, and nothing here does. A
 * CONTENT-BASED HEIGHT used to be on that list and is not: §10.6.3's own bullets say the font is needed only
 * for the LINE-BOX arm, and its block-level-children arm is a walk over used heights and collapsing margins
 * that core/layout/block_flow.h now runs — so a `height: auto` box whose children are block-level answers
 * here, and one with inline content crashes inside that walk naming §9.4.2:
 *   - A MARGIN OR PADDING whose computed value is an absolute length. Nothing in CSS 2.1 alters it. §10.3.3's
 *     constraint equation solves for `auto` values and, when the box is OVER-CONSTRAINED, for one horizontal
 *     margin; it never touches a vertical margin, and §10.6.3 gives the vertical pair exactly one rule (`auto`
 *     becomes 0). Padding has no `auto` and appears in no equation as an unknown. So the used value IS the
 *     computed length, and saying so is a derivation rather than a shrug.
 *   - A `width` or `height` whose computed value is an absolute length, for every box type but a table box and
 *     a flex or grid item. §10.3.3's equation solves for `width` only when `width` is `auto`; §10.3.5,
 *     §10.3.7 and §10.3.9 each say the same for their own box type. A TABLE may be widened past it (§17.5.2)
 *     and a FLEX ITEM's size is its container's algorithm and not §10's at all, so both crash. One further
 *     conjunct is ASSERTED rather than assumed, because it is a case in which the declared length is not the
 *     used one and it is not visible as a crash otherwise: §10.4/§10.7's clamp by a declared `min-`/`max-`
 *     limit.
 *   - AND THE SAME SIZE UNDER `box-sizing: border-box`, which is a COMPUTATION and not a second assertion.
 *     What stood here called it one, on the ground that css-sizing §5 "makes the declared value the BORDER
 *     box's while §10.2 and CSSOM §9 both mean the content box's" — and §5's own next sentence says the
 *     opposite: "Used values, as exposed for instance through getComputedStyle(), also refer to the border
 *     box." So the exposed used value is the border box's size, which is the declared length except when the
 *     paddings and borders alone exceed it and the content box floors at zero (§5's own worked example: "the
 *     border-box size ends up at 120px, even though width: 100px is specified"). It is the LARGER of the two,
 *     and the four terms it needs are the two paddings and the two border widths.
 *   - AND THE PADDING EDGE'S EXTENT, which is not a property at all and is exactly why it is an entry HERE
 *     rather than arithmetic at the one caller that wants it. CSS 2.1 §8's box model makes the padding box the
 *     CONTENT box plus the two paddings on the axis, and that one sentence is the whole derivation in BOTH
 *     `box-sizing` modes — what differs between them is not the padding edge, it is which box the USED SIZE
 *     above is the size OF. Under `content-box` that size IS the content box and the two paddings are added;
 *     under `border-box` css-sizing §5 makes it the BORDER box, so the content box is that size minus the same
 *     four terms §5's own conversion names, and the paddings are added back to THAT. A CALLER CANNOT WRITE
 *     THIS: `used_value_px(el, "width")` plus the two paddings is the answer in one mode and DOUBLE-COUNTS
 *     them in the other, and nothing in the number it got back says which mode produced it. So the four terms
 *     are computed once, in one function, and both directions of §5's conversion are stated over that one
 *     result — the double-count is not a mistake to avoid, it is a sentence there is no longer anywhere to
 *     write.
 *   - A MARGIN, PADDING OR `width` whose computed value is a PERCENTAGE, which §8.3, §8.4 and §10.2 all resolve
 *     against the same measure: the WIDTH of the containing block, and for §8.3 and §8.4 that is true of the
 *     VERTICAL sides too ("even for 'padding-top' and 'padding-bottom'"). That is the rule most implementations
 *     get wrong, and it is why neither arm below takes an axis. A percentage `height` is the exception and is
 *     not here: §10.5 makes it a COMPUTED-value question (it computes to `auto` when the containing block's
 *     height is not specified explicitly), so it belongs one stage earlier and crashes naming that stage.
 *   - AND §10.3.3's CONSTRAINT EQUATION, which is what `width: auto` on an ordinary block-level box resolves
 *     through and is therefore the arm most of the web reaches first. Its seven terms,
 *         margin-left + border-left-width + padding-left + width + padding-right + border-right-width +
 *         margin-right = width of containing block
 *     are every one of them read back through the arms above, so the section is a solve for whichever of them
 *     is `auto` (rule 5 for `width`, rules 2/4/6 for the margins) and nothing more — plus §10.4's clamp by
 *     `min-width`, whose initial 0 is always in effect and is therefore part of the answer rather than a
 *     second pass. What §10.3.3 still crashes for is its OVER-CONSTRAINED case, and it is not a layout gap:
 *     WHICH of the two margins is ignored is a fact about the containing block's computed `direction`, and
 *     `direction` is not among the properties core/css/css_computed_value.h models — the cascade inherits it
 *     now (core/css/css_defaulting.h), and there is no entry to read the computed value through.
 *   - AND §10.1's CONTAINING BLOCK, which every one of those percentages and every one of those `auto` values
 *     is stated against. It is a recursion — "the content edge of the nearest BLOCK CONTAINER ancestor box" —
 *     and it terminates because §10.1's first case makes the ROOT ELEMENT's containing block the INITIAL
 *     CONTAINING BLOCK, which "has the dimensions of the viewport" core/frame/viewport.h models. Its other two
 *     cases crash, and for reasons that are not this one's: a `fixed` box's containing block is the viewport
 *     (the same rectangle, but §10.3.7's equation is what turns it into a used width) and an `absolute` box's
 *     is the PADDING EDGE of the nearest positioned ancestor — a RECTANGLE, where this component computes
 *     extents, so it is waiting on box positions and §9.4's flow layout rather than on anything here.
 *
 * A GEOMETRY IS CONCRETE AND A GEOMETRY DERIVED FROM THE VIEWPORT IS NOT, WHICH IS WHY A USED VALUE IS A
 * `CssPx` AND NOT A `double`. c35f1fed decided the first half and it is right: viewport.h's test is whether the
 * model PICKED one point out of a range the environment leaves free or DERIVED the only value the model
 * permits, and a box's size is neither — it is what a LAYOUT determines from this tree and this cascade, so a
 * concolic there would invent an example nothing computed. But the ICB's width IS a picked environment fact,
 * so every used value the equation above derives from it inherits that domain:
 * `parseInt(getComputedStyle(el).width) < 768` is the same responsive gate as `innerWidth < 768`, and
 * answering it with a bare 1264 deletes a responsive bundle's whole mobile world exactly as viewport.h warns.
 * That is PROPAGATION and not a second policy — every operand's domain rides its result — and css_length.h
 * states the shape it rides in: the EXAMPLE is the number, which is what C compares and what the arithmetic
 * here runs on, and the FACT is what the JS boundary mints the domain from. `viewport_env_derived` is that
 * boundary and it is the only switch over the fact in the engine, so a used length either crosses to a page
 * through it or does not cross at all.
 * AND THE ICB IS NO LONGER THE ONLY FACT THAT ARRIVES HERE, which is why every arm below reads its operands
 * through core/css/css_computed_value.h's `css_computed_length` rather than parsing text. A computed value is
 * already absolutized when it reaches this component, so a `width: 50vw` arrives carrying the ICB's fact and a
 * `border: 1px solid` arrives carrying the DEVICE PIXEL RATIO's — css-values §6 snaps a border width to a whole
 * number of device pixels, so the seven terms of §10.3.3's equation are not all functions of the same
 * environment fact, and a box with a real border and a `width: auto` is a function of BOTH. THE ANSWER CARRIES
 * BOTH: css_length.h makes a length's fact a SET and every arm below unions its operands', so the used width is
 * one value whose domain is the RELATION between the initial containing block and the device pixel ratio —
 * solver/concolic.h's joint source identity — and not a choice between them. Nothing here has to know that:
 * the arithmetic is stated over the examples exactly as it was, and the union rides it.
 *
 * NOTHING HERE IS STORED, SO NOTHING HERE TIME-TRAVELS — and that is a decision with a reason, not an omission.
 * A layout is per-flow state: two flows with different DOMs have different boxes, and a box tree cached across
 * a context switch would be exactly the shared state the COW delta does not swap. So there is no box tree.
 * Every used value is DERIVED PER READ from the running flow's own tree and its own cascade — the identical
 * decision css_style_declaration.c made for the cascade itself and for the identical reason — which makes it
 * per-flow by construction, with no capture to write and no entry to unapply. The day a box tree exists for a
 * reason a derivation cannot serve (an inline formatting context's line boxes cannot be re-derived per read
 * without re-running the whole flow's layout), it is per-flow state and it needs solver/dom_cow.h's capture at
 * its accessor, exactly as a browser component's own C record does. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_USED_VALUE_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_USED_VALUE_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"

/* THE USED VALUE of `name` on `el`, in CSS pixels. `name` is one of the physical box-model lengths CSSOM §9
   routes here — the four margins, the four paddings, `width` and `height` — and the caller has ALREADY
   established §9's two conjuncts (the property applies to the element, and the element generates a box), which
   is why nothing here re-asks them. A case CSS 2.1 §10 defines and this component does not compute crashes
   naming its own section; there is no fallback answer.
   IT IS A `CssPx` AND NOT A `double` because §10.1's base case is the viewport — see the header above and
   css_length.h. A caller that reports one to a page mints its domain through `viewport_env_derived`; a caller
   that does arithmetic on one uses css_length.h's, which carries the UNION of its operands' facts, so a used
   value that is a joint function of several of them stays one value with one domain. */
CssPx used_value_px(lxb_dom_element_t *el, const char *name);

/* THE USED EXTENT OF THE PADDING EDGE on one axis, in CSS pixels — the horizontal one for `vertical` false and
   the vertical one for true. CSSOM VIEW §6's `clientWidth` and `clientHeight` step 3 is its caller, and the
   header above derives it: the content box on that axis plus the two paddings, with css-sizing §5 deciding
   which box `used_value_px` handed back.
   IT IS AN EXTENT AND NOT AN EDGE POSITION, which is the whole reason these two §6 members can be answered
   while `getClientRects()` cannot: a POSITION is a coordinate in the ICB's own space and needs §9.4's flow
   layout to place each box inside the containing block §10.1 gives it, and a distance between two parallel
   edges of ONE box needs none of that — only the chain's WIDTH, which §10.1 answers.
   The caller has already established §6's step 1 — the element has an associated box and that box is not
   inline — which is what makes the size properties apply to it at all. Every arm CSS 2.1 §10 defines and this
   component does not compute crashes through `used_value_px` naming its own section. */
CssPx used_value_padding_edge_px(lxb_dom_element_t *el, bool vertical);

/* THE USED EXTENT OF THE BORDER EDGE on one axis, in CSS pixels — CSS 2.1 §8.1's "Box dimensions", whose
   "border edge surrounds the box's border" and whose "four border edges define the box's border box". It is
   the padding edge plus the two border widths on the axis, and it is an ENTRY beside the padding edge rather
   than arithmetic at a caller for the same reason that one is: which box `used_value_px` handed back is
   css-sizing §5's question, and a caller holding only the number cannot answer it. Both go through the one
   four-term surround, so the two edges cannot come to describe different boxes.
   ITS CALLER IS CSSOM VIEW §6's `getClientRects()` STEP 3 — a box fragment's BORDER AREA — and CSSOM VIEW §7's
   `offsetWidth`/`offsetHeight` are the second, which is why it is stated here once rather than in either.
   IT IS AN EXTENT AND NOT AN AREA. A border AREA is this extent on both axes AND the box's POSITION;
   core/layout/flow_position.h owns that half. */
CssPx used_value_border_edge_px(lxb_dom_element_t *el, bool vertical);

/* THE SAME BORDER EDGE, for a caller that has ALREADY derived the box's CONTENT extent on that axis. CSS 2.1
   §8.1's box model is one nesting — content, then padding, then border — so this is that content extent plus
   the four terms css-sizing §5's conversion is stated over, computed in the one function that owns them.
   §10.4/§10.7's clamp is asserted here as it is on every other path.
   ITS CALLER IS core/layout/block_flow.c AND A CYCLE IS WHY IT EXISTS. A `height: auto` box's content extent
   IS CSS 2.1 §10.6.3's walk, and `used_value_px(el, "height")` is what RUNS that walk — so a walk that asked
   the entry above for one of its own children's border boxes would re-enter itself one level down, and every
   level of the tree would be laid out twice over. The conversion is stated once, here, and the walk hands in
   the number it already holds. */
CssPx used_value_border_edge_from_content_px(lxb_dom_element_t *el, CssPx content, bool vertical);

/* THE BOX'S OWN CONTENT EXTENT on one axis, in CSS pixels — CSS 2.1's `width` and `height` in the sense CSS
   2.1 itself means them, which is NOT always what `used_value_px` answers. css-sizing §5 makes the used value
   "as exposed for instance through getComputedStyle()" refer to the BORDER box under `box-sizing: border-box`,
   and a caller doing geometry with it wants the content box — so this is §5's conversion run once, in the one
   place that owns its four terms, rather than a subtraction each caller would have to know to perform and
   would double-count in the other mode.
   TWO SPECS ASK FOR IT BY NAME AND BOTH ARE ABOUT REPLACED CONTENT. css-images-3 §4.5 "Sizing Objects: the
   object-fit property" defines the CONCRETE OBJECT SIZE under the initial `fill` as "the element's used width
   and height" — the box the object is drawn into, which is the content box — and that is what HTML
   §4.8.4.3.11 "Parsing a sizes attribute" step 3.3 substitutes for a `sizes="auto"`. HTML §4.8.3's determine
   the dimensions asks the same question through "its RENDERED width and height", which is what
   `HTMLImageElement.width` reports. */
CssPx used_value_content_px(lxb_dom_element_t *el, bool vertical);

/* CSS 2.1 §10.3.2's AND §10.6.2's DEFAULT REPLACED SIZE — the 300 x 150 rectangle a replaced element with no
   natural dimensions gets, capped by the device. `vertical` false is the width.
   IT TAKES NO REALM, DELIBERATELY: the cap is against the OUTPUT DEVICE (core/frame/screen.h) and not against
   the viewport, so it is one answer for the whole agent. That is also what makes core/frame/viewport.c able to
   call it — a child navigable's viewport IS this rectangle when its container has no author size, so a value
   that depended on a viewport would be defining itself. See used_value.c for the citation and for the assert
   that keeps the answer's domain a single point. */
CssPx used_value_default_replaced_size(bool vertical);

/* CSS 2.1 §10.1's CONTAINING BLOCK as an ELEMENT — the box whose CONTENT EDGE is the rectangle every
   percentage and every `auto` in §10.3 is stated against. NULL exactly for the ROOT ELEMENT, whose containing
   block is §10.1's first case, the initial containing block, and is no element's box.
   IT IS EXPORTED BECAUSE A RECTANGLE HAS A POSITION AS WELL AS A WIDTH. This component derives the width and
   needs nothing else of the box; core/layout/flow_position.c and core/layout/block_flow.c need the BOX — its
   origin, its top and left border and padding, and the child list §9.4.1 stacks below it — and a second walk
   for it would be §10.1's four cases implemented twice, free to disagree about which ancestor the rectangle
   belongs to. Every case §10.1 defines and this component does not answer crashes naming that case. */
lxb_dom_element_t *used_value_containing_block(lxb_dom_element_t *el);

/* THE SAME RECTANGLE'S WIDTH — §10.1's first case out of the viewport, its second out of the CONTENT EDGE of
   the box above. It is a second entry rather than arithmetic over the first because the first case has no box
   at all, so a caller holding the NULL could not derive it. */
CssPx used_value_containing_block_width(lxb_dom_element_t *el);

#endif
