/* CSS 2.1 §17.5.2 Table width algorithms: the 'table-layout' property, over core/layout/table_grid.h's grid and
   core/layout/table_column_width.h's per-column pair. See table_width.h. */
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/html/html.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/css/css_property_applies.h"
#include "core/layout/box_subject.h"
#include "core/layout/intrinsic_size.h"
#include "core/layout/table_border_collapse.h"
#include "core/layout/table_box.h"
#include "core/layout/table_column_box.h"
#include "core/layout/table_column_width.h"
#include "core/layout/table_grid.h"
#include "core/layout/table_width.h"
#include "core/layout/table_wrapper.h"
#include "core/layout/used_value.h"

static char *tw_computed(lxb_dom_element_t *el, const char *name)
{
    char *v = css_computed_value(el, name);

    DCHECK(v != NULL, "the cascade produced no computed value for a property this engine models — every one of "
                      "them is in lexbor's registry with an initial value, so the last layer always answers");
    return v;
}

/* ---- §17.6 Borders' two models, asked once at the entry ---------------------------------------------------
   Two terms here belong to §17.6.1 The separated borders model alone. The cell spacing ("The lengths specify
   the distance that separates adjoining cell borders") is zero under §17.6.2 The collapsing border model,
   where "Borders are centered on the grid lines between the cells". The table's own edges are
   `tw_table_edges`. Column widths need no arm: `table_cell_border_edges` answers either model.
   `TableUsedWidths.spacing` stores this term, not the property, since consumers re-add it. */
static CssPx tw_used_spacing(CssPx horizontal, bool collapsing)
{
    return collapsing ? css_px(0.0) : horizontal;
}

/* ---- §17.6.1's cell spacing -------------------------------------------------------------------------------
   Between cells it is the property; at the two edges "The distance between the table border and the borders
   of the cells on the edge of the table is the table's padding for that side, plus the relevant border
   spacing distance". So it appears `ncols + 1` times, padding excluded. A table with no columns has no cell
   borders to separate, so zero. */
static CssPx tw_spacing_total(CssPx spacing, size_t ncols)
{
    if (ncols == 0) return css_px(0.0);
    return css_px_scale(spacing, (double) (ncols + 1));
}

/* ---- The table box's own horizontal padding and border ----------------------------------------------------
   §17.4 Tables in the visual formatting model puts them on the table box ("all other values of
   non-inheritable properties are used on the table box and not the table wrapper box"), confirmed with
   core/layout/table_wrapper.h. A percentage padding on the table resolves against the table's own containing
   block (CSS 2.1 §10.1 Definition of "containing block") through core/layout/used_value.h: not the cell
   cycle table_column_width.c refuses.
   Under §17.6.2 The collapsing border model: "Also, in this model, a table does not have padding (but does
   have margins)", and "The left border width of the table is half of the first cell's collapsed left border,
   and the right border width of the table is half of the last cell's collapsed right border."
   core/layout/table_border_collapse.h answers that pair; perimeter cells are charged nothing there, so the two
   meet at the grid line. */
static CssPx tw_table_edges(lxb_dom_element_t *table, bool collapsing)
{
    static const char *const PADDINGS[2] = { "padding-left", "padding-right" };
    static const char *const BORDERS[2] = { "border-left-width", "border-right-width" };
    CssPx edges = css_px(0.0);
    int i;

    if (collapsing) {
        TableCollapsedEdges own = table_collapsed_table_edges(table);

        return css_px_add(own.left, own.right);
    }
    DCHECK(!table_wrapper_owns_property("padding-left") && !table_wrapper_owns_property("border-left-width"),
           "CSS 2.1 §17.4 Tables in the visual formatting model's declaration split now puts a table element's "
           "padding or border on the TABLE WRAPPER BOX, and CSS 2.1 §17.5.2 Table width algorithms: the "
           "'table-layout' property is reading them as the TABLE BOX's — so the border-edge width §17.4 gives "
           "the wrapper would be assembled out of the wrapper's own edges twice and the table box's not at all");
    for (i = 0; i < 2; i++) {
        CssLength bor = css_computed_length(table, BORDERS[i]);

        DCHECK(bor.kind == CSS_LENGTH_ABSOLUTE,
               "a `border-*-width` computed to something that is not an absolute length. css-backgrounds-3 "
               "§3.3 \"Line Thickness: the border-width properties\"' `Computed value:` line is `absolute "
               "length, snapped as a border width` and every arm of that derivation produces one, so a "
               "percentage or a keyword here is a rule that did not run");
        edges = css_px_add(edges, css_px_add(bor.px, used_value_px(table, PADDINGS[i])));
    }
    DCHECK(edges.px >= 0.0,
           "CSS 2.1 §17.5.2's table edges came out NEGATIVE. CSS 2.1 §8.4 \"Padding properties: 'padding-top', "
           "'padding-right', 'padding-bottom', 'padding-left', and 'padding'\" states outright that \"unlike "
           "margin properties, values for padding values cannot be negative\" and css-backgrounds-3 §3.3's "
           "<line-width> is a non-negative <length>, so lexbor drops either declaration; a negative here is "
           "arithmetic that lost a sign rather than a document");
    return edges;
}

/* ---- Which box a declared `width` is on --------------------------------------------------------------------
   The border box if either css-sizing-3 §3.3 "Box Edges for Sizing: the box-sizing property"'s `border-box`
   applies, or CSS 2.1 §17.6.1 The separated borders model's "However, in HTML and XHTML1, the width of the
   <table> element is the distance from the left border edge to the right border edge." The second is about
   the element, so it is Lexbor's namespace-qualified tag test (a `display: table` div is not one; a
   `<table style="display: inline-table">` is). */
