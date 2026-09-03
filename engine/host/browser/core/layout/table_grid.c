/* CSS 2.1 §17.5 Visual layout of table contents' grid, over core/layout/table_box.h's rows. See table_grid.h
   for the contract, for why the spans come from HTML §4.9.11, and for why the overlap case is recorded rather
   than crashed on. */
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/html/integer_microsyntax.h"
#include "core/layout/table_box.h"
#include "core/layout/table_grid.h"

/* HTML §4.9.12.1 Forming a table's two ceilings, as the section states them. They are not this file's policy
   and not a bound in CLAUDE.md's sense — a page may write any digits it likes and the algorithm's own steps
   are "If colspan is greater than 1000, let it be 1000 instead" and "If rowspan is greater than 65534, let it
   be 65534 instead". */
#define TG_COLSPAN_MAX ((size_t) 1000)
#define TG_ROWSPAN_MAX ((size_t) 65534)

static void *tg_reserve(void *v, size_t n, size_t *cap, size_t esz)
{
    void *grown;

    if (n < *cap) return v;
    *cap = (*cap == 0) ? 8 : *cap * 2;
    grown = realloc(v, *cap * esz);
    CHECK(grown != NULL,
          "CSS 2.1 §17.5's grid could not allocate one table's cells or its per-column occupancy — the arrays "
          "hold one entry per cell box and one per grid column of the table being laid out");
    return grown;
}

/* An HTML element's local name, which is ASCII-lowercase already for an HTML-namespace element. */
static bool tg_html_local_name_is(lxb_dom_element_t *el, const char *name)
{
    size_t len = 0;
    const lxb_char_t *ln;

    if (el == NULL || lxb_dom_interface_node(el)->ns != LXB_NS_HTML) return false;
    ln = lxb_dom_element_local_name(el, &len);
    return ln != NULL && strlen(name) == len && memcmp(ln, name, len) == 0;
}

/* HTML §4.9.12.1 Forming a table's parse-and-clamp for ONE span attribute, which the section states twice in
   the same shape: parse the value with §2.3.4.2's rules for parsing non-negative integers, fall back to
   `fallback` when the attribute is absent or the parse failed, and clamp to `ceiling`.
   THE OVERFLOW ARM IS THE CLAMP AND NOT AN ERROR, which is why core/html/integer_microsyntax.h reports the two
   apart: its own header says every bound in the platform belongs to the CONSUMER, so a run of digits too large
   for a `long long` has not failed to parse — it has parsed to something above any ceiling here, and §4.9.12.1
   answers that with the ceiling. Treating it as a parse failure would give `colspan="99999999999999999999"`
   the value 1 where the section gives it 1000. */
static size_t tg_span_attr(lxb_dom_element_t *cell, const char *name, size_t fallback, size_t ceiling)
{
    size_t len = 0;
    const lxb_char_t *v;
    HtmlInteger n;

    /* HTML §4.9.11 Attributes common to td and th elements gives `colspan` and `rowspan` meaning on exactly
       these two elements. Anything else — a `display: table-cell` element that is not one, and §17.2.1's
       anonymous cell, which is not an element at all — spans one grid cell. */
    if (!tg_html_local_name_is(cell, "td") && !tg_html_local_name_is(cell, "th")) return fallback;
    v = lxb_dom_element_get_attribute(cell, (const lxb_char_t *) name, strlen(name), &len);
    if (v == NULL) return fallback;
    if (!html_parse_non_negative_integer((const char *) v, len, &n)) return fallback;
    if (n.overflow || n.value > (long long) ceiling) return ceiling;
    return (size_t) n.value;
}

/* §17.5's rule 6 — "A cell box cannot extend beyond the last row box of a table or row group; the user agents
   must shorten it until it fits" — needs each row's GROUP RUN. A run is a maximal stretch of consecutive rows
   reporting the same row group box, and a stretch reporting NONE is one run of its own: those are the rows
   whose parent is the table box itself, which HTML §4.9.12.1 Forming a table gathers into a row group as well.
   Answers, for each row, ONE PAST the last row index of its run — so `end[y] - y` is what a cell anchored in
   row `y` may still cover, and is at least 1. */
