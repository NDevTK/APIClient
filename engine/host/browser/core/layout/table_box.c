/* CSS 2.1 §17.2 The CSS table model and §17.2.1 Anonymous table objects: a table box's rows and cells. See
   table_box.h. */
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/layout/block_flow.h"
#include "core/layout/box_subject.h"
#include "core/layout/box_tree.h"
#include "core/layout/table_box.h"

static char *tb_computed(lxb_dom_element_t *el, const char *name)
{
    char *v = css_computed_value(el, name);

    DCHECK(v != NULL, "the cascade produced no computed value for a property this engine models — every one of "
                      "them is in lexbor's registry with an initial value, so the last layer always answers");
    return v;
}

/* One growth step for the three arrays this file builds. Allocation failure is a `CHECK`: a dropped row would
   be wrong geometry in release (a data-integrity failure), not an absent one. */
static void *tb_reserve(void *v, size_t n, size_t *cap, size_t esz)
{
    void *grown;

    if (n < *cap) return v;
    *cap = (*cap == 0) ? 8 : *cap * 2;
    grown = realloc(v, *cap * esz);
    CHECK(grown != NULL,
          "CSS 2.1 §17.2.1's box generation could not allocate one table box's rows or cells — the array holds "
          "one entry per row and per cell of the table being laid out");
    return grown;
}

/* ---- §17.2's box types ------------------------------------------------------------------------------------ */

TableBoxKind table_box_kind(const char *display)
{
    static const struct { const char *value; TableBoxKind kind; } MAP[] = {
        { "table", TABLE_BOX_TABLE },
        { "inline-table", TABLE_BOX_INLINE_TABLE },
        { "table-row-group", TABLE_BOX_ROW_GROUP },
        { "table-header-group", TABLE_BOX_HEADER_GROUP },
        { "table-footer-group", TABLE_BOX_FOOTER_GROUP },
        { "table-row", TABLE_BOX_ROW },
        { "table-cell", TABLE_BOX_CELL },
        { "table-column", TABLE_BOX_COLUMN },
        { "table-column-group", TABLE_BOX_COLUMN_GROUP },
        { "table-caption", TABLE_BOX_CAPTION },
    };
    size_t i;

    DCHECK(display != NULL,
           "CSS 2.1 §17.2's box-type question was asked about a NULL computed `display`. The section assigns "
           "table formatting rules by that value and by nothing else, so there is no box to classify without "
           "one");
    for (i = 0; i < COUNTOF(MAP); i++)
        if (strcmp(display, MAP[i].value) == 0) return MAP[i].kind;
    return TABLE_BOX_NOT_A_TABLE_BOX;
}

bool table_box_kind_is_row_group(TableBoxKind kind)
{
    return kind == TABLE_BOX_ROW_GROUP || kind == TABLE_BOX_HEADER_GROUP || kind == TABLE_BOX_FOOTER_GROUP;
}

bool table_box_kind_is_proper_table_child(TableBoxKind kind)
{
    return kind == TABLE_BOX_ROW || table_box_kind_is_row_group(kind) || kind == TABLE_BOX_COLUMN ||
           kind == TABLE_BOX_COLUMN_GROUP || kind == TABLE_BOX_CAPTION;
}

bool table_box_kind_generates_table_box(TableBoxKind kind)
{
    return kind == TABLE_BOX_TABLE || kind == TABLE_BOX_INLINE_TABLE;
}

bool table_box_kind_is_proper_table_row_parent(TableBoxKind kind)
{
    return table_box_kind_generates_table_box(kind) || table_box_kind_is_row_group(kind);
}

bool table_box_kind_is_internal(TableBoxKind kind)
{
    return kind == TABLE_BOX_CELL || kind == TABLE_BOX_ROW || table_box_kind_is_row_group(kind) ||
           kind == TABLE_BOX_COLUMN || kind == TABLE_BOX_COLUMN_GROUP;
}

bool table_box_kind_is_tabular_container(TableBoxKind kind)
{
    return kind == TABLE_BOX_ROW || table_box_kind_is_proper_table_row_parent(kind);
}