static bool tw_declared_width_is_border_box(lxb_dom_element_t *table)
{
    char *sizing = tw_computed(table, "box-sizing");
    bool border_box = strcmp(sizing, "border-box") == 0;

    DCHECK(border_box || strcmp(sizing, "content-box") == 0,
           "a `box-sizing` computed to something that is neither of the two keywords css-sizing-3 §3.3 \"Box "
           "Edges for Sizing: the box-sizing property\"'s `content-box | border-box` grammar admits, and its "
           "`Computed value:` line is `specified keyword`");
    free(sizing);
    return border_box || lxb_html_tree_node_is(lxb_dom_interface_node(table), LXB_TAG_TABLE);
}

/* ---- The table's own `width`, as a content width -----------------------------------------------------------
   False for `width: auto`; true with the declared width converted into §17.6.1's box. A percentage resolves
   against the containing block (CSS 2.1 §10.2 "Content width: the 'width' property"), which §17.5.2 takes as
   input ("Input to the automatic table layout must only include the width of the containing block and the
   content of, and any CSS properties set on, the table and any of its descendants"), so it is not a cycle. */
static bool tw_declared_content_width(lxb_dom_element_t *table, CssPx edges, CssPx *out)
{
    CssLength w = css_computed_length(table, "width");
    CssPx declared = css_px(0.0);
    char nbuf[160];

    DCHECK(css_property_applies(table, "width"),
           "CSS 2.1 §17.5.2 read a `width` on a box that property does not apply to. CSS 2.1 §10.2 \"Content "
           "width: the 'width' property\"' Applies-to line excludes table rows and row groups and no other "
           "table box, so a table reaching this is a `display` this component classified as a table box and the "
           "cascade classifies otherwise");
    if (css_length_is_keyword(w, CSS_KEYWORD_AUTO)) return false;
    if (w.kind == CSS_LENGTH_ABSOLUTE) {
        declared = w.px;
    } else if (w.kind == CSS_LENGTH_PERCENTAGE || w.kind == CSS_LENGTH_CALCULATED) {
        declared = css_length_resolve_pct(w, used_value_containing_block_width(table));
    } else {
        DFAILF("%s: CSS 2.1 §17.5.2 Table width algorithms: the 'table-layout' property compares a table whose "
               "\"'width' property has a computed value (W) other than 'auto'\" against CAPMIN and MIN, and "
               "this table's computed `width` is neither a length, a percentage nor `auto`. css-sizing-3 §3.1.1 "
               "\"Preferred Size Properties: the width and height properties\" is what widened that property "
               "past CSS 2.1's three arms — its value grammar carries the intrinsic sizing keywords beside the "
               "length — and each of those names a DIFFERENT rule from the comparison this section states, "
               "which is why none may be quietly read as `auto`: reading one as `auto` would send a table its "
               "author sized to §17.5.2.2 Automatic table layout's containing-block arm. BUILD each keyword's "
               "own arm here, over the MIN and MAX this component already has — css-sizing-3 §2.1 \"Auto Box "
               "Sizes\" pairs the two vocabularies itself, \"this is called the preferred minimum width in "
               "CSS2.1 10.3.5 and the minimum content width in CSS2.1 17.5.2.2\"",
               box_subject(table, nbuf, sizeof nbuf));
    }
    /* css-values-4 §9.1 "Numeric Functions"' used-value clamp and CSS 2.1 §10.2's "negative values for 'width'
       are illegal": a negative here is a math function's result, clamped. */
    declared = css_px_max(declared, css_px(0.0));
    if (tw_declared_width_is_border_box(table)) declared = css_px_max(css_px_sub(declared, edges), css_px(0.0));
    *out = declared;
    return true;
}

/* ---- §17.5.2.2's CAPMIN -----------------------------------------------------------------------------------
   "The caption width minimum (CAPMIN) is determined by calculating for each caption the minimum caption outer
   width as the MCW of a hypothetical table cell that contains the caption formatted as "display: block". The
   greatest of the minimum caption outer widths is CAPMIN."
   The `display: block` substitution changes nothing measured (core/layout/block_flow.h treats `table-caption`
   and `block` alike as block containers), so it is not performed. The hypothetical cell is anonymous and
   contributes nothing: "Non-inherited properties have their initial value" (CSS 2 §9.2.1.1 Anonymous block
   boxes), so no padding, border or declared `width`. "Outer" is css-sizing-3 §2.2 "Intrinsic Size
   Contributions"' outer size ("auto margins are treated as zero"); a caption's margins apply (CSS 2.1 §8.3
   "Margin properties"), asked of the component that owns that line. */
/* ONE caption's "minimum caption outer width", over core/layout/intrinsic_size.h's §5.2 terms. Every
   percentage here is cyclic (a caption's containing block is the wrapper, sized from this algorithm's output),
   so css-sizing-3 §5.2.1 "Intrinsic Contributions of Percentage-Sized Boxes" applies: one on a margin or a
   padding resolves against zero, one on `width` is treated as `auto`. MCW is the width at which "the formatted
   content may span any number of lines but may not overflow the cell box", so a declared caption `width` above
   its min-content size raises it, as step 1 does for a cell. A recorded reading: §5.2 would let a declared
   width replace the content size and overflow; the two differ only for a width narrower than the longest word. */
