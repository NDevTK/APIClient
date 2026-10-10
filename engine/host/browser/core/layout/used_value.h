/* CSS 2.1 §10 "Visual formatting model details": a box's used values. css_computed_value.h owns the specified
 * and computed values; the used value is the third stage: "the values used are the same as the computed
 * values, with 'auto' replaced by some suitable value, and percentages calculated based on the containing
 * block, but there are exceptions" (§10.3's opening). The exceptions are §10.3.1 through §10.3.10 and §10.6.1
 * through §10.6.7, one per box type.
 *
 * The box type comes first: §10.3 gives `width` ten algorithms, selected by whether the box is inline or
 * block-level, replaced or not, floating, absolutely positioned or in normal flow, and an answer from the wrong
 * one is not close (a float's `width: auto` is §10.3.5's shrink-to-fit, a block's follows from §10.3.3's
 * equation). So `uv_box_kind` is the first step of every entry below, and a box type whose section is unbuilt
 * crashes naming that section. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_USED_VALUE_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_USED_VALUE_H

/* What is computed: every arm of §10 whose operands exist, the box edges stated over them, and the containing
   block they are stated against. Intrinsic sizes come from core/layout/intrinsic_size.h (§10.3.5's
   shrink-to-fit), and a content-based height from core/layout/block_flow.h's §10.6.3 walk.
     - A margin or padding with an absolute computed length is its own used value: §10.3.3 solves only for
       `auto` values and, over-constrained, one horizontal margin; §10.6.3 gives the vertical pair one rule
       (`auto` becomes 0); padding has no `auto`.
     - A `width` or `height` with an absolute computed length is its own used value (§10.3.3, §10.3.5,
       §10.3.7 and §10.3.9 solve for it only when it is `auto`), except for table boxes and flex or grid
       items. A flex item's declared cross size is css-flexbox-1 §9.4 step 11's used value; its main size and
       a grid item's sizes are their containers' algorithms and crash. */

/* Table boxes take a route per box type per axis, because each §17.5 section takes the declaration as an input
   to its own comparison: a table box's width is core/layout/table_width.h's §17.5.2 (§17.5.2.2 Automatic
   table layout makes a declared width a floor that CAPMIN and the columns' MIN may exceed) and its height
   core/layout/table_height.h's §17.5.3; a cell's width is its columns' and its height its rows'; a row's and a
   row group's width is their grid row's (§17.5's rules 1 and 2), and a row's height is §17.5.3's. A caption is
   a block box in the wrapper (§17.4: "as normal block boxes inside the table wrapper box"), answered by §10.
   Still crashing: a row group's height (§17.5.3 declines it) and a column or column group box on both axes
   (§17.5's rules 3 and 4 are a placement nothing here performs). */

/* §10.4 "Minimum and maximum widths: 'min-width' and 'max-width'" and §10.7 "Minimum and maximum heights:
   'min-height' and 'max-height'" are a second pass, not a clamp on the number: "the rules above are applied
   again, but this time using the computed value of 'max-width' as the computed value for 'width'", so the
   whole of §10.3 re-solves whichever margin was `auto` (`margin: 0 auto; max-width: 1200px` centres only that
   way). A percentage `max-width` resolves against the containing block's width, a percentage `max-height`
   against its height, and where that height is indefinite "the percentage value is treated as '0' (for
   'min-height') or 'none' (for 'max-height')". `min-width: auto` is css-sizing-3 §3.1.2's initial value,
   resolved by §3.2 to 0 for a box that is not a flex or grid item. A `min()`/`max()`/`clamp()` limit arrives
   resolved (core/css/css_math.h), carrying its operands' environment facts.
   §10.4's other algorithm, the constraint-violation table for a replaced element with an intrinsic ratio and
   both sizes `auto`, is a joint solve over both axes and crashes naming itself. */