static size_t *tg_group_ends(const TableBoxRow *rows, size_t nrows)
{
    size_t *end;
    size_t i = 0;

    if (nrows == 0) return NULL;
    end = (size_t *) calloc(nrows, sizeof *end);
    CHECK(end != NULL, "CSS 2.1 §17.5's rule 6 could not allocate one row-group extent per row of a table");
    while (i < nrows) {
        size_t j = i + 1;

        while (j < nrows && rows[j].group == rows[i].group) j++;
        for (; i < j; i++) end[i] = j;
    }
    return end;
}

void table_grid_build(lxb_dom_element_t *table, TableGrid *out)
{
    TableBoxRow *rows = NULL;
    size_t nrows, y, anchor;
    size_t *group_end;
    /* PER COLUMN, THE FIRST GRID ROW THAT COLUMN IS FREE AGAIN AT — the whole of §17.5's occupancy state, and
       it is a row index rather than a bitmap for a reason the section supplies: every cell covers a CONTIGUOUS
       run of rows starting at its own anchor row, and the rows are walked top-down, so one number per column
       says exactly which slots a later row must skip. A column index at or past `ncols` has never been
       occupied and needs no entry, which is why the array grows instead of being sized up front. */
    size_t *free_at = NULL, free_cap = 0, ncols = 0;
    TableGridCell *cells = NULL;
    size_t ncells = 0, cell_cap = 0;
    bool any_overlap = false;
    lxb_dom_element_t **row_els = NULL;

    DCHECK(table != NULL, "CSS 2.1 §17.5's grid was asked for of no element");
    DCHECK(out != NULL,
           "CSS 2.1 §17.5's grid was asked for with nowhere to put it. A table's column count alone names no "
           "cell, so §17.5.2's column widths — which are taken across the cells that occupy each column — "
           "could not be computed from it, and that is the whole of what this entry is asked for");
    out->cells = NULL;
    out->ncells = 0;
    out->rows = NULL;
    out->nrows = 0;
    out->ncols = 0;
    out->any_overlap = false;
    /* §17.5's rule 1 states the row count and this entry does not re-derive it: "Each row box occupies one row
       of grid cells… the table occupies exactly as many grid rows as there are row elements." */
    nrows = table_box_rows(table, &rows);
    group_end = tg_group_ends(rows, nrows);
    /* RULE 1's OTHER HALF, WHICH IS THE PLACEMENT AND NOT THE COUNT: "Together, the row boxes fill the table
       from top to bottom in the order they occur in the source document." `table_box_rows` answers them in
       §17.2's display order, so the index of a row in that answer IS its grid row and this loop copies rather
       than derives. It is a COPY and not a borrow of `rows` because `table_box_rows_free` runs below and every
       other field of this grid outlives it. */
    if (nrows != 0) {
        row_els = (lxb_dom_element_t **) calloc(nrows, sizeof *row_els);
        CHECK(row_els != NULL,
              "CSS 2.1 §17.5's rule 1 could not allocate one row element per grid row of the table being laid "
              "out");
        for (y = 0; y < nrows; y++) row_els[y] = rows[y].element;
    }
    for (y = 0; y < nrows; y++) {
        size_t x = 0, k;

        for (k = 0; k < rows[y].ncells; k++) {
            lxb_dom_element_t *el = rows[y].cells[k].element;
            size_t colspan, rowspan, remaining, c;
            bool grows_downward, overlaps = false;

            /* §17.5's rule 5: "The rectangle must be as far to the left as possible, but the part of the cell
               in the first column it occupies must not overlap with any other cell box (i.e., a row-spanning
               cell starting in a prior row), and the cell must be to the right of all cells in the same row
               that are earlier in the source document." The running `x` is the second clause and this scan is
               the first. HTML §4.9.12.1 Forming a table writes the same step over its own slot coordinates,
               advancing the current x past every slot that already has a cell assigned to it.
               THAT STEP IS DELIBERATELY NOT QUOTED, and the reason belongs beside it rather than in a report:
               its variables are SUBSCRIPTED in the source, so the sentence has no faithful one-line
               transcription — the text content separates the subscript as its own token while the rendering
               joins it, and engine/citegen.mjs's committed corpus holds the first. A quotation that matches
               neither form is worse than a paraphrase that claims to be one, so this is a paraphrase and the
               normative words above are CSS 2.1's, which have no subscripts in them. */
            while (x < ncols && free_at[x] > y) x++;
            colspan = tg_span_attr(el, "colspan", 1, TG_COLSPAN_MAX);
            /* §4.9.12.1: "If parsing that value failed, or returned zero, or if the attribute is absent, then
               let colspan be 1, instead." A parsed ZERO is the one case the fallback above cannot express,
               because zero is a successful parse of a valid non-negative integer. */
            if (colspan == 0) colspan = 1;
            rowspan = tg_span_attr(el, "rowspan", 1, TG_ROWSPAN_MAX);
            /* §4.9.11: "the value zero means that the cell is to span all the remaining rows in the row
               group", which §17.5's rule 6 then bounds — so the two sentences meet in the same clamp below and
               a zero needs no arm of its own beyond asking for everything. */
            grows_downward = (rowspan == 0);
            remaining = group_end[y] - y;
            if (grows_downward || rowspan > remaining) rowspan = remaining;
            /* Grow the occupancy to cover this cell, marking the new columns free (never occupied). */
            if (x + colspan > ncols) {
                size_t want = x + colspan;

                while (free_cap < want) free_at = (size_t *) tg_reserve(free_at, free_cap, &free_cap,
                                                                       sizeof *free_at);
                for (c = ncols; c < want; c++) free_at[c] = 0;
                ncols = want;
            }
            /* §17.5's rule 5's undefined case, detected rather than assumed away: the anchor slot is free by
               the scan above, so any collision is a COLUMN-spanning reach into a row-spanning cell — which is
               the exact case the section declines to define. */
            for (c = x; c < x + colspan; c++)
                if (free_at[c] > y) overlaps = true;
            any_overlap = any_overlap || overlaps;
            for (c = x; c < x + colspan; c++) free_at[c] = y + rowspan;
            cells = (TableGridCell *) tg_reserve(cells, ncells, &cell_cap, sizeof *cells);
            cells[ncells].element = el;
            cells[ncells].row = y;
            cells[ncells].col = x;
            cells[ncells].rowspan = rowspan;
            cells[ncells].colspan = colspan;
            cells[ncells].overlaps = overlaps;
            ncells++;
            DCHECK(rowspan >= 1 && colspan >= 1,
                   "CSS 2.1 §17.5's grid placed a cell covering no grid cell. Rule 5 makes every cell \"one or "
                   "more grid cells wide and high\", the column span is floored at one where a parsed zero is "
                   "replaced, and the row span is rule 6's remaining rows in the group, which is at least one "
                   "because a row is inside its own run — so a zero here is one of those three having been "
                   "computed from the wrong operand");
            x += colspan;
        }
    }
    free(free_at);
    free(group_end);
    table_box_rows_free(rows, nrows);
    out->cells = cells;
    out->ncells = ncells;
    out->rows = row_els;
    out->nrows = nrows;
    out->ncols = ncols;
    out->any_overlap = any_overlap;
    for (anchor = 0; anchor < ncells; anchor++)
        DCHECK(cells[anchor].row < nrows,
               "CSS 2.1 §17.5's grid anchored a cell in a grid row rule 1 did not place a row box at. \"The top "
               "row of this rectangle is in the row specified by the cell's parent\", and every cell here was "
               "reached by descending a row of this same walk, so a row index past the row array is the two "
               "halves of one loop having been carried apart");
}

