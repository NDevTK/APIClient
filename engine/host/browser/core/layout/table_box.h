/* CSS 2.1 §17.2 The CSS table model and §17.2.1 Anonymous table objects: a table box's rows and cells, the
 * operand of every table algorithm in this directory. §17.2.1 is one three-stage algorithm over a vocabulary
 * the section defines once ("row group box", "proper table child", "proper table row parent", "internal table
 * box", "tabular container", "consecutive"); it is exported as predicates over one classification of `display`
 * and one generation, so no consumer re-decides whether white space between two `<tr>`s is a box.
 *
 * Nothing is stored (layout is per-flow state solver/dom_cow.h does not swap), so an anonymous box is a range
 * of siblings, like `BlockFlowAnonBox`, and an element's box is the element; `element == NULL` positively says
 * the section generated a box no element names. `[first, end)` is a half-open run over core/layout/box_tree.h's
 * child sequence for the box's container (css-display-3 §2.5 "Box Generation: the none and contents keywords"'
 * splice: "anonymous box generation rules will ignore the elided elements entirely, as if they did not exist in
 * the box tree"), not over a DOM child list. `end` is the next surviving sibling box's node, or NULL. Nodes in
 * the range that generate no box are skipped by the same classification, so a re-walk with
 * `box_tree_next_sibling` reaches exactly these nodes. Every entry taking an element requires a table box
 * (`table_box_kind`) and crashes rather than answering "no rows". */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_TABLE_BOX_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_TABLE_BOX_H

/* Named residual: a range states its endpoints but not the container whose sequence they index, which
   `box_tree_next_sibling` needs, so no consumer can step one (for a cell it is the row; for a row, the row group
   or table, as `TableBoxRow.group` distinguishes). Next: the container beside the range, landed with the first
   consumer that walks one. Absence would show as that consumer re-deriving it as `first->parent`, which is the
   `contents` element rather than the box.
   §17.2's display order is part of the answer: rows "fill the table from top to bottom in the order they occur
   in the source document", except that a 'table-header-group' "is always displayed before all other rows and
   row groups and after any top captions", a 'table-footer-group' likewise after, and "If a table contains
   multiple elements with 'display: table-header-group', only the first is rendered as a header; the others are
   treated as if they had 'display: table-row-group'". A row's index is its grid row under §17.5's rule 1.
   Not here: §17.2.1's third stage, "Generate missing parents" (its subject is a non-table child list, so it is
   core/layout/block_flow.c's); §17.5's grid (core/layout/table_grid.h, which reads HTML's spans, per rule 5's
   "a user agent may have special knowledge about the source document"); the table wrapper's geometry
   (core/layout/used_value.h, core/layout/block_flow.h; `table_box_captions` lists its captions); and sizes. */

#include <stdbool.h>
#include <stddef.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_display.h"

/* §17.2's box types, keyed by the `display` value that assigns each one's table formatting rules, plus
   `TABLE_BOX_NOT_A_TABLE_BOX` for every other value (including the anonymous inline box of a text run, which
   §17.2.1's first stage names). */
typedef enum {
    TABLE_BOX_NOT_A_TABLE_BOX = 0,
    TABLE_BOX_TABLE,           /* 'table'              — a block-level table */
    TABLE_BOX_INLINE_TABLE,    /* 'inline-table'       — an inline-level table */
    TABLE_BOX_ROW_GROUP,       /* 'table-row-group'    (HTML: TBODY) */
    TABLE_BOX_HEADER_GROUP,    /* 'table-header-group' (HTML: THEAD) */
    TABLE_BOX_FOOTER_GROUP,    /* 'table-footer-group' (HTML: TFOOT) */
    TABLE_BOX_ROW,             /* 'table-row'          (HTML: TR) */
    TABLE_BOX_CELL,            /* 'table-cell'         (HTML: TD, TH) */
    TABLE_BOX_COLUMN,          /* 'table-column'       (HTML: COL) */
    TABLE_BOX_COLUMN_GROUP,    /* 'table-column-group' (HTML: COLGROUP) */
    TABLE_BOX_CAPTION          /* 'table-caption'      (HTML: CAPTION) */
} TableBoxKind;

/* The computed `display` read as one of §17.2's box types. */
TableBoxKind table_box_kind(CssDisplay display);

/* §17.2.1's defined terms, one predicate each, so its rules can be checked against the text. */

/* "row group box: A 'table-row-group', 'table-header-group', or 'table-footer-group'". */
bool table_box_kind_is_row_group(TableBoxKind kind);

/* "proper table child: A 'table-row' box, row group box, 'table-column' box, 'table-column-group' box, or
   'table-caption' box." */
bool table_box_kind_is_proper_table_child(TableBoxKind kind);