/* Under `box-sizing: border-box` the exposed used size is the border box's: css-sizing-3 §3.3 "Box Edges for
   Sizing: the box-sizing property": "Used values of the sizing properties, as exposed for instance through
   getComputedStyle(), also refer to the border box." It is the declared length unless the paddings and borders
   alone exceed it ("the border-box size ends up at 120px, even though width: 100px is specified for the border
   box"), so it needs the two paddings and the two border widths.
   The padding edge's extent is an entry for the same reason: CSS 2.1 §8's padding box is the content box plus
   the two paddings in both modes, and only which box the used size is of differs, so a caller adding paddings
   to `used_value_px(el, "width")` would double-count them under `border-box`. The four terms are computed in
   one function and both directions of §3.3's conversion are stated over it. */

/* A percentage margin, padding or `width` resolves against the containing block's width (§8.3, §8.4, §10.2),
   for the vertical sides too ("even for 'padding-top' and 'padding-bottom'"), so neither arm takes an axis.
   A percentage `height` resolves against the containing block's height (§10.5), which may not exist; then
   css-sizing-3 §3.2.1 makes the property behave as auto (`used_value_height_behaves_as_auto`).
   §10.3.3's constraint equation,
       margin-left + border-left-width + padding-left + width + padding-right + border-right-width +
       margin-right = width of containing block
   solves for whichever term is `auto` (rule 5 for `width`, rules 2/4/6 for the margins), and its
   over-constrained case ignores the margin the containing block's `direction` names
   (`used_value_containing_block_is_rtl`). The floor at zero is css-sizing-3 §3.1 "Sizing Properties"' ("the
   inner size is always floored at zero"), not §10.4. §3.3's own floor is one step of the `border-box`
   conversion ("flooring the result at zero (as the inner size of a box cannot be negative)") and is cited only
   for it; css-ui-3 §3.1's floor sentence belongs to a superseded edition (css-ui-4 records the move to
   css-sizing-3 §3.3) and is cited nowhere. */

/* §10.1's containing block, which every percentage and `auto` above is stated against, is a recursion ("the
   content edge of the nearest block container ancestor box") that terminates at the root element, whose
   containing block is the initial containing block, which "has the dimensions of the viewport"
   (core/frame/viewport.h). A `fixed` box's containing block is the viewport and an `absolute` box's the
   padding edge of its nearest positioned ancestor: `used_value_abs_containing_block` names each rectangle and
   `used_value_abs_offset_px` answers §10.3.7's and §10.6.4's offsets over the static position
   core/layout/block_flow.h and core/layout/flow_position.h derive. */

/* A used value is a `CssPx`, not a `double`. A box's size is what layout determines from this tree and
   cascade, so it is concrete rather than a picked environment value (core/frame/viewport.h's test), but the
   initial containing block's width is a picked environment fact and every used value derived from it inherits
   that domain: `parseInt(getComputedStyle(el).width) < 768` is the same responsive gate as `innerWidth < 768`.
   css_length.h carries it: the example is the number C compares and computes on, and the fact is what the JS
   boundary mints the domain from, through `viewport_env_derived`, the one switch over the fact.
   Operands are read through `css_computed_length`, already absolutized, so `width: 50vw` carries the ICB's
   fact and `border: 1px solid` the device pixel ratio's (css-values §6 snaps a border width to whole device
   pixels). A length's fact is a set and every arm unions its operands', so a used width over both is one
   value whose domain is their relation (solver/concolic.h's joint source identity). */

/* This component stores nothing: every used value is derived per read from the running flow's own tree and
   cascade, as css_style_declaration.c derives the cascade, so it is per-flow by construction with no capture
   to write. A box tree cached across a context switch would be shared state the COW delta does not swap.
   Named residual. Not covered: state that must outlive a read, such as an inline formatting context's line
   boxes, which cannot be re-derived per read without re-running the flow's layout. Next: when such a record
   exists, its accessor captures it through solver/cow.h's `cow_capture_host_record` over the record's
   owned-value layout (solver/dom_cow.h's delta covers only the DOM tree and the attribute list). How its
   absence would show: a geometry that does not move when the running flow's own tree does, two flows that
   mutated one document differently reading one box back. */

#include <stdbool.h>
#include <stddef.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"
#include "core/layout/replaced_element.h"

