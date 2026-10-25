/* CSS 2 §9.4.1 "Block formatting contexts": where a box is. core/layout/used_value.h computes extents (§10.3.3
 * over §10.1's containing block, §8.1's edges); a rectangle also needs a position, which CSSOM View §6's
 * `getClientRects()` and §7's `offsetTop`/`offsetLeft` read from here.
 *
 * Coordinates are in the initial containing block's space, the one CSSOM View measures in: §10.1 anchors the
 * ICB "at the canvas origin", §7 states `offsetTop` "relative to the initial containing block origin", and §4's
 * `scrollX` is "relative to the initial containing block origin, of the left of the viewport". A client
 * coordinate subtracts the scroll position (§10's `clientX` is "relative to the origin of the viewport"); that
 * is the reporting member's step. Each coordinate is a `CssPx` for used_value.h's reason: derived from the
 * viewport, it carries the ICB's environment fact (`rect.left < 768` gates like `innerWidth < 768`); the
 * example crosses to a page only through core/frame/viewport.h. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_FLOW_POSITION_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_FLOW_POSITION_H

/* Block-level boxes. §9.4.1: "boxes are laid out one after the other, vertically, beginning at the top of a
   containing block. The vertical distance between two sibling boxes is determined by the margin properties",
   and "each box's left outer edge touches the left edge of the containing block (for right-to-left formatting,
   right edges touch)". For the root element both reduce to its own margins: its containing block is the ICB
   (§10.1's first case), it has no preceding sibling, and "margins of the root element's box do not collapse"
   (§8.3.1 "Collapsing margins"). Every other in-flow block-level box is induction over §10.1's second case
   ("the content edge of the nearest block container ancestor box"): that ancestor's origin plus its leading
   border and padding plus §9.4.1's offsets, the vertical one from core/layout/block_flow.h's stack. Which
   horizontal touching applies is the containing block's `direction` (used_value.h, with §10.1's first-case
   exception); the two are one distance from opposite edges. */

/* Boxes placed by other sections, which return before the induction:
     - A box on a line (§9.4.2: "boxes are laid out horizontally, one after the other"): its origin is its first
       fragment's, from core/layout/line_box.h in the establishing block's content box. The level decides, not
       "replaced": §9.2.2 puts a non-replaced `inline` box (split across lines) and an atomic inline-level box
       ("a single opaque box": a replaced element or an `inline-block`) on the line.
     - A table row, row group or cell: CSS 2.1 §17.5 Visual layout of table contents ("Internal table elements
       do not have margins"). Rules 1, 2 and 5 place them at rectangles of one grid, so one function turns a
       rectangle into a corner (a row is `[0, ncols)`), with §17.6.1's spacing (zero under §17.6.2). Logical
       column 0 is at the left under `ltr` and the right under `rtl`, of the table's own `direction`.
     - An out-of-flow box: §9.3.2's offsets (CSS 2.1 §10.3.7, §10.6.4) over `flow_static_position`.
   Still refused, each by its own missing piece: column and column-group boxes (no element-to-column mapping),
   captions and a table box with captions (the wrapper's child box list), floats (§9.5), `inline-flex` and
   `inline-grid` (their modules' intrinsic main sizes and baselines), `inline-table` (CSS 2.2 §10.8.1 "Leading
   and half-leading"'s first-row baseline; core/layout/table_height.h reports no baseline), and a writing mode
   other than `horizontal-tb` (css-writing-modes-4 §7.4's flow-relative restatement). */

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"

/* A point in the initial containing block's coordinate space (see the header). */
typedef struct {
    CssPx x;
    CssPx y;
} FlowPoint;

/* The top-left corner of `el`'s border box, in that space. The caller has established that the element has a
   box (core/dom/element_view.h); every box CSS 2 §9.4 places and this component does not crashes naming its
   section, with no fallback coordinate.
   §10.1's second case makes this a recursion over ancestors, costing each box its depth. The layout job's
   placement record (core/layout/flow_placement.h) remembers answers for the job's lifetime, collapsing the
   climb to one derivation per box, and each served answer is re-checked against its §10.1 equation; the
   synchronous job holds the record only while a drive is open. */
FlowPoint flow_border_box_origin(lxb_dom_element_t *el);

/* The same origin asked without a drive of its own: false ("not yet") when a question it asked was deferred,
   with `*out` unwritten and the question suspended on the current job, whose drive the caller answers to.
   `flow_border_box_origin` is this ask inside a drive that computes what it deferred. */
bool flow_border_box_origin_ask(lxb_dom_element_t *el, FlowPoint *out);