static CssPx tw_caption_outer_min(lxb_dom_element_t *caption)
{
    IntrinsicInlineSizes inner;
    CssPx declared;

    DCHECK(css_property_applies(caption, "margin-left") && css_property_applies(caption, "margin-right"),
           "CSS 2.1 §17.5.2.2's CAPMIN is a caption's OUTER width and css-sizing-3 §2.2 \"Intrinsic Size "
           "Contributions\" makes an outer size include the margins, and CSS 2.1 §8.3 \"Margin properties\"' "
           "Applies-to line now says the horizontal margins do NOT apply to a caption box — so either that "
           "line changed or this box is not a caption, and CAPMIN would be reported by two margins too small");
    inner = intrinsic_inline_sizes(caption);
    if (intrinsic_declared_sizing_px(caption, INTRINSIC_AXIS_HORIZONTAL, "width", CSS_KEYWORD_AUTO, &declared))
        inner.min_content = css_px_max(inner.min_content, declared);
    inner.max_content = inner.min_content;
    return intrinsic_outer_contribution(caption, inner).min_content;
}

static CssPx tw_capmin(lxb_dom_element_t *table)
{
    lxb_dom_element_t **captions = NULL;
    CssPx capmin = css_px(0.0);
    size_t n, i;

    /* The caption boxes are the wrapper's (§17.4), listed by core/layout/table_box.h. "The greatest of the
       minimum caption outer widths is CAPMIN", and over no captions that is zero. */
    n = table_box_captions(table, &captions);
    DCHECK(captions != NULL || n == 0,
           "CSS 2.1 §17.4's caption boxes came back as a non-zero count with no array — `table_box_captions` "
           "stores NULL only for a table with no caption, so the two have been carried apart since it answered");
    for (i = 0; i < n; i++) capmin = css_px_max(capmin, tw_caption_outer_min(captions[i]));
    free(captions);
    DCHECK(capmin.px >= 0.0,
           "CSS 2.1 §17.5.2.2's CAPMIN came out NEGATIVE. Every term of a caption's outer width is an intrinsic "
           "size, a padding, a border width or a margin, and only the last of those may be negative — a total "
           "below zero is a caption whose margins outrun its content, and §17.5.2.2 then uses that total as a "
           "FLOOR under the table's width, where it would pull the table narrower than its own columns");
    return capmin;
}

/* ---- The distribution -------------------------------------------------------------------------------------
   §17.5.2.2: "If the used width is greater than MIN, the extra width should be distributed over the columns";
   §17.5.2.1: "If the table is wider than the columns, the extra space should be distributed over the
   columns". Neither says how, so the recorded reading splits the extra in proportion to each column's
   headroom (max less min), equally when all headroom is zero or (fixed algorithm, `head` NULL) absent.
   A column may end wider than its maximum: MAX chose the used width, it does not cap columns. The last column
   takes the remainder so the shares sum to `extra`. The ratio uses `css_px_div`, which carries both operands'
   environment facts (a `rem` cell, a viewport-derived containing block); a `double` divide would drop them. */
static void tw_distribute(CssPx *cols, const TableColumnWidth *head, size_t n, CssPx extra)
{
    CssPx weight_total = css_px(0.0);
    CssPx given = css_px(0.0);
    size_t i;

    if (n == 0 || extra.px <= 0.0) return;
    if (head != NULL)
        for (i = 0; i < n; i++) weight_total = css_px_add(weight_total, css_px_sub(head[i].max, head[i].min));
    for (i = 0; i < n; i++) {
        CssPx share;

        if (i + 1 == n) share = css_px_sub(extra, given);
        else if (head != NULL && weight_total.px > 0.0)
            share = css_px_mul(css_px_sub(head[i].max, head[i].min), css_px_div(extra, weight_total));
        else share = css_px_scale(extra, 1.0 / (double) n);
        cols[i] = css_px_add(cols[i], share);
        given = css_px_add(given, share);
    }
}

/* ---- §17.5.2's subject -----------------------------------------------------------------------------------
   One refusal for both entries: §17.5.2 states both over "the 'table' or 'inline-table' element", and a second
   copy would be a second reading of §17.2 The CSS table model's box types. It names the element, not a call
   site. */
static void tw_require_table_box(lxb_dom_element_t *table)
{
    char *display = tw_computed(table, "display");
    bool is_table_box = table_box_kind_generates_table_box(table_box_kind(display));
    char nbuf[160];

    free(display);
    if (is_table_box) return;
    DFAILF("%s: CSS 2.1 §17.5.2 Table width algorithms: the 'table-layout' property is stated over a TABLE "
           "box — both of its final rules name \"the 'table' or 'inline-table' element\" — and this box's "
           "computed `display` (printed above) is a different one of CSS 2.1 §17.2 The CSS table model's "
           "box types. NEITHER OF THIS COMPONENT'S TWO QUESTIONS IS ASKED OF ONE — §17.5.2 states the used "
           "width over \"the 'table' or 'inline-table' element\" and css-sizing-3 §5.1 \"Intrinsic Sizes\"' "
           "pair is the same box's. The width of a table-internal box must not be answered "
           "from it: a CELL's is the used width of the columns it occupies, which this component "
           "distributes and reports in `TableUsedWidths.columns` and which "
           "`table_cell_used_border_box` takes out of them; a ROW's and a ROW GROUP's is NOT the "
           "table's own content width, which an earlier form of this line claimed — §17.5 Visual layout "
           "of table contents' rules 1 and 2 give each of them the whole grid row, and that section's own "
           "last paragraph then places their EDGES, \"in the separated borders model the edges coincide "
           "with the border edges of cells, and thus in this model there may be gaps between the rows, "
           "columns, row groups or column groups corresponding to the 'border-spacing' property\", so a "
           "row is narrower than the table's content width by the TWO OUTER spacings §17.6.1 The "
           "separated borders model counts into it; and a COLUMN box's is that section's rules 3 and 4, "
           "which nothing in this directory places yet. ROUTE the caller to the table box above this one "
           "(core/layout/table_box.h's `table_box_table_of`) and ask it for the columns the caller wants",
           box_subject(table, nbuf, sizeof nbuf));
}