/* The used value of `name` on `el`, in CSS pixels. `name` is one of the physical box-model lengths CSSOM §9
   routes here (the four margins, the four paddings, `width`, `height`), and the caller has established §9's
   two conjuncts (the property applies, and the element generates a box). A case CSS 2.1 §10 defines and this
   component does not compute crashes naming its section. A caller reporting the value to a page mints its
   domain through `viewport_env_derived`; arithmetic on it uses css_length.h's, which unions operands' facts. */
CssPx used_value_px(lxb_dom_element_t *el, const char *name);

/* The used extent of the padding edge on one axis (horizontal for `vertical` false), in CSS pixels: the
   content box plus the two paddings, with css-sizing-3 §3.3 deciding which box `used_value_px` handed back.
   Readers include CSSOM View §6's `clientWidth`/`clientHeight` step 3, core/dom/element_scrolling.c and
   core/layout/scrolling_area.c.
   It is an extent, not an edge position: a position is a coordinate in the ICB's space that
   core/layout/flow_position.h derives by placing each box, while a distance between two parallel edges of one
   box needs only §10.1's width. `clientWidth` is this extent; `scrollWidth` is a position over the box and its
   descendants (core/layout/scrolling_area.h). The caller has established §6's step 1 (the element has an
   associated box and it is not inline). */
CssPx used_value_padding_edge_px(lxb_dom_element_t *el, bool vertical);

/* The used extent of the border edge on one axis: CSS 2.1 §8.1 "Box dimensions"' "border edge surrounds the
   box's border", whose "four border edges define the box's border box". It is the padding edge plus the two
   border widths, an entry for the padding edge's reason (which box `used_value_px` returned is §3.3's
   question); both go through the one four-term surround, so the two edges describe one box. Readers include
   CSSOM View §6's `getClientRects()` step 3 and §7's `offsetWidth`/`offsetHeight`. It is an extent; the
   border area's position is core/layout/flow_position.h's. */
CssPx used_value_border_edge_px(lxb_dom_element_t *el, bool vertical);

/* CSS 2.1 §8.1 "Box dimensions"' one edge between a box's border box and its padding box on the axis's
   leading side, as a used value: `border-left-width` for `vertical` false, `border-top-width` for true. It is
   an entry because the computed value is not always the used one: CSS 2.1 §17.6.1 The separated borders model
   makes a row, row group, column or column group box's widths zero ("Rows, columns, row groups, and column
   groups cannot have borders (i.e., user agents must ignore the border properties for those elements)"), and
   §17.6.2 The collapsing border model gives a table box half of the collapsed border at the grid's edge and a
   cell its half at each interior grid line. Its caller is core/layout/flow_position.c's padding-box origin. */
CssPx used_value_leading_border_px(lxb_dom_element_t *el, bool vertical);

/* CSS 2.1 §8.5 "Border properties"' four used border widths at once, written to `out` in top/right/bottom/
   left order, the order CSS 2.1 §8.5.1 "Border width: 'border-top-width', 'border-right-width',
   'border-bottom-width', 'border-left-width', and 'border-width'" defines its shorthand over ("If there are
   four values, they apply to the top, right, bottom, and left, respectively"). The entry above reaches only
   the two leading sides. One entry rather than four asks also keeps the answers one derivation: CSS 2.1
   §17.6.2 "The collapsing border model"'s widths build the table's grid per ask
   (core/layout/table_border_collapse.h), so four asks could return widths from two builds. Readers are
   core/paint/box_paint.c's CSS 2.1 §E.2 "Painting order" border mark, which needs all four together because
   CSS 2.1 §8 states no rule for where two borders meet, and core/layout/flex_line.c and
   core/layout/flex_cross_size.c. */
void used_value_border_widths_px(lxb_dom_element_t *el, CssPx out[4]);

/* The same side's two edges: that border width plus that side's used padding, CSS 2.1 §8.1's distance from a
   box's border edge to its content edge on the leading side of one axis. Its callers in
   core/layout/flow_position.c turn a border-edge coordinate into §10.1's "the content edge of the nearest
   block container ancestor box" and place §9.4.2's fragments.
   The padding is not always the cascade's either: CSS 2.1 §8.4 "Padding properties: 'padding-top',
   'padding-right', 'padding-bottom', 'padding-left', and 'padding'"' Applies-to line excludes row, row group,
   column and column group boxes, and CSS 2.1 §17.6.2 The collapsing border model says "in this model, a table
   does not have padding (but does have margins)". See used_value.c for the routing. */
