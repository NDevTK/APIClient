/* CSS 2.1 §17.5.3 Table height algorithms, over core/layout/table_grid.h's grid and core/layout/table_box.h's
   rows. See table_height.h for the contract, for which box each number is measured in, for the reduction of
   §17.5.3's alignment procedure to MIN, and for every place the section's own declines are taken as choices. */
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/css/css_property_applies.h"
#include "core/layout/block_flow.h"
#include "core/layout/box_subject.h"
#include "core/layout/replaced_element.h"
#include "core/layout/table_border_collapse.h"
#include "core/layout/table_box.h"
#include "core/layout/table_grid.h"
#include "core/layout/table_height.h"
#include "core/layout/table_wrapper.h"
#include "core/layout/used_value.h"

static char *th_computed(lxb_dom_element_t *el, const char *name)
{
    char *v = css_computed_value(el, name);

    DCHECK(v != NULL, "the cascade produced no computed value for a property this engine models — every one of "
                      "them is in lexbor's registry with an initial value, so the last layer always answers");
    return v;
}

/* ---- §17.6 Borders' TWO MODELS, asked once, at the entry ---------------------------------------------------
   §17.6's `border-collapse` selects between §17.6.1 The separated borders model and §17.6.2 The collapsing
   border model, and this is the VERTICAL twin of core/layout/table_width.c's split rather than a second copy
   of it: that one is about the terms of a WIDTH — the horizontal spacing and the table's own left and right
   edges — and this one is about the terms of a HEIGHT, which §17.6.2 changes independently. A caller reaching
   §17.5.3 without having gone through §17.5.2 (a table whose height is asked for and whose width is not) meets
   its own arm here rather than none.
   §17.5.3's SUM IS "the row heights plus any cell spacing or borders", and the cell spacing is §17.6.1's
   VERTICAL `border-spacing`, which §17.6.2 gives no meaning to at all — "Borders are centered on the grid
   lines between the cells" and the cells meet, so what stands between two rows there is the resolved border
   and not a spacing. The ROW HEIGHTS need no arm at this level: they are maxima over CELL BOXES, and
   `th_cell_vertical_edges_pair` below answers a cell's vertical edges under either model. */
static CssPx th_used_spacing(CssPx vertical, bool collapsing)
{
    return collapsing ? css_px(0.0) : vertical;
}

/* ---- §17.6.1's CELL SPACING, ON THE BLOCK AXIS -------------------------------------------------------------
   The vertical twin of core/layout/table_width.c's `tw_spacing_total`, over the same two §17.6.1 sentences:
   BETWEEN rows it is the property itself, and at the TWO EDGES of the table it is stated separately — "The
   distance between the table border and the borders of the cells on the edge of the table is the table's
   padding for that side, plus the relevant border spacing distance", which is written "for that side" and
   therefore covers the top and the bottom exactly as it covers the left and the right. So the spacing appears
   `nrows + 1` times, and the table's own padding is NOT part of it (§17.6.1's next sentence excludes padding
   and border from the table's own extent by name).
   A TABLE WITH NO ROWS HAS NO SUCH DISTANCE AT ALL, which is a derivation from those sentences rather than a
   floor chosen here: both are stated over the borders of CELLS, and a table with no rows has none. */
static CssPx th_spacing_total(CssPx spacing, size_t nrows)
{
    if (nrows == 0) return css_px(0.0);
    return css_px_scale(spacing, (double) (nrows + 1));
}

/* ---- ONE CELL'S VERTICAL PADDING AND BORDER --------------------------------------------------------------
   See table_height.h for why this lives beside §17.5.3 rather than beside its horizontal twin, and for CSS 2.1
   §8.4's own undefined case that makes a percentage a crash rather than a lookup. The two sides are reported
   separately to the callers inside this file because §17.5.3's cell BASELINE needs the top alone — "if there
   is no such line box or table-row, the baseline is the bottom of content edge of the cell box" is the box
   height less the BOTTOM pair — while the box height needs the total. */