/* ---- §17.5.2.2's MIN and MAX ----------------------------------------------------------------------------
   "the minimum width required by all the columns plus cell spacing or borders (MIN)" and "the maximum width
   required by the columns plus cell spacing or borders (MAX)". The spacing is `spacing_total`, zero under
   §17.6.2 The collapsing border model, where borders arrive inside the column widths. Both entries use this
   one sum; the order relation is asserted here since table_column_width.c asserts it per column. */
typedef struct {
    CssPx min;   /* §17.5.2.2's MIN */
    CssPx max;   /* §17.5.2.2's MAX */
} TwColumnSums;

static TwColumnSums tw_column_sums(const TableColumnWidth *head, size_t n, CssPx spacing_total)
{
    TwColumnSums s;
    size_t i;

    DCHECK(head != NULL || n == 0,
           "CSS 2.1 §17.5.2.2's MIN and MAX were summed over a non-zero column count with no array — "
           "`table_column_widths` stores NULL only for a grid with no columns");
    s.min = spacing_total;
    s.max = spacing_total;
    for (i = 0; i < n; i++) {
        s.min = css_px_add(s.min, head[i].min);
        s.max = css_px_add(s.max, head[i].max);
    }
    DCHECK(s.min.px >= 0.0 && s.max.px >= s.min.px,
           "CSS 2.1 §17.5.2.2's MIN and MAX came out NEGATIVE or CROSSED. Every term of either is a "
           "non-negative column width or the same non-negative spacing total, and core/layout/"
           "table_column_width.c asserts `max >= min` for each column where it answers them — so a crossed sum "
           "here is arithmetic that lost an operand, and §17.5.2.2's final rules would then put the table's "
           "own width on the wrong side of a comparison with nothing downstream to say so");
    return s;
}

/* ---- §17.5.2.2 Automatic table layout's final rules ---------------------------------------------------------
   The two rules under "Column and caption widths influence the final table width as follows", over MIN and
   MAX; the four steps are core/layout/table_column_width.h's. */
static void tw_auto_layout(lxb_dom_element_t *table, const TableGrid *grid, CssPx spacing,
                           bool has_declared, CssPx declared, TableUsedWidths *out)
{
    TableColumnWidth *head = NULL;
    TwColumnSums sums;
    CssPx min_sum, max_sum, capmin, used;
    size_t n, i;

    n = table_column_widths(table, grid, &head);
    DCHECK(n == grid->ncols,
           "CSS 2.1 §17.5.2.2's four steps answered a different number of columns than the grid they were run "
           "over holds — that entry answers `grid->ncols` and nothing else, so the two have come apart");
    DCHECK(head != NULL || n == 0,
           "CSS 2.1 §17.5.2.2's four steps answered a non-zero column count with no array — that entry stores "
           "NULL only for a grid with no columns");
    sums = tw_column_sums(head, n, tw_spacing_total(spacing, n));
    min_sum = sums.min;
    max_sum = sums.max;
    capmin = tw_capmin(table);
    if (has_declared) {
        /* Rule 1: "If the 'table' or 'inline-table' element's 'width' property has a computed value (W) other
           than 'auto', the used width is the greater of W, CAPMIN, and the minimum width required by all the
           columns plus cell spacing or borders (MIN)." */
        used = css_px_max(css_px_max(declared, capmin), min_sum);
    } else {
        /* Rule 2: "If the 'table' or 'inline-table' element has 'width: auto', the used width is the greater of
           the table's containing block width, CAPMIN, and MIN. However, if either CAPMIN or the maximum width
           required by the columns plus cell spacing or borders (MAX) is less than that of the containing
           block, use max(MAX, CAPMIN)."
           The "However" is read literally as a disjunction: a table whose MAX exceeds its containing block
           while CAPMIN does not still gets max(MAX, CAPMIN), wider than its containing block. A shrink-to-fit
           reading is equally conforming (the section is non-normative and a UA "can use any other algorithm
           even if it results in different behavior"); this one is recorded rather than crashed on. */
        CssPx cb = used_value_containing_block_width(table);

        if (capmin.px < cb.px || max_sum.px < cb.px) used = css_px_max(max_sum, capmin);
        else used = css_px_max(css_px_max(cb, capmin), min_sum);
    }
    out->ncols = n;
    out->columns = NULL;
    if (n > 0) {
        out->columns = (CssPx *) calloc(n, sizeof *out->columns);
        CHECK(out->columns != NULL,
              "CSS 2.1 §17.5.2.2's final rules could not allocate one used width per grid column of the table "
              "being laid out");
        for (i = 0; i < n; i++) out->columns[i] = head[i].min;
        tw_distribute(out->columns, head, n, css_px_sub(used, min_sum));
    }
    free(head);
    out->content = used;
}