CssPx used_value_leading_edge_px(lxb_dom_element_t *el, bool vertical);

/* The used extent of the margin edge on one axis: CSS 2 §8.1 "Box dimensions"' outermost nesting, "the margin
   edge surrounds the box margin … the four margin edges define the box's margin box". It is the border edge
   plus the two margins, an entry for the border edge's reason. Its caller is core/layout/line_box.c: CSS 2.2
   §10.8's step 1 ("for replaced elements, inline-block elements, and inline-table elements, this is the height
   of their margin box; for inline boxes, this is their 'line-height'") on one axis, and the inline extent of
   the run item that carries an atomic inline on the other (CSS 2.2 §9.4.2: "horizontal margins, borders, and
   padding are respected between these boxes").
   It can be negative (CSS 2.2 §8.3 "Margin properties": "negative values for margin properties are
   allowed"), so `margin: -100px` on a 10px box gives −190px; there is no floor. It is an extent; the area's
   position is core/layout/flow_position.h's. */
CssPx used_value_margin_edge_px(lxb_dom_element_t *el, bool vertical);

/* The border edge for a caller that has already derived the box's content extent on that axis: that extent
   plus the four terms css-sizing-3 §3.3's conversion is stated over. §10.7's clamp runs here over the extent
   handed in, which is §10.4/§10.7 step 1's "tentative used height"; a stacked child reporting an unclamped
   height would make its parent's §10.6.3 height wrong too.
   Its caller is core/layout/block_flow.c, and a cycle is why it exists: a `height: auto` box's content extent
   is §10.6.3's walk, which `used_value_px(el, "height")` runs, so a walk asking the entry above for a child's
   border box would re-enter itself one level down. The walk hands in the number it holds. */
CssPx used_value_border_edge_from_content_px(lxb_dom_element_t *el, CssPx content, bool vertical);

/* The box's own content extent on one axis, in CSS pixels: CSS 2.1's `width` and `height` in CSS 2.1's
   sense, which `used_value_px` does not answer under `box-sizing: border-box` (css-sizing-3 §3.3 makes the
   used value "as exposed for instance through getComputedStyle()" the border box's). This is §3.3's
   conversion run once, where its four terms are owned, rather than a subtraction each caller would
   double-count in the other mode.
   css-images-3 §4.5 "Sizing Objects: the object-fit property" defines the concrete object size under `fill`
   as "the element's used width and height", which HTML §4.8.4.3.11 "Parsing a sizes attribute" step 3.3
   substitutes for `sizes="auto"`; HTML §4.8.3's determine the dimensions asks it as "its rendered width and
   height", which `HTMLImageElement.width` reports. */
CssPx used_value_content_px(lxb_dom_element_t *el, bool vertical);

/* The same extent computed as though the box were block-level in normal flow: CSS 2.1 §10.6 "Calculating
   heights and margins"' and §10.7 "Minimum and maximum heights: 'min-height' and 'max-height'"' rules with the
   box classified as §10.3.3's, as a content-box extent. Its one caller is css-flexbox-1 §9.4 "Cross Size
   Determination"' step 7: "Determine the hypothetical cross size of each item by performing layout as if it
   were an in-flow block-level box with the used main size and the given available space, treating auto as
   fit-content." (css-flexbox-1's Changes list records, against Issue 5188, that this means the block-level
   rules.) `used_value_px` reads the real kind and answers step 11's used cross size, which for a stretched
   item is the line's cross size, the number step 8 computes; a caller resolving the declaration would copy
   §10.6 and §10.7. `min-height: auto` answers 0: css-sizing-3 §3.2 "Sizing Values: the <length-percentage
   [0,∞]>, auto | none, stretch, min-content, max-content, and fit-content values" falls back to a used 0, and
   css-flexbox-1 §4.5 "Automatic Minimum Size of Flex Items" claims only the main axis.
   A table box is refused (CSS 2.1 §17.5.3 Table height algorithms owns its height), and a replaced element
   only where its size on the asked axis behaves as auto (§10.6.2's natural-dimension and ratio arms need the
   width css-flexbox-1 §9.3 "Main Size Determination" owns). Other flex items arrive blockified (css-display-3
   §2.7 "Automatic Box Type Transformations"). */
