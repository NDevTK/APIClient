/* css-flexbox-1 §9.3 "Main Size Determination" and §9.7 "Resolving Flexible Lengths": a flex item's used main
 * size. core/layout/used_value.h cannot answer it, because CSS 2.1 §10.3.3 divides one box's slack between its
 * own margins while a flex item's main size comes from a free space shared with its siblings.
 *
 * Main axis only, per §5.1's mapping: a flex item's cross size is §9.4 "Cross Size Determination"'s step 11.
 * A caller holding a physical axis asks core/layout/flex_item.h's `flex_container_main_axis` first.
 * §9.7 operates on a flex line ("To resolve the flexible lengths of the items within a flex line"), and §9.3's
 * step 5 makes the lines: one for a single-line container, and for a multi-line one consecutive items, in
 * css-display-3 §3 "Display Order: the order property"'s order-modified document order, broken where the next
 * outer hypothetical main size would not fit the container's inner main size.
 *
 * A container's lines are one layout job question (LAYOUT_Q_FLEX_LINES, core/layout/layout_job.h), answered once
 * per job for every item, since each item's answer depends on all of them. This stops at "Set each item's used main size to its target main size."; §9.5 "Main-Axis
 * Alignment" (positions) is not built here. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_FLEX_LINE_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_FLEX_LINE_H

#include <stdbool.h>
#include <stddef.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"
#include "core/layout/layout_question.h"

/* §9.7's last step for one item, as a content-box extent in CSS pixels in `container`'s main axis.
   `item` is the item's first node: the element for `FLEX_ITEM_CHILD_ELEMENT`, or the first text node of a
   child text sequence for `FLEX_ITEM_CHILD_TEXT` (core/layout/flex_item.h; `flex_item_text_sequence_end`
   delimits it), since css-flexbox-1 §4 "Flex Items" wraps each such sequence in an "anonymous block container
   flex item". A node the child list holds names every item uniquely without a sentinel element.
   `container`'s main axis must be its inline axis (`row` or `row-reverse`, §5.1 "Flex Flow Direction: the
   flex-direction property"); both preconditions are asserted here, since reading §5, §7 or §9's properties
   off a box outside their `Applies to:` line answers the initial value.
   The result is a content box, as §9.7's arithmetic is ("its content-box size", and §7.2.3 "The flex-basis
   property": "flex-basis determines the size of the content box"); converting to css-sizing-3 §3.3's
   `box-sizing` used value is the exposing caller's job. */
CssPx flex_line_used_main_size(lxb_dom_element_t *container, lxb_dom_node_t *item);
/* The same as an ask: true with `*out` once the container's lines are answered, false ("not yet") with them
   suspended on the current job for the caller's drive. */
bool flex_line_used_main_size_ask(lxb_dom_element_t *container, lxb_dom_node_t *item, CssPx *out);

/* One flex item of a container as §9.3's step 5 collected it: its identity node (as `flex_line_used_main_size`
   takes it), whether it is §4's anonymous item, one past its text sequence when it is (NULL at the end of the
   child list, and for an element item), and its line, counted from zero. */
typedef struct {
    lxb_dom_node_t *first;
    bool            anonymous;
    lxb_dom_node_t *end;
    size_t          line;
} FlexLineMember;

/* §9.3's step 5 over `container`'s items, for §9.4's per-line steps: writes the items in order-modified
   document order to a calloc'd `*out` the caller frees (NULL when there are none) and returns how many. Lines
   are contiguous runs of equal `line`. The preconditions are `flex_line_used_main_size`'s on `container`. */
size_t flex_line_members(lxb_dom_element_t *container, FlexLineMember **out);

/* The same as an ask: true with `*out` and `*n` as above once every item's §9.2 measurement is answered, false
   ("not yet") with the deferred questions suspended on the current job for the caller's drive. */
bool flex_line_members_ask(lxb_dom_element_t *container, FlexLineMember **out, size_t *n);

/* The lines as an ask, for a body or an operand pass that can propagate "not yet": true once the container's
   LAYOUT_Q_FLEX_LINES answer is memoized, false with it suspended on the current job for the caller's drive. */
bool flex_line_lines_ask(lxb_dom_element_t *container);

/* LAYOUT_Q_FLEX_LINES' body, which only block_flow_drive_deferred calls: the container's main size and each item's
   measurement asked, then §9.3's steps 5 and 6. */
void flex_line_question_run(LayoutQuestion q, size_t at);

#endif