/* ---- §17.5.2.1 Fixed table layout ---------------------------------------------------------------------------
   "With this (fast) algorithm, the horizontal layout of the table does not depend on the contents of the cells;
   it only depends on the table's width, the width of the columns, and borders or cell spacing." So it reads
   no intrinsic size, and its step 2 is the first row alone ("Cells in subsequent rows do not affect column
   widths"). It is reached only with a declared width: "A value of 'auto' (for both 'display: table' and
   'display: inline-table') means use the automatic table layout algorithm." */
static void tw_fixed_layout(lxb_dom_element_t *table, const TableGrid *grid, CssPx spacing, CssPx declared,
                            TableUsedWidths *out)
{
    TableColumnBoxMap boxes;
    CssPx spacing_total = tw_spacing_total(spacing, grid->ncols);
    CssPx assigned_sum = css_px(0.0);
    CssPx avail, sum;
    bool *assigned = NULL;
    size_t n = grid->ncols, remaining = 0, k, i;
    char nbuf[160];

    table_column_boxes_build(table, n, &boxes);
    /* The column count is the section's: "the greater of the number determined by the table-column elements
       and the number determined by the first row". core/layout/table_grid.h grows `ncols` only over cells, so
       `<colgroup span="5">` over two-cell rows would divide the width over two columns. Unlike §17.5.2.2,
       where an `auto` column floors nothing, every column takes a share of step 3's equal division, so this
       crash is unconditional. */
    if (boxes.noccupied > n)
        DFAILF("%s: CSS 2.1 §17.5.2.1 Fixed table layout is dividing this table's width over the %zu grid "
               "columns core/layout/table_grid.h places from its cells, and its own column boxes occupy %zu — "
               "\"the greater of the number determined by the table-column elements and the number determined "
               "by the first row\" is this section's own count, so every column this algorithm reports is "
               "wider than a browser lays it out. BUILD HTML §4.9.12.1 Forming a table's COLUMN-GROUP "
               "CONTRIBUTION TO x_width in core/layout/table_grid.c, whose `ncols` today is only ever grown to "
               "`col + colspan` for a cell it places: that algorithm walks the `colgroup` children first and "
               "increases x_width by each span before it reaches a row, so the grid's column count must start "
               "at core/layout/table_column_box.h's `noccupied` rather than at zero",
               box_subject(table, nbuf, sizeof nbuf), n, boxes.noccupied);
    out->ncols = n;
    out->columns = NULL;
    if (n > 0) {
        out->columns = (CssPx *) calloc(n, sizeof *out->columns);
        assigned = (bool *) calloc(n, sizeof *assigned);
        CHECK(out->columns != NULL && assigned != NULL,
              "CSS 2.1 §17.5.2.1 Fixed table layout could not allocate one used width per grid column of the "
              "table being laid out");
        for (i = 0; i < n; i++) out->columns[i] = css_px(0.0);
    }
    /* Step 1: "A column element with a value other than 'auto' for the 'width' property sets the width for that
       column." Which grid columns a box covers is core/layout/table_column_box.h.
       "A column element" is the column box, not the group (§17.2 makes them two box types, and the section
       later says "the number determined by the table-column elements"), so a `<colgroup>` width sets nothing
       here; §17.5.2.2's step 4 gives group widths a different effect. Read literally and recorded.
       No box conversion: in this model "Rows, columns, row groups and column groups cannot have borders (i.e.,
       user agents must ignore the border properties for those elements)", and "Cells have padding as well"
       (cells alone), so the declared number is the column's whole border-box width. */
    for (i = 0; i < n; i++) {
        CssLength cw;
        CssPx width;

        if (boxes.cols[i].column == NULL) continue;
        cw = css_computed_length(boxes.cols[i].column, "width");
        if (css_length_is_keyword(cw, CSS_KEYWORD_AUTO)) continue;
        if (cw.kind == CSS_LENGTH_ABSOLUTE) {
            width = cw.px;
        } else if (cw.kind == CSS_LENGTH_PERCENTAGE || cw.kind == CSS_LENGTH_CALCULATED) {
            /* "A percentage value for a column width is relative to the table width", and here that is the
               declared width already in hand, so it resolves (it is a cycle in §17.5.2.2). Step 2 below does
               the same. */
            width = css_length_resolve_pct(cw, declared);
        } else {
            DFAILF("%s: CSS 2.1 §17.5.2.1 Fixed table layout's step 1 reads \"A column element with a value "
                   "other than 'auto' for the 'width' property\", and this column box's computed `width` is "
                   "neither a length, a percentage nor `auto`. css-sizing-3 §3.1.1 \"Preferred Size Properties: "
                   "the width and height properties\" carries the intrinsic sizing keywords in that grammar, "
                   "and every one of them is a measurement of CONTENT — which this algorithm's own first "
                   "sentence forbids: \"the horizontal layout of the table does not depend on the contents of "
                   "the cells\", and a column box has no content of its own to measure instead (CSS 2.1 §17.2 "
                   "The CSS table model: the column boxes \"are not rendered (exactly as if they had 'display: "
                   "none')\"). So there is no arm here that is this algorithm's, and what CSS 2.1 leaves open "
                   "is whether such a column counts as `auto` for step 1. RECORD whichever reading is built "
                   "rather than guessing it",
                   box_subject(boxes.cols[i].column, nbuf, sizeof nbuf));
            continue;
        }
        out->columns[i] = css_px_max(width, css_px(0.0));
        assigned[i] = true;
    }
    /* Step 2: "Otherwise, a cell in the first row with a value other than 'auto' for the 'width' property
       determines the width for that column. If the cell spans more than one column, the width is divided over
       the columns." The division is equal, a recorded reading (§17.5.2.2's step 3 suggests "approximately the
       same amount"). The first cell to reach a column determines it ("Otherwise", "a cell"), in
       core/layout/table_grid.h's first-row tree order. */
    for (k = 0; k < grid->ncells && n > 0; k++) {
        const TableGridCell *cell = &grid->cells[k];
        CssLength cw;
        CssPx width = css_px(0.0);
        size_t c;

        if (cell->row != 0) continue;
        /* §17.2.1 Anonymous table objects' anonymous cell has no element and declares no `width`. */
        if (cell->element == NULL) continue;
        cw = css_computed_length(cell->element, "width");
        if (css_length_is_keyword(cw, CSS_KEYWORD_AUTO)) continue;
        if (cw.kind == CSS_LENGTH_ABSOLUTE) {
            width = cw.px;
        } else if (cw.kind == CSS_LENGTH_PERCENTAGE || cw.kind == CSS_LENGTH_CALCULATED) {
            /* "A percentage value for a column width is relative to the table width": the declared width, in hand. */
            width = css_length_resolve_pct(cw, declared);
        } else {
            DFAILF("%s: CSS 2.1 §17.5.2.1 Fixed table layout's step 2 reads \"a cell in the first row with a "
                   "value other than 'auto' for the 'width' property\", and this cell's computed `width` is "
                   "neither a length, a percentage nor `auto`. css-sizing-3 §3.1.1 \"Preferred Size Properties: "
                   "the width and height properties\" carries the intrinsic sizing keywords in that grammar, "
                   "and every one of them is a measurement of the CELL'S CONTENT — which this algorithm's own "
                   "first sentence forbids it to make: \"the horizontal layout of the table does not depend on "
                   "the contents of the cells\". So there is no arm here that is this algorithm's, and what CSS "
                   "2.1 leaves open is whether such a cell counts as `auto` for step 2. RECORD whichever "
                   "reading is built rather than guessing it",
                   box_subject(cell->element, nbuf, sizeof nbuf));
        }
        width = css_px_max(width, css_px(0.0));
        /* A column width is a border-box width, so a content-box cell width gets the cell's padding and border
           added, and a border-box one is floored at that sum (css-sizing-3 §3.3's subtraction "and flooring
           the result at zero" read from the outside). */
        {
            CssPx cell_edges = table_cell_border_edges(cell->element);
            char *sizing = tw_computed(cell->element, "box-sizing");

            if (strcmp(sizing, "border-box") == 0) width = css_px_max(width, cell_edges);
            else width = css_px_add(width, cell_edges);
            free(sizing);
        }
        DCHECK(cell->col + cell->colspan <= n,
               "CSS 2.1 §17.5.2.1's step 2 found a first-row cell spanning past the grid's own column count. "
               "core/layout/table_grid.h grows `ncols` to `col + colspan` for every cell it places, so this is "
               "a grid whose cell array and column count were carried apart");
        for (c = cell->col; c < cell->col + cell->colspan; c++) {
            if (assigned[c]) continue;
            out->columns[c] = css_px_scale(width, 1.0 / (double) cell->colspan);
            assigned[c] = true;
        }
    }
    for (i = 0; i < n; i++) {
        if (assigned[i]) assigned_sum = css_px_add(assigned_sum, out->columns[i]);
        else remaining++;
    }
    /* Step 3: "Any remaining columns equally divide the remaining horizontal table space (minus borders or cell
       spacing)." The table space is the declared width; the spacing is §17.6.1's term. */
    avail = css_px_max(css_px_sub(css_px_sub(declared, spacing_total), assigned_sum), css_px(0.0));
    if (remaining > 0) {
        CssPx share = css_px_scale(avail, 1.0 / (double) remaining);

        for (i = 0; i < n; i++) if (!assigned[i]) out->columns[i] = share;
    }
    sum = spacing_total;
    for (i = 0; i < n; i++) sum = css_px_add(sum, out->columns[i]);
    /* "The width of the table is then the greater of the value of the 'width' property for the table element
       and the sum of the column widths (plus cell spacing or borders). If the table is wider than the columns,
       the extra space should be distributed over the columns." Step 3 has already spent the surplus unless the
       first row declared every column. */
    out->content = css_px_max(declared, sum);
    tw_distribute(out->columns, NULL, n, css_px_sub(out->content, sum));
    free(assigned);
    table_column_boxes_release(&boxes);
}