/* §17.2.1's first stage names its subjects as "either 'table-caption' or internal table boxes", twice. */
static bool tb_caption_or_internal(TableBoxKind kind)
{
    return kind == TABLE_BOX_CAPTION || table_box_kind_is_internal(kind);
}

/* §17.2.1: "A box D is a proper table descendant of A if D can be a descendant of A without causing the
   generation of any intervening 'table' or 'inline-table' boxes." From the parentage rules: captions, column
   groups, columns and row groups under a table box; rows under a table box or row group; cells under a row.
   Anything else (a caption under a row group) needs §17.2.1's third stage's anonymous table. */
static bool tb_proper_table_descendant(TableBoxKind ancestor, TableBoxKind d)
{
    switch (ancestor) {
    case TABLE_BOX_TABLE:
    case TABLE_BOX_INLINE_TABLE:
        return table_box_kind_is_proper_table_child(d) || d == TABLE_BOX_CELL;
    case TABLE_BOX_ROW_GROUP:
    case TABLE_BOX_HEADER_GROUP:
    case TABLE_BOX_FOOTER_GROUP:
        return d == TABLE_BOX_ROW || d == TABLE_BOX_CELL;
    case TABLE_BOX_ROW:
        return d == TABLE_BOX_CELL;
    default:
        return false;
    }
}

/* ---- One parent's child boxes ------------------------------------------------------------------------------ */

typedef struct {
    lxb_dom_node_t *node;
    TableBoxKind kind;
    /* "an anonymous inline box that contains only white space" is two facts: the box exists (CSS 2.2
       §9.2.2.1 Anonymous inline boxes, core/layout/block_flow.h) and its data is all white space. A `pre`
       run can be both; a `normal` one generates no box and never reaches this array. */
    bool anon_inline_ws;
} TbChild;

/* The data half: block_flow.c's §9.2.2.1 predicate's four characters, asked of the character data alone. That
   predicate also reads the inherited `white-space` of the run's own DOM parent, which after css-display-3
   §2.5's splice need not be the container walked here. */
static bool tb_text_is_all_whitespace(const lxb_dom_node_t *n)
{
    const lxb_dom_character_data_t *cd = lxb_dom_interface_character_data((lxb_dom_node_t *) n);
    const lxb_char_t *d = cd->data.data;
    size_t len = cd->data.length, i;

    if (d == NULL) return true;
    for (i = 0; i < len; i++) {
        char c = (char) d[i];

        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return false;
    }
    return true;
}

/* `parent`'s child boxes, in tree order, each classified by its computed `display`. Answers the count and
   stores a newly allocated array at `*out`, which the caller frees. A child generating no box is absent (CSS 2
   §9.2): a `display: none` element, a comment, a processing instruction, a doctype, collapsed white space.
   Children come from core/layout/box_tree.h, per css-display-3 §2.5 "Box Generation: the none and contents
   keywords" ("the element must be treated as if it had been replaced in the element tree by its contents";
   its Note names anonymous box generation), so a `display: contents` row's cells are children of the table
   box and "consecutive siblings" means consecutive in that sequence. The array exists because every §17.2.1
   rule reads a child's neighbours; callers index it and never step siblings. `none` elements are still yielded
   by box_tree.h (it pierces only `contents`) and skipped below. */
