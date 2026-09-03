/* CSS 2.1 §17.2 The CSS table model and §17.2.1 Anonymous table objects — a table box's rows and cells. See
   table_box.h for the contract, for the range convention every anonymous box is reported through, and for what
   each neighbouring algorithm is still waiting on. */
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/layout/block_flow.h"
#include "core/layout/box_subject.h"
#include "core/layout/table_box.h"

static char *tb_computed(lxb_dom_element_t *el, const char *name)
{
    char *v = css_computed_value(el, name);

    DCHECK(v != NULL, "the cascade produced no computed value for a property this engine models — every one of "
                      "them is in lexbor's registry with an initial value, so the last layer always answers");
    return v;
}

/* ONE GROWTH STEP for the three arrays this file builds. The allocation failure is a `CHECK` and not a
   `DCHECK` for CLAUDE.md's own reason: a dropped row is a table laid out with fewer rows than it has, which is
   a wrong geometry in release rather than an absent one. */
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
   generation of any intervening 'table' or 'inline-table' boxes." Read off the section's own parentage rules:
   a caption, a column group, a column and a row group are children of a table box; a row is a child of a table
   box or of a row group; a cell is a child of a row. Anything reachable only by generating a table box on the
   way — a caption under a row group, a row group under a row — is NOT one, which is exactly the case
   §17.2.1's third stage wraps in an anonymous table. */
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

/* ---- one parent's child BOXES ------------------------------------------------------------------------------ */

typedef struct {
    lxb_dom_node_t *node;
    TableBoxKind kind;
    /* §17.2.1's first stage is stated about "an anonymous inline box that contains only white space", which is
       TWO facts about one child and not one: the box exists (CSS 2.2 §9.2.2.1 Anonymous inline boxes decides
       that, and core/layout/block_flow.h owns the sentence), and the character data it holds is white space
       throughout. A run under `white-space: pre` satisfies both; a run under `normal` generates no box at all
       and never reaches this array. */
    bool anon_inline_ws;
} TbChild;

/* The four characters core/layout/block_flow.c's §9.2.2.1 predicate reads, asked of the character data alone.
   It is not a second copy of that predicate: that one answers whether a run GENERATES A BOX, which is a joint
   question about the data and the parent's inherited `white-space`, and this one is the data half §17.2.1
   asks for separately once the box is known to exist. */
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

/* `parent`'s child boxes, in tree order, each with §17.2's classification of its computed `display`. Answers
   the count and stores a newly allocated array at `*out`, which the caller frees. A child that generates no
   box is not in it — which is CSS 2 §9.2's own answer and not this file's, so a `display: none` element, a
   comment, a processing instruction, a doctype and a collapsed run of white space are all simply absent. */