static void th_cell_vertical_edges_pair(lxb_dom_element_t *cell, CssPx *top, CssPx *bottom)
{
    static const char *const PADDINGS[2] = { "padding-top", "padding-bottom" };
    static const char *const BORDERS[2] = { "border-top-width", "border-bottom-width" };
    CssPx side[2];
    TableCollapsedEdges charge;
    lxb_dom_element_t *table;
    bool collapsing;
    char nbuf[160];
    int i;

    /* §17.5.3's row height is a maximum over CELL BOXES, and a cell box is its border box — so the claim that
       nothing else stands between a cell and its row rests on the vertical margins not applying to it. It is
       ASKED of the component that owns CSS 2.1 §8.3 "Margin properties"' Applies-to line rather than restated,
       and a day the two disagree is a day this walk stops rather than under-reporting every row by two
       margins. */
    DCHECK(!css_property_applies(cell, "margin-top") && !css_property_applies(cell, "margin-bottom"),
           "CSS 2.1 §17.5.3 Table height algorithms took a cell box to be its BORDER box, and CSS 2.1 §8.3 "
           "\"Margin properties\"' Applies-to line now says the vertical margins DO apply to this box — so a "
           "cell's contribution to its row has a term this walk is not adding, and §17.5's own \"the edges "
           "coincide with the border edges of cells\" no longer places the row where this component puts it");
    /* §17.6's `border-collapse` is `Inherited: yes` and its Applies-to line is "'table' and 'inline-table'
       elements", so the model is a fact about the TABLE and one ask covers both of this cell's sides. */
    table = table_box_table_of(cell);
    collapsing = table_border_collapse_selected(table);
    charge.top = css_px(0.0);
    charge.right = css_px(0.0);
    charge.bottom = css_px(0.0);
    charge.left = css_px(0.0);
    if (collapsing) charge = table_collapsed_cell_edges(cell);
    for (i = 0; i < 2; i++) {
        CssLength pad = css_computed_length(cell, PADDINGS[i]);
        CssLength bor;

        if (pad.kind != CSS_LENGTH_ABSOLUTE)
            DFAILF("%s: a table cell's `%s` is not an absolute length, and CSS 2.1 §17.5.3 Table height "
                   "algorithms cannot resolve it. CSS 2.1 §8.4 \"Padding properties: 'padding-top', "
                   "'padding-right', 'padding-bottom', 'padding-left', and 'padding'\" refers a padding "
                   "percentage to the containing block's WIDTH — \"even for 'padding-top' and "
                   "'padding-bottom'\" — and then names THIS case as the one it does not define: \"If the "
                   "containing block's width depends on this element, then the resulting layout is undefined "
                   "in CSS 2.1.\" For a cell that rectangle is the TABLE BOX's content width, and CSS 2.1 "
                   "§17.5.2.2 Automatic table layout's MCW makes it depend on this very cell — so the "
                   "resolution is a CYCLE and not a missing lookup, and it is the identical cycle "
                   "core/layout/table_column_width.c refuses on the other axis. A KEYWORD cannot arrive here "
                   "at all: CSS 2.1 §8.4's `Value:` line is `<padding-width> | inherit` and its `Computed "
                   "value:` line is \"the percentage as specified or the absolute length\"",
                   box_subject(cell, nbuf, sizeof nbuf), PADDINGS[i]);
        if (collapsing) {
            /* §17.6.2 replaces this cell's OWN border with half the resolution at the grid line it abuts —
               top first, then bottom, which is the order `TableCollapsedEdges` states its four sides in and
               the order this loop runs in. */
            side[i] = css_px_add(pad.px, i == 0 ? charge.top : charge.bottom);
        } else {
            bor = css_computed_length(cell, BORDERS[i]);
            DCHECK(bor.kind == CSS_LENGTH_ABSOLUTE,
                   "a `border-*-width` computed to something that is not an absolute length. css-backgrounds-3 "
                   "§3.3 \"Line Thickness: the border-width properties\"' `Computed value:` line is `absolute "
                   "length, snapped as a border width` and every arm of that derivation produces one, so a "
                   "percentage or a keyword here is a rule that did not run");
            side[i] = css_px_add(pad.px, bor.px);
        }
        DCHECK(side[i].px >= 0.0,
               "CSS 2.1 §17.5.3's cell edges came out NEGATIVE on one side. CSS 2.1 §8.4 states outright that "
               "\"unlike margin properties, values for padding values cannot be negative\" and "
               "css-backgrounds-3 §3.3's <line-width> is a non-negative <length>, so lexbor drops either "
               "declaration; a negative here is arithmetic that lost a sign rather than a document");
    }
    *top = side[0];
    *bottom = side[1];
}

CssPx table_cell_vertical_edges(lxb_dom_element_t *cell)
{
    CssPx top, bottom;

    DCHECK(cell != NULL, "a cell's vertical edges were asked for with no element");
    th_cell_vertical_edges_pair(cell, &top, &bottom);
    return css_px_add(top, bottom);
}

/* ---- §17.5.3's CELL BOX HEIGHT ------------------------------------------------------------------------------
   "In CSS 2.1, the height of a cell box is the minimum height required by the content" — which is CSS 2.1
   §10.6.3's content-based height for a block container, over the cell's USED WIDTH, converted to the BORDER box
   the row is measured in. The used width is not read here and is not an argument either: core/layout/
   block_flow.h's walk resolves each descendant's containing block through §10.1, whose second case lands on
   this cell, and core/layout/used_value.c answers a CELL's own width out of §17.5.2's column widths — so the
   operand arrives through the ordinary chain and nothing in this file composes it.
   §17.5.3's NEXT SENTENCE IS WHY THE DECLARED `height` IS NOT READ HERE: "The table cell's 'height' property
   can influence the height of the row (see above), but it does not increase the height of the cell box." So a
   declared cell height is a TERM OF THE ROW'S MAXIMUM and never this number, and reading it here would count it
   twice.
   §10.7's CLAMP IS NOT APPLIED, and that is §10.7's own sentence rather than an omission: "In CSS 2.1, the
   effect of 'min-height' and 'max-height' on tables, inline tables, table cells, table rows, and row groups is
   undefined." Running the clamp would be this engine deciding a case the standard declines to. */
static CssPx th_cell_box_height(lxb_dom_element_t *cell)
{
    ReplacedElement rep = replaced_element_of(cell);
    char nbuf[160];

    if (rep.replaced)
        DFAILF("%s: CSS 2.1 §17.5.3 Table height algorithms gives a cell box's height as \"the minimum height "
               "required by the content\", which for a block container is CSS 2.1 §10.6.3's walk — and this "
               "cell is a REPLACED element, whose height is §10.6.2's over its own natural dimensions and not "
               "a walk over children it does not have. CSS 2.1 §17.2 The CSS table model is what puts it here "
               "at all: \"Replaced elements with these 'display' values are treated as their given display "
               "types during layout.\" BUILD the arm: the cell box's height is then §10.6.2's used height "
               "(core/layout/replaced_element.h's natural dimensions, or CSS 2.1 §10.6.2's 300x150 default) "
               "plus this cell's own vertical padding and border, and every other term of §17.5.3's row "
               "maximum is unchanged. It is NOT reachable through core/layout/used_value.c's replaced arm, "
               "which crashes for a replaced TABLE BOX one classification up and never sees a cell",
               box_subject(cell, nbuf, sizeof nbuf));
    return css_px_add(block_flow_auto_height(cell), table_cell_vertical_edges(cell));
}