static size_t tb_children(lxb_dom_element_t *parent, TbChild **out)
{
    BoxTreeChild m;
    TbChild *v = NULL;
    size_t cnt = 0, cap = 0;
    char nbuf[160], pbuf[160];

    *out = NULL;
    for (m = box_tree_first_child(parent); box_tree_child_exists(m); m = box_tree_next_sibling(parent, m)) {
        /* Every member is a node: a table, row group or row is not a list item (css-display-3 §2.3
           "Generating Marker Boxes: the list-item keyword" pairs `list-item` with `flow` or `flow-root`). */
        lxb_dom_node_t *n = box_tree_child_node(m);
        TableBoxKind kind = TABLE_BOX_NOT_A_TABLE_BOX;
        bool ws = false;

        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            lxb_dom_element_t *el = lxb_dom_interface_element(n);
            char *d = tb_computed(el, "display");
            bool none = strcmp(d, "none") == 0;

            kind = table_box_kind(d);
            free(d);
            if (none) continue;
            /* No `contents` arm: box_tree.h replaces such an element by its contents (css-display-3 §2.5) and
               asserts it never yields one. The `none` arm above is CSS 2 §9.2's. */
        } else if (n->type == LXB_DOM_NODE_TYPE_TEXT) {
            /* §9.2.2.1's white-space rule reads an inherited property from the run's own DOM parent, not from
               `parent` (css-display-3 §2.5's Note keeps inheritance on the document tree); the predicate
               derives it and takes no element (see core/layout/block_flow.h). */
            if (!block_flow_text_child_generates_box(n)) continue;
            ws = tb_text_is_all_whitespace(n);
        } else if (n->type == LXB_DOM_NODE_TYPE_COMMENT ||
                   n->type == LXB_DOM_NODE_TYPE_PROCESSING_INSTRUCTION ||
                   n->type == LXB_DOM_NODE_TYPE_DOCUMENT_TYPE) {
            /* CSS 2 §9.2 generates boxes for elements and text only. */
            continue;
        } else {
            DFAILF("%s, a child of the table box %s: "
                   "a node type CSS 2 §9.2 Controlling box generation does not describe is a child of a box "
                   "CSS 2.1 §17.2.1 Anonymous table objects is classifying. The tree this walk iterates holds "
                   "elements, text, comments, processing instructions and a doctype; a CDATA section, a "
                   "document or a document fragment is not a child any parser this engine runs produces there. "
                   "Find the writer that inserted it",
                   box_subject_node(n, nbuf, sizeof nbuf), box_subject(parent, pbuf, sizeof pbuf));
            continue;
        }
        v = tb_reserve(v, cnt, &cap, sizeof *v);
        v[cnt].node = n;
        v[cnt].kind = kind;
        v[cnt].anon_inline_ws = ws;
        cnt++;
    }
    *out = v;
    return cnt;
}

/* ---- §17.2.1's first stage, "Remove irrelevant boxes" ------------------------------------------------------- */

/* The four rules over the child boxes of a parent of type `pk`, in place; answers how many survive. Every drop
   is decided against the pre-removal neighbours, since the section states one stage, not a fixpoint, and the
   scan order must not decide the answer. */
static size_t tb_remove_irrelevant(TableBoxKind pk, TbChild *v, size_t n)
{
    size_t i, kept = 0;
    bool *drop;

    if (n == 0) return 0;
    /* Rule 1: "All child boxes of a 'table-column' parent are treated as if they had 'display: none'." */
    if (pk == TABLE_BOX_COLUMN) return 0;
    drop = (bool *) calloc(n, sizeof *drop);
    CHECK(drop != NULL,
          "CSS 2.1 §17.2.1's first stage could not allocate one flag per child box of the box it is removing "
          "irrelevant boxes from");
    for (i = 0; i < n; i++) {
        bool have_prev = i > 0, have_next = i + 1 < n;
        TableBoxKind prev = have_prev ? v[i - 1].kind : TABLE_BOX_NOT_A_TABLE_BOX;
        TableBoxKind next = have_next ? v[i + 1].kind : TABLE_BOX_NOT_A_TABLE_BOX;
        bool rule3, rule4;

        /* Rule 2: "If a child C of a 'table-column-group' parent is not a 'table-column' box, then it is
           treated as if it had 'display: none'." */
        if (pk == TABLE_BOX_COLUMN_GROUP && v[i].kind != TABLE_BOX_COLUMN) {
            drop[i] = true;
            continue;
        }
        if (!v[i].anon_inline_ws) continue;
        /* Rule 3: "If a child C of a tabular container P is an anonymous inline box that contains only white
           space, and its immediately preceding and following siblings, if any, are proper table descendants of
           P and are either 'table-caption' or internal table boxes, then it is treated as if it had
           'display: none'." With "if any", a missing side states no condition. */
        rule3 = table_box_kind_is_tabular_container(pk) &&
                (!have_prev || (tb_proper_table_descendant(pk, prev) && tb_caption_or_internal(prev))) &&
                (!have_next || (tb_proper_table_descendant(pk, next) && tb_caption_or_internal(next)));
        /* Rule 4: "If a box B is an anonymous inline containing only white space, and is between two immediate
           siblings each of which is either an internal table box or a 'table-caption' box then B is treated as
           if it had 'display: none'." Both siblings must exist, and the parent need not be a tabular container
           (§17.2.1's third stage case). */
        rule4 = have_prev && have_next && tb_caption_or_internal(prev) && tb_caption_or_internal(next);
        if (rule3 || rule4) drop[i] = true;
    }
    for (i = 0; i < n; i++)
        if (!drop[i]) v[kept++] = v[i];
    free(drop);
    return kept;
}

