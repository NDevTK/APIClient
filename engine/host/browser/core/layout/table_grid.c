/* CSS 2.1 §17.5 Visual layout of table contents' grid, over core/layout/table_box.h's rows. See table_grid.h. */
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/html/integer_microsyntax.h"
#include "core/layout/box_subject.h"
#include "core/layout/table_box.h"
#include "core/layout/table_grid.h"
#include <lexbor/core/base.h>   /* lexbor_realloc: the allocator the engine gives its refusal edge */

/* HTML §4.9.12.1 Forming a table's two ceilings, the section's own steps rather than engine bounds: "If colspan
   is greater than 1000, let it be 1000 instead" and "If rowspan is greater than 65534, let it be 65534
   instead". */
#define TG_COLSPAN_MAX ((size_t) 1000)
#define TG_ROWSPAN_MAX ((size_t) 65534)

static void *tg_reserve(void *v, size_t n, size_t *cap, size_t esz)
{
    void *grown;

    if (n < *cap) return v;
    *cap = (*cap == 0) ? 8 : *cap * 2;
    grown = lexbor_realloc(v, *cap * esz);
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

/* HTML §4.9.12.1 Forming a table's parse-and-clamp for one span attribute: §2.3.4.2's rules for parsing
   non-negative integers, `fallback` when absent or unparsable, then the `ceiling`. An overflowing digit run has
   parsed (core/html/integer_microsyntax.h leaves bounds to the consumer) and takes the ceiling:
   `colspan="99999999999999999999"` is 1000, not 1. */
static size_t tg_span_attr(lxb_dom_element_t *cell, const char *name, size_t fallback, size_t ceiling)
{
    size_t len = 0;
    const lxb_char_t *v;
    HtmlInteger n;

    /* HTML §4.9.11 Attributes common to td and th elements gives the spans meaning on `td` and `th` only;
       anything else, including §17.2.1's anonymous cell, spans one grid cell. */
    if (!tg_html_local_name_is(cell, "td") && !tg_html_local_name_is(cell, "th")) return fallback;
    v = lxb_dom_element_get_attribute(cell, (const lxb_char_t *) name, strlen(name), &len);
    if (v == NULL) return fallback;
    if (!html_parse_non_negative_integer((const char *) v, len, &n)) return fallback;
    if (n.overflow || n.value > (long long) ceiling) return ceiling;
    return (size_t) n.value;
}

/* Rule 2 ("A row group occupies the same grid cells as the rows it contains") and rule 6 ("A cell box cannot
   extend beyond the last row box of a table or row group; the user agents must shorten it until it fits") over
   one array: maximal stretches of consecutive rows with the same row group box, a NULL-group stretch (rows the
   table box parents) being a run of its own, as HTML §4.9.12.1 Forming a table gathers them.
   One group lands in one run because table_box.h's display order keeps each group's rows together; that is
   asserted at the end of `table_grid_build`, since a split group would mis-clamp a `rowspan` and halve rule 2's
   extent. Answers the run count and stores a newly allocated array at `*out`; zero stores NULL. */
static size_t tg_group_runs(const TableBoxRow *rows, size_t nrows, TableGridRowGroup **out)
{
    TableGridRowGroup *runs;
    size_t n = 0, i = 0;

    *out = NULL;
    if (nrows == 0) return 0;
    /* At most one run per row, which is the `<tbody><tr></tbody>` repeated case. */
    runs = (TableGridRowGroup *) lexbor_calloc(nrows, sizeof *runs);
    CHECK(runs != NULL, "CSS 2.1 §17.5's rules 2 and 6 could not allocate one row-group run per row of a table");
    while (i < nrows) {
        size_t j = i + 1;

        while (j < nrows && rows[j].group == rows[i].group) j++;
        runs[n].element = rows[i].group;
        runs[n].first = i;
        runs[n].nrows = j - i;
        n++;
        i = j;
    }
    *out = runs;
    return n;
}

void table_grid_build(lxb_dom_element_t *table, TableGrid *out)
{
    TableBoxRow *rows = NULL;
    size_t nrows, y, anchor;
    TableGridRowGroup *groups = NULL;
    size_t ngroups, g = 0;
    /* Per column, the first grid row at which that column is free again: §17.5's whole occupancy state. A row
       index suffices because every cell covers a contiguous run of rows from its anchor and rows are walked
       top-down. Columns at or past `ncols` were never occupied, so the array grows. */
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
    out->table = table;
    out->cells = NULL;
    out->ncells = 0;
    out->rows = NULL;
    out->nrows = 0;
    out->groups = NULL;
    out->ngroups = 0;
    out->ncols = 0;
    out->any_overlap = false;
    /* Rule 1: "Each row box occupies one row of grid cells… the table occupies exactly as many grid rows as
       there are row elements." */
    nrows = table_box_rows(table, &rows);
    ngroups = tg_group_runs(rows, nrows, &groups);
    /* Rule 1's placement: "Together, the row boxes fill the table from top to bottom in the order they occur
       in the source document." `table_box_rows` answers in display order, so its index is the grid row. Copied,
       because `rows` is freed below while the grid outlives it. */
    if (nrows != 0) {
        row_els = (lxb_dom_element_t **) lexbor_calloc(nrows, sizeof *row_els);
        CHECK(row_els != NULL,
              "CSS 2.1 §17.5's rule 1 could not allocate one row element per grid row of the table being laid "
              "out");
        for (y = 0; y < nrows; y++) row_els[y] = rows[y].element;
    }
    for (y = 0; y < nrows; y++) {
        size_t x = 0, k;

        /* Rule 6 is bounded by the run, read from the same array as rule 2's extent. Runs and rows are in the
           same order, so the current run advances at most once per row. */
        while (g + 1 < ngroups && y >= groups[g].first + groups[g].nrows) g++;
        DCHECK(g < ngroups && y >= groups[g].first && y < groups[g].first + groups[g].nrows,
               "CSS 2.1 §17.5's rule 6 was applied to a grid row that no row-group run contains. The runs are "
               "maximal stretches over the SAME row array this loop walks, so they partition its indices by "
               "construction — a row outside every run is that array having been rebuilt between the two "
               "passes, and the `rowspan` clamp below would then shorten a cell against another table's rows");
        for (k = 0; k < rows[y].ncells; k++) {
            lxb_dom_element_t *el = rows[y].cells[k].element;
            size_t colspan, rowspan, remaining, c;
            bool grows_downward, overlaps = false;

            /* §17.5's rule 5: "The rectangle must be as far to the left as possible, but the part of the cell
               in the first column it occupies must not overlap with any other cell box (i.e., a row-spanning
               cell starting in a prior row), and the cell must be to the right of all cells in the same row
               that are earlier in the source document." The running `x` is the second clause and this scan is
               the first. HTML §4.9.12.1 Forming a table's step advancing the current x past assigned slots is
               paraphrased, not quoted: its subscripted variables have no faithful one-line transcription in
               engine/citegen.mjs's corpus. */
            while (x < ncols && free_at[x] > y) x++;
            colspan = tg_span_attr(el, "colspan", 1, TG_COLSPAN_MAX);
            /* §4.9.12.1: "If parsing that value failed, or returned zero, or if the attribute is absent, then
               let colspan be 1, instead." Zero is a successful parse, so the fallback above cannot express it. */
            if (colspan == 0) colspan = 1;
            rowspan = tg_span_attr(el, "rowspan", 1, TG_ROWSPAN_MAX);
            /* §4.9.11: "the value zero means that the cell is to span all the remaining rows in the row
               group"; rule 6's clamp below bounds it, so zero just asks for everything. */
            grows_downward = (rowspan == 0);
            remaining = groups[g].first + groups[g].nrows - y;
            if (grows_downward || rowspan > remaining) rowspan = remaining;
            /* Grow the occupancy to cover this cell, marking the new columns free (never occupied). */
            if (x + colspan > ncols) {
                size_t want = x + colspan;

                while (free_cap < want) free_at = (size_t *) tg_reserve(free_at, free_cap, &free_cap,
                                                                       sizeof *free_at);
                for (c = ncols; c < want; c++) free_at[c] = 0;
                ncols = want;
            }
            /* Rule 5's undefined case, detected: the anchor slot is free, so a collision is a column-spanning
               reach into a row-spanning cell. */
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
    table_box_rows_free(rows, nrows);
    out->cells = cells;
    out->ncells = ncells;
    out->rows = row_els;
    out->nrows = nrows;
    out->groups = groups;
    out->ngroups = ngroups;
    out->ncols = ncols;
    out->any_overlap = any_overlap;
    for (anchor = 0; anchor < ncells; anchor++)
        DCHECK(cells[anchor].row < nrows,
               "CSS 2.1 §17.5's grid anchored a cell in a grid row rule 1 did not place a row box at. \"The top "
               "row of this rectangle is in the row specified by the cell's parent\", and every cell here was "
               "reached by descending a row of this same walk, so a row index past the row array is the two "
               "halves of one loop having been carried apart");
    /* The partition, asserted: `table_grid_group_of_row` and every consumer of a run rely on it. */
    for (g = 0; g < ngroups; g++) {
        DCHECK(groups[g].nrows >= 1,
               "CSS 2.1 §17.5's rule 2 produced a row-group run covering NO grid row. The runs are maximal "
               "stretches of the row array and a stretch starts at a row that is in it, so an empty run is the "
               "loop that built them having advanced without consuming a row. An EMPTY row group is not this: "
               "it holds no row at all, so `table_box_rows` reports none for it and no run is built for it");
        DCHECK(groups[g].first == (g == 0 ? (size_t) 0 : groups[g - 1].first + groups[g - 1].nrows),
               "CSS 2.1 §17.5's rule 2's runs do not partition the grid rows — one run does not begin where "
               "the previous one ends. They are built as maximal stretches over one array in one pass, so a "
               "gap or an overlap here is that pass having been carried apart, and rule 6's clamp would then "
               "shorten a `rowspan` at a boundary the document does not have");
    }
    DCHECK(ngroups == 0 ? nrows == 0
                        : groups[ngroups - 1].first + groups[ngroups - 1].nrows == nrows,
           "CSS 2.1 §17.5's rule 2's runs do not cover every grid row rule 1 placed. The last run must end at "
           "the row count, and a table with no rows must have no runs — otherwise a row belongs to no row "
           "group box and rule 6 has nothing to shorten a cell anchored in it against");
    /* §17.2's display order keeps each group's rows together, checked here: a group in two runs would mis-clamp
       a `rowspan` and report one piece as rule 2's whole extent. NULL-element runs are exempt: two separated
       stretches of table-parented rows are two implicit groups (HTML §4.9.12.1 Forming a table). */
    for (g = 0; g < ngroups; g++) {
        size_t h;

        if (groups[g].element == NULL) continue;
        for (h = g + 1; h < ngroups; h++)
            DCHECK(groups[h].element != groups[g].element,
                   "CSS 2.1 §17.5's rule 2 placed ONE row group box at TWO separated runs of grid rows. Rule 2 "
                   "gives it \"the same grid cells as the rows it contains\", and CSS 2.1 §17.2 The CSS table "
                   "model's display order keeps those rows together — a header group's before every other row "
                   "and a footer group's after every other row — so this is core/layout/table_box.h's row "
                   "order having interleaved two groups, and neither the clamp above nor the extent "
                   "core/layout/table_height.h takes is answering about the whole group any more");
    }
}

/* The scan asserts one match and so runs to the end: §17.5 places each cell box once ("Each cell is thus a
   rectangular box, one or more grid cells wide and high"), and a second entry would disagree about its columns
   while a first-hit answer hid it. */
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

/* The same one-match invariant for rows, from rule 1: "Each row box occupies one row of grid cells". */
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

/* See table_grid.h for the single cause of NULL and why `group` is not `const`. One match, per rule 2: a group
   occupies one run, and a first-hit answer would size the box from one piece. */
const TableGridRowGroup *table_grid_row_group_of(const TableGrid *grid, lxb_dom_element_t *group)
{
    const TableGridRowGroup *found = NULL;
    size_t i;

    DCHECK(grid != NULL, "CSS 2.1 §17.5's grid was asked which grid rows a row group occupies through no grid");
    DCHECK(grid->groups != NULL || grid->ngroups == 0,
           "CSS 2.1 §17.5's grid was asked for a row group while holding no run array and a non-zero run count "
           "— `table_grid_build` stores NULL only for a table with no rows at all, so the two have been carried "
           "apart since it answered");
    DCHECK(group != NULL,
           "CSS 2.1 §17.5's rule 2 was asked which grid rows a NULL row group occupies. The NULL-element run is "
           "the stretch whose rows' parent is the TABLE BOX itself and it names no box — CSS 2.1 §17.2.1 "
           "Anonymous table objects generates no anonymous row group — so a NULL would match the first such "
           "stretch and answer a question about a box that does not exist");
    DCHECK(table_box_table_of(group) == grid->table,
           "CSS 2.1 §17.5's rule 2 was asked about a ROW GROUP box that is not in this grid's table. This entry "
           "is the one place that distinction has to be made rather than handed to the consumer: an EMPTY "
           "`<tbody>` is a real row group box of this table holding no row, so the NULL this entry answers with "
           "is rule 2's union over zero rows and MUST NOT also mean a caller that walked to the wrong table box "
           "— a consumer cannot tell those apart and would report a real extent for another table's group. "
           "core/layout/table_box.h's `table_box_table_of` climbs §17.2's own box nesting, so a mismatch here "
           "is that walk and this grid having been built from two different table boxes");
    for (i = 0; i < grid->ngroups; i++) {
        if (grid->groups[i].element != group) continue;
        DCHECK(found == NULL,
               "CSS 2.1 §17.5 Visual layout of table contents placed ONE row group box at TWO runs of grid "
               "rows. Rule 2 gives it \"the same grid cells as the rows it contains\" — one run — so this is "
               "core/layout/table_box.h's row order having interleaved two groups, and either answer names "
               "only part of the box");
        found = &grid->groups[i];
    }
    return found;
}

/* See table_grid.h for why an empty row group is answered by re-running §17.2's box generation and why the line
   is a fact while the distance is chosen at placement. */
size_t table_grid_empty_row_group_line(const TableGrid *grid, lxb_dom_element_t *group)
{
    TableBoxRowGroup *groups = NULL;
    size_t ngroups, i, line = 0;
    bool found = false;
    char nbuf[160];

    DCHECK(grid != NULL,
           "CSS 2.1 §17.5's rule 2 was asked which grid line an empty row group stands on through no grid");
    DCHECK(group != NULL,
           "CSS 2.1 §17.5's rule 2 was asked which grid line a NULL row group stands on. The NULL-element run "
           "is the stretch whose rows' parent is the TABLE BOX itself and it names no box — §17.2.1 Anonymous "
           "table objects generates no anonymous row group — so there is no box here to place");
    DCHECK(table_box_table_of(group) == grid->table,
           "CSS 2.1 §17.5's rule 2 was asked about a ROW GROUP box that is not in this grid's table. The walk "
           "below re-runs §17.2's box generation over THIS grid's table box, so a group of another table would "
           "be absent from it and the crash a reader met would name the wrong absence");
    DCHECK(table_grid_row_group_of(grid, group) == NULL,
           "CSS 2.1 §17.5's rule 2 was asked which grid LINE a row group with ROWS stands on. A group holding "
           "rows occupies grid CELLS — \"the same grid cells as the rows it contains\" — so its first grid row "
           "is `first` of the run `table_grid_row_group_of` answers with, and this entry's line is the answer "
           "for the one group that has no such row. Asking here would place a real group at the line above its "
           "own first row's top edge, which is one `border-spacing` out under §17.6.1 The separated borders "
           "model and exactly right under §17.6.2 The collapsing border model — so a document would have to "
           "declare one to show it");
    ngroups = table_box_row_groups(grid->table, &groups);
    for (i = 0; i < ngroups; i++) {
        if (groups[i].element != group) continue;
        DCHECK(!found,
               "CSS 2.1 §17.2 The CSS table model's display order reported ONE row group box TWICE. That walk "
               "visits each of the table box's children once and moves at most one header and one footer, so a "
               "second visit is the box generation having placed one element in two positions of the order");
        DCHECK(groups[i].nrows == 0,
               "CSS 2.1 §17.2's box generation reported ROWS for a row group core/layout/table_grid.h has no "
               "run for. This grid's runs are built from the same walk's row array, so a group with rows there "
               "and no run here is those two readings of one box generation having come apart");
        line = groups[i].first;
        found = true;
    }
    free(groups);
    DCHECKF(found,
            "%s: CSS 2.1 §17.2 The CSS table model's display order does not contain this ROW GROUP box, though "
            "core/layout/table_box.h's own upward walk puts it in this grid's table. The two readings of "
            "§17.2.1 Anonymous table objects' box generation have come apart — `table_box_table_of` climbed "
            "from the group to the table and `table_box_row_groups` descended from the table and did not reach "
            "it — and there is no line to place it on",
            box_subject(group, nbuf, sizeof nbuf));
    DCHECK(line <= grid->nrows,
           "CSS 2.1 §17.5's rule 2 put an empty row group past the last GRID LINE. There are `nrows + 1` lines "
           "and §17.2's display order counts the rows before the group, so a count above the total is that "
           "order having emitted rows this grid did not store");
    return line;
}

/* See table_grid.h. The partition is asserted where built, so this walk cannot fail for `row < nrows`; a
   larger `row` is the caller's error. */
const TableGridRowGroup *table_grid_group_of_row(const TableGrid *grid, size_t row)
{
    size_t i;

    DCHECK(grid != NULL, "CSS 2.1 §17.5's grid was asked which row group a grid row is in through no grid");
    DCHECK(row < grid->nrows,
           "CSS 2.1 §17.5's rule 2 was asked which row group a grid row is in for a row past the last one rule "
           "1 placed — \"the table occupies exactly as many grid rows as there are row elements\" — so a row "
           "at or past that count names no row box and there is no run to answer with");
    for (i = 0; i < grid->ngroups; i++)
        if (row >= grid->groups[i].first && row < grid->groups[i].first + grid->groups[i].nrows)
            return &grid->groups[i];
    DFAIL("CSS 2.1 §17.5's rule 2's runs do not cover a grid row rule 1 placed. `table_grid_build` asserts the "
          "partition where it builds the runs — every run begins where the previous one ends and the last ends "
          "at the row count — so a row inside the count and outside every run is that array having been "
          "replaced since it answered");
    return NULL;
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
    DCHECK(grid->groups != NULL || grid->ngroups == 0,
           "a table grid was released holding no row-group run array with a non-zero run count — "
           "`table_grid_build` stores NULL only for a table with no rows at all, so the two have been carried "
           "apart since it answered");
    free(grid->cells);
    free(grid->rows);
    free(grid->groups);
    grid->table = NULL;
    grid->cells = NULL;
    grid->ncells = 0;
    grid->rows = NULL;
    grid->nrows = 0;
    grid->groups = NULL;
    grid->ngroups = 0;
    grid->ncols = 0;
    grid->any_overlap = false;
}