/* ---- §17.5.3's FOUR CELL ALIGNMENTS -------------------------------------------------------------------------
   The section's own list — "In the context of tables, values for 'vertical-align' have the following meanings:
   baseline / top / bottom / middle", with a closed catch-all beneath it, "sub, super, text-top, text-bottom,
   <length>, <percentage> … These values do not apply to cells; the cell is aligned at the baseline instead."
   READ THROUGH css-inline-3's LONGHANDS BECAUSE THAT IS WHAT THIS CASCADE HOLDS. See table_height.h for the
   whole argument and for §4.2's own mapping note; the order below is that note's order — `baseline-shift`'s
   `top` and `bottom` first, then `alignment-baseline`'s `middle` — and everything else is §17.5.3's catch-all
   arm, which is why there is no `else` deciding anything. */
typedef enum {
    TH_ALIGN_BASELINE = 0,
    TH_ALIGN_TOP,
    TH_ALIGN_BOTTOM,
    TH_ALIGN_MIDDLE
} ThAlign;

static ThAlign th_cell_align(lxb_dom_element_t *cell)
{
    CssLength shift = css_computed_length(cell, "baseline-shift");
    char *ab;
    bool middle;

    if (shift.kind == CSS_LENGTH_KEYWORD && strcmp(shift.keyword, "top") == 0) return TH_ALIGN_TOP;
    if (shift.kind == CSS_LENGTH_KEYWORD && strcmp(shift.keyword, "bottom") == 0) return TH_ALIGN_BOTTOM;
    ab = th_computed(cell, "alignment-baseline");
    middle = strcmp(ab, "middle") == 0;
    free(ab);
    return middle ? TH_ALIGN_MIDDLE : TH_ALIGN_BASELINE;
}

/* §17.5.3's CELL BASELINE: "The baseline of a cell is the baseline of the first in-flow line box in the cell,
   or the first in-flow table-row in the cell, whichever comes first. If there is no such line box or
   table-row, the baseline is the bottom of content edge of the cell box." Measured from the cell box's own TOP
   BORDER EDGE, because that is the frame the row's own "maximum distance between the top of the cell box and
   the baseline" is stated in.
   THE SECOND ARM IS core/layout/block_flow.c's CRASH AND NOT AN ABSENCE HERE: a nested table reached before any
   line box aborts inside that walk naming §17.5.3, at the one point that knows which came first. So a `true`
   from the entry below is the FIRST arm and a `false` is the THIRD, with no third possibility left to test. */
static CssPx th_cell_baseline(lxb_dom_element_t *cell, CssPx box_height)
{
    CssPx top, bottom, inner = css_px(0.0);

    th_cell_vertical_edges_pair(cell, &top, &bottom);
    if (block_flow_first_line_box_baseline(cell, &inner)) return css_px_add(top, inner);
    return css_px_sub(box_height, bottom);
}

/* ---- §17.5.3's MIN ------------------------------------------------------------------------------------------
   The four-step alignment procedure reduced to two maxima — see table_height.h for the derivation, for why
   `max(H)` alone is a FLOOR and not the answer, and for the proof that a row with at most one baseline-aligned
   cell needs no baseline measured at all.
   `cells` are the cell boxes ANCHORED in this row whose rectangle is one row high; a row-SPANNING cell is
   §17.5.3's own declined case and is handled by the constraint pass at the entry, not here. */
typedef struct {
    lxb_dom_element_t *element;
    CssPx              height;
    ThAlign            align;
} ThRowCell;

static CssPx th_row_min(const ThRowCell *cells, size_t n)
{
    CssPx tallest = css_px(0.0), above = css_px(0.0), below = css_px(0.0);
    size_t i, nbaseline = 0;

    for (i = 0; i < n; i++) {
        tallest = css_px_max(tallest, cells[i].height);
        if (cells[i].align == TH_ALIGN_BASELINE) nbaseline++;
    }
    /* THE EXACT ARM, TAKEN WITHOUT MEASURING A BASELINE. With no baseline cell the procedure's first two steps
       contribute nothing and every remaining step is a maximum of cell box heights; with exactly one, that
       cell's own `A + (H - A)` is its `H`, which the same maximum already holds. Either way MIN is `max(H)` by
       the procedure itself — so this is §17.5.3 running, not a case it is being spared. */
    if (nbaseline <= 1) return tallest;
    for (i = 0; i < n; i++) {
        CssPx a;

        if (cells[i].align != TH_ALIGN_BASELINE) continue;
        a = th_cell_baseline(cells[i].element, cells[i].height);
        DCHECK(a.px >= -1e-9 && a.px <= cells[i].height.px + 1e-9,
               "CSS 2.1 §17.5.3 Table height algorithms put a cell's BASELINE outside its own cell box. The "
               "section measures it from the top of the cell box — either to \"the baseline of the first "
               "in-flow line box\", which core/layout/block_flow.h reports as an offset inside this box's "
               "content edge, or to \"the bottom of content edge of the cell box\", which is the box height "
               "less its bottom padding and border. Both are inside the border box by construction, so a "
               "baseline outside it is a frame conversion that added the wrong edge — and the row height built "
               "on it would be a real distance measured from the wrong origin");
        above = css_px_max(above, a);
        below = css_px_max(below, css_px_sub(cells[i].height, a));
    }
    /* Step 1 establishes the row's baseline at `above` and step 3's provisional height reaches `above + below`;
       steps 2 and 3 then raise it to any taller cell of the other three alignments, which `tallest` holds. */
    return css_px_max(css_px_add(above, below), tallest);
}