/* ---- §17.2.1's second stage, "Generate missing child wrappers" ---------------------------------------------- */

typedef struct {
    TableBoxRow *v;
    size_t n, cap;
} TbRowSink;

/* The row group boxes §17.2's display order visits, recorded by the walk that emits the rows, so groups and
   rows share one ordering. */
typedef struct {
    TableBoxRowGroup *v;
    size_t n, cap;
} TbGroupSink;

static void tb_group_push(TbGroupSink *s, lxb_dom_element_t *element, size_t first, size_t nrows)
{
    s->v = (TableBoxRowGroup *) tb_reserve(s->v, s->n, &s->cap, sizeof *s->v);
    s->v[s->n].element = element;
    s->v[s->n].first = first;
    s->v[s->n].nrows = nrows;
    s->n++;
}

static void tb_row_push(TbRowSink *s, lxb_dom_element_t *element, lxb_dom_element_t *group,
                        lxb_dom_node_t *first, lxb_dom_node_t *end, TableBoxCell *cells, size_t ncells)
{
    s->v = (TableBoxRow *) tb_reserve(s->v, s->n, &s->cap, sizeof *s->v);
    s->v[s->n].element = element;
    s->v[s->n].group = group;
    s->v[s->n].first = first;
    s->v[s->n].end = end;
    s->v[s->n].cells = cells;
    s->v[s->n].ncells = ncells;
    s->n++;
}

/* §17.2.1's second stage, third rule, over the surviving child boxes `[from, to)` of one row box: "If a child C
   of a 'table-row' box is not a 'table-cell', then generate an anonymous 'table-cell' box around C and all
   consecutive siblings of C that are not 'table-cell' boxes."
   `v`/`n` is the whole surviving list and `[from, to)` the row's slice (they differ for an anonymous row,
   whose children are a run of its parent's list); the node past the slice is what `end` names. */
static size_t tb_cells(const TbChild *v, size_t n, size_t from, size_t to, TableBoxCell **out)
{
    TableBoxCell *cells = NULL;
    size_t cap = 0, cnt = 0, i = from;
    lxb_dom_node_t *range_end = (to < n) ? v[to].node : NULL;

    *out = NULL;
    while (i < to) {
        lxb_dom_element_t *el = NULL;
        size_t j;

        if (v[i].kind == TABLE_BOX_CELL) {
            el = lxb_dom_interface_element(v[i].node);
            j = i + 1;
        } else {
            for (j = i; j < to && v[j].kind != TABLE_BOX_CELL; j++) { }
        }
        cells = (TableBoxCell *) tb_reserve(cells, cnt, &cap, sizeof *cells);
        cells[cnt].element = el;
        cells[cnt].first = v[i].node;
        cells[cnt].end = (j < to) ? v[j].node : range_end;
        cnt++;
        i = j;
    }
    *out = cells;
    return cnt;
}

/* The same rule over a 'table-row' element's own child list, with `TABLE_BOX_ROW` as the tabular container. */
static size_t tb_cells_of_row_element(lxb_dom_element_t *row, TableBoxCell **out)
{
    TbChild *v = NULL;
    size_t n = tb_children(row, &v);
    size_t cnt;

    n = tb_remove_irrelevant(TABLE_BOX_ROW, v, n);
    cnt = tb_cells(v, n, 0, n, out);
    free(v);
    return cnt;
}

/* §17.2.1's second stage, second rule, over one row group box: "If a child C of a row group box is not a
   'table-row' box, then generate an anonymous 'table-row' box around C and all consecutive siblings of C that
   are not 'table-row' boxes." */
