/* css-sizing-3 §5.1 "Intrinsic Sizes": a box's min-content and max-content inline sizes, CSS 2.2 §10.3.5
 * "Floating, non-replaced elements"' preferred minimum width and preferred width.
 *
 * §5.1 defines them through a float ("the size it would have if it was a float given an auto preferred size in
 * that axis … and if its containing block was zero-sized", max-content "infinitely-sized") and defers: "this
 * specification does not define how to determine the sizes of floats. Please refer to [CSS2]." CSS 2.2 §10.3.5
 * is the operation: "calculate the preferred width by formatting the content without breaking lines other than
 * where explicit line breaks occur, and also calculate the preferred minimum width, e.g., by trying all possible
 * line breaks." css-sizing-3 §2.1 "Auto Box Sizes" pins the names ("this is called the 'preferred width' in
 * CSS2.1§10.3.5") and states the walk implemented: sizes "if none"/"if all" "of the soft wrap opportunities
 * within the box were taken".
 *
 * core/layout/text_run.h owns the characters (css-text-3 §4.1, §5, [UAX14]); this owns the box tree and how
 * css-sizing-3 §5.2 "Intrinsic Contributions" combines children. Answers are content-box widths; css-sizing-3
 * §3.3 "Box Edges for Sizing: the box-sizing property"' conversion is the caller's, as for §10.3.3. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_INTRINSIC_SIZE_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_INTRINSIC_SIZE_H

/* Which of CSS 2.2 §9.4's formatting contexts the box establishes is asked once, over the whole child list,
   through core/layout/block_flow.h's predicate ("A block container box either contains only block-level boxes
   or establishes an inline formatting context and thus contains only inline-level boxes"): §9.4.2 sums along a
   line, §9.4.1 takes the maximum down a column, and the two share no step. The §9.4.1 arm maximises over the
   box list §9.2.1.1 "Anonymous block boxes" forces (block-level children plus one anonymous box per inline run),
   each operand css-sizing-3 §2.2 "Intrinsic Size Contributions"' outer size, using block_flow.h's
   classification and run delimitation, not `block_flow_anonymous_boxes` (which places, needing a used width
   that is a shrink-to-fit over this entry).
   A child's declared inline size applies (`is_declared_inline_sizes`, with CSS 2.1 §10.4's clamp and §3.3's
   conversion): §5.2's contribution is a float containing the child, so `width: 500px` contributes 500px.
   §5.2.1 substitutes a cyclic percentage away. §5.1's own sizes assume "an auto preferred size in that axis and
   no minimum or maximum size", so the box's own declarations are applied only in §5.2's child contribution,
   not in `intrinsic_inline_sizes`. Nothing is stored (per-flow state solver/dom_cow.h does not swap). */

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"
#include "core/layout/block_flow.h"

/* CSS 2.2 §9.4.2 "Inline formatting contexts"' outer size at one side of an inline box, as used values in CSS
   pixels (the trailing side for `trailing` true): "horizontal margins, borders, and padding are respected
   between these boxes", at the box's boundaries since "inline box boundaries do not introduce a forced line
   break or soft wrap opportunity in the flow" (css-text-3 §5.5 "Line Breaking Details"). core/layout/line_box.c
   hands it to core/layout/text_run.h as an edge item at a position, never into a total.
   It is the used-value half of one property triple. css-sizing-3 §5.2.1 "Intrinsic Contributions of
   Percentage-Sized Boxes" answers a percentage two ways: "a cyclic percentage is resolved against zero for
   determining intrinsic size contributions", but "Otherwise, the percentage is resolved against the containing
   block's size", which is a line's case (its width is determined). So two entries over one triple; the
   intrinsic one is `static` in intrinsic_size.c, unreachable from a used-value site where its zero would be
   silently wrong. */
CssPx used_inline_box_edge_px(lxb_dom_element_t *el, bool trailing);

/* Which physical axis a sizing property or box edge is read on. Physical because css-writing-modes-4 §7.2
   "Dimensional Mapping" pins the sizing properties physically ("The height properties (height, min-height, and
   max-height) refer to the physical height, and the width properties (width, min-width, and max-width) refer
   to the physical width") and every edge read here is physical; a caller with a flow-relative question composes
   css-writing-modes-4 §6.4 "Abstract-to-Physical Mappings" first (core/css/css_logical.h's
   `css_logical_axis_is_vertical`, or core/layout/flex_item.h's `flex_container_axis_is_vertical`).
   It is a required parameter of `intrinsic_declared_sizing_px` so a declared `height` cannot have the left and
   right edges subtracted. §2.2's outer size has two entries instead, since its floor applies only to a pair.
   The vertical value selects edges; `intrinsic_inline_sizes` is still the horizontal walk only
   (core/layout/intrinsic_block_size.h is the other). */
typedef enum {
    INTRINSIC_AXIS_HORIZONTAL = 0,
    INTRINSIC_AXIS_VERTICAL   = 1
} IntrinsicAxis;

/* css-sizing-3 §5.1's pair, returned together: §2.1 defines them over the same content with only soft wrap
   opportunities differing, so one walk produces both, and `min_content <= max_content` is a fact about the
   pair. */
typedef struct {
    CssPx min_content;   /* §2.1's min-content inline size — CSS 2.2 §10.3.5's "preferred minimum width" */
    CssPx max_content;   /* §2.1's max-content inline size — CSS 2.2 §10.3.5's "preferred width" */
} IntrinsicInlineSizes;

/* The intrinsic inline sizes of `el`'s box, as content-box widths in CSS pixels. The caller establishes
   nothing: every box type this walk cannot size crashes naming its section. */
