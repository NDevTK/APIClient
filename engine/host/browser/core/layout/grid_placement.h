/* css-grid-1 §8 "Placing Grid Items": which grid cell each grid item occupies, from §8.3 "Line-based Placement:
 * the grid-row-start, grid-column-start, grid-row-end, and grid-column-end properties", §8.4 "Placement
 * Shorthands: the grid-column, grid-row, and grid-area properties" and §8.5 "Grid Item Placement Algorithm" in
 * `grid-auto-flow: row` with sparse packing. core/layout/grid_track.h sizes the tracks the cells are in. */
/* Named residual. Not covered: named lines and areas (`<custom-ident>`), lines before the explicit grid, a
 * row-locked item past the implicit grid's columns, and a placement shorthand declared beside one of its own
 * longhands, each refused by name. Next diff builds: named lines, from `grid-template-columns`' and
 * `grid-template-rows`' `[<custom-ident>*]`. How its absence would show: a page's grid aborting at
 * grid_placement.c's refusal of a `<custom-ident>`. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_GRID_PLACEMENT_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_GRID_PLACEMENT_H

#include <stdbool.h>
#include <stddef.h>

#include <lexbor/dom/dom.h>

#include "core/layout/box_tree.h"

/* One item's grid area: its start row and column as zero-based track indices, and the number of rows and
   columns it spans, each at least one. */
typedef struct {
    size_t row, col, rows, cols;
} GridCell;

/* Every item's cell, in order-modified document order; `columns` and `rows` count the implicit grid's tracks,
   the explicit ones included. `cells` is owned and released by `grid_placement_free`. */
typedef struct {
    GridCell *cells;
    size_t    n, columns, rows;
} GridPlacement;

/* The grid item box-tree child `c` of `container` is, or NULL for a child that is none (css-grid-1 §6 "Grid
   Items"). Refuses, by name, a pseudo-element or text item. */
lxb_dom_element_t *grid_item_of(lxb_dom_element_t *container, BoxTreeChild c);

/* §8.5 over `container`'s grid items, with an explicit grid of `explicit_columns` by `explicit_rows` tracks.
   The implicit grid has at least one column, which an auto-placed item needs. */
void grid_placement_run(lxb_dom_element_t *container, size_t explicit_columns, size_t explicit_rows,
                        GridPlacement *out);
void grid_placement_free(GridPlacement *p);

#endif