/* "proper table row parent: A 'table' or 'inline-table' box or row group box". */
bool table_box_kind_is_proper_table_row_parent(TableBoxKind kind);

/* "internal table box: A 'table-cell' box, 'table-row' box, row group box, 'table-column' box, or
   'table-column-group' box." */
bool table_box_kind_is_internal(TableBoxKind kind);

/* "tabular container: A 'table-row' box or proper table row parent". */
bool table_box_kind_is_tabular_container(TableBoxKind kind);

/* §17.2's two values that generate a table box, 'table' and 'inline-table'. They differ only in the wrapper's
   outer display type (§17.4: "a table can behave like a block-level (for 'display: table') or inline-level (for
   'display: inline-table') element"), so one predicate serves every rule over "a 'table' or 'inline-table'
   box". */
bool table_box_kind_generates_table_box(TableBoxKind kind);

/* One cell box of one row, in the range convention above; `element` is NULL for the anonymous 'table-cell'
   box §17.2.1's second stage generates around content a row did not wrap. */
typedef struct {
    lxb_dom_element_t *element;
    lxb_dom_node_t *first;
    lxb_dom_node_t *end;
} TableBoxCell;

/* One row box, with its cells in child-list order, which is not column order (§17.5's rule 5 placement is
   core/layout/table_grid.h's). `group` is the row group box containing the row, or NULL when the row's parent
   is the table box; §17.2.1 generates no anonymous row group, so NULL is the tree's own shape. */
typedef struct {
    lxb_dom_element_t *element;
    lxb_dom_element_t *group;
    lxb_dom_node_t *first;
    lxb_dom_node_t *end;
    TableBoxCell *cells;
    size_t ncells;
} TableBoxRow;

/* §17.2.1's first two stages over `table`'s subtree: the rows in §17.2's display order, each with the cells
   stage two's third rule leaves it. Answers the count and stores a newly allocated array at `*out`, which the
   caller owns and releases with `table_box_rows_free`; zero stores NULL. Zero rows is a real table
   (`<table></table>`). */
size_t table_box_rows(lxb_dom_element_t *table, TableBoxRow **out);

/* Releases what `table_box_rows` stored, including each row's own cell array. A NULL array with a count of
   zero is the answer that entry gives for a table with no rows and is accepted here. */
void table_box_rows_free(TableBoxRow *rows, size_t nrows);

/* One row group box with the grid rows it contributes: CSS 2.1 §17.5 Visual layout of table contents' rule 2
   ("A row group occupies the same grid cells as the rows it contains") read from the box's side, where an
   empty union is visible. `first` is an index into `table_box_rows`' answer and so a grid row (rule 1: "the
   table occupies exactly as many grid rows as there are row elements"). `nrows` is zero for an empty group
   (`<tbody></tbody>`), which no row array shows; its `first` is the count of rows before it, possibly the
   table's row count (the last of `nrows + 1` grid lines). */
typedef struct {
    lxb_dom_element_t *element;
    size_t first;
    size_t nrows;
} TableBoxRowGroup;

/* §17.2 The CSS table model's row group boxes of `table`, in the display order `table_box_rows` uses: one
   walk produces both, so groups and their rows cannot be ordered differently. Answers the count and stores a
   newly allocated array at `*out`, which the caller frees with `free` (entries own nothing); zero stores NULL.
   A row parented by the table box is in no group and appears nowhere here, so summing `nrows` does not give
   the table's row count. */
size_t table_box_row_groups(lxb_dom_element_t *table, TableBoxRowGroup **out);

/* The table box an internal table box is inside: §17.2.1's box generation read upward. `internal` must satisfy
   `table_box_kind_is_internal`; a 'table-caption' is not one, since §17.4 puts it in the wrapper beside the table
   box. Never NULL: where no element generates the table box (a `display: table-cell` under ordinary blocks is
   inside §17.2.1's anonymous table box), it crashes rather than naming the nearest `<table>` further up, which
   is a different table. Exported so the permitted nesting (a row, a row group, nothing else) is stated once. */
lxb_dom_element_t *table_box_table_of(lxb_dom_element_t *internal);

/* §17.4's caption boxes of `table`, in document order: "The caption boxes are block-level boxes that retain
   their own content, padding, margin, and border areas, and are rendered as normal block boxes inside the table
   wrapper box." A separate entry because §17.5.3's height must not see them and §17.4's wrapper must. Answers
   the count and stores a newly allocated array of elements at `*out`, which the caller frees; zero stores NULL.
   Placement relative to the table box is `caption-side`'s (§17.4.1 Caption position and alignment). */
size_t table_box_captions(lxb_dom_element_t *table, lxb_dom_element_t ***out);

#endif