static void tb_rows_of_group(lxb_dom_element_t *group, TableBoxKind gk, TbRowSink *sink)
{
    TbChild *v = NULL;
    size_t n = tb_children(group, &v);
    size_t i = 0;

    n = tb_remove_irrelevant(gk, v, n);
    while (i < n) {
        TableBoxCell *cells = NULL;
        size_t ncells, j;

        if (v[i].kind == TABLE_BOX_ROW) {
            lxb_dom_element_t *row = lxb_dom_interface_element(v[i].node);

            ncells = tb_cells_of_row_element(row, &cells);
            j = i + 1;
            tb_row_push(sink, row, group, v[i].node, (j < n) ? v[j].node : NULL, cells, ncells);
        } else {
            for (j = i; j < n && v[j].kind != TABLE_BOX_ROW; j++) { }
            ncells = tb_cells(v, n, i, j, &cells);
            tb_row_push(sink, NULL, group, v[i].node, (j < n) ? v[j].node : NULL, cells, ncells);
        }
        i = j;
    }
    free(v);
}

/* One row group box visited: its rows go to `sink` and, if asked, the box goes to `gsink` with the rows it
   contributed. The range is taken around the call, which is the only moment an empty `<tbody></tbody>` is
   distinguishable. */
static void tb_visit_group(lxb_dom_element_t *group, TableBoxKind gk, TbRowSink *sink, TbGroupSink *gsink)
{
    size_t first = sink->n;

    tb_rows_of_group(group, gk, sink);
    if (gsink != NULL) tb_group_push(gsink, group, first, sink->n - first);
}

/* ---- the answer -------------------------------------------------------------------------------------------- */

/* The table box's own kind, with the caller's classification asserted rather than re-decided. */
static TableBoxKind tb_table_kind(lxb_dom_element_t *table, const char *asked)
{
    char *d;
    TableBoxKind tk;
    char nbuf[160];

    d = tb_computed(table, "display");
    tk = table_box_kind(d);
    free(d);
    DCHECKF(table_box_kind_generates_table_box(tk),
            "%s: CSS 2.1 §17.2's %s were asked of an element that does not generate a TABLE box — its computed "
            "`display` is neither `table` nor `inline-table`. Every rule this component runs is stated over "
            "\"a 'table' or 'inline-table' box\", and answering for a box that is not one would report a row "
            "list for a container that has none. A caller classifies with `table_box_kind` first; this is that "
            "classification and this test having come apart",
            box_subject(table, nbuf, sizeof nbuf), asked);
    return tk;
}

/* §17.2.1's second stage over a whole table box, run once for both answers: `sink` gets the rows and `gsink`,
   if passed, the row group boxes. The display order, the anonymous-box rules and "only the first is rendered
   as a header" are stated here only. */
