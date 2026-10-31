/* css-grid-1 §10.3 "Inline-axis Alignment: the justify-self and justify-items properties" and §10.4
 * "Block-axis Alignment: the align-self and align-items properties": where a grid item's margin box sits in its
 * grid area, by css-align-3 §6.1's self-alignment and §4.1's positional keywords. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_GRID_ITEM_ALIGNMENT_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_GRID_ITEM_ALIGNMENT_H
#include <stdbool.h>
#include <lexbor/dom/dom.h>
#include "core/css/css_length.h"

/* The offset of `item`'s margin box from the start of its grid area of extent `area`, on the block axis when
   `vertical` and the inline axis otherwise, asked under a step machine's resting drive: false ("not yet") with
   `*out` unwritten when the item's size was deferred. `item` is a grid item of `container`. */
bool grid_item_self_alignment_offset_ask(lxb_dom_element_t *container, lxb_dom_element_t *item, CssPx area,
                                         bool vertical, CssPx *out);

#endif