/* THE ONE-MATCH INVARIANT IS THE POINT OF THE SCAN AND NOT A SIDE EFFECT OF IT. CSS 2.1 §17.5 Visual layout of
   table contents places each cell box ONCE — "Each cell is thus a rectangular box, one or more grid cells wide
   and high", one rectangle per cell — so an element appearing twice in `cells` is the placement above having
   run over the same row or the same cell node twice, and the two entries would disagree about which columns
   the cell covers. A consumer taking the first hit would then get a width for one of the two placements with
   nothing to say the other exists, so the loop runs to the END rather than returning early. */
const TableGridCell *table_grid_cell_of(const TableGrid *grid, const lxb_dom_element_t *cell)
{
    const TableGridCell *found = NULL;
    size_t i;

    DCHECK(grid != NULL, "CSS 2.1 §17.5's grid was asked which cell an element is through no grid");
    DCHECK(grid->cells != NULL || grid->ncells == 0,
           "CSS 2.1 §17.5's grid was asked for a cell while holding no cell array and a non-zero cell count — "
           "`table_grid_build` stores NULL only for a table whose rows generate no cell, so the two have been "
           "carried apart since it answered");
    DCHECK(cell != NULL,
           "CSS 2.1 §17.5's grid was asked which rectangle a NULL element occupies. CSS 2.1 §17.2.1 Anonymous "
           "table objects' anonymous 'table-cell' box is the only cell here with no element, and it is not "
           "something a caller can name — a NULL would match the first such box in the grid and answer a "
           "question about a different cell entirely");
    for (i = 0; i < grid->ncells; i++) {
        if (grid->cells[i].element != cell) continue;
        DCHECK(found == NULL,
               "CSS 2.1 §17.5 Visual layout of table contents placed ONE cell element at TWO rectangles of "
               "grid cells. The section gives each cell box exactly one — \"Each cell is thus a rectangular "
               "box, one or more grid cells wide and high\" — so this is the placement above having walked the "
               "same row or the same cell node twice, and the two entries name different columns for one box");
        found = &grid->cells[i];
    }
    return found;
}