static size_t tb_children(lxb_dom_element_t *parent, TbChild **out)
{
    lxb_dom_node_t *n;
    TbChild *v = NULL;
    size_t cnt = 0, cap = 0;
    char nbuf[160], pbuf[160];

    *out = NULL;
    for (n = lxb_dom_interface_node(parent)->first_child; n != NULL; n = n->next) {
        TableBoxKind kind = TABLE_BOX_NOT_A_TABLE_BOX;
        bool ws = false;

        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            lxb_dom_element_t *el = lxb_dom_interface_element(n);
            char *d = tb_computed(el, "display");
            bool none = strcmp(d, "none") == 0;
            bool contents = strcmp(d, "contents") == 0;

            kind = table_box_kind(d);
            free(d);
            if (none) continue;
            if (contents) {
                DFAILF("%s, a child of the table box %s: "
                       "this child's computed `display` is `contents`, so the ELEMENT tree and the BOX tree "
                       "are no longer the same shape here and the children CSS 2.1 §17.2.1 Anonymous table "
                       "objects must classify are this child's OWN children, spliced into this list at its "
                       "position. css-display-3 §2.5 Box Generation: the none and contents keywords is where "
                       "that splice is stated: \"For the purposes of box "
                       "generation and layout, the element must be treated as if it had been replaced in the "
                       "element tree by its contents (including both its source-document children and its "
                       "pseudo-elements, such as ::before and ::after pseudo-elements, which are generated "
                       "before/after the element's children as normal).\" THE SPLICE IS NOT THIS COMPONENT'S "
                       "TO BUILD BY HAND — core/layout/block_flow.c's own walk and core/layout/used_value.c's "
                       "containing-block walk each meet the same value and each names the same splice, so a "
                       "third copy here would be one box-tree rule with three answers about which children a "
                       "container has. BUILD §2.5's splice as the child list every walk over a container "
                       "iterates, and a `contents` element becomes invisible to all three at once",
                       box_subject(el, nbuf, sizeof nbuf), box_subject(parent, pbuf, sizeof pbuf));
                continue;
            }
        } else if (n->type == LXB_DOM_NODE_TYPE_TEXT) {
            if (!block_flow_text_child_generates_box(parent, n)) continue;
            ws = tb_text_is_all_whitespace(n);
        } else if (n->type == LXB_DOM_NODE_TYPE_COMMENT ||
                   n->type == LXB_DOM_NODE_TYPE_PROCESSING_INSTRUCTION ||
                   n->type == LXB_DOM_NODE_TYPE_DOCUMENT_TYPE) {
            /* CSS 2 §9.2 generates boxes for elements and for text; a comment, a processing instruction and a
               doctype are neither, and every user agent lays out none of the three. */
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

/* ---- §17.2.1's FIRST STAGE, "Remove irrelevant boxes" ------------------------------------------------------- */

/* The four rules, over the child boxes of a parent whose own box type is `pk`, applied in place; answers how
   many survive. Every drop is decided against the PRE-REMOVAL neighbour list, because the section states one
   stage and not a fixpoint: a whitespace box's neighbours are the siblings it had when the stage began, and
   evaluating a rule against a list the same stage is compacting would let the order of the scan decide the
   answer. */
static size_t tb_remove_irrelevant(TableBoxKind pk, TbChild *v, size_t n)
{
    size_t i, kept = 0;
    bool *drop;

    if (n == 0) return 0;
    /* RULE 1: "All child boxes of a 'table-column' parent are treated as if they had 'display: none'." */
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

        /* RULE 2: "If a child C of a 'table-column-group' parent is not a 'table-column' box, then it is
           treated as if it had 'display: none'." */
        if (pk == TABLE_BOX_COLUMN_GROUP && v[i].kind != TABLE_BOX_COLUMN) {
            drop[i] = true;
            continue;
        }
        if (!v[i].anon_inline_ws) continue;
        /* RULE 3: "If a child C of a tabular container P is an anonymous inline box that contains only white
           space, and its immediately preceding and following siblings, if any, are proper table descendants of
           P and are either 'table-caption' or internal table boxes, then it is treated as if it had
           'display: none'." The "if any" is what makes a run of white space between a table's first child and
           its start — and a table whose only child is white space — removable: a side with no sibling states
           no condition. */
        rule3 = table_box_kind_is_tabular_container(pk) &&
                (!have_prev || (tb_proper_table_descendant(pk, prev) && tb_caption_or_internal(prev))) &&
                (!have_next || (tb_proper_table_descendant(pk, next) && tb_caption_or_internal(next)));
        /* RULE 4: "If a box B is an anonymous inline containing only white space, and is between two immediate
           siblings each of which is either an internal table box or a 'table-caption' box then B is treated as
           if it had 'display: none'." It is not rule 3 with the descendant test dropped — it requires BOTH
           siblings to exist, and it holds in a parent that is not a tabular container at all, which is the
           case §17.2.1's third stage is about. */
        rule4 = have_prev && have_next && tb_caption_or_internal(prev) && tb_caption_or_internal(next);
        if (rule3 || rule4) drop[i] = true;
    }
    for (i = 0; i < n; i++)
        if (!drop[i]) v[kept++] = v[i];
    free(drop);
    return kept;
}

/* ---- §17.2.1's SECOND STAGE, "Generate missing child wrappers" ---------------------------------------------- */

typedef struct {
    TableBoxRow *v;
    size_t n, cap;
} TbRowSink;

/* THE ROW GROUP BOXES §17.2's display order visits, RECORDED BY THE SAME WALK THAT EMITS THE ROWS. It is a
   second sink and not a second walk because §17.2's two exceptions to source order (a header group's rows
   before every other row, a footer group's after) are ONE statement, and a walk written a second time to
   enumerate the groups would be free to order them differently from the rows they contain — which no document
   could distinguish from a table whose row groups are simply somewhere else. */
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

/* §17.2.1's second stage, THIRD rule, over the surviving child boxes `[from, to)` of one row box: "If a child C
   of a 'table-row' box is not a 'table-cell', then generate an anonymous 'table-cell' box around C and all
   consecutive siblings of C that are not 'table-cell' boxes."
   `v`/`n` is the WHOLE surviving child list and `[from, to)` is the row's slice of it, because the two differ
   for an ANONYMOUS row — its children are a run inside its own parent's list — and the node one past the slice
   is what `end` has to name. */
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

/* The same rule over a real 'table-row' ELEMENT's own child list, which is the ordinary case and the one whose
   first stage is asked with `TABLE_BOX_ROW` as the tabular container. */
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

/* §17.2.1's second stage, SECOND rule, over one row group box: "If a child C of a row group box is not a
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

/* ONE ROW GROUP BOX VISITED — its rows generated into `sink` and, where a caller asked for them, the box
   itself recorded into `gsink` with the range of rows it contributed. THE RANGE IS TAKEN AROUND THE CALL AND
   NOT COUNTED AFTERWARD, which is what makes an EMPTY row group box appear at all: `<tbody></tbody>` generates
   no row, so it is invisible in `table_box_rows`' answer and in every array derived from it, and the only
   moment at which it is distinguishable from a group that is not there is the moment the walk stands on it. */
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

/* §17.2.1's SECOND STAGE OVER A WHOLE TABLE BOX, RUN ONCE FOR BOTH ANSWERS THIS FILE GIVES ABOUT IT. `sink`
   collects the rows and `gsink`, when a caller passes one, collects the row group boxes the same visit passes
   through. The two entries below are the two ways of asking for what this walk produces and never two walks:
   §17.2's display order, §17.2.1's three anonymous-box rules and the "only the first is rendered as a header"
   sentence are stated HERE and nowhere else, so no second enumeration can order the groups differently from
   the rows inside them. */
static void tb_generate(lxb_dom_element_t *table, const char *asked, TbRowSink *sink, TbGroupSink *gsink)
{
    TbChild *v = NULL;
    TableBoxKind tk;
    size_t n, i, hdr, ftr;

    tk = tb_table_kind(table, asked);
    n = tb_children(table, &v);
    n = tb_remove_irrelevant(tk, v, n);
    /* §17.2's own two exceptions to source order, and the sentence that makes each of them apply to exactly
       ONE group: "If a table contains multiple elements with 'display: table-header-group', only the first is
       rendered as a header; the others are treated as if they had 'display: table-row-group'" — and the same
       for the footer. So the FIRST of each is found here, and every later one falls through the row-group arm
       below with no special case of its own. */
    hdr = n;
    ftr = n;
    for (i = 0; i < n; i++) {
        if (hdr == n && v[i].kind == TABLE_BOX_HEADER_GROUP) hdr = i;
        if (ftr == n && v[i].kind == TABLE_BOX_FOOTER_GROUP) ftr = i;
    }
    /* "for visual formatting, the row group is always displayed before all other rows and row groups and after
       any top captions" — the captions are §17.4's wrapper box's and not the table box's, so what that
       sentence decides HERE is only that the header's rows come first. */
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
        /* A 'table-column', a 'table-column-group' and a 'table-caption' are proper table children that are
           not rows and generate none: §17.2 says a column and a column group "are not rendered (exactly as if
           they had 'display: none')", and §17.4 puts the caption boxes in the WRAPPER box beside the table box
           rather than inside it (`table_box_captions`). Skipping them here is those two sentences and not an
           omission — a run they interrupted would otherwise be one anonymous row spanning them. */
        if (table_box_kind_is_proper_table_child(v[i].kind)) {
            i++;
            continue;
        }
        /* §17.2.1's second stage, FIRST rule: "If a child C of a 'table' or 'inline-table' box is not a proper
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
    /* THE ROWS ARE GENERATED AND RELEASED, AND THAT IS NOT WASTE THIS ENTRY COULD AVOID: a group's `first` is
       a COUNT OF ROWS EMITTED BEFORE IT, so the rows are this answer's arithmetic and not a by-product of it.
       Nothing in this directory caches a layout (core/layout/block_flow.h's reason — a cached one is shared
       state solver/dom_cow.h's delta does not swap), so a caller wanting both asks for both. */
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

/* §17.2's BOX NESTING READ UPWARD. What may stand between a cell and its table box is a 'table-row' and a row
   group box and nothing else, which is the same nesting `table_box_rows` descends and is why this is one
   `while` rather than a recursion: §17.2 The CSS table model nests a cell in a row, a row in a row group or in
   the table box, and a column in a column group, so the chain is at most two links deep before the table.
   A NON-TABLE BOX IN THE CHAIN IS §17.2.1's ANONYMOUS TABLE BOX AND IT CRASHES BY NAME. §17.2.1's third stage
   generates a table box around a run of internal boxes whose parent is not a tabular container, so a cell
   under an ordinary `<div>` IS in a table box — one no element in this tree names. The nearest `<table>`
   further up is a DIFFERENT box and returning it would report this cell's used width out of another table's
   columns, which is why this is a crash and not a wider walk. */
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
