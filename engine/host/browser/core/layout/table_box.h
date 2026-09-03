/* CSS 2.1 §17.2 The CSS table model and §17.2.1 Anonymous table objects — A TABLE BOX'S ROWS AND CELLS, which
 * is the OPERAND every table algorithm in this directory is blocked on and which several separate crashes each
 * named, in their own words, as the first missing thing.
 *
 * WHY THIS IS ONE COMPONENT. §17.2.1 is not a set of tidy-up rules that each consumer can apply for itself: it
 * is one algorithm, in three stages, stated over a VOCABULARY the same section defines once ("row group box",
 * "proper table child", "proper table row parent", "internal table box", "tabular container", "consecutive").
 * A walk that asked "is this child a row?" per call site would be that vocabulary written down once per site,
 * free to disagree about whether the white space between two `<tr>`s is a box — which is precisely the question
 * §17.2.1's first stage exists to settle. So the vocabulary is exported as predicates over ONE classification
 * of a computed `display`, and the generation is exported as ONE answer: the rows of a table box, each with its
 * cells, in the order §17.2 says they are displayed in.
 *
 * WHAT A ROW AND A CELL ARE HERE, AND WHY THEY ARE NOT POINTERS TO BOXES. core/layout/block_flow.h states the
 * rule this component obeys: NOTHING IS STORED, because a layout is per-flow state and a cached box tree is
 * shared state solver/dom_cow.h's delta does not swap. So an ANONYMOUS box — the ones §17.2.1's second stage
 * generates around content the author did not wrap — is a RANGE OF DOM SIBLINGS exactly as `BlockFlowAnonBox`
 * is, and a box an element generates is that element. `element == NULL` is the whole difference between the
 * two, and it is a positive statement rather than an absence: the section GENERATES that box, and no element
 * in the tree names it.
 *
 * THE RANGE CONVENTION IS ONE RULE FOR BOTH KINDS. `[first, end)` is a half-open run over the box's DOM
 * parent's child list; `end` is the node of the NEXT SURVIVING SIBLING BOX, or NULL where the box is the last
 * one. A node inside the range that generates no box (a comment, a `display: none` element, a run of white
 * space §9.2.2.1 collapses away, a box §17.2.1's first stage removed) is not content of the box — it is
 * skipped by the same classification that built the range, so a consumer re-walking the range reaches exactly
 * the nodes this component saw. Writing `end` as `first->next` for an element instead would be a SECOND
 * convention whose only visible difference is trailing white space, which is the shape of a disagreement
 * nobody notices until it moves a cell.
 *
 * §17.2's DISPLAY ORDER IS PART OF THE ANSWER AND NOT A RENDERING DETAIL. §17.5's first rule says the row boxes
 * "fill the table from top to bottom in the order they occur in the source document", and §17.2 states two
 * exceptions to that order in the definitions of the box types themselves: a 'table-header-group' "is always
 * displayed before all other rows and row groups and after any top captions" and a 'table-footer-group' "is
 * always displayed after all other rows and row groups and before any bottom captions", with "If a table
 * contains multiple elements with 'display: table-header-group', only the first is rendered as a header; the
 * others are treated as if they had 'display: table-row-group'" (and the same sentence for the footer). A row's
 * index in the answer IS its grid row under §17.5's rule 1, so an enumeration in document order would hand
 * §17.5.3 the wrong rows to accumulate heights over and nothing downstream could tell.
 *
 * WHAT IS DELIBERATELY NOT HERE, EACH NAMED WITH ITS OWN SUBJECT.
 *   - §17.2.1's THIRD STAGE, "Generate missing parents", is not this entry's and must not be folded into it.
 *     Its subject is a child list that is NOT a table's — a `table-row` or a `table-cell` sitting in a `<div>`,
 *     which the section wraps in an anonymous 'table' or 'inline-table' box. So its consumer is the BLOCK
 *     walk's own child list (core/layout/block_flow.c), where that wrapping changes what §9.4.1 stacks, and
 *     this entry is asked of a box that IS a table. Building it here would be a table box generating itself.
 *   - §17.5's GRID — which column each cell occupies, and how many rows and columns it spans — is a different
 *     algorithm over this answer and not a refinement of it, and it is core/layout/table_grid.h's. §17.5's own
 *     rule 5 says why it cannot be here: "Although CSS 2.1 does not define how the number of spanned rows or
 *     columns is determined, a user agent may have special knowledge about the source document", so the grid
 *     reads HTML §4.9.11's `rowspan` and `colspan` while this component reads only `display`. §17.5.3 needs
 *     the grid only for its own row-spanning sentence; §17.5.2's column widths need all of it.
 *   - §17.4's TABLE WRAPPER BOX is the box §9.4.1 actually stacks ("the table generates a principal block box
 *     called the table wrapper box that contains the table box itself and any caption boxes"), and it is a
 *     block container that no element names. `table_box_captions` below answers the half of it this component
 *     owns — which boxes are in it besides the table box — and the wrapper itself is core/layout's next box to
 *     build, because §10.1's containing-block walk and §9.4.1's stack both stop at a table for want of it.
 *   - §17.5.2's and §17.5.3's ALGORITHMS are what this operand is for. They are not here because they are
 *     sizes, and this component decides no size at all.
 *
 * A CALLER ASKS ABOUT A TABLE BOX AND CRASHES OTHERWISE. Every entry below that takes an element takes one
 * whose computed `display` generates a table box; a caller has classified it first (`table_box_kind`), and one
 * that has not gets an abort naming the classification rather than an empty answer that would read as "this
 * table has no rows". */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_TABLE_BOX_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_TABLE_BOX_H