/* THE ONE-MATCH INVARIANT IS THE SAME INVARIANT ONE AXIS OVER, AND IT IS RULE 1's OWN SENTENCE RATHER THAN AN
   ANALOGY. "Each row box occupies one row of grid cells" — one grid row per row box — so an element occupying
   two of them is `table_box_rows` having reported one row twice, and a consumer taking the first hit would read
   one of the two heights with nothing to say the other exists. The loop therefore runs to the END, exactly as
   the cell entry's does. */
bool table_grid_row_of(const TableGrid *grid, const lxb_dom_element_t *row, size_t *out)
{
    bool found = false;
    size_t i;

    DCHECK(grid != NULL, "CSS 2.1 §17.5's grid was asked which grid row an element occupies through no grid");
    DCHECK(grid->rows != NULL || grid->nrows == 0,
           "CSS 2.1 §17.5's grid was asked for a row while holding no row array and a non-zero row count — "
           "`table_grid_build` stores NULL only for a table with no rows at all, so the two have been carried "
           "apart since it answered");
    DCHECK(row != NULL,
           "CSS 2.1 §17.5's grid was asked which grid row a NULL element occupies. CSS 2.1 §17.2.1 Anonymous "
           "table objects' anonymous 'table-row' box is the only row here with no element, and it is not "
           "something a caller can name — a NULL would match the first such row in the grid and answer a "
           "question about a different row entirely");
    DCHECK(out != NULL,
           "CSS 2.1 §17.5's rule 1 was read back with nowhere to put the grid row. The index IS the answer — "
           "the boolean says only whether there is one — so a caller with nowhere to put it has asked a "
           "question it cannot use the answer to");
    for (i = 0; i < grid->nrows; i++) {
        if (grid->rows[i] != row) continue;
        DCHECK(!found,
               "CSS 2.1 §17.5 Visual layout of table contents placed ONE row element at TWO grid rows. Rule 1 "
               "gives each row box exactly one — \"Each row box occupies one row of grid cells\" — so this is "
               "core/layout/table_box.h's row generation having reported one row twice, and the two entries "
               "name different heights and different positions for one box");
        found = true;
        *out = i;
    }
    return found;
}

void table_grid_release(TableGrid *grid)
{
    DCHECK(grid != NULL, "CSS 2.1 §17.5's grid was released through no grid");
    DCHECK(grid->cells != NULL || grid->ncells == 0,
           "a table grid was released holding no cell array with a non-zero cell count — `table_grid_build` "
           "stores NULL only for a table whose rows generate no cell, so the two have been carried apart since "
           "it answered");
    DCHECK(grid->rows != NULL || grid->nrows == 0,
           "a table grid was released holding no row array with a non-zero row count — `table_grid_build` "
           "stores NULL only for a table with no rows at all, so the two have been carried apart since it "
           "answered");
    free(grid->cells);
    free(grid->rows);
    grid->cells = NULL;
    grid->ncells = 0;
    grid->rows = NULL;
    grid->nrows = 0;
    grid->ncols = 0;
    grid->any_overlap = false;
}