void table_widths(lxb_dom_element_t *table, const TableGrid *grid, TableUsedWidths *out)
{
    CssBorderSpacing spacing;
    CssPx used_spacing, edges, declared = css_px(0.0), check_sum, slack;
    char *layout;
    bool fixed, has_declared, collapsing;
    size_t i;

    DCHECK(table != NULL && grid != NULL && out != NULL,
           "CSS 2.1 §17.5.2's table width was asked for with no element, no grid or nowhere to put it. The grid "
           "is the OPERAND every column in the section is stated over, and nothing before "
           "core/layout/table_grid.h says which cells occupy which column");
    DCHECK(grid->cells != NULL || grid->ncells == 0,
           "CSS 2.1 §17.5.2 was handed a grid with no cell array and a non-zero cell count — `table_grid_build` "
           "stores NULL only for a table whose rows generate no cell, so the two have been carried apart since "
           "it answered");
    tw_require_table_box(table);
    /* §17.6's model, asked once: `border-collapse` applies to "'table' and 'inline-table' elements". */
    collapsing = table_border_collapse_selected(table);
    /* §17.6.1's `Computed value: two absolute lengths`, answered whole by core/css/css_computed_value.h. Only
       the horizontal half is a width term; the vertical half is CSS 2.1 §17.5.3 Table height algorithms'. */
    spacing = css_computed_border_spacing(table);
    DCHECK(spacing.horizontal.px >= 0.0,
           "CSS 2.1 §17.6.1 The separated borders model states \"Lengths may not be negative\" of "
           "`border-spacing`, and core/css/css_computed_value.c asserts that where it computes the pair — so a "
           "negative reaching here is arithmetic that lost a sign rather than a document");
    used_spacing = tw_used_spacing(spacing.horizontal, collapsing);
    edges = tw_table_edges(table, collapsing);
    layout = tw_computed(table, "table-layout");
    fixed = strcmp(layout, "fixed") == 0;
    DCHECK(fixed || strcmp(layout, "auto") == 0,
           "a `table-layout` computed to something that is neither of the two keywords CSS 2.1 §17.5.2 Table "
           "width algorithms: the 'table-layout' property's `auto | fixed | inherit` grammar admits, and that "
           "section gives it `Computed value: as specified` over a keyword-only value — so nothing between the "
           "declaration and here had a rule that could produce a third one");
    free(layout);
    has_declared = tw_declared_content_width(table, edges, &declared);
    /* The dispatch is a conjunction: the fixed algorithm is mandatory when selected ("except when the fixed
       layout algorithm is selected"), but §17.5.2.1 hands a `width: auto` table to the automatic one, which
       has no table space for step 3 otherwise. See table_width.h for the declined §10.3.3 arm. */
    if (fixed && has_declared) tw_fixed_layout(table, grid, used_spacing, declared, out);
    else tw_auto_layout(table, grid, used_spacing, has_declared, declared, out);
    /* The spacing is stored from the one read the layout ran under, past both algorithms (see table_width.h). */
    out->spacing = used_spacing;
    DCHECK(out->ncols == grid->ncols && (out->columns != NULL || out->ncols == 0),
           "CSS 2.1 §17.5.2 answered a different number of used column widths than the grid it was run over "
           "holds, or a non-zero count with no array — every consumer of this answer indexes it by the grid's "
           "own column number, so the two must not come apart");
    check_sum = tw_spacing_total(used_spacing, out->ncols);
    for (i = 0; i < out->ncols; i++) {
        DCHECK(out->columns[i].px >= 0.0,
               "CSS 2.1 §17.5.2 gave a grid column a NEGATIVE used width. Every term is an intrinsic size, a "
               "padding, a border width, a declared length floored at zero, or a share of a surplus this "
               "component only adds when it is positive — so this is arithmetic that lost a sign, and the "
               "rectangle it would put through CSSOM VIEW is one no reader can tell from a measured one");
        check_sum = css_px_add(check_sum, out->columns[i]);
    }
    slack = css_px_sub(check_sum, out->content);
    if (slack.px < 0.0) slack = css_px_sub(out->content, check_sum);
    /* §17.6.1's definition of the table's width, asserted: "The width of the table is the distance from the left
       inner padding edge to the right inner padding edge (including the border spacing but excluding padding
       and border)", which is the used columns plus the spacing, so `columns` and `content` describe one box.
       A zero-column table is exempt (no cells, yet its declared `width` and CAPMIN still apply). The tolerance
       is IEEE rounding: `tw_distribute` and this sum add the same terms in different orders.
       Under §17.6.2 the spacing is zero and the relation is `content` equals the columns: the row-width
       equation's two half-borders are the table box's (in `edges`), perimeter cells are charged neither, and
       each interior line is charged once as two neighbouring halves. */
    DCHECK(out->ncols == 0 ||
               slack.px <= 1e-9 * (1.0 + (out->content.px < 0.0 ? -out->content.px : out->content.px)),
           "CSS 2.1 §17.6 Borders' definition of a table's width — §17.6.1 The separated borders model's "
           "distance from its left inner padding edge to its right inner padding edge including the border "
           "spacing, or §17.6.2 The collapsing border model's same distance with no spacing in it — is exactly "
           "the used column widths plus this component's spacing term, and this answer's two halves disagree "
           "by more than the distribution's own rounding. A consumer placing cells from the columns and sizing "
           "the box from the content width would lay them out in two different rectangles");
    DCHECK(out->content.px >= 0.0,
           "CSS 2.1 §17.5.2 gave a table a NEGATIVE used width. Every operand of both algorithms' final "
           "comparison is a floored declared length, a containing block width, CAPMIN or a sum of non-negative "
           "column widths");
}