CssPx used_value_block_level_content_px(lxb_dom_element_t *el, bool vertical);

/* CSS 2.1 §10.3.2's and §10.6.2's default replaced size: the 300 x 150 rectangle for a replaced element with
   no natural dimensions, capped by the device; `vertical` false is the width. It takes no realm: the cap is
   against the output device (core/frame/screen.h), one answer for the agent, which is what lets
   core/frame/viewport.c call it (a child navigable's viewport is this rectangle when its container has no
   author size, so a viewport-dependent value would define itself). used_value.c asserts the answer's domain
   is a single point. */
CssPx used_value_default_replaced_size(bool vertical);

/* CSS 2.1 §10.3.2 "Inline, replaced elements"' five arms, as a content-box width in CSS pixels, over the
   natural dimensions `rep` states. `rep` is the caller's own `replaced_element_of` answer because HTML
   §15.4.2's classification changes under the running flow as a reply lands, so two reads in one algorithm
   could disagree. The precondition, `width` behaving as `auto`, is each caller's: §10.3's dispatch reads the
   computed value, and css-sizing-3 §5.1 "Intrinsic Sizes" holds it by definition ("the size it would have if
   it was a float given an auto preferred size in that axis (and no minimum or maximum size in that axis)"),
   so asserting it would abort on `<img width="18">`. The opposite axis is not removed, so `height` is still
   read (§5.1's Note: "size constraints in the opposite dimension will transfer through").
   `uv_replaced_size`'s box-type refusals are not part of this: an intrinsic size is §5.1's hypothetical float,
   which CSS 2.1 §10.3.6 "Floating, replaced elements" answers "as for inline replaced elements". Nor is §3.3's
   `box-sizing` conversion, which belongs to whichever box the caller reports. */
/* Named residual. Not covered: arm 2 reads the opposite axis through `used_value_px`, which routes a replaced
   element into `uv_replaced_size` and its refusals, so a replaced flex or grid item or table box with a
   non-`auto` `height` and a natural aspect ratio aborts there. Next: nothing here; that refusal is correct
   (css-flexbox-1 §9.2 "Line Length Determination" makes a declared size the flex base size) and is retired by
   the flex layout it names. How its absence would show: an intrinsic-size request aborts naming css-flexbox-1
   §9.2 from inside a css-sizing-3 §5.1 walk. */
CssPx used_value_replaced_auto_width_px(lxb_dom_element_t *el, const ReplacedElement *rep);

/* CSS 2.1 §10.1's containing block as an element: the box whose content edge is the rectangle §10.3's
   percentages and `auto`s are stated against. NULL exactly where the answer is the initial containing block,
   which is no element's box: the first case (the root element's) and the fourth case's last sentence, "If
   there is no such ancestor, the containing block is the initial containing block" (an absolutely positioned
   box at any depth with nothing positioned above it). NULL is never "no answer", and does not mean "the root".
   It is exported because core/layout/flow_position.c and core/layout/block_flow.c need the box (its origin,
   leading border and padding, and the children §9.4.1 stacks) and a second walk would implement §10.1 twice.
   This view refuses, naming what to build, answers its return type cannot spell: a caption's containing block
   (CSS 2.1 §17.4 Tables in the visual formatting model gives the table element a wrapper box and a table box;
   the wrapper's child box list is not a DOM child list), and §10.1's third and fourth cases (css-position-3
   §2.1's layout viewport is no element's box; "formed by the padding edge of the ancestor" is an edge this
   type cannot name, and adding `used_value_leading_edge_px` to it would inset by the ancestor's border). The
   entries below read the same walk and answer the width, the `direction` and those rectangles. */
lxb_dom_element_t *used_value_containing_block(lxb_dom_element_t *el);