/* ---- §17.5.3's THREE TERMS ---------------------------------------------------------------------------------
   "the maximum of the row's computed 'height', the computed 'height' of each cell in the row, and the minimum
   height (MIN) required by the cells" — this reads ONE of the first two terms off ONE element.
   `auto` CONTRIBUTES NOTHING, which is §17.5.3's own next sentence for a row ("A 'height' value of 'auto' for a
   'table-row' means the row height used for layout is MIN") and is the only reading that leaves MIN in charge.
   A PERCENTAGE CONTRIBUTES NOTHING EITHER, AND THAT IS A RECORDED CHOICE RATHER THAN A DERIVATION: §17.5.3 says
   "CSS 2.1 does not define how the height of table cells and table rows is calculated when their height is
   specified using percentage values", so no reading is unconforming and this one is chosen because it is the
   `auto` arm the same element would have taken with no declaration — the alternative, resolving it against the
   table's own height, is a cycle, since that height is the sum this term is an input to.
   THE ELEMENT MAY BE ABSENT and that is not a missing lookup: §17.2.1 Anonymous table objects generates rows
   and cells that no element names, and CSS 2 §9.2.1.1 Anonymous block boxes gives such a box the INITIAL value
   of every non-inherited property — `height: auto` — so an anonymous row or cell contributes nothing here by
   the same arm a declared `auto` takes. */
static CssPx th_declared_term(lxb_dom_element_t *el)
{
    CssLength h;

    if (el == NULL) return css_px(0.0);
    h = css_computed_length(el, "height");
    if (h.kind == CSS_LENGTH_ABSOLUTE) return css_px_max(h.px, css_px(0.0));
    return css_px(0.0);
}

/* ---- THE TABLE'S OWN DECLARED HEIGHT, WHICH §17.5.3 MAKES A MINIMUM ---------------------------------------
   "Any other value is treated as a minimum height." The `auto` question is asked as css-sizing-3 §3.2.1's
   BEHAVES AS AUTO rather than as a computed `auto`, for core/layout/used_value.h's reason: a percentage height
   whose containing block's height is indefinite behaves as auto, and §3.2.1's note says legacy CSS2 conditions
   phrased over a computed `auto` "should be interpreted as meaning behaves as auto".
   A PERCENTAGE THAT RESOLVES CRASHES rather than being read, and the reason is a MISSING BASIS rather than an
   unnameable box. §10.5 resolves it against the containing block's HEIGHT, and CSS 2.1 §17.4 Tables in the
   visual formatting model says outright which rectangle that is for a table: "Percentages on 'width' and
   'height' on the table are relative to the table wrapper box's containing block, not the table wrapper box
   itself." — so it is the table element's ordinary ancestor box, which §10.1's walk already answers, and NOT
   the wrapper. That rectangle's WIDTH is exported (`used_value_containing_block_width`); its HEIGHT is not,
   and there is nothing else to build: the predicate on the line above has already established that the basis
   is DEFINITE, since a percentage over an indefinite one behaves as auto and returns before the read.
   AN EARLIER FORM OF THIS PARAGRAPH SAID THE WRAPPER WAS ANONYMOUS AND THAT READING THE ANCESTOR WOULD BE A
   RECTANGLE ONE LEVEL TOO FAR OUT. Both halves were wrong, and the second was SPEC-WRONG in the direction that
   gets executed: §17.4 calls the wrapper the table's PRINCIPAL block box, generated by the table element, and
   the sentence quoted above sends this very percentage to the ancestor the old text ruled out. */
static bool th_declared_minimum(lxb_dom_element_t *table, CssPx *out)
{
    CssLength h;
    char nbuf[160];

    *out = css_px(0.0);
    if (used_value_height_behaves_as_auto(table)) return false;
    h = css_computed_length(table, "height");
    if (h.kind != CSS_LENGTH_ABSOLUTE)
        DFAILF("%s: a table box whose `height` is a PERCENTAGE that RESOLVES — css-sizing-3 §3.2.1 "
               "\"“Behaving as auto”\" says it does not behave as auto, so CSS 2.1 §17.5.3 Table "
               "height algorithms' \"Any other value is treated as a minimum height\" applies to it and the "
               "minimum has to be a number. CSS 2.1 §10.5 resolves it against the CONTAINING BLOCK's height, "
               "and CSS 2.1 §17.4 Tables in the visual formatting model says which rectangle that is in one "
               "sentence: \"Percentages on 'width' and 'height' on the table are relative to the table "
               "wrapper box's containing block, not the table wrapper box itself.\" That is the table "
               "element's ordinary ancestor box — the box §10.1's own walk answers for a table element — and "
               "it is DEFINITE here, because the css-sizing-3 predicate one line above returns for a "
               "percentage whose basis is not. WHAT IS MISSING IS THE BASIS AND NOT THE BOX. BUILD the HEIGHT "
               "twin of `used_value_containing_block_width` in core/layout/used_value.h: core/layout/"
               "used_value.c already computes it privately as `uv_cb_height` over the same `uv_cb` walk, and "
               "only the width half of that pair is exported. DO NOT REACH FOR `used_value_px(table, "
               "\"height\")` INSTEAD — it dispatches a table box to `uv_table_used_height`, which runs "
               "`table_heights`, which runs this function, so the obvious call is unbounded recursion. AN "
               "EARLIER FORM OF THIS MESSAGE CALLED THE WRAPPER AN ANONYMOUS BOX AND TOLD ITS READER TO BUILD "
               "IT: CSS 2 §9.2.1.1 Anonymous block boxes settles that the other way (its own last paragraph "
               "orders percentages past such boxes — \"Anonymous block boxes are ignored when resolving "
               "percentage values that would refer to it: the closest non-anonymous ancestor box is used "
               "instead.\"), §17.4 calls the wrapper the table's PRINCIPAL block box, and the wrapper is now "
               "an answer both §10.1's walk and §9.4.1's stack give",
               box_subject(table, nbuf, sizeof nbuf));
    *out = css_px_max(h.px, css_px(0.0));
    return true;
}

