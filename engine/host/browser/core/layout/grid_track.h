/* css-grid-1 §11.3 "Track Sizing Algorithm" over a grid container's columns, and the inline size of a grid
 * item's grid area, which css-grid-1 §3.3 "Grid Areas" makes the item's containing block: "A grid item's grid
 * area forms the containing block into which it is laid out."
 *
 * The algorithm is a step machine in the line fill's shape (core/layout/text_run.h's TextRunFill): one step
 * folds one grid item's contributions into its track, and the item's step asks its measurement first
 * (core/layout/intrinsic_size.h's `intrinsic_inline_sizes_ask`), so a resting drive can stop at any item and
 * the next activation continues at the same one with the measurement held. The track arithmetic after the
 * last item (§11.4's initialisation, §11.5's span-1 rule, §11.6 "Maximize Tracks", §11.8 "Stretch auto
 * Tracks") is a constant step. The reader below is a drive over the steps. */
/* Named residual. Not covered: an explicit grid, every grid property but `display`, and the block axis. The
 * grid is css-grid-1 §7.5 "The Implicit Grid"'s with §8.5 "Grid Item Placement Algorithm" placing every item
 * in row flow, so with no explicit columns there is one implicit `auto` column (§7.6 "Implicit Track Sizing:
 * the grid-auto-rows and grid-auto-columns properties"' initial value). A container or item that declares a
 * grid property this reads the initial value of is refused by name. Next diff builds: the rows over the
 * used column width and the grid area as a rectangle (core/layout/used_value.c's containing-block walk).
 * How its absence would show: a grid item's border-box origin, or its block size, still aborting at
 * used_value.c's grid-item crashes. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_GRID_TRACK_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_GRID_TRACK_H

#include <stdbool.h>
#include <stddef.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"
#include "core/layout/box_tree.h"

/* One run of §11.3 over `container`'s column tracks. `cursor` is the next box-tree child to classify;
   `base` and `growth` are the one implicit column's base size and growth limit (`growth_inf` while the
   limit is §11.4's infinity); `avail` is §11.2's available grid space, the container's content-box width. */
typedef struct {
    lxb_dom_element_t *container;
    BoxTreeChild       cursor;
    bool               items_done, sized;
    CssPx              avail, base, growth;
    bool               growth_inf;
} GridTrackSizing;

/* Begin §11.3 over `container`'s columns. `container` must generate a grid container box. Refuses, by name,
   every grid property it would otherwise read as its initial value. */
void grid_track_sizing_begin(GridTrackSizing *g, lxb_dom_element_t *container);
/* One step: fold the next grid item, or, after the last, size the column. False is "not yet": the item's
   measurement is not ready under a resting drive, and the same step is retried. */
bool grid_track_sizing_step(GridTrackSizing *g);
bool grid_track_sizing_done(const GridTrackSizing *g);

/* The inline size of `item`'s grid area in `container`, a content-box extent in CSS pixels: the used size of
   the column it is placed in. `item` must be a grid item of `container`. */
CssPx grid_item_area_inline_size(lxb_dom_element_t *container, lxb_dom_element_t *item);

/* css-grid-1 §6.6 "Automatic Minimum Size of Grid Items"' content size suggestion for `item`'s width: its
   min-content size, clamped by a definite `max-width`, as a content-box extent in CSS pixels. `item` must be a
   grid item of `container` and not a scroll container (§6.6's first condition, which is the caller's); the
   one implicit `auto` column meets §6.6's other two. A replaced item, whose suggestion is §6.6's transferred
   one, is refused by name. */
CssPx grid_item_content_size_suggestion_inline(lxb_dom_element_t *container, lxb_dom_element_t *item);

#endif