static void tb_generate(lxb_dom_element_t *table, const char *asked, TbRowSink *sink, TbGroupSink *gsink)
{
    TbChild *v = NULL;
    TableBoxKind tk;
    size_t n, i, hdr, ftr;

    tk = tb_table_kind(table, asked);
    n = tb_children(table, &v);
    n = tb_remove_irrelevant(tk, v, n);
    /* §17.2's two exceptions to source order apply to one group each: "If a table contains multiple elements
       with 'display: table-header-group', only the first is rendered as a header; the others are treated as if
       they had 'display: table-row-group'", likewise the footer. Later ones fall through the row-group arm. */
    hdr = n;
    ftr = n;
    for (i = 0; i < n; i++) {
        if (hdr == n && v[i].kind == TABLE_BOX_HEADER_GROUP) hdr = i;
        if (ftr == n && v[i].kind == TABLE_BOX_FOOTER_GROUP) ftr = i;
    }
    /* "for visual formatting, the row group is always displayed before all other rows and row groups and after
       any top captions": captions are the wrapper's (§17.4), so here only the header's rows come first. */
    if (hdr < n)
        tb_visit_group(lxb_dom_interface_element(v[hdr].node), TABLE_BOX_HEADER_GROUP, sink, gsink);
    i = 0;
    while (i < n) {
        TableBoxCell *cells = NULL;
        size_t ncells, j;

        if (i == hdr || i == ftr) {
            i++;
            continue;
        }
        if (table_box_kind_is_row_group(v[i].kind)) {
            tb_visit_group(lxb_dom_interface_element(v[i].node), v[i].kind, sink, gsink);
            i++;
            continue;
        }
        if (v[i].kind == TABLE_BOX_ROW) {
            lxb_dom_element_t *row = lxb_dom_interface_element(v[i].node);

            ncells = tb_cells_of_row_element(row, &cells);
            j = i + 1;
            tb_row_push(sink, row, NULL, v[i].node, (j < n) ? v[j].node : NULL, cells, ncells);
            i = j;
            continue;
        }
        /* A column, column group or caption is a proper table child that generates no row: columns "are not
           rendered (exactly as if they had 'display: none')", and captions are the wrapper's (§17.4,
           `table_box_captions`). Skipping them keeps a run they interrupt from becoming one anonymous row. */
        if (table_box_kind_is_proper_table_child(v[i].kind)) {
            i++;
            continue;
        }
        /* §17.2.1's second stage, first rule: "If a child C of a 'table' or 'inline-table' box is not a proper
           table child, then generate an anonymous 'table-row' box around C and all consecutive siblings of C
           that are not proper table children." */
        for (j = i; j < n && !table_box_kind_is_proper_table_child(v[j].kind); j++) { }
        ncells = tb_cells(v, n, i, j, &cells);
        tb_row_push(sink, NULL, NULL, v[i].node, (j < n) ? v[j].node : NULL, cells, ncells);
        i = j;
    }
    if (ftr < n)
        tb_visit_group(lxb_dom_interface_element(v[ftr].node), TABLE_BOX_FOOTER_GROUP, sink, gsink);
    free(v);
}

size_t table_box_rows(lxb_dom_element_t *table, TableBoxRow **out)
{
    TbRowSink sink;

    DCHECK(table != NULL, "CSS 2.1 §17.2.1's box generation was asked for the rows of no element");
    DCHECK(out != NULL,
           "CSS 2.1 §17.2.1's box generation was asked for a table's rows with nowhere to put them. A count "
           "alone names no row and no cell, so a caller holding one would know how many grid rows §17.5's "
           "first rule gives the table and have no way to reach any of them — which is the whole of what this "
           "entry is asked for");
    sink.v = NULL;
    sink.n = 0;
    sink.cap = 0;
    *out = NULL;
    tb_generate(table, "rows", &sink, NULL);
    *out = sink.v;
    return sink.n;
}

size_t table_box_row_groups(lxb_dom_element_t *table, TableBoxRowGroup **out)
{
    TbRowSink sink;
    TbGroupSink gsink;

    DCHECK(table != NULL, "CSS 2.1 §17.2's row group boxes were asked for of no element");
    DCHECK(out != NULL,
           "CSS 2.1 §17.2's row group boxes were asked for with nowhere to put them. A count alone names no "
           "box and no grid row, so a caller holding one could not tell which group it had counted — and the "
           "EMPTY row group box this entry exists to report is exactly the one that is indistinguishable from "
           "its neighbours by anything but its place in this array");
    sink.v = NULL;
    sink.n = 0;
    sink.cap = 0;
    gsink.v = NULL;
    gsink.n = 0;
    gsink.cap = 0;
    *out = NULL;
    /* The rows are generated and released: a group's `first` is a count of rows emitted before it. Nothing is
       cached (layout is per-flow state solver/dom_cow.h does not swap). */
    tb_generate(table, "row groups", &sink, &gsink);
    table_box_rows_free(sink.v, sink.n);
    *out = gsink.v;
    return gsink.n;
}

void table_box_rows_free(TableBoxRow *rows, size_t nrows)
{
    size_t i;

    DCHECK(rows != NULL || nrows == 0,
           "a table's rows were released as a NULL array with a non-zero count — `table_box_rows` stores NULL "
           "only for a count of zero, so the two have been carried apart since it answered");
    for (i = 0; i < nrows; i++)
        free(rows[i].cells);
    free(rows);
}

/* §17.2's box nesting read upward. Between a cell and its table box stand only a row and a row group (a column
   only a column group), so the chain is at most two links before the table. A non-table box in the chain means
   §17.2.1's third stage's anonymous table box, which no element names; returning the next `<table>` up would
   size this box from another table's columns, so it crashes by name. */