void table_heights(lxb_dom_element_t *table, const TableGrid *grid, TableUsedHeights *out)
{
    CssBorderSpacing spacing;
    TableBoxRow *rows = NULL;
    ThRowCell *bucket = NULL;
    CssPx used_spacing, declared = css_px(0.0), sum, check_sum, slack;
    char *display;
    bool is_table_box, has_declared, collapsing;
    size_t nrows, r, i;
    char nbuf[160];

    DCHECK(table != NULL && grid != NULL && out != NULL,
           "CSS 2.1 §17.5.3's table height was asked for with no element, no grid or nowhere to put it. The "
           "grid is the OPERAND every row below is stated over, and nothing before core/layout/table_grid.h "
           "says which cells are anchored in which row or how many rows each covers");
    DCHECK(grid->cells != NULL || grid->ncells == 0,
           "CSS 2.1 §17.5.3 was handed a grid with no cell array and a non-zero cell count — `table_grid_build` "
           "stores NULL only for a table whose rows generate no cell, so the two have been carried apart since "
           "it answered");
    display = th_computed(table, "display");
    is_table_box = table_box_kind_generates_table_box(table_box_kind(display));
    free(display);
    if (!is_table_box)
        DFAILF("%s: CSS 2.1 §17.5.3 Table height algorithms is stated over a TABLE box — \"The height of a "
               "table is given by the 'height' property for the 'table' or 'inline-table' element\" — and this "
               "box's computed `display` (printed above) is a different one of CSS 2.1 §17.2 The CSS table "
               "model's box types. The height of a table-internal box is NOT this question and must not be "
               "answered from it: a ROW's is one entry of `TableUsedHeights.rows`, a CELL's is the rows its "
               "rectangle covers (`table_cell_used_border_box_height` below), a ROW GROUP's is the case "
               "§17.5.3 declines outright (\"CSS 2.1 does not define the meaning of 'height' on row groups\"), "
               "and a CAPTION is not in the table box at all — §17.4 Tables in the visual formatting model "
               "renders it as a normal block box in the WRAPPER, so §10.6.3 owns its height. ROUTE the caller "
               "to the table box above this one (core/layout/table_box.h's `table_box_table_of`) and ask it "
               "for the rows the caller wants",
               box_subject(table, nbuf, sizeof nbuf));
    /* §17.6's MODEL, ASKED ONCE FOR THE WHOLE ALGORITHM — a fact about the TABLE (`border-collapse` is
       `Inherited: yes`, Applies-to "'table' and 'inline-table' elements"), so the spacing term below takes its
       arm from this one read and every cell's own edges take theirs from the same ask inside
       `th_cell_vertical_edges_pair`. */
    collapsing = table_border_collapse_selected(table);
    /* §17.6.1's `Computed value: two absolute lengths` — core/css/css_computed_value.h answers the pair whole
       because neither the text entry nor the single-length one can carry it. Only the VERTICAL half is a term
       of a height; the horizontal half is CSS 2.1 §17.5.2 Table width algorithms: the 'table-layout'
       property's. */
    spacing = css_computed_border_spacing(table);
    DCHECK(spacing.vertical.px >= 0.0,
           "CSS 2.1 §17.6.1 The separated borders model states \"Lengths may not be negative\" of "
           "`border-spacing`, and core/css/css_computed_value.c asserts that where it computes the pair — so a "
           "negative reaching here is arithmetic that lost a sign rather than a document");
    nrows = table_box_rows(table, &rows);
    DCHECK(nrows == grid->nrows,
           "CSS 2.1 §17.5 Visual layout of table contents' rule 1 makes the grid's row count the table's own "
           "row count — \"the table occupies exactly as many grid rows as there are row elements\" — and "
           "core/layout/table_grid.h states outright that it takes that number from `table_box_rows` rather "
           "than re-deriving it. Two different counts here means the two walks ran over different trees, and "
           "every row index below would name a different row in each");
    out->nrows = nrows;
    used_spacing = th_used_spacing(spacing.vertical, collapsing);
    out->spacing = used_spacing;
    out->rows = nrows == 0 ? NULL : calloc(nrows, sizeof(*out->rows));
    CHECK(nrows == 0 || out->rows != NULL, "out of memory allocating CSS 2.1 §17.5.3's row heights");
    bucket = grid->ncells == 0 ? NULL : calloc(grid->ncells, sizeof(*bucket));
    CHECK(grid->ncells == 0 || bucket != NULL, "out of memory collecting a row's cells for CSS 2.1 §17.5.3");
    for (r = 0; r < nrows; r++) {
        CssPx term = th_declared_term(rows[r].element);
        size_t n = 0;

        /* §17.5.3's SECOND AND THIRD TERMS are over "each cell in the row", and the cells this row OWNS are the
           ones ANCHORED in it — `TableGridCell.row` is "the grid row it is anchored at", which §17.5 fixes as
           "the row specified by the cell's parent". A cell anchored ABOVE and reaching down into this row is
           §17.5.3's declined spanning case and contributes through the constraint pass below instead, which is
           the only thing the section requires of it. */
        for (i = 0; i < grid->ncells; i++) {
            const TableGridCell *c = &grid->cells[i];

            if (c->row != r || c->rowspan != 1) continue;
            DCHECK(n < grid->ncells, "more cells were collected for one row than the grid holds");
            bucket[n].element = c->element;
            bucket[n].height = c->element == NULL ? css_px(0.0) : th_cell_box_height(c->element);
            /* §17.2.1's ANONYMOUS cell box has no element to declare anything, so CSS 2 §9.2.1.1's "non-
               inherited properties have their initial value" gives it `vertical-align: baseline` — the initial
               value css-inline-3 §4.2 states — and a zero-height empty box. Both are read as the constants
               they are rather than through a cascade that has no element to run over. */
            bucket[n].align = c->element == NULL ? TH_ALIGN_BASELINE : th_cell_align(c->element);
            if (c->element != NULL) term = css_px_max(term, th_declared_term(c->element));
            n++;
        }
        out->rows[r] = css_px_max(term, th_row_min(bucket, n));
        DCHECK(out->rows[r].px >= 0.0,
               "CSS 2.1 §17.5.3 Table height algorithms gave a grid row a NEGATIVE used height. Every term is "
               "a declared length floored at zero, a content height core/layout/block_flow.c floors at zero, a "
               "padding, a border width, or a maximum of those — so a negative here is arithmetic that lost a "
               "sign, and the rectangle it would put through CSSOM VIEW is one no reader can tell from a "
               "measured one");
    }
    /* ---- §17.5.3's SPANNING CONSTRAINT, WHICH IS THE ONE THING THE SECTION REQUIRES OF A ROW-SPANNING CELL --
       "CSS 2.1 does not specify how cells that span more than one row affect row height calculations EXCEPT
       that the sum of the row heights involved must be great enough to encompass the cell spanning the rows."
       So this is a floor to SATISFY and not a distribution to invent, and the freedom the sentence leaves —
       WHICH of the spanned rows grows — is the recorded choice: the whole deficit goes to the LAST row the cell
       covers. Growing the last row keeps every row above the cell where the rows without a spanning cell put
       it, so a document whose only spanning cell is in its final column does not move its own earlier rows.
       THE PASS IS ONE SWEEP IN GRID ORDER AND THAT IS SOUND rather than lucky: each step only INCREASES a row,
       and each cell's available height is re-read from the array, so a later cell sees every earlier
       widening — and the boundary assert below re-checks every cell against the final heights.
       THE CELL'S OWN DECLARED `height` IS FOLDED INTO WHAT MUST BE ENCOMPASSED, because §17.5.3's row maximum
       could not take it: that maximum is over "each cell in the row" and a spanning cell is in several. */
    for (i = 0; i < grid->ncells; i++) {
        const TableGridCell *c = &grid->cells[i];
        CssPx need, have;
        size_t k;

        if (c->rowspan == 1) continue;
        DCHECK(c->row + c->rowspan <= nrows,
               "CSS 2.1 §17.5 Visual layout of table contents' grid placed a cell reaching past the last row "
               "the table has. Rule 1 makes the row count the number of row elements — \"the table occupies "
               "exactly as many grid rows as there are row elements\" — and the section's own last placement "
               "rule shortens a `rowspan` that would overflow it: \"A cell box cannot extend beyond the last "
               "row box of a table or row group; the user agents must shorten it until it fits.\" So a cell "
               "past the end is that shortening having been lost between the placement and this read");
        /* RULE 6's OTHER HALF, WHICH THIS ASSERT USED TO SAY IT COULD NOT MAKE. The sentence binds a cell to
           its ROW GROUP as well as to the table, and core/layout/table_grid.h now reports the run of grid rows
           each group holds — so a `rowspan` reaching out of its `<tbody>` and into the next one is caught here
           instead of passing as a cell that is merely inside the table. The lookup is inside the DEV guard and
           not inside the DCHECK's own condition because it is a walk with an abort in it, and a condition this
           file states must be one a release build can drop whole. */
#if APICLIENT_DEV
        {
            const TableGridRowGroup *run = table_grid_group_of_row(grid, c->row);

            DCHECK(c->row + c->rowspan <= run->first + run->nrows,
                   "CSS 2.1 §17.5 Visual layout of table contents' rule 6 binds a cell to its ROW GROUP and "
                   "not only to the table — \"A cell box cannot extend beyond the last row box of a table or "
                   "row group; the user agents must shorten it until it fits\" — and this cell reaches past "
                   "the last grid row of the run its anchor row is in. core/layout/table_grid.c applies that "
                   "clamp against the same runs this reads, so a cell past its group's end is that clamp "
                   "having been computed against a different partition of the rows, and the rows that would "
                   "encompass it below belong to a row group it is not in");
        }
#endif
        need = c->element == NULL ? css_px(0.0)
                                  : css_px_max(th_cell_box_height(c->element), th_declared_term(c->element));
        have = css_px_scale(used_spacing, (double) (c->rowspan - 1));
        for (k = 0; k < c->rowspan; k++) have = css_px_add(have, out->rows[c->row + k]);
        if (need.px > have.px)
            out->rows[c->row + c->rowspan - 1] =
                css_px_add(out->rows[c->row + c->rowspan - 1], css_px_sub(need, have));
    }
    sum = th_spacing_total(used_spacing, nrows);
    for (r = 0; r < nrows; r++) sum = css_px_add(sum, out->rows[r]);
    /* ---- §17.5's LAST PARAGRAPH OVER THE ROW GROUPS, ASSERTED AGAINST THE SUM ABOVE -------------------------
       The two readings of one gap, checked against each other. The paragraph names the gaps "between the rows,
       columns, row groups or column groups" as one set corresponding to `border-spacing`, so the distance
       between the last row of one group and the first row of the next is ONE spacing described twice — and
       §17.5.3's sum has no term for a row group at all, which is only consistent if that is so. Every run's
       extent plus the gaps BETWEEN the runs plus the one at each end must therefore be the same number this
       algorithm answers the table with. THE FAILURE THIS CATCHES IS THE ONE A READER WOULD MAKE: charging a
       second spacing between two groups, which no document can distinguish from a taller table and which
       `table_row_group_used_extent` would then carry into every row group's rectangle.
       IT IS UNDER `nrows != 0` BECAUSE §17.6.1's DISTANCE IS STATED OVER THE BORDERS OF CELLS: a table with no
       rows has no such distance and no runs either, so both sides are zero and the `ngroups + 1` count below
       would invent one spacing out of neither.
       THE WHOLE BLOCK IS UNDER THE DEV GUARD and not only its DCHECK, because the sum it compares against is
       computed here rather than being a value the algorithm already holds — a release build that computed it
       and dropped the comparison would be doing one pass over the runs for nothing. */
#if APICLIENT_DEV
    if (nrows != 0) {
        CssPx runs_total = css_px_scale(used_spacing, (double) (grid->ngroups + 1));
        CssPx runs_slack;

        for (i = 0; i < grid->ngroups; i++)
            runs_total = css_px_add(runs_total, table_row_group_used_extent(out, &grid->groups[i]));
        runs_slack = css_px_sub(runs_total, sum);
        DCHECK(runs_slack.px <= 1e-9 * (1.0 + (sum.px < 0.0 ? -sum.px : sum.px)) &&
                   runs_slack.px >= -1e-9 * (1.0 + (sum.px < 0.0 ? -sum.px : sum.px)),
               "CSS 2.1 §17.5 Visual layout of table contents' row groups do not add up to the table CSS 2.1 "
               "§17.5.3 Table height algorithms answers. §17.5's last paragraph names the gaps \"between the "
               "rows, columns, row groups or column groups\" as ONE set corresponding to 'border-spacing', so "
               "the gap between two adjoining row groups is the same single vertical spacing that stands "
               "between the two rows meeting there — and §17.5.3's own sum, \"the sum of the row heights plus "
               "any cell spacing or borders\", has no row-group term because of it. Either the runs "
               "core/layout/table_grid.h reports no longer partition the rows, or a spacing is being counted "
               "twice at a group boundary — and the second is invisible in every document, since a row group "
               "box one spacing too tall looks exactly like a table one spacing taller");
    }
#endif
    has_declared = th_declared_minimum(table, &declared);
    /* §17.5.3's TWO SENTENCES, IN ORDER: the `auto` height IS the sum, and any other value "is treated as a
       minimum height" — so the used height is the greater of the two. WHAT THE SURPLUS DOES IS THE RECORDED
       CHOICE: "CSS 2.1 does not define how extra space is distributed when the 'height' property causes the
       table to be taller than it otherwise would be", so the rows keep the heights the algorithm gave them and
       the difference stands below the last one, inside the table box's content edge. The alternative readings
       — spreading it over the rows, or over the last row alone — are equally conforming and equally arbitrary,
       and this one is chosen because it leaves every row where a reader measuring one row can predict. */
    out->content = has_declared ? css_px_max(sum, declared) : sum;
    check_sum = th_spacing_total(used_spacing, out->nrows);
    for (r = 0; r < out->nrows; r++) check_sum = css_px_add(check_sum, out->rows[r]);
    slack = css_px_sub(out->content, check_sum);
    /* §17.5.3's SUM, ASSERTED RATHER THAN ASSUMED — the vertical twin of the identity core/layout/table_width.c
       asserts over §17.6.1's width, and stated as an INEQUALITY because the declared minimum may legitimately
       exceed the rows: the section's own decline is exactly about that surplus. What must never happen is the
       other direction, a content height SHORTER than the rows it is made of, which would place every row after
       the first outside the box a consumer sizes from `content`. */
    DCHECK(slack.px >= -1e-9 * (1.0 + (out->content.px < 0.0 ? -out->content.px : out->content.px)),
           "CSS 2.1 §17.5.3 Table height algorithms makes a table's height \"the sum of the row heights plus "
           "any cell spacing or borders\", floored by a declared minimum — and this answer's content height is "
           "SMALLER than that sum. A consumer placing rows from the array and sizing the box from `content` "
           "would lay them out in two different rectangles, with every row past the first outside the box");
    for (i = 0; i < grid->ncells; i++) {
        const TableGridCell *c = &grid->cells[i];
        CssPx need, have;
        size_t k;

        if (c->rowspan == 1 || c->element == NULL) continue;
        need = css_px_max(th_cell_box_height(c->element), th_declared_term(c->element));
        have = css_px_scale(used_spacing, (double) (c->rowspan - 1));
        for (k = 0; k < c->rowspan; k++) have = css_px_add(have, out->rows[c->row + k]);
        DCHECK(have.px >= need.px - 1e-9 * (1.0 + (need.px < 0.0 ? -need.px : need.px)),
               "CSS 2.1 §17.5.3 Table height algorithms states the ONE thing it does require of a row-spanning "
               "cell — \"the sum of the row heights involved must be great enough to encompass the cell "
               "spanning the rows\" — and the rows this cell covers do not encompass it. The pass above only "
               "ever increases a row and re-reads the array per cell, so a violation here is two cells' "
               "requirements having been satisfied against different copies of the heights");
    }
    free(bucket);
    table_box_rows_free(rows, nrows);
    DCHECK(out->content.px >= 0.0,
           "CSS 2.1 §17.5.3 gave a table a NEGATIVE used height. Every operand is a non-negative row height, a "
           "non-negative spacing counted a non-negative number of times, or a declared length floored at zero");
}