/* CSS 2.2 §9.4.1's horizontal rule for in-flow block-level `el`: the distance from its containing block's left
   content edge to its left border edge, over that block's used width and `direction`. */
CssPx flow_block_inline_offset(lxb_dom_element_t *el);

/* The top-left corner of `el`'s padding box: the border box origin moved inward by CSS 2 §8.1 "Box dimensions"'
   leading border on each axis. Derived from `flow_border_box_origin`, so unplaceable boxes crash there first.
   CSSOM View §2 "Terminology"'s scrolling area ("the element's top padding edge", core/layout/scrolling_area.h)
   and css-overflow-3 §2.3 "Scrolling Overflow"'s scrollport ("coincide with its padding box") both need it. */
FlowPoint flow_padding_box_origin(lxb_dom_element_t *el);

/* The top-left corner of `table`'s TABLE box's border box. `flow_border_box_origin(table)` is its table wrapper
   box's, which CSS 2.1 §17.4 "Tables in the visual formatting model" gives the element's margins; the table box
   has the element's border and padding, starts at the wrapper's content edge and sits below the captions
   §17.4.1 puts above it. `table` must generate a table wrapper box (asserted). */
FlowPoint flow_table_box_origin(lxb_dom_element_t *table);
/* The same corner asked without a drive of its own, as `flow_border_box_origin_ask` is. */
bool flow_table_box_origin_ask(lxb_dom_element_t *table, FlowPoint *out);

/* CSS 2.1 §10.3.7's and §10.6.4's static position: the distance from the leading edge of absolutely positioned
   `el`'s containing block (§10.1's third or fourth case) to the leading margin edge of the hypothetical box, the
   element "if its specified `position` value had been `static` and its specified `float` had been `none`".
   It is the difference between core/layout/block_flow.h's in-flow position and §10.1's other rectangle, both
   coordinates here.
   `trailing` is §10.3.7's `right`, with its sign: "the static position for `right` is the distance from the
   right edge of the containing block to the right margin edge of the same hypothetical box… the value is
   positive if the hypothetical box is to the left of the containing block's edge". Asked only for an `rtl`
   static-position containing block, and crashes on the block axis (§10.6.4 has no `bottom` static position).
   For a fixed box, "the containing block of fixed positioned elements is the initial containing block instead
   of the viewport, and all scrollable boxes should be assumed to be scrolled to their origin", applied here.
   The leading answer may be negative ("the value is negative if the hypothetical box is to the left of the
   containing block"). */
CssPx flow_static_position(lxb_dom_element_t *el, bool vertical, bool trailing);

/* The same position asked without a drive of its own: false ("not yet") when a question it asked was
   deferred, with `*out` unwritten. `flow_static_position` is this ask inside a drive. */
bool flow_static_position_ask(lxb_dom_element_t *el, bool vertical, bool trailing, CssPx *out);

/* One border area in the same space: a position and its two extents. */
typedef struct {
    CssPx x, y, width, height;
} FlowRect;

/* Every border area of a box on a line, in content order, in the ICB's space. Answers the count (never zero;
 * exactly one for an atomic inline-level box, "a single opaque box") and stores a newly allocated array at
 * `*out`, which the caller owns and must free.
 *
 * An inline box can have several: "when an inline box exceeds the width of a line box, it is split into
 * several boxes and these boxes are distributed across several line boxes" (CSS 2 §9.4.2), and CSSOM View §6
 * "Extensions to the Element Interface"'s getClientRects() returns "one for each box fragment".
 * `flow_border_box_origin` answers from `*out[0]`, so the reported corner and the first rectangle agree.
 *
 * It answers full rectangles: a non-replaced inline box's inline extent is its fragment's run on that line (not
 * §10.3.1's `width`, which "does not apply") and its block extent is §10.6.1's content area, both from
 * core/layout/line_box.h. A replaced element's rectangle is composed by core/dom/element_view.c from §10.3.2
 * and §10.6.2's used sizes at this file's origin; line_box.c asserts its fragment's inline extent against
 * §10.3.2's used border edge, so the two routes check each other.
 * `el` must be a box on a line (`inline` or `inline-block`, in normal flow, `horizontal-tb`). */
size_t flow_inline_fragment_rects(lxb_dom_element_t *el, FlowRect **out);

/* The same rectangles without a drive of their own: false ("not yet") when the establishing context or its
   origin deferred a question, with nothing stored; true with the array at `*out` and its count at `*count`. */
bool flow_inline_fragment_rects_ask(lxb_dom_element_t *el, FlowRect **out, size_t *count);

#endif