lxb_dom_element_t *table_box_table_of(lxb_dom_element_t *internal)
{
    lxb_dom_node_t *n;
    char nbuf[160], abuf[160];

    DCHECK(internal != NULL, "CSS 2.1 §17.2's box nesting was asked for the table box of no element");
    {
        char *d = tb_computed(internal, "display");
        TableBoxKind k = table_box_kind(d);

        free(d);
        DCHECKF(table_box_kind_is_internal(k),
                "%s: CSS 2.1 §17.2's box nesting was asked which TABLE BOX this box is inside, and it is not "
                "an INTERNAL table box — §17.2's own definition is \"A 'table-cell' box, 'table-row' box, row "
                "group box, 'table-column' box, or 'table-column-group' box\". A 'table-caption' is the near "
                "miss and is the one this test exists to stop: §17.4 Tables in the visual formatting model "
                "puts it in the table WRAPPER box beside the table box, not inside it, so the table returned "
                "for one would be the wrong box and every number taken from it would be a real width of a "
                "rectangle the caption is not in",
                box_subject(internal, nbuf, sizeof nbuf));
    }
    for (n = lxb_dom_interface_node(internal)->parent;
         n != NULL && n->type == LXB_DOM_NODE_TYPE_ELEMENT;
         n = n->parent) {
        lxb_dom_element_t *anc = lxb_dom_interface_element(n);
        char *d = tb_computed(anc, "display");
        TableBoxKind k = table_box_kind(d);

        free(d);
        if (table_box_kind_generates_table_box(k)) return anc;
        if (table_box_kind_is_internal(k)) continue;
        DFAILF("%s, whose ancestor %s generates neither a TABLE box nor an INTERNAL table box: CSS 2.1 "
               "§17.2.1 Anonymous table objects' third stage puts an ANONYMOUS 'table' box around a run of "
               "internal boxes whose parent is not a tabular container, so this box IS inside a table box and "
               "that box is one NO ELEMENT NAMES. Walking past this ancestor to the next `<table>` above would "
               "answer with a different table entirely — its columns, its cell spacing, its width — and every "
               "number taken from it would be a real measurement of a rectangle this box is not in. BUILD "
               "§17.2.1's third stage as a BOX this walk can return, which is the same anonymous box "
               "core/layout/used_value.c's §10.1 containing-block walk needs for the table WRAPPER and is "
               "the reason neither can be answered by an element",
               box_subject(internal, nbuf, sizeof nbuf), box_subject(anc, abuf, sizeof abuf));
    }
    DFAILF("%s: CSS 2.1 §17.2's box nesting walked off the top of the tree without reaching a TABLE box. "
           "§17.2.1 Anonymous table objects' third stage generates one over every run of internal boxes, so "
           "this box is inside an ANONYMOUS table box that no element names — the same crash as the ancestor "
           "arm above, reached where the run has no element ancestor at all",
           box_subject(internal, nbuf, sizeof nbuf));
    return NULL;
}

size_t table_box_captions(lxb_dom_element_t *table, lxb_dom_element_t ***out)
{
    TbChild *v = NULL;
    lxb_dom_element_t **caps = NULL;
    TableBoxKind tk;
    size_t n, i, cnt = 0, cap = 0;

    DCHECK(table != NULL, "CSS 2.1 §17.4's caption boxes were asked for of no element");
    DCHECK(out != NULL,
           "CSS 2.1 §17.4's caption boxes were asked for with nowhere to put them — a count alone names no box "
           "for §17.4's wrapper to contain");
    *out = NULL;
    tk = tb_table_kind(table, "caption boxes");
    n = tb_children(table, &v);
    n = tb_remove_irrelevant(tk, v, n);
    for (i = 0; i < n; i++) {
        if (v[i].kind != TABLE_BOX_CAPTION) continue;
        caps = (lxb_dom_element_t **) tb_reserve(caps, cnt, &cap, sizeof *caps);
        caps[cnt] = lxb_dom_interface_element(v[i].node);
        cnt++;
    }
    free(v);
    *out = caps;
    return cnt;
}