/* See table_height.h for the reading the N-1 spacings encode and for why this is a BORDER-box number. */
CssPx table_cell_used_border_box_height(const TableUsedHeights *heights, const TableGridCell *cell)
{
    CssPx h;
    size_t r;

    DCHECK(heights != NULL && cell != NULL,
           "CSS 2.1 §17.5's cell height was asked for with no table height answer or no grid cell — a cell box "
           "fills the rows its rectangle covers, so neither operand has a substitute");
    DCHECK(heights->rows != NULL || heights->nrows == 0,
           "CSS 2.1 §17.5.3's answer was indexed for a cell while holding no row array and a non-zero row "
           "count — `table_heights` stores NULL only for a table with no rows, so the two have been carried "
           "apart since it answered");
    DCHECK(cell->rowspan >= 1,
           "CSS 2.1 §17.5 Visual layout of table contents' grid reported a cell covering NO row, and that "
           "section states the floor in the sentence that defines a cell's rectangle: \"Each cell is thus a "
           "rectangular box, one or more grid cells wide and high\". core/layout/table_grid.h answers that "
           "field and its own contract puts the same floor of one on it, so a zero here is that guarantee "
           "having been lost between the placement and this read");
    DCHECK(cell->row + cell->rowspan <= heights->nrows,
           "CSS 2.1 §17.5's grid placed a cell past the last row CSS 2.1 §17.5.3 Table height algorithms "
           "answered a height for. Rule 6 clips a `rowspan` to the table, so the two walks have come apart and "
           "the rows in hand belong to a grid this cell is not in");
    h = css_px_scale(heights->spacing, (double) (cell->rowspan - 1));
    for (r = 0; r < cell->rowspan; r++) h = css_px_add(h, heights->rows[cell->row + r]);
    DCHECK(h.px >= 0.0,
           "CSS 2.1 §17.5's cell height came out NEGATIVE over rows CSS 2.1 §17.5.3 asserts are each "
           "non-negative and a `border-spacing` §17.6.1 forbids to be negative");
    return h;
}