/* ---- css-sizing-3 §5.1's pair for a table box ---------------------------------------------------------------
   §17.5.2.2 Automatic table layout's second final rule at css-sizing-3 §5.1's two hypothetical containing
   blocks; see core/layout/table_width.h. Nothing here reads the containing block: `table_column_widths` reads
   none, `tw_capmin` zeroes cyclic percentages (css-sizing-3 §5.2.1), and the table's `width` is removed by
   §5.1 ("given an auto preferred size in that axis"). Adding such a read recurses through CSS 2.2 §10.3.5's
   shrink-to-fit for a table in a float. `tw_table_edges` would be one (its percentage padding resolves
   against the containing block) and is not needed: the answer is a content-box width and the table's own
   edges are the caller's css-sizing-3 §3.3 conversion. */
IntrinsicInlineSizes table_intrinsic_inline_sizes(lxb_dom_element_t *table, const TableGrid *grid)
{
    TableColumnWidth *head = NULL;
    IntrinsicInlineSizes out;
    TwColumnSums sums;
    CssBorderSpacing spacing;
    CssPx capmin;
    bool collapsing;
    size_t n;

    DCHECK(table != NULL && grid != NULL,
           "css-sizing-3 §5.1's intrinsic inline sizes were asked of a table with no element or no grid. The "
           "grid is the OPERAND §17.5.2.2 Automatic table layout's four steps are stated over, and nothing "
           "before core/layout/table_grid.h says which cells occupy which column");
    tw_require_table_box(table);
    /* §17.6's model and §17.6.1's spacing, read as `table_widths` reads them; the spacing is a term of MIN and
       MAX ("plus cell spacing or borders") and comes from `tw_used_spacing`, not the property. */
    collapsing = table_border_collapse_selected(table);
    spacing = css_computed_border_spacing(table);
    n = table_column_widths(table, grid, &head);
    DCHECK(n == grid->ncols,
           "CSS 2.1 §17.5.2.2's four steps answered a different number of columns than the grid they were run "
           "over holds — that entry answers `grid->ncols` and nothing else, so the two have come apart");
    sums = tw_column_sums(head, n, tw_spacing_total(tw_used_spacing(spacing.horizontal, collapsing), n));
    free(head);
    /* CAPMIN is a term of both sizes: §17.5.2.2's rule holds it at both containing blocks, max(CAPMIN, MIN) at
       zero and max(MAX, CAPMIN) at infinity. The caption is the wrapper's (§17.4), but §17.5.2.2 floors the
       table's own width by CAPMIN and the wrapper takes its width from that, here as in `table_widths`. */
    capmin = tw_capmin(table);
    out.min_content = css_px_max(sums.min, capmin);
    out.max_content = css_px_max(sums.max, capmin);
    DCHECK(out.min_content.px >= 0.0 && out.max_content.px >= out.min_content.px,
           "css-sizing-3 §5.1's intrinsic inline sizes of a TABLE box came out NEGATIVE or CROSSED. Each is a "
           "maximum of CAPMIN with one of §17.5.2.2's two sums, both of which `tw_column_sums` has just "
           "asserted non-negative and uncrossed and `tw_capmin` non-negative — and a maximum taken against ONE "
           "common operand preserves the order of the other two, so a crossed pair here is arithmetic rather "
           "than a document. It is asserted at the boundary because core/layout/intrinsic_size.h's consumers "
           "read the pair as two ends of CSS 2.2 §10.3.5's clamp, where a crossed pair silently returns the "
           "wrong end");
    return out;
}

