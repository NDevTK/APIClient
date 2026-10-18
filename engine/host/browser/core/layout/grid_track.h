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
/* Named residual. Not covered: an explicit grid, gutters, content alignment that moves a track, and every
 * other grid property but `display`. The grid is css-grid-1 §7.5 "The Implicit Grid"'s with
 * §8.5 "Grid Item Placement Algorithm" placing every item in row flow: one implicit `auto` column, and one
 * implicit `auto` row per item (§7.6 "Implicit Track Sizing: the grid-auto-rows and grid-auto-columns
 * properties"' initial values). A container or item declaring a grid property read here as initial is refused
 * by name. Next diff builds: explicit `grid-template-columns`, with §8.5 then placing several items in a row.
 * How its absence would show: a page's grid aborting at grid_track.c's refusal of a declared track list. */
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
/* Whether `container`'s columns can be sized in this activation: every step run, false at the first "not yet". */
bool grid_track_columns_ask(lxb_dom_element_t *container);

/* One run of §11.3 over `container`'s rows, one `auto` row per item in item order. `target` is the item whose
   row is wanted, or NULL. `sum` is the rows' base sizes so far, `rows` their count; `target_index` and
   `before` are the target's row number and the base sizes ahead of it. `definite` is whether §11.2's
   available grid space `avail` exists in this axis. Once sized, `share` is §11.8's addition to every row. */
typedef struct {
    lxb_dom_element_t *container, *target;
    BoxTreeChild       cursor;
    bool               items_done, sized, definite, found;
    size_t             rows, target_index;
    CssPx              avail, sum, before, target_base, share;
} GridRowSizing;

/* Begin §11.3 over `container`'s rows. `container` must generate a grid container box and `target`, when not
   NULL, must be one of its grid items. Refuses, by name, every grid property it would read as initial. */
void grid_row_sizing_begin(GridRowSizing *g, lxb_dom_element_t *container, lxb_dom_element_t *target);
/* One step: fold the next item's row, or, after the last, size the rows. False is "not yet". */
bool grid_row_sizing_step(GridRowSizing *g);
bool grid_row_sizing_done(const GridRowSizing *g);

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

/* css-grid-1 §6.6 "Automatic Minimum Size of Grid Items"' content size suggestion for `item`'s width: its
   min-content size, clamped by a definite `max-width`, as a content-box extent in CSS pixels. `item` must be a
   grid item of `container` and not a scroll container (§6.6's first condition, which is the caller's); the
   one implicit `auto` column meets §6.6's other two. A replaced item, whose suggestion is §6.6's transferred
   one, is refused by name. */
CssPx grid_item_content_size_suggestion_inline(lxb_dom_element_t *container, lxb_dom_element_t *item);

#endif