/* See table_height.h for §17.5's last paragraph in both border models, for why ONE arithmetic is right in each
   of them, and for why this is an EXTENT and never a used `height`. */
CssPx table_row_group_used_extent(const TableUsedHeights *heights, const TableGridRowGroup *group)
{
    CssPx h;
    size_t r;

    DCHECK(heights != NULL,
           "CSS 2.1 §17.5's rule 2 was asked for a row group's extent with no table height answer. Rule 2 gives "
           "the box \"the same grid cells as the rows it contains\" and nothing else — the grid rows are an "
           "index and the DISTANCE is CSS 2.1 §17.5.3 Table height algorithms', so there is no substitute "
           "operand");
    DCHECK(heights->rows != NULL || heights->nrows == 0,
           "CSS 2.1 §17.5.3's answer was indexed for a row group while holding no row array and a non-zero row "
           "count — `table_heights` stores NULL only for a table with no rows, so the two have been carried "
           "apart since it answered");
    /* AN EMPTY ROW GROUP IS A CHOICE AND IT IS RECORDED HERE, WITH ITS ALTERNATIVE, BECAUSE CSS 2.1 DECIDES
       NEITHER. `<tbody></tbody>` contains no row, so rule 2's union over its rows is EMPTY and §17.5's last
       paragraph names no edges for a box occupying no grid cell — core/layout/table_grid.h's
       `table_grid_row_group_of` answers NULL for exactly that and for nothing else. The extent taken here is
       ZERO, and the reasoning is rule 2 itself: on THIS axis a group's extent IS a function of how many grid
       rows it holds, so a group holding none is zero rows tall the same way a group holding two is two rows
       tall. That is the exact inverse of the choice core/layout/used_value.c records on the INLINE axis, where
       an empty group takes the WHOLE grid row — and the two are consistent rather than opposed, because there
       rule 1 makes the extent independent of the row count and a zero would have invented a dependence, while
       here the dependence is what rule 2 states. THE ALTERNATIVE READING is that a box the section places
       nowhere has no rectangle at all, which would be a crash; it is declined because §17.5's rules are a
       PLACEMENT and a box a placement gives no grid cell to is placed, not unplaceable, and a crash would
       refuse a document CSS 2.1 conforms for. A REAL BROWSER MAY DISAGREE AND NOTHING HERE WOULD SAY SO: the
       zero is unobservable in the table's own height (the group holds no row, so no row height and no spacing
       moves), and it shows only in `getBoundingClientRect().height` on the empty `<tbody>` itself. */
    if (group == NULL) return css_px(0.0);
    DCHECK(group->nrows >= 1,
           "CSS 2.1 §17.5's rule 2 reported a row group run covering NO grid row. core/layout/table_grid.h "
           "builds a run only around a row it holds and asserts that floor where it builds them, so a zero "
           "here is not the EMPTY row group — that group has no run at all and arrives as NULL above — but "
           "that array having been carried apart since it answered");
    DCHECK(group->first + group->nrows <= heights->nrows,
           "CSS 2.1 §17.5's rule 2 placed a row group past the last row CSS 2.1 §17.5.3 Table height "
           "algorithms answered a height for. The runs partition the grid rows of the grid `table_heights` was "
           "given, and that entry answers one height per grid row of it — so the run in hand belongs to a grid "
           "these heights are not of, and every row it names is another table's");
    /* THE N-1 SPACINGS ARE THE ONES INSIDE THE GROUP AND THERE ARE NO OTHERS. §17.5's last paragraph puts the
       group's edges at the border edges of the cells in its first and last grid rows under §17.6.1 The
       separated borders model, so the gaps between its OWN rows are inside it and the gaps to the groups above
       and below it are outside — the same reading `table_cell_used_border_box_height` takes over a cell's
       rectangle, over a group's run instead. Under §17.6.2 The collapsing border model `spacing` is zero, the
       term vanishes, and what is left is the run's row heights, which is that model's own sentence: "the rows
       together exactly cover the table, leaving no gaps". */
    h = css_px_scale(heights->spacing, (double) (group->nrows - 1));
    for (r = 0; r < group->nrows; r++) h = css_px_add(h, heights->rows[group->first + r]);
    DCHECK(h.px >= 0.0,
           "CSS 2.1 §17.5's rule 2 gave a row group a NEGATIVE extent over rows CSS 2.1 §17.5.3 asserts are "
           "each non-negative and a `border-spacing` §17.6.1 The separated borders model forbids to be "
           "negative (\"Lengths may not be negative\")");
    return h;
}

void table_heights_release(TableUsedHeights *h)
{
    DCHECK(h != NULL, "CSS 2.1 §17.5.3's answer was released with nothing to release");
    DCHECK(h->rows != NULL || h->nrows == 0,
           "CSS 2.1 §17.5.3's answer was released holding no row array and a non-zero row count");
    free(h->rows);
    h->rows = NULL;
    h->nrows = 0;
}