IntrinsicInlineSizes intrinsic_inline_sizes(lxb_dom_element_t *el);

/* One of `el`'s three sizing properties on `axis`, as a content-box extent in CSS pixels for an intrinsic
   pass: true when it states a size to apply, false when it states none. `name` is `width`, `min-width` or
   `max-width` (or the `height` triple vertically) and `initial` its initial value (`auto`, `auto`, `none`), the
   only keyword admitted. `axis` is not derived from `name`: it chooses which edges css-sizing-3 §3.3 "Box Edges
   for Sizing: the box-sizing property" subtracts, and a second name table could disagree with the edges'.
   Callers compose it differently: css-sizing-3 §5.2 "Intrinsic Contributions" replaces the measurement, while
   css-flexbox-1 §9.9.3 "Flex Item Intrinsic Size Contributions" takes "the larger of its outer min-content size
   and outer preferred size".
   False is a real answer: §3.2's initial keywords state no size, and §5.2.1 "Intrinsic Contributions of
   Percentage-Sized Boxes" drops a cyclic percentage (always cyclic in an intrinsic pass). §3.2's used 0 for
   `min-width: auto` is the caller's, since css-flexbox-1 §4.5 "Automatic Minimum Size of Flex Items" overrides
   it for flex items. §3.3's conversion is applied here over the intrinsic surround. */
bool intrinsic_declared_sizing_px(lxb_dom_element_t *el, IntrinsicAxis axis, const char *name,
                                 CssKeyword initial, CssPx *out);

/* CSS 2.2 §9.4.2 "Inline formatting contexts" over one run of `el`'s content (core/layout/block_flow.h's
   `BlockFlowRun`), as content-box inline sizes in CSS pixels, styled by `el`, which an anonymous box around
   the run inherits from. Exported because CSS 2.2 §9.2.1.1 "Anonymous block boxes" (this file's §9.4.1 arm) and
   css-flexbox-1 §4 "Flex Items" (a child text sequence) both wrap a run in an anonymous box; the delimiters
   differ, the inline formatting context inside does not. A `BlockFlowRun` is `after`'s next box-tree sibling
   onward, so either caller states `[first, end)` with it.
   The run's own edges are not added: an anonymous box has initial non-inherited values ("the margins will be
   0"; §4: "the anonymous item's box is unstyleable"), so its outer size is its inner size. */
IntrinsicInlineSizes intrinsic_inline_run_sizes(lxb_dom_element_t *el, BlockFlowRun run);

/* Whether `el`'s intrinsic inline sizes, or one run's, are ready to read: false ("not yet") when a resting
   drive's line box items (core/layout/layout_job.h) ran out first, with the measurement held for the next
   ask. Outside a resting drive, or under a drive nested in a body, each answers true. The two readers above
   are drives over these asks, so a body that can propagate "not yet" asks first and the read is served. */
bool intrinsic_inline_sizes_ask(lxb_dom_element_t *el);
bool intrinsic_inline_run_sizes_ask(lxb_dom_element_t *el, BlockFlowRun run);

/* A layout job's intrinsic size record: one held measurement per run of a container and one held walk per
   container's §9.4.1 stack, owned by the job and released with its memo, whose lifetime sees no DOM write.
   An ask finds its entry answered (`served`), creates it (`built`) or continues it (`resumed`); `release`
   asserts the three sum to `asks` and frees every entry. */
struct IsEntry;
typedef struct {
    struct IsEntry *tab;   /* a power-of-two table, NULL with `cap` 0 before the first entry */
    size_t          cap, used;
    long long       asks, served, built, resumed;
} IntrinsicSizeRecord;
void intrinsic_size_record_release(IntrinsicSizeRecord *r);

/* The outer size of `el`'s box over inner sizes the caller computed. css-sizing-3 §2 "Terminology": an outer
   size is "the margin-box size of a box", an inner size "the content-box size of a box"; §2.2 "Intrinsic Size
   Contributions" makes it every contribution's operand, "auto margins are treated as zero". css-sizing-3 §5.2.1
   resolves a cyclic percentage on the six edge properties against zero.
   The inner sizes are an argument because css-flexbox-1 §9.9.3 caps, floors and clamps before the outer step;
   that commutes because every operand is one item's size taking the same edge sum.
   §2.2's floor is applied here ("if the ideal max-content contribution would be smaller than the min-content
   contribution (e.g. due to the use of negative margins), the effective max-content contribution is floored by
   the min-content contribution"), so neither inner size is asserted ordered.
   `el` is NULL for an anonymous box, whose edges are zero (CSS 2.2 §9.2.1.1: "the margins will be 0";
   css-flexbox-1 §4: "the anonymous item's box is unstyleable, since there is no element to assign style rules
   to"), so callers cannot substitute the container's edges. The edge itself is not exported: its §5.2.1 zero is
   wrong at any used-value site. */
IntrinsicInlineSizes intrinsic_outer_contribution(lxb_dom_element_t *el, IntrinsicInlineSizes inner);

/* css-sizing-3 §2.2's outer size of `el`'s box in the vertical axis over an inner size the caller computed: top
   and bottom margin, border and padding, each at §5.2.1's zero basis. One number because a block size has one
   intrinsic size (css-sizing-3 §3.2: "for a box's block size, unless otherwise specified, this is equivalent to
   its automatic size"; core/layout/intrinsic_block_size.h measures it). A separate entry, not an axis parameter
   on the pair: §2.2's floor would compare a number with itself. The edge arithmetic is shared (`IS_EDGE`'s two
   rows). `el` is NULL for an anonymous box, with zero edges as above. */
CssPx intrinsic_outer_block_contribution(lxb_dom_element_t *el, CssPx inner);

#endif
