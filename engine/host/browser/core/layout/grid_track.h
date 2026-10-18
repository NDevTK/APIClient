/* css-grid-1 §11.3 "Track Sizing Algorithm" over a grid container's columns and rows, and the grid area each
 * item is placed in, which css-grid-1 §3.3 "Grid Areas" makes the item's containing block: "A grid item's grid
 * area forms the containing block into which it is laid out."
 *
 * Each axis is a step machine in the line fill's shape (core/layout/text_run.h's TextRunFill): one step folds
 * one grid item's contribution into its track, and the item's step asks its measurement first, so a resting
 * drive can stop at any item and the next activation continues at the same one with the measurement held.
 * The track arithmetic after the last item is a constant step. Columns are sized before rows (css-grid-1 §11
 * "Grid Layout Algorithm"), so a row's item contributes at its used column width. Readers are drives over the
 * steps; an `_ask` reader returns "not yet" instead and is re-entered from its first step. */
/* Named residual. Not covered: gutters, content alignment that moves a track, and every grid property but
 * `display`, the two track lists (core/css/css_grid_track_list.h's residual covers their grammar) and the
 * placement properties (core/layout/grid_placement.h's covers spans). Implicit tracks are `auto`, the initial
 * `grid-auto-rows` and `grid-auto-columns` (§7.6 "Implicit Track Sizing"). Next diff builds: gutters, CSS
 * Gaps 1's `row-gap` and `column-gap` as fixed-size tracks between the grid's tracks. How its absence would
 * show: a page's grid aborting at grid_track.c's refusal of a declared gap. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_GRID_TRACK_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_GRID_TRACK_H

#include <stdbool.h>
#include <stddef.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_grid_track_list.h"
#include "core/css/css_length.h"
#include "core/layout/box_tree.h"
#include "core/layout/grid_placement.h"

/* What an axis is sized under: the available grid space (§11.2), or a min-content or max-content constraint
   (css-sizing-3 §2.1 "Auto Box Sizes"), under which §11.6's free space is zero or infinite. */
typedef enum {
    GRID_SIZING_AVAILABLE = 0,
    GRID_SIZING_MIN_CONTENT,
    GRID_SIZING_MAX_CONTENT
} GridSizingConstraint;

/* One track. `kind` and `flex` are its sizing function (css_grid_track_list.h); `fixed` its resolved length
   for a fixed one. `base` and `growth` are §11.4's base size and growth limit, `growth_inf` while the limit is
   infinite. `flex_maxc` is the largest max-content contribution among a flexible track's items, which §11.7's
   indefinite case reads. */
typedef struct {
    CssGridTrackKind kind;
    double           flex;
    CssPx            fixed, base, growth, flex_maxc;
    bool             growth_inf, has_items;
} GridTrack;

/* One run of §11.3 over one axis of `container`. `placement` is §8.5's cell for every item (owned). `target`
   is the item whose track is wanted, or NULL; `target_index` its item number once `found`. `tracks` is owned,
   the explicit tracks then the implicit ones; both are released by `grid_axis_sizing_end`. */
typedef struct {
    lxb_dom_element_t   *container, *target;
    bool                 vertical;
    GridSizingConstraint constraint;
    BoxTreeChild         cursor;
    bool                 items_done, sized, found;
    GridPlacement        placement;
    size_t               items, target_index;
    GridTrack           *tracks;
    size_t               ntracks, capacity;
    CssPx                avail;
} GridAxisSizing;

/* Begin §11.3 over `container`'s columns (`vertical` false) or rows. `container` must generate a grid
   container box and `target`, when not NULL, must be one of its grid items. A row run whose container's
   height is indefinite is sized under a max-content constraint whatever `constraint` says. Refuses, by
   name, every grid property it would otherwise read as its initial value. */
void grid_axis_sizing_begin(GridAxisSizing *g, lxb_dom_element_t *container, lxb_dom_element_t *target,
                            bool vertical, GridSizingConstraint constraint);
/* One step: fold the next grid item, or, after the last, size the tracks. False is "not yet": the item's
   measurement is not ready under a resting drive, and the same step is retried. */
bool grid_axis_sizing_step(GridAxisSizing *g);
bool grid_axis_sizing_done(const GridAxisSizing *g);
void grid_axis_sizing_end(GridAxisSizing *g);

/* §8.5's placement of `container`'s items over its explicit grid (core/layout/grid_placement.h), owned by the
   caller and released with `grid_placement_free`. */
void grid_track_placement(lxb_dom_element_t *container, GridPlacement *out);
/* Whether `container`'s columns can be sized in this activation: every step run, false at the first "not yet". */
bool grid_track_columns_ask(lxb_dom_element_t *container);
/* The inline size of `item`'s grid area in `container`, a content-box extent in CSS pixels: the used size of
   the column it is placed in. `item` must be a grid item of `container`. */
CssPx grid_item_area_inline_size(lxb_dom_element_t *container, lxb_dom_element_t *item);
/* The block size of `item`'s grid area: the used size of its row. Same contract. */
CssPx grid_item_area_block_size(lxb_dom_element_t *container, lxb_dom_element_t *item);
/* The offset of `item`'s grid area from `container`'s content-box origin, in CSS pixels on each axis. False
   is "not yet". Refuses, by name, a content alignment (css-grid-1 §10.5 "Aligning the Grid") that would move
   a track. */
bool grid_item_area_origin_ask(lxb_dom_element_t *container, lxb_dom_element_t *item, CssPx *x, CssPx *y);
/* css-grid-1 §5.2 "Sizing Grid Containers"' max-content block size of `container`, a content-box extent: the
   sum of its row sizes under a max-content constraint. False is "not yet". */
bool grid_container_max_content_block_size_ask(lxb_dom_element_t *container, CssPx *out);
/* §5.2's min-content and max-content inline sizes of `container`: "the sum of the grid container's track sizes
   (including gutters) in the appropriate axis, when the grid is sized under a max-content constraint
   (min-content constraint)". Content-box extents. False is "not yet". */
bool grid_container_inline_sizes_ask(lxb_dom_element_t *container, CssPx *min_content, CssPx *max_content);

/* Whether the track `item` is placed in on one axis has an `auto` min track sizing function — §6.6 "Automatic
   Minimum Size of Grid Items"' second condition, which `auto` and a bare `<flex>` meet and a length does not. */
bool grid_item_track_min_is_auto(lxb_dom_element_t *container, lxb_dom_element_t *item, bool vertical);
/* css-grid-1 §6.6's content size suggestion for `item`'s width: its min-content size, clamped by a definite
   `max-width`, as a content-box extent in CSS pixels. `item` must be a grid item of `container` and not a
   scroll container (§6.6's first condition, which is the caller's). A replaced item, whose suggestion is
   §6.6's transferred one, is refused by name. */
CssPx grid_item_content_size_suggestion_inline(lxb_dom_element_t *container, lxb_dom_element_t *item);

#endif