/* ---- §10.1's third and fourth cases, as a rectangle a caller can compose an origin from ----------------
   The view the entry above refuses. The tag says which of three rectangles it is; `*element` is the ancestor
   for the padding-edge case and NULL for the other two. `uv_cb` decides §10.1's four cases once and both views
   read it. */
typedef enum {
    USED_VALUE_ABS_CB_INITIAL = 0,   /* §10.1's fourth case's last sentence — "if there is no such ancestor,
                                        the containing block is the initial containing block". No element. */
    USED_VALUE_ABS_CB_VIEWPORT,      /* §10.1's third case: a `position: fixed` box's. No element. */
    USED_VALUE_ABS_CB_PADDING_EDGE   /* §10.1's fourth case: `*element`'s padding edge. */
} UsedValueAbsCb;

UsedValueAbsCb used_value_abs_containing_block(lxb_dom_element_t *el, lxb_dom_element_t **element);

/* CSS 2.1 §10.3.7's and §10.6.4's used `left` (or `top`): the distance from the leading edge of the rectangle
   above to this box's leading margin edge, solved by those sections' one constraint equation with the size
   and the two margins. It is the offset, not the origin: every term of the equation is an extent, while where
   the containing block's leading edge sits in the ICB's space is core/layout/flow_position.h's (and §10.1's
   third case has no element to return). The size it is solved against is §10.4/§10.7's final pass's, so a
   `max-width`-clamped box has that clamp's offset; this reads `uv_sized` rather than the property. */
CssPx used_value_abs_offset_px(lxb_dom_element_t *el, bool vertical);

/* The same solve's trailing offset, the used `right` (or `bottom`), read from the one run: `UvAbs` carries
   `before` and `after` because the five-term equation solves for whichever term is `auto`, so the two offsets
   cannot disagree. A sibling entry, not a parameter on the one above, because core/layout/flow_position.c
   places a box from its leading pair; core/css/css_computed_value.c's CSSOM §9 inset arm reads both. */
CssPx used_value_abs_offset_trailing_px(lxb_dom_element_t *el, bool vertical);

/* CSS 2.2 §9.4.3 "Relative positioning"' used translation on one axis: the signed distance a relatively
   positioned box is shifted by after normal flow places it, the used `left` horizontally and `top` vertically
   ("'Left' moves the boxes to the right", "'Top' moves the boxes down"). §9.4.3 states a pair, "the used
   values are always: left = -right", and its four cases are conditions over both members, so the entry
   answers the leading member and the trailing one is its negation (core/css/css_computed_value.c's CSSOM §9
   inset arm negates it).
   Over-constrained horizontally §9.4.3 reads the containing block's `direction` ("If the 'direction'
   property of the containing block is 'ltr', the value of 'left' wins … If 'direction' of the containing
   block is 'rtl', 'right' wins and 'left' is ignored"), through `used_value_containing_block_is_rtl`;
   vertically "If neither is 'auto', 'bottom' is ignored". The precondition, a relatively positioned box
   (§9.3.1's scheme), is asserted: an absolutely positioned box is §10.3.7's and a static one has no offset
   (§9.3.2's `Applies to:` "positioned elements"). */
CssPx used_value_rel_offset_px(lxb_dom_element_t *el, bool vertical);

/* One declared inset resolved to pixels against its own axis: css-position-3 §3.1 "Box Insets: the top,
   right, bottom, left, inset-block-start, inset-inline-start, inset-block-end, and inset-inline-end
   properties"' `<percentage>` arm, "The inset is a percentage relative to the containing block's size in the
   corresponding axis (e.g. width for left or right, height for top and bottom)." `len` is the caller's because
   every caller has already separated `auto` from a `<length-percentage>` (§3.1's `Value:` line).
   It is not the used value where §3.5.2 applies: when the inset-modified containing block goes below zero
   "the weaker inset in the affected axis is reduced (possibly becoming negative) to bring that size up to
   zero". A sticky box is not a caller for a percentage: "For sticky positioned boxes, the inset is instead
   relative to the relevant scrollport's size", and this reads §10.1's containing block. */
