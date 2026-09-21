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
 * WHAT THIS COMPONENT COMPUTES TODAY, AND WHY THAT SET AND NOT A LARGER ONE. The set is every arm of §10 whose
 * operands exist, plus the box edge stated over them and the containing block they are all stated against.
 * INTRINSIC SIZES ARE NO LONGER OUTSIDE IT — §10.3.5's shrink-to-fit reads core/layout/intrinsic_size.h, which
 * measures the box's own text with the first available font (core/css/font_metrics.h) and finds its soft wrap
 * opportunities (core/layout/text_run.h, over core/layout/line_break.h's [UAX14] rules, which answer for every
 * Unicode code point) — so what bounds the set now is which BOXES that walk can measure rather than which
 * CHARACTERS: it crashes for a child that is an element, naming css-sizing-3 §5.2's contributions, and for a
 * `white-space` value whose white space css-text-3 §4.1.1 preserves. A CONTENT-BASED HEIGHT used to be outside
 * the set too: §10.6.3's own bullets say the font is needed only for the LINE-BOX arm, and its
 * block-level-children arm is a walk over used heights and collapsing margins
 * that core/layout/block_flow.h now runs — so a `height: auto` box whose children are block-level answers
 * here, and one with inline content crashes inside that walk naming §9.4.2:
 *   - A MARGIN OR PADDING whose computed value is an absolute length. Nothing in CSS 2.1 alters it. §10.3.3's
 *     constraint equation solves for `auto` values and, when the box is OVER-CONSTRAINED, for one horizontal
 *     margin; it never touches a vertical margin, and §10.6.3 gives the vertical pair exactly one rule (`auto`
 *     becomes 0). Padding has no `auto` and appears in no equation as an unknown. So the used value IS the
 *     computed length, and saying so is a derivation rather than a shrug.
 *   - A `width` or `height` whose computed value is an absolute length, for every box type but a table box and
 *     a flex or grid item. §10.3.3's equation solves for `width` only when `width` is `auto`; §10.3.5,
 *     §10.3.7 and §10.3.9 each say the same for their own box type. A TABLE may be widened past it (§17.5.2.2
 *     Automatic table layout makes a declared width a FLOOR that CAPMIN and the columns' MIN may exceed) and a
 *     FLEX ITEM's size is its container's algorithm and not §10's at all, so this bullet answers for neither.
 *     WHAT HAPPENS INSTEAD IS A ROUTE PER BOX TYPE PER AXIS, and neither half of that sentence is a spare
 *     word: a TABLE box's width is core/layout/table_width.h's §17.5.2 and its height is
 *     core/layout/table_height.h's §17.5.3, both on the declared arm and the `auto` arm alike, because each of
 *     those sections takes the declaration as an INPUT to its own comparison rather than as the used value. A
 *     CELL is answered on both axes too — its width is the used width of the columns its rectangle covers and
 *     its height is the rows' — and a ROW is answered on the block axis alone, which is where §17.5.3 states
 *     it and §17.5.2 does not. A CAPTION IS NOT ONE OF §17.5's BOXES AT ALL and is answered by §10 like any
 *     other block-level box in normal flow: §17.4 renders it "as normal block boxes inside the table wrapper
 *     box", and no algorithm of §17.5 is stated over it — §17.5.2 reads it only as CAPMIN, an intrinsic
 *     minimum it feeds into the TABLE's width. WHAT STILL CRASHES IS THREE BOXES AND EACH FOR ITS OWN REASON,
 *     not one gap: a ROW's width and a ROW GROUP's both ways (§17.5's rules 1 and 2 place them and §17.5.3
 *     declines a row group's height outright), and a COLUMN or COLUMN GROUP both ways (§17.5's rules 3 and 4
 *     are a placement nothing here performs). A flex or grid item crashes on both.
 *   - AND §10.4 "Minimum and maximum widths: 'min-width' and 'max-width'" and §10.7 "Minimum and maximum
 *     heights: 'min-height' and 'max-height'", WHICH ARE A SECOND PASS AND NOT A CLAMP ON THE NUMBER. Both
 *     sections say the same three sentences about their own axis: the tentative used value is §10.3's answer
 *     computed WITHOUT the limits, and then "the rules above are applied AGAIN, but this time using the
 *     computed value of 'max-width' AS THE COMPUTED VALUE FOR 'width'" — so what is substituted is the input
 *     to the whole of §10.3, which re-solves whichever MARGIN was `auto`. `margin: 0 auto; max-width: 1200px`
 *     on an `auto`-width block is the case that makes the difference visible: with the substitution the
 *     margins reach §10.3.3's rules 4 and 6 and split the slack, which is what centres the box; with a clamp
 *     on the number alone rule 5 still sees `auto` and both margins are 0. The two limits' own used values are
 *     part of this: a percentage `max-width` resolves against the containing block's WIDTH (§10.4), a
 *     percentage `max-height` against its HEIGHT — and §10.7 has a rule rather than an omission for the common
 *     case where that height is indefinite, "the percentage value is treated as '0' (for 'min-height') or
 *     'none' (for 'max-height')". `min-width: auto` is not CSS 2.1's value at all: css-sizing-3 §3.1.2 makes
 *     it the INITIAL value and §3.2 resolves it to a used 0 for every box that is not a flex or grid item,
 *     which is the same 0 CSS 2.1 initialises the property to. A `min()`/`max()`/`clamp()` LIMIT needs nothing
 *     here — core/css/css_math.h resolves a math function inside the computed value, so it arrives as one
 *     absolute length already carrying the union of its operands' environment facts.
 *     §10.4's OTHER algorithm — the constraint-violation table, for a replaced element with an intrinsic ratio
 *     and both sizes `auto` — is a joint solve over BOTH axes that preserves the ratio, and it CRASHES naming
 *     itself. Its antecedent is false in this build for a reason that is a fact about the tree rather than
 *     about the spec: core/layout/replaced_element.h mints an intrinsic ratio nowhere, because the only object
 *     with natural dimensions is HTML §15.4.2's fourth rule's 0-by-0 one and css-images-3 §4.1's degenerate
 *     test denies that a ratio. An image DECODER is what makes it reachable.
 *   - AND THE SAME SIZE UNDER `box-sizing: border-box`, which is a COMPUTATION and not a second assertion.
 *     What stood here called it one, on the ground that css-sizing §5 "makes the declared value the BORDER
 *     box's while §10.2 and CSSOM §9 both mean the content box's" — and BOTH HALVES OF THAT WERE
 *     UNRESOLVABLE: css-sizing-3 §5 is "Intrinsic Size Determination", which decides nothing about
 *     `box-sizing`, and an unlevelled `css-sizing` names no document. The section is css-sizing-3 §3.3 "Box
 *     Edges for Sizing: the box-sizing property", and its own sentence says the opposite of what stood here:
 *     "Used values of the sizing properties, as exposed for instance through getComputedStyle(), also refer to
 *     the border box." So the exposed used value is the border box's size, which is the declared length except
 *     when the paddings and borders alone exceed it and the content box floors at zero (§3.3's own worked
 *     example: "the border-box size ends up at 120px, even though width: 100px is specified for the border
 *     box"). It is the LARGER of the two, and the four terms it needs are the two paddings and the two border
 *     widths.
 *   - AND THE PADDING EDGE'S EXTENT, which is not a property at all and is exactly why it is an entry HERE
 *     rather than arithmetic at the one caller that wants it. CSS 2.1 §8's box model makes the padding box the
 *     CONTENT box plus the two paddings on the axis, and that one sentence is the whole derivation in BOTH
 *     `box-sizing` modes — what differs between them is not the padding edge, it is which box the USED SIZE
 *     above is the size OF. Under `content-box` that size IS the content box and the two paddings are added;
 *     under `border-box` css-sizing-3 §3.3 makes it the BORDER box, so the content box is that size minus the
 *     same four terms §3.3's own conversion names, and the paddings are added back to THAT. A CALLER CANNOT
 *     WRITE THIS: `used_value_px(el, "width")` plus the two paddings is the answer in one mode and DOUBLE-COUNTS
 *     them in the other, and nothing in the number it got back says which mode produced it. So the four terms
 *     are computed once, in one function, and both directions of §3.3's conversion are stated over that one
 *     result — the double-count is not a mistake to avoid, it is a sentence there is no longer anywhere to
 *     write.
 *   - A MARGIN, PADDING OR `width` whose computed value is a PERCENTAGE, which §8.3, §8.4 and §10.2 all resolve
 *     against the same measure: the WIDTH of the containing block, and for §8.3 and §8.4 that is true of the
 *     VERTICAL sides too ("even for 'padding-top' and 'padding-bottom'"). That is the rule most implementations
 *     get wrong, and it is why neither arm below takes an axis. A percentage `height` is the exception and it
 *     IS here, on its own basis: §10.5 resolves it against the containing block's HEIGHT, which unlike a width
 *     may not exist at all — and when it does not, css-sizing-3 §3.2.1 makes the property BEHAVE AS AUTO and
 *     the §10.6 arms run instead. `used_value_height_behaves_as_auto` below is that question, and it is a
 *     used-value question rather than the computed-value rule CSS 2.1's own prose describes.
 *   - AND §10.3.3's CONSTRAINT EQUATION, which is what `width: auto` on an ordinary block-level box resolves
 *     through and is therefore the arm most of the web reaches first. Its seven terms,
 *         margin-left + border-left-width + padding-left + width + padding-right + border-right-width +
 *         margin-right = width of containing block
 *     are every one of them read back through the arms above, so the section is a solve for whichever of them
 *     is `auto` (rule 5 for `width`, rules 2/4/6 for the margins) and nothing more. Its own floor at zero is
 *     css-sizing-3 §3.1 "Sizing Properties"' — "the inner size is always floored at zero" — and NOT §10.4's
 *     `min-width: 0` running early, which is what stood here and made §10.4's real second pass look already
 *     done.
 *     WHICH css-sizing-3 SECTION OWNS THAT FLOOR IS WHAT THIS TREE GOT WRONG AT EIGHT SITES, AND THE TWO
 *     CANDIDATE SECTIONS FLOOR DIFFERENT COMPUTATIONS, so the choice is not a matter of taste. §3.1 states
 *     the floor UNCONDITIONALLY, of every sizing property, with no box model and no declaration in sight,
 *     and §2 "Terminology" defines an "inner size" as "The content-box size of a box" — so §3.1 is the
 *     floor any derivation of a content size may lean on, this equation's included. §3.3 "Box Edges for
 *     Sizing: the box-sizing property"' floor is NARROWER and is a step of ONE computation: "the content
 *     box width and height are calculated by subtracting the border and padding in the corresponding axis
 *     from the specified <length-percentage>, and flooring the result at zero (as the inner size of a box
 *     cannot be negative)". Cite §3.3 for the `border-box` conversion and §3.1 for everything else; citing
 *     §3.3 for a floor that is not that conversion quotes a sentence about a computation the site never
 *     performs.
 *     AND THE RETIRED WORDING WAS A SUPERSEDED STANDARD'S REAL SENTENCE RATHER THAN A FABRICATION, which is
 *     why it read so well and why eight sites agreed on it: `box-sizing` was defined by css-ui-3 §3.1
 *     "Changing the Box Model: the box-sizing property" before css-sizing-3 took it over, and that section
 *     states its own floor at 0 in wording css-sizing-3 REWROTE — so quoting css-ui-3's rendering under a
 *     css-sizing-3 citation is a mis-aimed quotation, not an invented one, and no quotation check can see
 *     it because the words really are some standard's. css-ui-4 records the handover in its own prose
 *     ("has been moved to CSS Sizing 3 § 3.3 Box Edges for Sizing: the box-sizing property"), which is what
 *     makes css-sizing-3 the maintained document and css-ui-3's sentence a retired edition's. A reader who
 *     finds that sentence in css-ui-3 and reaches for it again is re-deriving the defect: it is owed to no
 *     citation here, and css-sizing-3 says neither of the two sentences above in those words.
 *     What §10.3.3 still crashes for is its OVER-CONSTRAINED case, and it is not a layout gap:
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
 *     extents. BOTH ARE ANSWERED NOW and this clause used to say they still waited on the STATIC POSITION
 *     their `auto` offsets fall back to: core/layout/block_flow.h reads that would-be position out of
 *     §9.4.1's own walk, core/layout/flow_position.h turns it into a coordinate, and `uv_abs_solve` runs
 *     §10.3.7's and §10.6.4's constraint equations over it — so `used_value_abs_containing_block` names each
 *     rectangle and `used_value_abs_offset_px` answers the offset. A reader who follows the retired clause
 *     will build a second static position.
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
 * without re-running the whole flow's layout), it is per-flow state and it needs a capture at its accessor,
 * exactly as a browser component's own C record does: solver/cow.h's `cow_capture_host_record`, over the
 * record's owned-value layout. THIS CLAUSE NAMED solver/dom_cow.h AND THAT WAS THE WRONG HEADER, recorded
 * here rather than quietly corrected because a next-diff clause is read once, by someone who has already
 * decided to do the work. That header's delta is the DOM TREE and the ATTRIBUTE LIST; it has no host-record
 * primitive at all, so a reader sent there finds nothing, concludes the pattern does not exist, and builds a
 * second capture beside the one every component with an owned-value record already shares.
 * HOW A MISSING CAPTURE WOULD SHOW: a geometry that does not move when the running flow's own tree does —
 * two flows that mutated one document differently reading one box back. RETIREMENT: this record goes when a
 * box tree exists and its accessor carries that capture. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_USED_VALUE_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_USED_VALUE_H

#include <stdbool.h>
#include <stddef.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"
#include "core/layout/replaced_element.h"

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
   header above derives it: the content box on that axis plus the two paddings, with css-sizing-3 §3.3
   deciding which box `used_value_px` handed back.
   IT IS AN EXTENT AND NOT AN EDGE POSITION, and the two are different components for that reason: a POSITION
   is a coordinate in the ICB's own space that §9.4's flow layout produces by placing each box inside the
   containing block §10.1 gives it (core/layout/flow_position.h), while a distance between two parallel edges
   of ONE box needs none of that — only the chain's WIDTH, which §10.1 answers. CSSOM VIEW §6's `clientWidth`
   is the extent and its `scrollWidth` is a right-most POSITION over this box and every descendant's
   (core/layout/scrolling_area.h), which is why one member of one section reaches both.
   The caller has already established §6's step 1 — the element has an associated box and that box is not
   inline — which is what makes the size properties apply to it at all. Every arm CSS 2.1 §10 defines and this
   component does not compute crashes through `used_value_px` naming its own section. */
CssPx used_value_padding_edge_px(lxb_dom_element_t *el, bool vertical);

/* THE USED EXTENT OF THE BORDER EDGE on one axis, in CSS pixels — CSS 2.1 §8.1's "Box dimensions", whose
   "border edge surrounds the box's border" and whose "four border edges define the box's border box". It is
   the padding edge plus the two border widths on the axis, and it is an ENTRY beside the padding edge rather
   than arithmetic at a caller for the same reason that one is: which box `used_value_px` handed back is
   css-sizing-3 §3.3's question, and a caller holding only the number cannot answer it. Both go through the
   one
   four-term surround, so the two edges cannot come to describe different boxes.
   ITS CALLER IS CSSOM VIEW §6's `getClientRects()` STEP 3 — a box fragment's BORDER AREA — and CSSOM VIEW §7's
   `offsetWidth`/`offsetHeight` are the second, which is why it is stated here once rather than in either.
   IT IS AN EXTENT AND NOT AN AREA. A border AREA is this extent on both axes AND the box's POSITION;
   core/layout/flow_position.h owns that half. */
CssPx used_value_border_edge_px(lxb_dom_element_t *el, bool vertical);

/* CSS 2.1 §8.1 "Box dimensions"' ONE EDGE between a box's BORDER box and its PADDING box on the axis's LEADING
   side — the `border-left-width` for `vertical` false and the `border-top-width` for true, as USED values.
   IT IS AN ENTRY BECAUSE THE COMPUTED VALUE IS NOT ALWAYS THE USED ONE, and that is the whole of its reason
   for existing. CSS 2.1 §17.6.1 The separated borders model makes a row, row group, column or column group
   box's border widths ZERO ("Rows, columns, row groups, and column groups cannot have borders (i.e., user
   agents must ignore the border properties for those elements)"), §17.6.2 The collapsing border model makes a
   TABLE box's HALF of the collapsed border at the grid's edge and a CELL's half of the resolved border at
   each INTERIOR grid line, and none of those is a value the cascade holds. A caller reading
   `css_computed_length(el, "border-left-width")` gets the declaration in every one of those cases, so the
   question is asked here, once, for every consumer.
   ITS CALLER IS core/layout/flow_position.c's PADDING-BOX ORIGIN — CSS 2.1 §8.1's border box origin moved
   inward by this one edge on each axis. */
CssPx used_value_leading_border_px(lxb_dom_element_t *el, bool vertical);

/* CSS 2.1 §8.5 "Border properties"' FOUR USED BORDER WIDTHS AT ONCE, written to `out` in the top/right/bottom/
   left order every four-side rule in CSS states — which is the order CSS 2.1 §8.5.1 "Border width:
   'border-top-width', 'border-right-width', 'border-bottom-width', 'border-left-width', and 'border-width'"
   defines its own shorthand over ("If there are four values, they apply to the top, right, bottom, and left,
   respectively").
   IT IS ONE ENTRY FOR FOUR SIDES RATHER THAN FOUR ASKS OF THE ENTRY ABOVE, AND THE ENTRY ABOVE CANNOT ANSWER
   THREE OF THEM ANYWAY. `used_value_leading_border_px` is the LEADING side of one axis — the top for
   `vertical` and the left otherwise — so between its two arms it reaches two of the four sides and there is no
   argument it takes that names the right or the bottom. A caller wanting all four has to have this entry.
   AND FOUR SEPARATE ASKS WOULD NOT MERELY BE SLOWER, THEY COULD DISAGREE. CSS 2.1 §17.6.2 "The collapsing
   border model"'s widths are answered by gathering the table's boxes and BUILDING ITS GRID per ask
   (core/layout/table_border_collapse.h names that cost), so four asks build one table's grid four times — and
   a caller assembling four independent answers could be handed widths from two different builds of one grid.
   Deriving the model and the collapsed edges ONCE here is what makes the four answers one derivation.
   ITS CALLER IS core/paint/box_paint.c's CSS 2.1 §E.2 "Painting order" BORDER MARK, which needs all four and
   needs them TOGETHER: CSS 2.1 §8 states no rule anywhere for where two borders meet — the string "corner"
   does not occur in it — so the region between two adjacent sides belongs to neither of them alone, and a
   painter holding one side's width cannot place even that side's own area. */
void used_value_border_widths_px(lxb_dom_element_t *el, CssPx out[4]);

/* THE SAME SIDE'S TWO EDGES — that border width plus that side's USED padding, which is CSS 2.1 §8.1's
   distance from a box's BORDER edge to its CONTENT edge on the leading side of one axis.
   ITS CALLERS ARE §10.1's SECOND CASE AND §9.4.2's FRAGMENTS, both core/layout/flow_position.c: a containing
   block is "the content edge of the nearest block container ancestor box" while the coordinate this engine
   places every box at is its BORDER edge, and this pair is the whole of the difference.
   THE PADDING IS NOT ALWAYS THE CASCADE'S EITHER, which is why this is not the entry above plus
   `used_value_px(el, "padding-left")` at a caller. CSS 2.1 §8.4 "Padding properties: 'padding-top',
   'padding-right', 'padding-bottom', 'padding-left', and 'padding'"' Applies-to line excludes the row, row
   group, column and column group boxes, and CSS 2.1 §17.6.2 The collapsing border model says of a table box
   that "in this model, a table does not have padding (but does have margins)" — so a caller composing the two
   reads itself would place every row of a `border-collapse: collapse` table by a padding the table does not
   have. See used_value.c for the routing and for the five box types it answers. */
CssPx used_value_leading_edge_px(lxb_dom_element_t *el, bool vertical);

/* THE USED EXTENT OF THE MARGIN EDGE on one axis, in CSS pixels — CSS 2 §8.1 "Box dimensions"' outermost
   nesting, "the margin edge surrounds the box margin … the four margin edges define the box's MARGIN BOX". It
   is the border edge plus the two margins on the axis, and it is the third entry of the same one nesting for
   the reason the second is: which box `used_value_px` handed back is css-sizing-3 §3.3's question, so a
   caller holding only the number cannot add the right terms to it.
   ITS CALLER IS CSS 2.2 §10.8's STEP 1 — "for replaced elements, inline-block elements, and inline-table
   elements, this is the HEIGHT OF THEIR MARGIN BOX; for inline boxes, this is their 'line-height'" — and CSS
   2.2 §9.4.2's line, which puts the same box's inline extent between its neighbours ("horizontal margins,
   borders, and padding are respected between these boxes"). Both are core/layout/line_box.c, which reads this
   on one axis for the height and on the other for the width of the run item that carries an atomic inline.
   IT CAN BE NEGATIVE AND THAT IS NOT A DEFECT, which is the one way it differs from the two edges above: CSS
   2.2 §8.3 "Margin properties" says "negative values for margin properties are allowed", so `margin: -100px`
   on a 10px box makes its margin box −190px on that axis. §8.1's nesting is unconditional and the arithmetic
   is stated over it, so there is no floor here — a caller that needs one is asking a different section's
   question.
   IT IS AN EXTENT AND NOT AN AREA, exactly as the two above are: an area is this extent on both axes AND the
   box's POSITION, which core/layout/flow_position.h owns. */
CssPx used_value_margin_edge_px(lxb_dom_element_t *el, bool vertical);

/* THE SAME BORDER EDGE, for a caller that has ALREADY derived the box's CONTENT extent on that axis. CSS 2.1
   §8.1's box model is one nesting — content, then padding, then border — so this is that content extent plus
   the four terms css-sizing-3 §3.3's conversion is stated over, computed in the one function that owns them.
   §10.7's CLAMP RUNS HERE, over the extent handed in — which IS §10.4/§10.7 step 1's "tentative used height",
   so this is the same algorithm and not a second copy of it. It must run: this box is being stacked inside its
   parent's own §10.6.3 walk, and a child that reported an unclamped height would make the parent's height
   wrong as well as its own, with nothing downstream to say so.
   ITS CALLER IS core/layout/block_flow.c AND A CYCLE IS WHY IT EXISTS. A `height: auto` box's content extent
   IS CSS 2.1 §10.6.3's walk, and `used_value_px(el, "height")` is what RUNS that walk — so a walk that asked
   the entry above for one of its own children's border boxes would re-enter itself one level down, and every
   level of the tree would be laid out twice over. The conversion is stated once, here, and the walk hands in
   the number it already holds. */
CssPx used_value_border_edge_from_content_px(lxb_dom_element_t *el, CssPx content, bool vertical);

/* THE BOX'S OWN CONTENT EXTENT on one axis, in CSS pixels — CSS 2.1's `width` and `height` in the sense CSS
   2.1 itself means them, which is NOT always what `used_value_px` answers. css-sizing-3 §3.3 makes the used
   value "as exposed for instance through getComputedStyle()" refer to the BORDER box under
   `box-sizing: border-box`, and a caller doing geometry with it wants the content box — so this is §3.3's
   conversion run once, in the one place that owns its four terms, rather than a subtraction each caller would
   have to know to perform and
   would double-count in the other mode.
   TWO SPECS ASK FOR IT BY NAME AND BOTH ARE ABOUT REPLACED CONTENT. css-images-3 §4.5 "Sizing Objects: the
   object-fit property" defines the CONCRETE OBJECT SIZE under the initial `fill` as "the element's used width
   and height" — the box the object is drawn into, which is the content box — and that is what HTML
   §4.8.4.3.11 "Parsing a sizes attribute" step 3.3 substitutes for a `sizes="auto"`. HTML §4.8.3's determine
   the dimensions asks the same question through "its RENDERED width and height", which is what
   `HTMLImageElement.width` reports. */
CssPx used_value_content_px(lxb_dom_element_t *el, bool vertical);

/* THE SAME EXTENT COMPUTED AS THOUGH THE BOX WERE BLOCK-LEVEL IN NORMAL FLOW — CSS 2.1 §10.6 "Calculating
   heights and margins"' and §10.7 "Minimum and maximum heights: 'min-height' and 'max-height'"' rules run with
   the box classified as §10.3.3's rather than as whatever it is, as a CONTENT-box extent.
   ITS ONE CALLER IS css-flexbox-1 §9.4 "Cross Size Determination"' STEP 7 AND THE "AS THOUGH" IS THAT STEP'S
   OWN WORDS: "Determine the hypothetical cross size of each item by performing layout as if it were an
   in-flow block-level box with the used main size and the given available space, treating auto as
   fit-content." WHICH LAYOUT "PERFORMING LAYOUT" MEANS IS NOT LEFT TO THE READER, and it is worth saying where
   that is settled because the sentence above does not settle it: css-flexbox-1's Changes list records the
   clarification that performing layout there means using the block-level layout rules, against its Issue 5188.
   So this is not a reclassification invented for a caller's convenience — it is the section naming the
   algorithm it wants, and this entry running exactly that one.
   IT IS THE ONLY WAY TO ASK, WHICH IS WHY IT IS AN ENTRY AND NOT A CALLER'S TWO LINES. `used_value_px` reads
   the box's REAL kind, and for a flex item that kind answers §9.4's step 11 — the item's USED cross size,
   which depends on the cross size of the line it is on. Step 7's HYPOTHETICAL cross size is a DIFFERENT
   QUANTITY, stated one step earlier and deliberately about a box that is NOT being treated as a flex item,
   and it is what step 8 takes the largest of; asking `used_value_px` for it would get step 11's answer, which
   for a stretched item is the LINE's cross size and is therefore the very number step 8 is trying to compute.
   THE SENTENCE HERE USED TO SAY `used_value_px` CRASHES ON BOTH ARMS FOR A FLEX ITEM, and it is rewritten
   rather than deleted because the reason it gave is still the reason this entry exists and a reader who
   re-derives it from the crashes that remain will re-add it: those crashes are now what step 11's route
   DECLINES — a GRID item, a `column` container's item, a `contents` splice — and not the flex cross axis.
   A caller resolving the declaration itself would be a second copy of §10.6 and §10.7, including their
   percentage rules and their non-commutative min/max order.
   `min-height: auto` ANSWERS 0 HERE AND THAT IS THE POINT RATHER THAN A SIDE EFFECT. css-sizing-3 §3.2
   "Sizing Values: the <length-percentage [0,∞]>, auto | none, stretch, min-content, max-content, and
   fit-content values" hands that keyword to "the relevant layout module" and falls back to a used value of 0,
   and css-flexbox-1 §4.5 "Automatic Minimum Size of Flex Items" claims only ONE axis of it — "the used value
   of a main axis automatic minimum size on a flex item whose computed overflow value is non-scrollable is its
   content-based minimum size". So the CROSS axis has no module rule to be handed to and §3.2's 0 stands, which
   is exactly what §10.7 reads when the box is read as block-level.
   WHAT IT REFUSES IS TWO BOXES AND ONE OF THEM ONLY ON ONE ARM, which is a distinction worth keeping because
   the narrower refusal is the one a reader would otherwise widen. A TABLE box is refused outright: CSS 2.1
   §17.5.3 Table height algorithms owns its height under every value of the property, so there is no arm on
   which §10.6 is its section. A REPLACED element is refused only where its size on the asked-for axis BEHAVES
   AS AUTO — with a declared one, `uv_pass_size` takes the arm a non-replaced box takes and this entry is
   simply right about it, and it is only §10.6.2's natural-dimension and intrinsic-ratio arms that would divide
   by a width this entry's own reclassification has taken away from css-flexbox-1 §9.3 "Main Size
   Determination". EVERY OTHER FLEX ITEM IS ALREADY BLOCK-LEVEL: css-display-3 §2.7 "Automatic Box Type
   Transformations" has blockified it, so `inline-block` and `inline-flex` arrive here as the block-level boxes
   this entry reads them as, and a FLOAT arrives as one because css-flexbox-1 §4 "Flex Items" says outright
   that for a flex item "floating is ignored". */
CssPx used_value_block_level_content_px(lxb_dom_element_t *el, bool vertical);

/* CSS 2.1 §10.3.2's AND §10.6.2's DEFAULT REPLACED SIZE — the 300 x 150 rectangle a replaced element with no
   natural dimensions gets, capped by the device. `vertical` false is the width.
   IT TAKES NO REALM, DELIBERATELY: the cap is against the OUTPUT DEVICE (core/frame/screen.h) and not against
   the viewport, so it is one answer for the whole agent. That is also what makes core/frame/viewport.c able to
   call it — a child navigable's viewport IS this rectangle when its container has no author size, so a value
   that depended on a viewport would be defining itself. See used_value.c for the citation and for the assert
   that keeps the answer's domain a single point. */
CssPx used_value_default_replaced_size(bool vertical);

/* CSS 2.1 §10.3.2 "Inline, replaced elements"' FIVE ARMS, as a CONTENT-box width in CSS pixels, over the
   natural dimensions `rep` states. `rep` is the CALLER'S OWN `replaced_element_of` answer and not one derived
   here, because HTML §15.4.2's classification "CHANGES under the running flow as a reply lands"
   (core/layout/replaced_element.h) — so two reads inside one algorithm are free to disagree about whether the
   element is replaced at all, and the caller that already holds one hands it over.
   THE PRECONDITION IS `width` BEHAVING AS `auto`, AND THE TWO CALLERS ESTABLISH IT BY DIFFERENT ROUTES, which
   is why it is not asserted here and why each caller states its own. §10.3's dispatch establishes it by
   READING the computed value, because a declared `width` takes §10.3.3's equation instead. css-sizing-3 §5.1
   "Intrinsic Sizes" establishes it BY DEFINITION and reads nothing: a box's intrinsic size in an axis is "the
   size it would have if it was a float given an AUTO preferred size IN THAT AXIS (and no minimum or maximum
   size in that axis)", so the box's own declared `width` is removed before this runs and asserting it `auto`
   would abort on `<img width="18">`, which is a document rather than a defect. The OPPOSITE axis is NOT
   removed by that sentence — css-sizing-3 §5.1 says "in that axis" twice — which is why this function still
   reads `height`, and why css-sizing-3 §5.1's own Note says "when the box has a preferred aspect ratio, size
   constraints in the opposite dimension will transfer through and can affect the auto size in the considered
   one".
   THE BOX-TYPE REFUSALS IN `uv_replaced_size` ARE DELIBERATELY NOT PART OF THIS, and that is a derivation
   rather than a convenience. That wrapper refuses a TABLE box and a FLEX/GRID ITEM because a USED width for
   either comes from its container's algorithm; an INTRINSIC size is css-sizing-3 §5.1's hypothetical FLOAT,
   and CSS 2.1 §10.3.6 "Floating, replaced elements" answers a float in one sentence — "the used value of
   'width' is determined as for inline replaced elements". So the INLINE side needs no box-type branch at all,
   and routing it through the wrapper would abort on a replaced flex item whose css-flexbox-1 §9.9.3 "Flex Item
   Intrinsic Size Contributions" contribution is exactly this number.
   NAMED RESIDUAL — THAT IS TRUE OF THE INLINE SIDE AND NOT OF THE WHOLE CALL, because arm 2 reads the
   OPPOSITE axis through the PUBLIC `used_value_px`, which routes a replaced element straight back into
   `uv_replaced_size` and therefore does carry the two refusals. WHAT IS NOT COVERED: a replaced element that
   is a FLEX or GRID ITEM or a TABLE box, whose `height` computes to something other than `auto` and which has
   a natural aspect ratio — arm 2's only shape that reads the other axis. WHAT THE NEXT DIFF BUILDS: nothing
   here. That abort is CORRECT and is the one `uv_replaced_size` already names — css-flexbox-1 §9.2 "Line
   Length Determination" makes a declared size the FLEX BASE SIZE, so the item's used height is its
   container's algorithm's answer and not §10.6.2's, and the diff that retires it is the flex layout that
   refusal asks for rather than a second door beside it. HOW ITS ABSENCE WOULD SHOW: an intrinsic-size request
   for such a box aborts naming css-flexbox-1 §9.2 from inside a css-sizing-3 §5.1 walk, so a reader arrives
   at a flex refusal while asking a sizing question. It is written down because the paragraph above reads as a
   promise that no box-type branch is reachable from here, and one is.
   NEITHER IS css-sizing-3 §3.3's `box-sizing` CONVERSION, for the reason the arms themselves have: every term
   §10.3.2 names is CSS 2.1's and CSS 2.1 knows only the content box, so the conversion belongs to whichever
   box the caller is reporting — the wrapper's used value is exposed as the border box, and
   core/layout/intrinsic_size.h's pair is documented as content-box widths. */
CssPx used_value_replaced_auto_width_px(lxb_dom_element_t *el, const ReplacedElement *rep);

/* CSS 2.1 §10.1's CONTAINING BLOCK as an ELEMENT — the box whose CONTENT EDGE is the rectangle every
   percentage and every `auto` in §10.3 is stated against. NULL exactly where §10.1's answer is the INITIAL
   CONTAINING BLOCK, which is no element's box — its FIRST case, the root element's; and the last sentence of
   its FOURTH case, "If there is no such ancestor, the containing block is the initial containing block",
   which is an absolutely positioned box at any depth with nothing positioned above it. THE NULL USED TO BE
   DOCUMENTED AS THE ROOT ELEMENT'S ALONE and that was true while the fourth case crashed: the two sentences
   name one rectangle, and the tree is what gained the second way of reaching it. A caller that read the NULL
   as "this element is the root" was reading a coincidence, and `core/layout/scrolling_area.c` held exactly
   that reading in an assert.
   IT IS EXPORTED BECAUSE A RECTANGLE HAS A POSITION AS WELL AS A WIDTH. This component derives the width and
   needs nothing else of the box; core/layout/flow_position.c and core/layout/block_flow.c need the BOX — its
   origin, its top and left border and padding, and the child list §9.4.1 stacks below it — and a second walk
   for it would be §10.1's four cases implemented twice, free to disagree about which ancestor the rectangle
   belongs to. Every case §10.1 defines and this component does not answer crashes naming that case.
   AN ELEMENT IS NOT ALWAYS ENOUGH TO NAME THE BOX, AND THIS ENTRY IS THE VIEW THAT SAYS SO RATHER THAN THE
   ONE THAT DECIDES IT. CSS 2.1 §17.4 Tables in the visual formatting model has one table element generate TWO
   boxes with two different content edges — the TABLE WRAPPER BOX and the table box inside it — so a caption's
   containing block is a box this return type cannot spell. The WALK answers it; this view REFUSES it, naming
   what a caller wanting the box still needs (the wrapper's own child box list, which is not any element's DOM
   child list). A caller that wants only the rectangle's WIDTH or its `direction` is answered by the two
   entries below, which read the same walk and do not refuse. THE `NULL` STAYS THE INITIAL CONTAINING BLOCK —
   it is never "there is no answer", which is what makes the refusal a crash rather than a third meaning for
   it.
   §10.1's THIRD AND FOURTH CASES ARE REFUSED BY THIS VIEW AND ANSWERED BY THE TWO BELOW, because their
   rectangles are not any box's CONTENT edge: the third is css-position-3 §2.1's layout viewport, which is no
   element's box at all, and the fourth is "formed by the padding edge of the ancestor", which is an element's
   box at an edge this return type cannot name. A caller adding `used_value_leading_edge_px` to what it gets
   back — which is what both callers in core/layout do — would be inset by that ancestor's own border on
   every side. */
lxb_dom_element_t *used_value_containing_block(lxb_dom_element_t *el);

/* ---- §10.1's THIRD and FOURTH cases, AS A RECTANGLE A CALLER CAN COMPOSE AN ORIGIN FROM ----------------
   The view the entry above refuses by name. Its return type is an element, and §10.1 gives an absolutely
   positioned box a rectangle that is either NO BOX AT ALL (the initial containing block, the viewport) or a
   box at an edge that entry does not carry ("the padding edge of the ancestor"), so the tag is which of the
   three and `*element` is the ancestor for the one case that has one and NULL for the other two.
   IT IS A TAG AND NOT A SECOND WALK: `uv_cb` decides §10.1's four cases once and both views read it. */
typedef enum {
    USED_VALUE_ABS_CB_INITIAL = 0,   /* §10.1's FOURTH case's last sentence — "if there is no such ancestor,
                                        the containing block is the initial containing block". No element. */
    USED_VALUE_ABS_CB_VIEWPORT,      /* §10.1's THIRD case: a `position: fixed` box's. No element. */
    USED_VALUE_ABS_CB_PADDING_EDGE   /* §10.1's FOURTH case: `*element`'s PADDING edge. */
} UsedValueAbsCb;

UsedValueAbsCb used_value_abs_containing_block(lxb_dom_element_t *el, lxb_dom_element_t **element);

/* CSS 2.1 §10.3.7's and §10.6.4's USED `left` (or `top`) — the distance from the LEADING EDGE of the
   rectangle above to this box's leading MARGIN edge, which is what those sections' one constraint equation
   solves for alongside the size and the two margins.
   IT IS THE OFFSET AND NOT THE ORIGIN, and the difference is the whole of why the two components split here:
   this file derives an EXTENT and every term of §10.3.7's equation is one, while WHERE the containing block's
   leading edge sits in the initial containing block's coordinate space is core/layout/flow_position.h's, and
   §10.1's third case has no element for this file to hand back at all.
   THE SIZE IT IS SOLVED AGAINST IS §10.4/§10.7's FINAL PASS's, so a box whose `width` was clamped by a
   `max-width` has the offset that clamp produced. That is the same substitution `margin: 0 auto` centres by
   one axis over, and it is why this entry reads `uv_sized` rather than the property.
   IT DOES NOT ANSWER THE TRAILING OFFSET, and that is not an omission: §10.3.7 and §10.6.4 place the box from
   the leading pair (`left + margin-left`, `top + margin-top`) and the trailing one is the same equation's
   remainder, so a caller composing an origin from both would be adding a number it had just subtracted. The
   day CSSOM §9's inset arm reports a resolved `right`, it is a second view over the same solve. */
CssPx used_value_abs_offset_px(lxb_dom_element_t *el, bool vertical);

/* CSS 2.2 §9.4.3 "Relative positioning"' USED TRANSLATION ON ONE AXIS — the signed distance a relatively
   positioned box is shifted by AFTER normal flow has placed it, which is the used `left` horizontally and the
   used `top` vertically because §9.4.3 states the sign in its own words ("'Left' moves the boxes to the
   right", "'Top' moves the boxes down").
   IT IS AN ENTRY BECAUSE §9.4.3 STATES A PAIR, and that is the difference between this and a `used_value_px`
   row. "Since boxes are not split or stretched as a result of 'left' or 'right', the used values are always:
   left = -right" — so a per-property answer would have to solve the other member anyway, and the four cases
   the section writes ("If both … are 'auto'", "If 'left' is 'auto'", "If 'right' is specified as 'auto'", "If
   neither … is 'auto'") are conditions over BOTH. core/css/css_computed_value.c's CSSOM §9 inset arm names
   this pair as the second of the three things it waits on; it is the same solve read a second way.
   IT ANSWERS THE LEADING MEMBER AND THE TRAILING ONE IS ITS NEGATION, for the reason `used_value_abs_offset_px`
   above answers only the leading offset: a caller composing an origin from both would add a number it had
   just subtracted. That identity is §9.4.3's own sentence and not this entry's convention.
   THE TWO AXES DIFFER IN ONE ARM ONLY. Over-constrained horizontally, §9.4.3 reads the CONTAINING BLOCK's
   `direction` — "If the 'direction' property of the containing block is 'ltr', the value of 'left' wins …
   If 'direction' of the containing block is 'rtl', 'right' wins and 'left' is ignored" — and asks
   `used_value_containing_block_is_rtl`, which is the same entry §10.3.3's over-constrained margin arm asks.
   Over-constrained vertically it names no direction at all: "If neither is 'auto', 'bottom' is ignored".
   ITS PRECONDITION IS §9.3.1's POSITIONING SCHEME AND IT IS ASSERTED. §9.4.3 is stated over a box already
   placed by normal flow; an absolutely positioned box is `used_value_abs_offset_px`'s §10.3.7 solve and a
   statically positioned one has no offset, §9.3.2's `Applies to:` line being "positioned elements". The
   caller decides which section places the box, exactly as core/layout/flow_position.c already decides it for
   `absolute` and `fixed`, and this entry refuses rather than answering zero for a box it is not about. */
CssPx used_value_rel_offset_px(lxb_dom_element_t *el, bool vertical);

/* css-position-3 §2.1 "Containing Blocks of Positioned Boxes"' OPEN LIST OF PROPERTIES, AS ONE FACT WITH TWO
   READERS. §2.1's two Notes name what can make a box establish an absolute or a fixed positioning containing
   block — "Properties that can cause a box to establish an absolute positioning containing block include
   position, transform, will-change, contain…", and the same list minus `position` for the fixed one — and
   every name on it but `position` is a property core/css/css_computed_value.h derives no computed value for.
   So the list is a DETECTOR for an input neither reader can see, and `n` is its length; the entry hands back
   the array AND its length rather than a NULL-terminated array, because a terminator every caller must
   supply is a contract nothing checks: a scan that runs off the end is undefined behaviour an optimiser is
   entitled to assume cannot happen, and what it emits from that assumption is a loop with no exit.
   IT IS EXPORTED SO THE NOTE IS SPELLED ONCE. core/layout/used_value.c asks whether ONE ancestor can be
   classified while §10.1's third and fourth cases are being answered; core/html/html_element_view.c asks
   whether a declaration ANYWHERE ON A CSSOM VIEW §7 CHAIN makes one of that section's members unreadable.
   Two questions, one set — and a second copy of the set is free to gain a property the first did not, after
   which the component that fell behind goes on answering from a chain it can no longer read. */
const char *const *used_value_positioning_cb_properties(size_t *n);

/* THE SAME RECTANGLE'S WIDTH — §10.1's first case out of the viewport, its second out of the CONTENT EDGE of
   the box above, and §17.4's table wrapper box out of §17.4's own sentence ("The width of the table wrapper
   box is the border-edge width of the table box inside it, as described by section 17.5.2"). It is a second
   entry rather than arithmetic over the first because the first case has no box at all and the third is a box
   the first entry cannot name, so a caller holding that entry's answer could derive neither. */
CssPx used_value_containing_block_width(lxb_dom_element_t *el);

/* THE SAME RECTANGLE'S HEIGHT, AND IT IS A `bool` WHERE THE WIDTH IS A `CssPx` BECAUSE §10.1's CHAIN ALWAYS
   HAS A WIDTH AND FREQUENTLY HAS NO HEIGHT. CSS 2.1 §10.5 "Content height: the 'height' property" states the
   basis and its absence in one sentence — "The percentage is calculated with respect to the height of the
   generated box's containing block. If the height of the containing block is not specified explicitly (i.e.,
   it depends on content height), and this element is not absolutely positioned, the value computes to
   'auto'" — and §10.7 "Minimum and maximum heights: 'min-height' and 'max-height'" repeats the antecedent
   with a DIFFERENT consequence: "the percentage value is treated as '0' (for 'min-height') or 'none' (for
   'max-height')". So the two sections disagree about what to DO with an absent basis and agree about how to
   ASK for one, which is why this entry answers only the asking half and `*out` is written only on `true`.
   FALSE IS A POSITIVE STATEMENT AND NOT A MISSING NUMBER, so a caller reads it as one rather than defaulting
   past it. "The containing block's height is indefinite" is a fact each of those sections has its own rule
   for, and a zero substituted here would be a resolved percentage neither of them wrote — the same reason
   `used_value_height_behaves_as_auto` below is a separate question and not this one's return value.
   THE TWO EXTENTS TAKE THE ONE WALK, so they cannot disagree about WHICH box they measure: §10.1 decides the
   rectangle once and both entries read that answer. What they do not share is the ANSWER'S SHAPE, which is
   the whole of what this declaration adds. */
bool used_value_containing_block_height(lxb_dom_element_t *el, CssPx *out);

/* css-sizing-3 §3.2.1 "“Behaving as auto”" — DOES `el`'s `height` BEHAVE AS AUTO, which is the question CSS
   2.1 asks in several places as "a computed value of `auto`" and which is NOT answerable from a computed value.
   TRUE for a computed `auto`, and for a PERCENTAGE (bare or inside a math function) whose containing block's
   own height is indefinite — §3.2.1's own words, "block percentage heights resolving against an indefinite
   size, see CSS2§10.5". FALSE for a length, and for a percentage that resolves, which §4.1 "Percentage Sizing"
   says is itself definite "because it's a percentage resolved against a definite length".
   IT IS NOT A COMPUTED-VALUE RULE AND MUST NOT BECOME ONE. CSS 2.1 §10.5's prose says such a percentage
   "computes to 'auto'", and css-sizing-3 §3.1.1 "Preferred Size Properties: the width and height properties"
   supersedes it with `Computed value: as specified, with <length-percentage> values computed` — the percentage
   survives, so `getComputedStyle(el).height` on a `display: none` element declaring `height: 50%` answers
   `50%`, which is what every user agent answers. Moving this into core/css/css_computed_value.h would break
   that and would put a layout question in a component that cannot see the layout.
   EVERY CONDITION PHRASED OVER AN `auto` COMPUTED HEIGHT ASKS THIS ONE, and §3.2.1's note is the instruction:
   "legacy spec prose defining layout behavior, particularly in [CSS2], might explicitly refer to width/height
   having a computed value of auto as a condition; some of these cases should be interpreted as meaning behaves
   as auto". §10.6.3's content-based height and §8.3.1's two collapsing pairs are those cases here — §3.2.1's
   own test list names the margin-collapsing ones — so core/layout/block_flow.c asks this and never the
   property. §10.3.2's intrinsic-ratio arms and §10.4's ratio table stay literal: their antecedent needs an
   intrinsic ratio no box in this build has, so widening them would be a guess nothing can exercise.
   IT READS COMPUTED VALUES AND WALKS §10.1's CHAIN, computing no size — which is what lets the callers above
   ask it before deciding whether to run a layout at all. */
bool used_value_height_behaves_as_auto(lxb_dom_element_t *el);

/* THE SAME RECTANGLE'S `direction` — true for `rtl`. §10.3.3's over-constrained case ("if the 'direction'
   property of the containing block has the value 'ltr', the specified value of 'margin-right' is ignored") and
   CSS 2 §9.4.1's horizontal placement ("each box's left outer edge touches the left edge of the containing
   block (for right-to-left formatting, right edges touch)") both name THE CONTAINING BLOCK's value and not the
   box's own, so both ask this and neither reads the property directly.
   IT IS A SEPARATE ENTRY FOR THE SAME REASON THE TWO EXTENTS ARE: §10.1's FIRST case has no box, and the
   section answers it in its own sentence — "the 'direction' property of the initial containing block is the
   same as for the root element" — so a caller holding the NULL could not derive it and would have to carry a
   second copy of that exception. */
bool used_value_containing_block_is_rtl(lxb_dom_element_t *el);

#endif