#include <stdbool.h>
#include <stddef.h>

#include <lexbor/dom/dom.h>

/* §17.2's NINE BOX TYPES, plus the one answer that is not a table box at all. The section lists them by the
   `display` value that assigns each one's table formatting rules, so this enum is that list and nothing else —
   `TABLE_BOX_NOT_A_TABLE_BOX` is every other computed value, including the anonymous inline box a run of text
   generates, which §17.2.1's first stage is stated about by name. */
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

/* The computed `display` read as one of §17.2's box types. `display` must not be NULL. */
TableBoxKind table_box_kind(const char *display);

/* §17.2.1's OWN DEFINED TERMS, one predicate each, because the section's rules are written in them and a rule
   spelled out of raw keyword comparisons is a rule a reader cannot check against the text. */

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

/* §17.2's two values that generate a TABLE box — 'table' and 'inline-table'. They differ only in the outer
   display type §17.4 gives the wrapper box around them ("a table can behave like a block-level (for
   'display: table') or inline-level (for 'display: inline-table') element"), and nothing inside §17.2.1 or
   §17.5 tells them apart, which is why one predicate serves every rule stated over "a 'table' or
   'inline-table' box". */
bool table_box_kind_generates_table_box(TableBoxKind kind);

/* ONE CELL BOX of one row. See the header's range convention; `element` is NULL for the anonymous 'table-cell'
   box §17.2.1's second stage generates around content a row did not wrap. */
typedef struct {
    lxb_dom_element_t *element;
    lxb_dom_node_t *first;
    lxb_dom_node_t *end;
} TableBoxCell;

/* ONE ROW BOX, with its cells in the order they occur in its child list — which is NOT their column order:
   §17.5's rule 5 places a cell in the leftmost grid column that its earlier siblings and any row-spanning cell
   from a prior row leave free, and that assignment is core/layout/table_grid.h's. `group` is the
   row group box the row is inside, or NULL where the row's parent is the table box itself; §17.2.1 generates
   no anonymous row group, so a NULL there is the tree's own shape and never a missing box. */
typedef struct {
    lxb_dom_element_t *element;
    lxb_dom_element_t *group;
    lxb_dom_node_t *first;
    lxb_dom_node_t *end;
    TableBoxCell *cells;
    size_t ncells;
} TableBoxRow;

/* §17.2.1's FIRST TWO STAGES over `table`'s subtree: the table box's rows, in §17.2's display order, each
   carrying the cells that stage two's third rule leaves it with. Answers the count and stores a newly
   allocated array of that many at `*out`, WHICH THE CALLER OWNS AND RELEASES WITH `table_box_rows_free`; a
   count of zero stores NULL.
   ZERO IS A REAL TABLE WITH NO ROWS — `<table></table>`, or a table whose only children are a caption and a
   column group — and §17.5.3's height over it is the sum of no row heights plus the table's own borders and
   cell spacing, which is a number and not a gap. */
size_t table_box_rows(lxb_dom_element_t *table, TableBoxRow **out);

/* Releases what `table_box_rows` stored, including each row's own cell array. A NULL array with a count of
   zero is the answer that entry gives for a table with no rows and is accepted here. */
void table_box_rows_free(TableBoxRow *rows, size_t nrows);

/* THE TABLE BOX an INTERNAL TABLE BOX is inside — §17.2.1 Anonymous table objects' box generation read UPWARD,
   which is the direction every consumer holding one internal box has to read it and the opposite of the two
   entries above. `internal` must be one of §17.2's internal table boxes (`table_box_kind_is_internal`); a
   'table-caption' is NOT one and must not be asked here, because §17.4 Tables in the visual formatting model
   puts a caption in the WRAPPER beside the table box rather than in it, so "which table box is it inside" has
   no answer for a caption and a table returned for one would be the wrong box.
   THE ANSWER IS AN ELEMENT AND NEVER NULL: every case where the walk cannot name one CRASHES here rather than
   handing a consumer an absence to interpret, because §17.2.1's box generation guarantees a table box exists
   over every internal box — what it does not guarantee is that an ELEMENT generates it. A `display:
   table-cell` box whose ancestors are ordinary block boxes really is inside a table box; that box is the
   ANONYMOUS one §17.2.1's third stage generates, no element names it, and answering with the nearest table
   element up the tree would name a table this cell is not in.
   IT IS EXPORTED RATHER THAN WRITTEN AT ITS CONSUMER because the walk encodes WHICH BOXES MAY STAND BETWEEN a
   cell and its table — a row, a row group, and nothing else — and that is §17.2's own box nesting, so a second
   copy at a second consumer would be free to disagree about which table an anonymous-table cell belongs to
   while both looked locally right. */
lxb_dom_element_t *table_box_table_of(lxb_dom_element_t *internal);

/* §17.4's CAPTION BOXES of `table`, in document order — "The caption boxes are block-level boxes that retain
   their own content, padding, margin, and border areas, and are rendered as normal block boxes inside the
   table wrapper box." They are not rows and are not in the table box at all, which is why they are a second
   entry rather than a field of one: a consumer computing §17.5.3's height must not see them, and a consumer
   building §17.4's wrapper must.
   Answers the count and stores a newly allocated array of that many ELEMENTS at `*out`, which the caller owns
   and frees; a count of zero stores NULL. Where the caption boxes sit relative to the table box is
   `caption-side`'s answer (§17.4.1 Caption position and alignment) and is not decided here. */
size_t table_box_captions(lxb_dom_element_t *table, lxb_dom_element_t ***out);

#endif