CssPx used_value_inset_length_px(lxb_dom_element_t *el, CssLength len, bool vertical);

/* css-position-3 §2.1 "Containing Blocks of Positioned Boxes"' open list of properties, as one fact with two
   readers. §2.1's Notes name what can make a box establish an absolute or fixed positioning containing block
   ("Properties that can cause a box to establish an absolute positioning containing block include position,
   transform, will-change, contain…", the fixed list minus `position`), and every name but `position` is a
   property core/css/css_computed_value.h computes no value for, so the list detects an input neither reader
   can see. It returns the array and its length `*n`, not a terminator a caller must remember to supply.
   Readers: core/layout/used_value.c, classifying an ancestor for §10.1's third and fourth cases, and
   core/html/html_element_view.c, asking whether a declaration on a CSSOM View §7 chain makes a member
   unreadable. One copy, so the two cannot drift. */
const char *const *used_value_positioning_cb_properties(size_t *n);

/* The same rectangle's width: §10.1's first case from the viewport, its second from the content edge of the
   box above, and §17.4's table wrapper box from §17.4's own sentence ("The width of the table wrapper box is
   the border-edge width of the table box inside it, as described by section 17.5.2"). A second entry because
   the first case has no box and the wrapper is a box the first entry cannot name. */
CssPx used_value_containing_block_width(lxb_dom_element_t *el);

/* The same rectangle's height, a `bool` because §10.1's chain always has a width and often no height. CSS 2.1
   §10.5 "Content height: the 'height' property": "The percentage is calculated with respect to the height of
   the generated box's containing block. If the height of the containing block is not specified explicitly
   (i.e., it depends on content height), and this element is not absolutely positioned, the value computes to
   'auto'"; §10.7 "Minimum and maximum heights: 'min-height' and 'max-height'" treats the same antecedent as
   "'0' (for 'min-height') or 'none' (for 'max-height')". The two disagree about what to do with an absent
   basis and agree on how to ask, so this answers only the asking: `*out` is written only on true, and false
   is a positive statement (indefinite) a caller does not default past. One walk serves both extents. */
bool used_value_containing_block_height(lxb_dom_element_t *el, CssPx *out);

/* css-sizing-3 §3.2.1 "“Behaving as auto”": whether `el`'s `height` behaves as auto, which CSS 2.1 asks as "a
   computed value of `auto`" and which a computed value cannot answer. True for a computed `auto` and for a
   percentage (bare or in a math function) whose containing block's height is indefinite ("block percentage
   heights resolving against an indefinite size, see CSS2§10.5"); false for a length and a percentage that
   resolves (§4.1 "Percentage Sizing": definite "because it's a percentage resolved against a definite
   length").
   It is not a computed-value rule: css-sizing-3 §3.1.1 "Preferred Size Properties: the width and height
   properties" supersedes CSS 2.1 §10.5's "computes to 'auto'" with `Computed value: as specified, with
   <length-percentage> values computed`, so `getComputedStyle` on a `display: none` element with `height: 50%`
   answers `50%`. Per §3.2.1's note ("some of these cases should be interpreted as meaning behaves as auto"),
   §10.6.3's content-based height and §8.3.1's collapsing pairs ask this, in core/layout/block_flow.c; §10.3.2's
   intrinsic-ratio arms and §10.4's ratio table read the computed `auto` literally. It reads computed values
   and walks §10.1's chain, computing no size, so a caller can ask it before deciding to lay out. */
bool used_value_height_behaves_as_auto(lxb_dom_element_t *el);

/* The same rectangle's `direction`, true for `rtl`. §10.3.3's over-constrained case ("if the 'direction'
   property of the containing block has the value 'ltr', the specified value of 'margin-right' is ignored") and
   CSS 2 §9.4.1's horizontal placement ("each box's left outer edge touches the left edge of the containing
   block (for right-to-left formatting, right edges touch)") name the containing block's value, not the box's.
   A separate entry because §10.1's first case has no box and answers in its own sentence: "the 'direction'
   property of the initial containing block is the same as for the root element". */
bool used_value_containing_block_is_rtl(lxb_dom_element_t *el);

#endif