/* See table_width.h for the reading the N-1 spacings encode and for why this is a border-box number. */
CssPx table_cell_used_border_box(const TableUsedWidths *widths, const TableGridCell *cell)
{
    CssPx w;
    size_t c;

    DCHECK(widths != NULL && cell != NULL,
           "CSS 2.1 §17.5's cell width was asked for with no table width answer or no grid cell — a cell's "
           "used width IS the used width of the columns it covers, so neither operand has a substitute");
    DCHECK(widths->columns != NULL || widths->ncols == 0,
           "CSS 2.1 §17.5.2's answer was indexed for a cell while holding no column array and a non-zero "
           "column count — `table_widths` stores NULL only for a table with no columns, so the two have been "
           "carried apart since it answered");
    DCHECK(cell->colspan >= 1,
           "CSS 2.1 §17.5 Visual layout of table contents' grid reported a cell covering NO column, and that "
           "section states the floor in the sentence that defines a cell's rectangle: \"Each cell is thus a "
           "rectangular box, one or more grid cells wide and high\". core/layout/table_grid.h answers that "
           "field and its own contract puts the same floor of one on it, so a zero here is that guarantee "
           "having been lost between the placement and this read");
    DCHECK(cell->col + cell->colspan <= widths->ncols,
           "CSS 2.1 §17.5's grid placed a cell past the last column CSS 2.1 §17.5.2 Table width algorithms: "
           "the 'table-layout' property gave a used width. `table_grid_build` grows `ncols` to `col + colspan` "
           "for every cell it places and `table_widths` answers one width per grid column, so this is a cell "
           "and a width array from TWO DIFFERENT GRIDS — the caller walked to the wrong table box, and the "
           "widths it would take out of this one are real widths of another table's columns");
    w = css_px(0.0);
    for (c = cell->col; c < cell->col + cell->colspan; c++) w = css_px_add(w, widths->columns[c]);
    /* The N-1 spacings, added only when there are any: multiplying `border-spacing` by zero would still union
       its environment facts into the answer (core/css/css_length.h). */
    if (cell->colspan > 1)
        w = css_px_add(w, css_px_scale(widths->spacing, (double) (cell->colspan - 1)));
    DCHECK(w.px >= 0.0,
           "CSS 2.1 §17.5 gave a cell a NEGATIVE used border-box width. Every term is a used column width, "
           "which `table_widths` asserts non-negative where it answers, or a multiple of `border-spacing`, "
           "which CSS 2.1 §17.6.1 The separated borders model states \"Lengths may not be negative\" of — so "
           "this is arithmetic that lost a sign rather than a document");
    return w;
}

void table_widths_release(TableUsedWidths *w)
{
    if (w == NULL) return;
    DCHECK(w->columns != NULL || w->ncols == 0,
           "CSS 2.1 §17.5.2's answer was released with a non-zero column count and no array — `table_widths` "
           "stores NULL only for a table with no columns");
    free(w->columns);
    w->columns = NULL;
    w->ncols = 0;
}
