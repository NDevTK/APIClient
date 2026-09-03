/* CSS 2 §9.4.1 "Block formatting contexts" — a box's position. See flow_position.h for the coordinate space,
   for why the root element is the one box that is answered, and for what each other box is waiting on. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/dom/document.h"
#include "core/frame/viewport.h"
#include "core/layout/block_flow.h"
#include "core/layout/box_subject.h"
#include "core/layout/flow_position.h"
#include "core/layout/line_box.h"
#include "core/layout/replaced_element.h"
#include "core/layout/table_box.h"
#include "core/layout/table_grid.h"
#include "core/layout/table_height.h"
#include "core/layout/table_width.h"
#include "core/layout/table_wrapper.h"
#include "core/layout/used_value.h"

/* THE SUBJECT OF EVERY CRASH IN THIS FILE, WHICH IS A BOX AND NOT A PLACE.
   §AN-ASSERT-THAT-NAMES-A-REMEDY's test is to count the call sites that can reach an abort and to make the
   ADDRESS part of the assert once that number is larger than one would read by hand. Measured by reverse
   reachability over every `.c` file under engine/host: `flow_border_box_origin` is reached from 73 call sites
   over 46 functions in 9 files, 7 of them outside core/layout — every CSSOM VIEW §6 member that asks for a
   position or an extent, §7's scroll algorithms, a Range's client rects, IntersectionObserver's update and the
   rendering step. So it is.
   AND THAT COUNT UNDERSTATES IT, because ONE call site reaches this file once per BOX IN THE DOCUMENT rather
   than once. core/layout/scrolling_area.c's descendant walk asks `flow_border_box_origin` for every
   descendant's margin edge, and core/dom/element_view.c's `ev_scroll_extent` runs the VIEWPORT's walk — over
   the document element's whole subtree — before its step 6 has-a-box guard, which is the step everyone reads
   as coming first. So one `element.scrollWidth` places every box in the document, and the caller a frame list
   names says nothing whatever about WHICH of them had no rule.
   WHICH ADDRESS IS THE QUESTION, and core/layout/line_box.c answered it for the same shape. A `__FILE__`/
   `__LINE__` threaded from those callers arrives through `sa_descendant_edge`, `fp_line_box_origin` and
   `flow_padding_box_origin` — a handful of forwarding functions for the whole tree, which is the capture point
   that rule names as the WRONG one — and it would still not say which box has no rule. Every remedy the aborts
   below state names a computed `display`, `position`, `float` or `writing-mode` and a module to build, so the
   address their reader is standing in front of is the ELEMENT and that value.
   THE COMPOSITION ITSELF IS core/layout/box_subject.h's, which is where the argument that it must be TOTAL now
   lives, together with the ownership contract and the `%.*s` rule. Two things this file owes that component:
   `box_subject_computed` is reached instead of `fp_computed_is` below, whose own DCHECK is correct where this
   file reads a property to compute geometry with and would REPLACE the defect being reported where it reads
   one to name a box; and `replaced_element_of` is reached from no message here, for the sharper form of the
   same reason — it ABORTS by name for an `embed`, a `video`, a `canvas`, an `object`, an `audio` and an
   `input`, so a message asking it whether this box is replaced would report core/layout/replaced_element.c's
   gap for precisely the boxes whose own gap is being reported.
   THE ONE ASSERT BELOW WHOSE SUBJECT IS A NODE rather than a box is the containing-block one, whose whole
   question is what this element's PARENT is — §10.1's first case is the root element's alone and
   core/layout/used_value.c decides the root by that parent being the Document — so `box_subject_node` is what
   it asks and "(no element)" would be a wrong answer there rather than a missing one. */
static bool fp_computed_is(lxb_dom_element_t *el, const char *name, const char *kw)
{
    char *v = css_computed_value(el, name);
    char nbuf[160];
    bool same;

    DCHECKF(v != NULL,
            "%s, property `%s`: "
            "the cascade produced no computed value for a property this engine models — every one of "
            "them is in lexbor's registry with an initial value, so the last layer always answers",
            box_subject(el, nbuf, sizeof nbuf), name);
    same = strcmp(v, kw) == 0;
    free(v);
    return same;
}

/* §9.4.1's TWO RULES ARE STATED IN PHYSICAL AXES — "laid out one after the other, VERTICALLY" and "each box's
   LEFT outer edge touches the LEFT edge of the containing block" — and those are the physical axes of a
   `horizontal-tb` writing mode and of no other. css-writing-modes-4 §3.2 "Block Flow Direction: the
   writing-mode property" is what makes the block flow direction a property at all, and §7.4 "Flow-Relative
   Mappings" is what re-states §9.4.1's rules over the flow-relative axes so a vertical mode can be laid out.
   THE VALUE IS READABLE NOW, WHICH IS WHY THIS IS AN ASSERT AND NOT A SILENCE: this component stacked
   downwards for every box while nothing could read the property, and a `vertical-rl` box got a coordinate
   computed by the wrong rule with nothing to say so. */
static void fp_require_horizontal_tb(lxb_dom_element_t *el)
{
    char nbuf[160], wbuf[64];

    if (!fp_computed_is(el, "writing-mode", "horizontal-tb"))
        DFAILF("%s, computed `writing-mode` `%s`: "
              "this box's computed `writing-mode` is not `horizontal-tb`, so its BLOCK FLOW DIRECTION is not "
              "downwards and its inline axis is not horizontal — css-writing-modes-4 §3.2 \"Block Flow "
              "Direction: the writing-mode property\" gives `vertical-rl` and `sideways-rl` a right-to-left "
              "block flow and `vertical-lr` and `sideways-lr` a left-to-right one. CSS 2 §9.4.1's two rules are "
              "written in the PHYSICAL axes of a horizontal-tb mode ('one after the other, vertically' and "
              "'each box's left outer edge touches the left edge of the containing block'), and this component "
              "implements them physically, so placing a vertical-mode box by them would answer a coordinate "
              "from the wrong axis rather than fail. BUILD css-writing-modes-4 §7.4 \"Flow-Relative Mappings\", "
              "which restates §9.4.1 over the block and inline axes, and then this file's stacking and "
              "touching become the flow-relative pair with §6.4 \"Abstract-to-Physical Mappings\" applied once "
              "at the end. core/layout/block_flow.c's own vertical walk is the same subproblem seen from the "
              "height side and lands with it",
              box_subject(el, nbuf, sizeof nbuf),
              box_subject_computed(el, "writing-mode", wbuf, sizeof wbuf));
}

/* "The containing block in which the root element lives is a rectangle called the initial containing block"
   (§10.1) — the element whose parent is the Document itself, which is the same test core/layout/used_value.c
   makes for the base case of its own recursion. */
static bool fp_is_root(const lxb_dom_node_t *n)
{
    return n->parent != NULL && n->parent->type == LXB_DOM_NODE_TYPE_DOCUMENT;
}

/* THE INITIAL CONTAINING BLOCK'S WIDTH, asked of the ELEMENT'S own document and never of a running realm — an
   iframe's ICB is 300 CSS pixels wide and its parent's is 1280. The caller has established that the element has
   a box, which is defined over the document being presented, so the viewport exists here rather than being
   checked for a second time. */
static CssPx fp_icb_width(lxb_dom_element_t *el)
{
    lxb_dom_node_t *n = lxb_dom_interface_node(el);
    char nbuf[160];
    JSContext *dctx;

    DCHECKF(n->owner_document != NULL,
           "%s: "
           "the initial containing block was asked for an element whose node has no owner document — every node "
           "this engine mints belongs to the document that created it",
           box_subject(el, nbuf, sizeof nbuf));
    dctx = document_active_realm_of(lxb_dom_interface_node(n->owner_document));
    DCHECKF(dctx != NULL && viewport_exists(dctx),
           "%s (realm %s): "
           "CSS 2 §9.4.1's placement was asked for an element whose document is not being presented, so there "
           "is no viewport and §10.1's INITIAL CONTAINING BLOCK does not exist. The caller's own first step is "
           "the has-a-box predicate (core/dom/element_view.h), which is defined over exactly that question — so "
           "the two answers have come apart",
           box_subject(el, nbuf, sizeof nbuf), dctx == NULL ? "absent" : "present but presenting no viewport");
    return viewport_icb_width(dctx);
}

/* §9.4.1's HORIZONTAL RULE, which is one rule with two answers: "each box's left outer edge touches the left
   edge of the containing block (for right-to-left formatting, right edges touch)". WHICH ONE APPLIES IS THE
   CONTAINING BLOCK'S COMPUTED `direction`, which core/layout/used_value.h answers with §10.1's own first-case
   exception folded in ("the 'direction' property of the initial containing block is the same as for the root
   element").
   THE TWO ANSWERS ARE NOT A SLACK APART — they are the SAME DISTANCE measured from opposite edges, and stating
   them that way is what makes the rule one rule. In `ltr` the box's left MARGIN edge is at the containing
   block's left content edge, so its border box begins one `margin-left` in. In `rtl` its right margin edge is
   at the right content edge, so its border box begins at the block's width less its own `margin-right` and
   less its border box's width. Where §10.3.3's constraint equation held exactly — every `width: auto` box in
   normal flow, and every over-constrained box now that used_value.c recomputes the ignored margin — the two
   answers COINCIDE, which is the agreement this function used to assert and no longer needs to: an
   over-constrained `rtl` box now differs from an `ltr` one exactly where the spec says it does. */
static CssPx fp_left_offset(lxb_dom_element_t *el, CssPx cb_width)
{
    if (!used_value_containing_block_is_rtl(el)) return used_value_px(el, "margin-left");
    return css_px_sub(css_px_sub(cb_width, used_value_px(el, "margin-right")),
                      used_value_border_edge_px(el, false));
}

/* CSS 2 §8.1's edges between a box's BORDER box and its PADDING box, and between that and its CONTENT box, on
 * the leading side of one axis, are `used_value_leading_border_px` and `used_value_leading_edge_px`.
 *
 * THEY ARE READ FROM core/layout/used_value.h AND WERE ONCE COMPOSED HERE, AND THE DIFFERENCE IS NOT TIDINESS.
 * What stood here read `border-top-width`/`border-left-width` off the cascade and added
 * `used_value_px(el, "padding-*")` beside it — two computed values, composed by a caller that had no way to
 * ask whether the box HAS them. CSS 2.1 §17 Tables says it does not, for five of §17.2 The CSS table model's
 * ten box types: §17.6.1 The separated borders model refuses a row, row group, column or column group box a
 * border outright, §8.4 "Padding properties: 'padding-top', 'padding-right', 'padding-bottom', 'padding-left',
 * and 'padding'"' Applies-to line refuses those same four a padding, and §17.6.2 The collapsing border model
 * says of a table box that "in this model, a table does not have padding (but does have margins)" and makes
 * its border widths halves of the collapsed borders at the grid's edges rather than the declared value. So
 * `fp_table_row_origin` below placed every row of a `border-collapse: collapse` table by a padding the table
 * does not have and a border twice the one §17.6.2 gives it — a whole-table offset with nothing to say so,
 * because both operands were real lengths the author really wrote.
 * THE FIX IS THE ENTRY AND NOT A BRANCH HERE. Four call sites composed the pair and a fifth composed the
 * border alone; a §17 branch at each would have been the same routing decision written five times over a
 * difference none of them can see. */

/* CSS 2.1 §17.5 Visual layout of table contents' PLACEMENT OF A ROW BOX AND OF A CELL BOX, which is §17.5's
 * rules 1 and 5 and §17.6.1 The separated borders model's spacing turned into the coordinates this file
 * reports, and NOT §9.4.1's two rules — neither box is stacked after its siblings by its margins, because CSS
 * 2.1 §17.5 Visual layout of table contents' opening sentence says "Internal table elements do not have
 * margins."
 *
 * WHERE THE EDGES ARE IS THE SECTION'S OWN SENTENCE AND NOT A CHOICE. §17.5's last paragraph: "In the
 * separated borders model, the edges coincide with the border edges of cells. (And thus, in this model, there
 * may be gaps between the rows, columns, row groups or column groups, corresponding to the 'border-spacing'
 * property.)" So a row's top border edge is the top border edge of the cells anchored in its grid row, and its
 * left border edge is the left border edge of the cells in the first grid column. CSS 2.1 §17.6.1 The
 * separated borders model places those: "The distance between the table border and the borders of the cells
 * on the edge of the table is the table's padding for that side, plus the relevant border spacing distance."
 * Measured from the table box's CONTENT edge — which is past that padding — the first cell border is exactly
 * one border-spacing in, on each axis.
 *
 * THE SPACINGS ARE READ OFF THE TWO ALGORITHMS' ANSWERS AND NEVER OFF THE PROPERTY, which is the rule
 * core/layout/table_width.h and core/layout/table_height.h each state about their own `spacing` field: a second
 * read of `border-spacing` would be a second place for CSS 2.1 §17.6.1 The separated borders model's
 * "Computed value: two absolute lengths" to be resolved, free to disagree with the one the columns and rows
 * were laid out under. IT IS ALSO WHY §17.6.2 The collapsing border model NEEDS NO REFUSAL HERE. Each
 * `spacing` is the algorithm's own cell-spacing TERM and not the `border-spacing` property, and §17.6.2 gives
 * that property no meaning ("Borders are centered on the grid lines between the cells" — there is no distance
 * between adjoining cell borders to separate), so the term the two algorithms answer with is ZERO under the
 * collapsing model. The sums below then place the first cell's border edge AT the table box's content edge,
 * which is exactly where §17.5 Visual layout of table contents' last paragraph puts it in that model — the row
 * edges "coincide with the hypothetical grid lines on which the borders of the cells are centered" and "the
 * rows together exactly cover the table, leaving no gaps". So ONE arithmetic is correct in both models
 * precisely because it reads the ANSWERS; a second read of the property would have needed a branch here and
 * would have been the place the two models came apart.
 *
 * THE TABLE BOX'S OWN ORIGIN IS THE WRAPPER'S, AND THAT IS ASSERTED RATHER THAN ASSUMED. §17.4 Tables in the
 * visual formatting model gives the table element's `margin-*` to the WRAPPER and everything else to the table
 * box, and its parenthesis gives each box the initial value of what it does not get — so the wrapper has no
 * border and no padding, the table box has no margin, and with no caption between them the table box's border
 * edge IS the wrapper's content edge, which is the point `flow_border_box_origin` answers for a table element.
 * A CAPTION BREAKS EXACTLY THAT and is refused by name below. */

/* THE ONE TABLE LAYOUT BOTH PLACEMENTS ARE READ OUT OF, gathered once rather than per box kind. §17.5's rules
   1 and 5 place a ROW and a CELL at rectangles of the SAME grid, and each coordinate is that rectangle turned
   into a distance from the TABLE BOX's content edge — so two copies of this preamble would be two places for
   §17.5.2 Table width algorithms: the 'table-layout' property's columns and §17.5.3 Table height algorithms'
   rows to be built, free to be built over a different grid than the rectangle was read out of.
   NOTHING IS STORED BETWEEN CALLS, for the reason core/layout/block_flow.h states of every layout in this
   directory: a layout is per-flow state, so a cached grid is shared state solver/dom_cow.h's delta does not
   swap and a stale one is another flow's document. */
typedef struct {
    lxb_dom_element_t *table;
    FlowPoint          origin;    /* the TABLE box's border-box origin, which §17.4 makes the wrapper's */
    TableGrid          grid;
    TableUsedWidths    widths;
    TableUsedHeights   heights;
} FpTableFrame;

/* `el` IS THE BOX BEING PLACED and not the table — it is what the two refusals below name, because a reader
   of either is standing in front of the box whose position was asked for and not in front of the table. */
static void fp_table_frame_build(lxb_dom_element_t *el, FpTableFrame *out)
{
    lxb_dom_element_t **captions = NULL;
    size_t ncaptions;
    char nbuf[160], tbuf[160];

    out->table = table_box_table_of(el);
    DCHECK(!table_wrapper_owns_property("border-top-width") && !table_wrapper_owns_property("padding-top") &&
               !table_wrapper_owns_property("padding-left") && table_wrapper_owns_property("margin-top"),
           "CSS 2.1 §17.4 Tables in the visual formatting model's declaration split is what makes the table "
           "box's border-box origin the same point as the table wrapper box's: the wrapper takes the table "
           "element's `margin-*` and the INITIAL value of its border and padding, and the table box takes the "
           "border and padding and the initial `margin-*`. This arithmetic reads the table element's own border "
           "and padding as the TABLE BOX's and adds nothing for the wrapper — so if that list has moved, the "
           "two boxes no longer coincide and every box below is offset by a border nobody accounted for");
    ncaptions = table_box_captions(out->table, &captions);
    free(captions);
    if (ncaptions != 0)
        DFAILF("%s, in %s: CSS 2.1 §17.4 Tables in the visual formatting model puts this box's table box inside "
               "a WRAPPER that also contains %zu CAPTION box(es) — \"the table generates a principal block box "
               "called the table wrapper box that contains the table box itself and any caption boxes (in "
               "document order)\" — and §17.4.1 Caption position and alignment decides which of them stand "
               "ABOVE it (`caption-side: top`, the initial value, \"Positions the caption box above the table "
               "box\"). Every such caption shifts the table box down inside the wrapper, and this function "
               "takes the table box's origin to BE the wrapper's, which is exact only when nothing precedes it. "
               "There is no distance to add: a caption's own margin-box height is CSS 2.1 §10.6.3's over its "
               "content, taken on the WRAPPER's child box list, which is not any element's DOM child list — "
               "core/layout/block_flow.c owns that walk and names what it still lacks. BUILD it there and this "
               "arm becomes a sum over the captions §17.4.1 puts above the table box",
               box_subject(el, nbuf, sizeof nbuf), box_subject(out->table, tbuf, sizeof tbuf), ncaptions);
    /* §17.4's wrapper is what §9.4.1 stacks, so the table element's own origin is an ordinary recursion into
       this file's entry and not a second rule. */
    out->origin = flow_border_box_origin(out->table);
    table_grid_build(out->table, &out->grid);
    table_widths(out->table, &out->grid, &out->widths);
    table_heights(out->table, &out->grid, &out->heights);
}

static void fp_table_frame_release(FpTableFrame *f)
{
    table_heights_release(&f->heights);
    table_widths_release(&f->widths);
    table_grid_release(&f->grid);
}

/* WHICH END OF THE TABLE §17.5's LOGICAL COLUMN 0 IS PAINTED AT. It is rule 3's own sentence rather than a
   reading taken here — "A column box occupies one or more columns of grid cells. Column boxes are placed next
   to each other in the order they occur. The first column box may be either on the left or on the right,
   depending on the value of the 'direction' property of the table." — and rule 5 states the same fact about
   the placement it constrains: "(This constraint holds if the 'direction' property of the table is 'ltr'; if
   the 'direction' is 'rtl', interchange \"left\" and \"right\" in the previous two sentences.)"
   IT IS THE TABLE'S OWN COMPUTED `direction` AND NOT THE CONTAINING BLOCK'S, which both sentences say in the
   same four words ("the 'direction' property of the table"). That is why
   core/layout/used_value.h's `used_value_containing_block_is_rtl` is the WRONG entry here and reaching for it
   would be the quiet kind of wrong: it answers §9.4.1's horizontal rule, whose subject is the block a box is
   laid out IN, while this is a fact about the TABLE whose grid the box is placed in — and `direction` is
   INHERITED (css-writing-modes-4 §2.1 "Specifying Directionality: the direction property"), so the two agree
   on every table that declares none of its own and disagree only where a document actually exercises the
   difference. */
static bool fp_table_direction_is_rtl(lxb_dom_element_t *table)
{
    char *d = css_computed_value(table, "direction");
    char nbuf[160], vbuf[64];
    bool rtl, ltr;

    DCHECKF(d != NULL,
            "%s: the cascade produced no computed `direction` for a TABLE box whose grid a CSS 2.1 §17.5 "
            "Visual layout of table contents placement is being read out of — the property is in lexbor's "
            "registry with an initial value, so the last layer always answers",
            box_subject(table, nbuf, sizeof nbuf));
    rtl = strcmp(d, "rtl") == 0;
    ltr = strcmp(d, "ltr") == 0;
    free(d);
    DCHECKF(rtl || ltr,
            "%s, computed `direction` `%s`: a computed `direction` that is neither `ltr` nor `rtl`. "
            "css-writing-modes-4 §2.1 \"Specifying Directionality: the direction property\" gives the property "
            "the `Value:` line `ltr | rtl` and nothing else, and its `Computed value:` line is `specified "
            "value` — so a third spelling is a declaration that reached the cascade without its grammar, and "
            "CSS 2.1 §17.5's rules 3 "
            "and 5 have no third answer for which end of the table logical column 0 is painted at",
            box_subject(table, nbuf, sizeof nbuf),
            box_subject_computed(table, "direction", vbuf, sizeof vbuf));
    return rtl;
}

/* ONE RECTANGLE OF GRID CELLS TURNED INTO THE BORDER-BOX CORNER OF THE BOX THAT OCCUPIES IT — §17.5's rules 1
 * and 5 as ONE arithmetic, because they place two boxes at rectangles of the same grid and differ only in
 * WHICH rectangle. Rule 1 gives a ROW "one row of grid cells", which is every column of its own grid row;
 * rule 5 gives a CELL "a rectangular box, one or more grid cells wide and high", which
 * core/layout/table_grid.h has already placed. A ROW IS THEREFORE NOT A SPECIAL CASE OF THIS FUNCTION, IT IS
 * THE RECTANGLE `[0, ncols)` — and that is what makes this one function rather than two that would each have
 * to be right about §17.6.1's spacing separately.
 *
 * THE COLUMN AXIS IS NOT A MIRROR OF THE ROW AXIS AND `direction` IS THE WHOLE DIFFERENCE. This grid numbers
 * its columns in §17.5's own LOGICAL order (core/layout/table_grid.h says so of its own `col`), and rules 3
 * and 5 put logical column 0 at the LEFT under `ltr` and at the RIGHT under `rtl` — see
 * `fp_table_direction_is_rtl` above for both sentences. The block axis has no such question: rule 1 stacks the
 * rows "from top to bottom in the order they occur in the source document", with no property to interchange
 * the two ends.
 *
 * THE `rtl` ARM IS THE INTERCHANGE PERFORMED ON THE COLUMN INDEX AND NOT AS A SUBTRACTION FROM THE TABLE'S
 * CONTENT WIDTH, and the two are not equivalent in the way that matters here. Reading the rectangle's RIGHT
 * edge as `content` less the spacings and columns before it, and then stepping back by the box's own width,
 * would make every `rtl` coordinate depend on core/layout/table_width.h's `content` field agreeing with its
 * `columns` — an identity that component asserts only to within its distribution's own rounding, and one that
 * legitimately FAILS for a zero-column table, where `content` is a declared width or CAPMIN with no spacing in
 * it at all. Mirroring the INDEX instead makes the `rtl` sum the same prefix sum over the columns AFTER the
 * rectangle, so both arms read the same two operands (`columns` and `spacing`) in the same order and neither
 * reads `content`. IT ALSO MAKES RULE 1's "A ROW NEEDS NO SUCH ARM" A CONSEQUENCE RATHER THAN AN ASSUMPTION:
 * a whole-grid-row rectangle has no columns before it and none after it, so both arms sum over an EMPTY range
 * and a row's x is BYTE-IDENTICAL under the two `direction` values — where the subtraction form would have
 * differed from the `ltr` answer by an ulp and left a real disagreement and a rounding artifact looking the
 * same.
 *
 * THE BLOCK COORDINATE IS THE ANCHOR ROW'S IN BOTH CASES, which is §17.5's own sentence for a cell — "The top
 * row of this rectangle is in the row specified by the cell's parent" — read together with that section's last
 * paragraph putting the row edges at the cells' border edges in the separated model and exactly covering the
 * table in the collapsing one. So a cell's top border edge IS its row's, and there is one vertical sum rather
 * than a second rule for the box that spans rows. */
static FlowPoint fp_table_grid_origin(const FpTableFrame *f, size_t row, size_t col, size_t colspan)
{
    FlowPoint p = { { 0.0, CSS_ENV_NONE, NULL }, { 0.0, CSS_ENV_NONE, NULL } };
    CssPx x, y;
    size_t j;

    DCHECK(row < f->heights.nrows,
           "CSS 2.1 §17.5's rules 1 and 5 placed a box at a grid row CSS 2.1 §17.5.3 Table height algorithms "
           "answered no height for. `table_heights` answers one height per grid row of the grid it was given "
           "and asserts that count against the grid's own, so the two arrays have been carried apart");
    DCHECK(col + colspan <= f->widths.ncols && col + colspan >= col,
           "CSS 2.1 §17.5's rules 1 and 5 placed a box at a rectangle reaching past the grid's own column "
           "count, or at one whose span wrapped. core/layout/table_grid.h grows `ncols` to `col + colspan` for "
           "every cell it places and gives a row every column of its grid row, and core/layout/table_width.c "
           "asserts its own column count against the grid's — so a rectangle outside it is a grid and a "
           "placement that were carried apart, and the prefix sum below would read a used width that is not "
           "this table's");
    /* §17.5.3's OWN PREFIX SUM, ASKED RATHER THAN RE-SPELLED. `table_row_used_block_offset`
       (core/layout/table_height.h) is the distance from the table box's content edge to the TOP EDGE of grid
       row `row` — `row` used row heights and `row + 1` of §17.5.3's cell-spacing term — and that component
       states why the one arithmetic is right under both of §17.6's border models: the term is the ALGORITHM's
       and never the `border-spacing` property, so it is §17.6.1 The separated borders model's leading spacing
       and ZERO under §17.6.2 The collapsing border model. A loop here would be a second spelling of terms that
       belong to `TableUsedHeights`, free to come apart from the answer the rows were laid out under — the
       defect `table_cell_vertical_edges` is exported one axis over to prevent. */
    y = css_px_add(css_px_add(f->origin.y, used_value_leading_edge_px(f->table, true)),
                   table_row_used_block_offset(&f->heights, row));
    /* The same on the inline axis, over the columns §17.5's rules 3 and 5 put BEFORE this rectangle — which
       under `rtl` are the ones logically after it. */
    x = css_px_add(css_px_add(f->origin.x, used_value_leading_edge_px(f->table, false)), f->widths.spacing);
    if (fp_table_direction_is_rtl(f->table))
        for (j = col + colspan; j < f->widths.ncols; j++)
            x = css_px_add(css_px_add(x, f->widths.columns[j]), f->widths.spacing);
    else
        for (j = 0; j < col; j++)
            x = css_px_add(css_px_add(x, f->widths.columns[j]), f->widths.spacing);
    p.x = x;
    p.y = y;
    return p;
}

/* §17.5's RULE 1 — "Each row box occupies one row of grid cells" — which is the rectangle spanning every
   column of the row's own grid row, handed to the one placement above. */
static FlowPoint fp_table_row_origin(lxb_dom_element_t *el)
{
    FpTableFrame f;
    FlowPoint p = { { 0.0, CSS_ENV_NONE, NULL }, { 0.0, CSS_ENV_NONE, NULL } };
    size_t row = 0;
    char nbuf[160], tbuf[160];

    fp_table_frame_build(el, &f);
    if (!table_grid_row_of(&f.grid, el, &row)) {
        /* THE FRAME IS RELEASED BEFORE THE CRASH AND `f.table` SURVIVES IT — `fp_table_frame_release` frees the
           three layouts' arrays and owns no element, so the message below still names the table it walked to.
           The release is not decoration: `DFAILF` is compiled out in release, where this arm falls through to
           the zero point above. */
        fp_table_frame_release(&f);
        DFAILF("%s: CSS 2.1 §17.5 Visual layout of table contents' rule 1 placed NO grid row for this ROW box "
               "in %s's grid, which is the table box CSS 2.1 §17.2's own nesting puts it inside. The two walks "
               "have come apart: core/layout/table_box.h reached this table by climbing the internal boxes "
               "above the row, and core/layout/table_grid.h reached the rows by descending the same table's row "
               "groups, so a box that is in one and not the other is a row this table's own box generation does "
               "not report as a row of it. There is no position to answer with — the grid in hand is not this "
               "row's",
               box_subject(el, nbuf, sizeof nbuf), box_subject(f.table, tbuf, sizeof tbuf));
        return p;
    }
    p = fp_table_grid_origin(&f, row, 0, f.widths.ncols);
    fp_table_frame_release(&f);
    return p;
}

/* §17.5's RULE 5 — "Each cell is thus a rectangular box, one or more grid cells wide and high" — which
   core/layout/table_grid.h has already placed, handed to the one placement above. THE CELL BRINGS ITS OWN
   RECTANGLE AND NOTHING ELSE: everything that made a row placeable is the same table's, so what separated
   the two boxes was never an operand but the RECTANGLE, and this entry is the rectangle read back. */
static FlowPoint fp_table_cell_origin(lxb_dom_element_t *el)
{
    FpTableFrame f;
    FlowPoint p = { { 0.0, CSS_ENV_NONE, NULL }, { 0.0, CSS_ENV_NONE, NULL } };
    const TableGridCell *cell;
    char nbuf[160], tbuf[160];

    fp_table_frame_build(el, &f);
    cell = table_grid_cell_of(&f.grid, el);
    if (cell == NULL) {
        /* Released before the crash for the reason `fp_table_row_origin` above states. */
        fp_table_frame_release(&f);
        DFAILF("%s: CSS 2.1 §17.5 Visual layout of table contents' rule 5 placed NO cell for this CELL box in "
               "%s's grid, which is the table box CSS 2.1 §17.2's own nesting puts it inside. The two walks "
               "have come apart: core/layout/table_box.h reached this table by climbing the internal boxes "
               "above the cell, and core/layout/table_grid.h reached the cells by descending the same table's "
               "rows, so a box that is in one and not the other hangs under a row §17.2.1 Anonymous table "
               "objects' box generation does not report as a row of this table. There is no position to answer "
               "with — the grid in hand is not this cell's",
               box_subject(el, nbuf, sizeof nbuf), box_subject(f.table, tbuf, sizeof tbuf));
        return p;
    }
    p = fp_table_grid_origin(&f, cell->row, cell->col, cell->colspan);
    fp_table_frame_release(&f);
    return p;
}

/* WHICH BOX TYPES §9.4.1 PLACES, asked HERE and not left to whichever component this one calls next. The two
 * rules §9.4.1 states — "boxes are laid out one after the other, vertically, beginning at the top of a
 * containing block" and "each box's left outer edge touches the left edge of the containing block" — are
 * written about a BLOCK-LEVEL box in a block formatting context, and they say nothing whatever about a box of
 * any other type. This used to test the one value `inline`, on the reading that everything else this engine
 * could compute a `display` for was block-level; core/css/css_style_declaration.c's UA sheet now answers
 * `inline-block` for `input`, `button`, `select`, `textarea` and `marquee` and the eight table-internal types
 * for a table's own children, so that reading is gone and the rest would have fallen through to the two rules
 * below and come out with an x and a y for a box neither rule describes.
 *
 * A CRASH TWO COMPONENTS AWAY IS NOT THE SAME ASSERT. Every one of these does still abort today — the walk in
 * core/layout/block_flow.c classifies the same child and refuses it — but that is a crash at the box's PARENT,
 * naming the parent's height walk, for a question that was asked about the CHILD's position. The invariant
 * belongs where it is born (CLAUDE.md §Offensive programming), and it is the only thing standing between a
 * release build and a coordinate computed from the wrong rule.
 *
 * §9.4.1 IS NOT ASKED ABOUT A BOX THAT DOES NOT EXIST, which is the caller's own first step (flow_position.h),
 * so `none` and `contents` are a DCHECK rather than an arm: reaching here with either means the predicate that
 * decides box existence and this one disagree. */
static bool fp_is_on_a_line_box(lxb_dom_element_t *el)
{
    /* THE QUESTION IS THE BOX'S LEVEL AND NOT WHETHER IT IS REPLACED, which is what this predicate used to
       answer and is the reason a replaced element crashed here. CSS 2.1 §9.2.2 "Inline-level elements and
       inline boxes" puts BOTH kinds on the line: a non-replaced `display: inline` element "generates an inline
       box", and an inline-level box that is not one — "such as replaced inline-level elements, inline-block
       elements, and inline-table elements" — is an ATOMIC inline-level box that "participate[s] in [its]
       inline formatting context as a single opaque box". §9.4's two normal-flow formatting contexts are
       alternatives decided by exactly that level, so both leave through §9.4.2 and neither is placed by
       §9.4.1's two rules.
       WHAT SEPARATES THEM IS THE FRAGMENT'S SHAPE AND core/layout/line_box.h OWNS IT: an inline box is
       delimited by its two EDGE items and split across as many line boxes as it spans, a replaced element by
       the ONE run item css-text-3 §5.5 "Line Breaking Details" collects for "each replaced element or other
       atomic inline". One answer, one component, and this file composes the coordinate out of it either way.
       THE `inline-block` COMES THROUGH HERE TOO, AND IT IS THE SAME SENTENCE THAT PUTS IT HERE. §9.2.2 names
       it in the list that defines the class — "such as replaced inline-level elements, inline-block elements,
       and inline-table elements" — so it is on a line box for exactly the reason a replaced `display: inline`
       element is, and core/layout/line_box.h delimits its fragment by the same single item index. WHAT USED TO
       KEEP IT OUT WAS NOT ITS LEVEL BUT A MISSING NUMBER, and the number now exists: §10.8.1 "Leading and
       half-leading" puts a non-replaced `inline-block`'s baseline INSIDE the box ("The baseline of an
       'inline-block' is the baseline of its last line box in the normal flow, unless it has either no in-flow
       line boxes or if its 'overflow' property has a computed value other than 'visible', in which case the
       baseline is the bottom margin edge"), so its margin box hangs BELOW the line's baseline rather than
       resting on it, and `line_box_inline_fragments` reads that split (`lb_atomic_extent`) instead of assuming
       the bottom margin edge. BOTH KINDS OF `inline-block` take this arm: CSS 2.2 §10.3.10 "'Inline-block',
       replaced elements in normal flow" is one sentence long — "Exactly as inline replaced elements." — and
       core/layout/used_value.c asks `replaced_element_of` on its `auto` arm BEFORE it asks `uv_box_kind`, so
       the replaced half is sized by §10.3.2 and §10.6.2 with nothing here to arrange.
       THE THREE REMAINING ATOMIC INLINE-LEVEL BOXES DO NOT COME THROUGH HERE, and it is no longer their
       `display` that stops them but a MODULE this engine does not have: an `inline-table`, an `inline-flex`
       and an `inline-grid` leave through `fp_require_placeable` below, which names what each still needs. A
       REPLACED element whose `display` is `block` is not inline-level at all: §9.2.1 makes it a block box and
       §9.4.1's two rules DO place it, so it takes the induction at the end of this function, with §10.3.4's
       and §10.6.2's used extents under it. */
    return fp_computed_is(el, "display", "inline") || fp_computed_is(el, "display", "inline-block");
}

/* CSS 2 §9.4.2 "Inline formatting contexts"' PLACEMENT of a box on a line — "boxes are laid out horizontally,
   one after the other, beginning at the top of a containing block", broken into line boxes whose width the
   containing block decides. It answers a NON-REPLACED inline box's fragments and an ATOMIC inline-level box's
   one fragment through the same entry, because §9.4.2 places both and core/layout/line_box.h owns the one
   difference between them (a range of two edge items, or one atomic item's own index). The atomic half is
   every shape this engine can measure: a REPLACED element, and both halves of the `inline-block` CSS 2.2
   §10.3.9 and §10.3.10 divide between.
   ITS ORIGIN IS ITS FIRST FRAGMENT'S, and that is §9.4.2's own consequence rather than a choice among several:
   "when an inline box exceeds the width of a line box, it is SPLIT into several boxes and these boxes are
   distributed across several line boxes", so the box has as many border areas as it has fragments and exactly
   one of them begins first in content order. CSSOM VIEW §7's `offsetTop`/`offsetLeft` and §6's
   `getBoundingClientRect` both want that one; §6's `getClientRects` wants them ALL and does not come through
   here — core/dom/element_view.c asks core/layout/line_box.h for the list directly, so this entry answering
   the first is not this component deciding the others do not matter.
   THE FRAME IS THE ESTABLISHING BOX'S CONTENT BOX, which core/layout/line_box.h reports in and which is the
   same second case of §10.1 the block-level arm below composes: that box's own origin (this function, one
   level up) plus CSS 2 §8.1's leading border and padding. */
size_t flow_inline_fragment_rects(lxb_dom_element_t *el, FlowRect **out)
{
    lxb_dom_element_t *style = NULL;
    LineBoxFragment *frags = NULL;
    FlowRect *rects;
    FlowPoint o;
    CssPx left, top;
    char ebuf[160], sbuf[160];
    size_t n, i;

    DCHECK(el != NULL && out != NULL,
           "CSS 2 §9.4.2's fragment rectangles were asked for with no element or nowhere to report them");
    n = line_box_inline_fragments(el, &style, &frags);
    DCHECKF(n >= 1 && frags != NULL && style != NULL,
           "%s, establishing box %s, %zu fragment(s) reported%s: "
           "CSS 2 §9.4.2's fragments were reported as none for an inline-level box that generates one. That "
           "entry's own asserts make a zero count impossible — an inline box's two edge items and an atomic "
           "inline-level box's one item are alike content the fill partitions — so this is that contract "
           "having been broken between the two files",
           box_subject(el, ebuf, sizeof ebuf), box_subject(style, sbuf, sizeof sbuf), n,
           frags == NULL ? " and no fragment array" : "");
    /* §10.1's SECOND CASE, composed exactly as the block-level arm below composes it: the establishing box's
       own origin (this function, one level up — which is where a float, an out-of-flow ancestor or a vertical
       writing mode crashes by its own section) plus CSS 2 §8.1's leading border and padding, which is the
       difference between that box's BORDER edge and the CONTENT edge core/layout/line_box.h measures from. */
    o = flow_border_box_origin(style);
    left = css_px_add(o.x, used_value_leading_edge_px(style, false));
    top = css_px_add(o.y, used_value_leading_edge_px(style, true));
    rects = malloc(n * sizeof *rects);
    CHECK(rects != NULL, "out of memory placing CSS 2 §9.4.2's box fragments — one entry per fragment of one "
                         "inline box, so a failure here is the physical floor");
    for (i = 0; i < n; i++) {
        rects[i].x = css_px_add(left, frags[i].inline_start);
        rects[i].y = css_px_add(top, frags[i].block_start);
        rects[i].width = css_px_sub(frags[i].inline_end, frags[i].inline_start);
        rects[i].height = css_px_sub(frags[i].block_end, frags[i].block_start);
    }
    free(frags);
    *out = rects;
    return n;
}

/* THE ORIGIN OF A BOX §9.4.2 PLACES, WHICH IS ITS FIRST FRAGMENT'S CORNER FOR BOTH SHAPES OF SUCH A BOX. An
   inline box has one border area per fragment and exactly one begins first in content order; an ATOMIC
   inline-level box is CSS 2.1 §9.2.2's "single opaque box" and has exactly one, so the same index answers both
   and no caller has to know which it is holding. */
static FlowPoint fp_line_box_origin(lxb_dom_element_t *el)
{
    FlowRect *rects = NULL;
    FlowPoint p;

    (void)flow_inline_fragment_rects(el, &rects);
    p.x = rects[0].x;
    p.y = rects[0].y;
    free(rects);
    return p;
}

static void fp_require_placeable(lxb_dom_element_t *el)
{
    /* NEITHER `table-row` NOR `table-cell` IS ON THIS LIST AND THEIR ABSENCE IS THE PLACEMENT, NOT AN
       OVERSIGHT: CSS 2.1 §17.5's rules 1 and 5 place both, and each leaves through `fp_table_row_origin` or
       `fp_table_cell_origin` above exactly as a box on a line box leaves through `fp_line_box_origin`. */
    static const char *const TABLE_INTERNAL[] = {
        "table-row-group", "table-header-group", "table-footer-group",
        "table-column-group", "table-column", "table-caption",
    };
    char *d = css_computed_value(el, "display");
    char nbuf[160];
    bool table_internal = false, inline_level;
    unsigned i;

    /* THERE IS NO GUARD UNDER THIS ASSERT, AND THE REASON IS A PROPERTY OF THE ENTRY RATHER THAN A PREFERENCE.
       `css_computed_value(el, "display")` cannot answer NULL: it routes `display` through `computed_display`,
       which dereferences the specified value at its own first `strcmp` — so a NULL would fault THERE and never
       reach this line — and every one of that function's return paths is either the specified string itself or
       a `css_cv_strdup`, which CHECKs its allocation. A `if (d == NULL) return;` here was therefore dead in
       both builds while reading as the thing standing between this file and a null, which is worse than
       either: in dev the assert already aborts, and in release it silently declined to place a box for a state
       that cannot arise. A pointer whose non-nullness the callee establishes is asserted here and guarded
       nowhere — the guard's only effect was to make the assert look recoverable. */
    DCHECKF(d != NULL, "%s: the cascade produced no computed `display` for an element whose box is being placed",
            box_subject(el, nbuf, sizeof nbuf));
    DCHECKF(strcmp(d, "none") != 0 && strcmp(d, "contents") != 0,
           "%s: "
           "CSS 2 §9.4.1's placement was asked for an element that GENERATES NO BOX — css-display-3 §2.5 "
           "\"Box Generation: the none and contents keywords\" gives `contents` no box of its own and `none` "
           "no box at all (\"The element and its descendants generate no boxes or text sequences.\"), and "
           "core/dom/element_view.h's one box "
           "predicate reads exactly those two values. The caller's own step establishes the box exists before "
           "asking where it is, so this is that predicate and this test disagreeing",
           box_subject(el, nbuf, sizeof nbuf));
    for (i = 0; i < sizeof(TABLE_INTERNAL) / sizeof(TABLE_INTERNAL[0]); i++)
        if (strcmp(d, TABLE_INTERNAL[i]) == 0) table_internal = true;
    /* NEITHER `inline` NOR `inline-block` IS IN THIS LIST, and that is what makes this function's name true: a
       box whose computed `display` is either one is PLACED now, by §9.4.2 through core/layout/line_box.h, and
       it leaves through `fp_line_box_origin` before this classification is asked — a non-replaced inline box
       out of its two edge items, and every ATOMIC inline-level box this engine can measure out of the one
       atomic item that carries it. What is left here is the atomic inline-level boxes CSS 2.1 §9.2.2
       "Inline-level elements and inline boxes" separates from an inline box AND for which a MODULE outside CSS
       2.1 §10 owns a number §9.4.2's line cannot be filled without. */
    inline_level = strcmp(d, "inline-table") == 0 || strcmp(d, "inline-flex") == 0 ||
                   strcmp(d, "inline-grid") == 0;
    /* THE STRING IS RELEASED BEFORE EITHER CRASH AND NEITHER CRASH READS IT AGAIN. `DFAIL` is compiled out in
       release, so a `free` beside one is a `free` the release build FALLS THROUGH — freeing inside the loop and
       carrying on comparing would be a use-after-free there, and freeing in each arm would be a double free at
       the end. The classification is decided first, the buffer is released once, and the two arms then hold
       nothing.
       AND THE TWO CRASHES NAME THE `display` WITHOUT READING `d`, which is why this rule survived their gaining
       an address: `box_subject` asks the cascade for its own copy and releases it inside itself, so the arms
       still hold nothing after the free above. Folding `d` into either message to save that second read is the
       use-after-free this paragraph is about, arriving through the part of the line that looks like prose. */
    free(d);
    if (table_internal)
        DFAILF("%s: "
              "this box is TABLE-INTERNAL, and CSS 2.1 §17.5 'Visual layout of table contents' positions it "
              "rather than §9.4.1: a row group, a column and a column group are laid out inside the TABLE's "
              "own grid of rows and columns, so a row group's position is the accumulated heights of the "
              "rows above its first, and a column's is the accumulated widths of the columns before it — "
              "neither is a distance §9.4.1's two rules can produce. A CAPTION IS ON THIS LIST FOR A "
              "DIFFERENT REASON AND NOT FOR THAT ONE: §17.4 Tables in the visual formatting model takes it "
              "OUT of the grid entirely, so what refuses it below is the WRAPPER's child box list and not "
              "§17.5 at all. "
              "A ROW AND A CELL ARE NO LONGER AMONG THEM AND THAT IS THE SHAPE OF EVERY ANSWER BELOW: §17.5's "
              "rules 1 and 5 PLACE both (\"Each row box occupies one row of grid cells\" and \"Each cell is "
              "thus a rectangular box, one or more grid cells wide and high\"), core/layout/table_grid.h "
              "reports each placement back per element, and `fp_table_grid_origin` above turns ONE RECTANGLE OF "
              "GRID CELLS into this coordinate — the prefix sums over `TableUsedHeights.rows` and "
              "`TableUsedWidths.columns` with §17.6.1 The separated borders model's spacing between and before "
              "them, measured from the table box's content edge, with rules 3 and 5's `direction` deciding "
              "which end logical column 0 is painted at. So the OPERANDS and the ARITHMETIC are both built and "
              "what each box below still lacks is a NAME FOR ITS OWN RECTANGLE IN THE GRID, which is a "
              "different missing thing per box and not one blocker. "
              "THE CELL'S INSTRUCTION THAT STOOD HERE IS RETIRED WITH THE CODE IT GUARDED, and the reading is "
              "kept rather than deleted because it named the answer's SHAPE wrongly and a reader re-deriving it "
              "would build a second placement: it said a cell's PHYSICAL x is the prefix sum under `ltr` and "
              "\"the table's content width less that sum and less the cell's own width\" under `rtl`. Those "
              "two are the same point, and the second spelling makes every `rtl` coordinate depend on "
              "core/layout/table_width.h's `content` agreeing with its `columns` — an identity that component "
              "asserts only to within its own distribution's rounding and which legitimately FAILS for a "
              "zero-column table. `fp_table_grid_origin` performs rule 5's interchange on the COLUMN INDEX "
              "instead, so both arms read `columns` and `spacing` and neither reads `content`. "
              "A ROW GROUP NEEDS CSS 2.1 §17.5's RULE 2 AND ONE DECISION THE STANDARD DECLINES: \"A row group "
              "occupies the same grid "
              "cells as the rows it contains\", so its rectangle is its rows' — every column, over the RUN of "
              "grid rows core/layout/table_grid.h's `TableGridRowGroup` partitions the grid into, and "
              "`fp_table_grid_origin` above answers `(group->first, 0, ncols)` with no new arithmetic. THE "
              "SENTENCE THAT STOOD HERE SAID THAT RUN WAS NOT STORED YET AND THAT WAS FALSE WHEN IT WAS READ: "
              "the runs are built, `table_grid_row_group_of` reads one back per element, and "
              "core/layout/table_height.h already turns one into an extent. What is genuinely open is rule 2's "
              "EMPTY union — an empty `<tbody>` is a real row group box occupying NO grid cell, which "
              "`table_grid_row_group_of` answers NULL for and which §17.5 gives no edges; the extent taken for "
              "it is ZERO (core/layout/table_height.h records that choice at its site) and there is no "
              "corresponding reading for a POSITION, because a zero-extent box still has to sit somewhere and "
              "the grid names nowhere. DECIDE THAT ONE CASE and the other arm is three lines. "
              "A COLUMN AND A COLUMN GROUP NEED A MAPPING THAT DOES NOT EXIST IN EITHER DIRECTION YET, and this "
              "is the one place below where the operand is genuinely absent rather than merely unrouted. "
              "core/layout/table_column_box.h answers §17.5's rules 3 and 4 from the GRID COLUMN's side — "
              "`TableColumnBoxMap.cols[i]` names the column and column-group boxes occupying column `i` — and a "
              "box holding an ELEMENT needs the reverse, which is that component's entry to add and not this "
              "file's scan. Note its own `ncols` can EXCEED the grid's, since HTML §4.9.12.1 Forming a table "
              "counts a `<colgroup span>` into the table's width, so the reverse answer can name a column no "
              "cell occupies and the horizontal sum above has no width for it. "
              "A CAPTION IS NOT IN THE GRID AT ALL and no longer waits on a box nothing can name — §17.4 "
              "Tables in the visual formatting model puts it in the table WRAPPER, which §10.1's walk now "
              "reports (core/layout/used_value.h) and whose used width and height "
              "core/layout/used_value.c answers; what a caption still waits on is §9.4.1's stack over the "
              "WRAPPER'S OWN CHILD BOX LIST, which core/layout/block_flow.c names in full",
              box_subject(el, nbuf, sizeof nbuf));
    if (inline_level)
        DFAILF("%s: "
              "CSS 2.1 §9.2.2 'Inline-level elements and inline boxes' makes this an ATOMIC INLINE-LEVEL box — "
              "an `inline-table`, or css-display-3 §2's `inline flex` and `inline grid` — boxes CSS 2.1 §9.2.2 "
              "calls atomic \"because they participate in their inline formatting context as a single opaque "
              "box\". So §9.4.2 places it, not §9.4.1, and its position is a position ON A LINE BOX exactly as "
              "a non-replaced inline box's is. "
              "THE PLACEMENT IS BUILT AND SO IS THE `inline-block`, WHICH IS WHY NEITHER IS STILL NAMED HERE: "
              "`line_box_inline_fragments` (core/layout/line_box.h) delimits an atomic inline's fragment by its "
              "own item index, hangs its margin box from the line's baseline SPLIT at the box's own baseline "
              "(`lb_atomic_extent`), and `fp_line_box_origin` above turns that rectangle into this coordinate — "
              "for a `display: inline` box, for a REPLACED element, and for both halves of the `inline-block` "
              "CSS 2.2 §10.3.9 and §10.3.10 divide between. Following an earlier form of this line would have "
              "built that a second time. "
              "WHAT IS LEFT IS NOT A PLACEMENT AT ALL: each of the three boxes above needs a NUMBER from a "
              "module outside CSS 2.1 §10, without which there is nothing for §9.4.2's fill to put on a line. "
              "AN `inline-table` HAS CSS 2.1 §17.2 The CSS table model's box structure — the row groups, rows "
              "and cells §17.2.1 Anonymous table objects generates, which core/layout/table_box.h answers — so "
              "what it still needs is NOT that inline size — CSS 2.1 §17.5.2 Table width algorithms: the "
              "'table-layout' property is core/layout/table_width.h and core/layout/used_value.c routes a "
              "table box's `width` to it, so `used_value_margin_edge_px` answers an `inline-table`'s margin-box "
              "inline size like any other box's — but the baseline CSS 2.2 §10.8.1 \"Leading and half-leading\" states "
              "for it (\"The baseline of an 'inline-table' is the baseline of the first row of the table\"), "
              "which is a position inside that structure and not a rule that can be built beside it. "
              "AN `inline-flex` AND AN `inline-grid` ARE ALREADY CLASSIFIED — `uv_box_kind`'s "
              "`UV_BOX_INLINE_FLEX_GRID` — so an `auto` width on one CRASHES by its own module's name rather "
              "than answering with §10.3.3's constraint equation over a box no part of §10.3 describes. WHAT "
              "THAT CLASSIFICATION LEFT IS THE MODULE'S OWN INTRINSIC MAIN SIZES, which §10.3.9's "
              "shrink-to-fit reads as its two terms: css-flexbox-1 §9.9.1 \"Flex Container Intrinsic Main "
              "Sizes\" and css-grid-1 §5.2 \"Sizing Grid Containers\", whose max-content size is \"the sum "
              "of the grid container's track sizes (including gutters) in the appropriate axis\". Each module "
              "owns that box's BASELINE too and §10.8.1 names neither of them — css-flexbox-1 §8.5 \"Flex "
              "Container Baselines\" derives one from the items on the container's startmost flex line and "
              "css-grid-1 §10.6 \"Grid Container Baselines\" does the same — so both numbers fall out of the "
              "module's layout and neither is this file's to invent. BUILD the module this box's `display` "
              "names as a second producer of `IntrinsicInlineSizes`. An `inline-table` is NOT waiting on that "
              "and this line used to say it was: its inline size is answered, and what it needs is §10.8.1's "
              "own sentence above, which is a position inside CSS 2.1 §17.5.3 Table height algorithms' rows. "
              "Then this box reaches the line and this arm deletes",
              box_subject(el, nbuf, sizeof nbuf));
    /* What is left is a box CSS 2.1 §9.2.1 'Block-level elements and block boxes' makes block-level, which is
       what the two rules below are written about. A `table` is one of them and stays on this path: §17.4
       'Tables in the visual formatting model' says the table wrapper box is block-level for `display: table`
       and inline-level for `display: inline-table`, so the wrapper's own origin is §9.4.1's and what §17.5
       owns is everything INSIDE it — which is the arm above, plus the `table-caption` §17.4 renders as a
       normal block box inside the table WRAPPER box and therefore not over this containing-block chain. */
}

FlowPoint flow_border_box_origin(lxb_dom_element_t *el)
{
    FlowPoint p = { { 0.0, CSS_ENV_NONE, NULL }, { 0.0, CSS_ENV_NONE, NULL } };
    lxb_dom_node_t *n;
    lxb_dom_element_t *cb;
    FlowPoint o;
    /* `vbuf` holds a computed value at two sites and a NODE NAME at the third, so it is sized for the wider of
       the two rather than for the value it happens to carry first. */
    char nbuf[160], vbuf[160];

    DCHECK(el != NULL, "a border box's position was asked for with no element");
    n = lxb_dom_interface_node(el);

    /* §9.4.1 PLACES BOXES IN NORMAL FLOW, so a box that is not in one leaves through its own section first —
       §9.3's positioning scheme is what decides which, and it decides it before any placement rule applies. */
    if (fp_computed_is(el, "position", "absolute") || fp_computed_is(el, "position", "fixed"))
        DFAILF("%s, computed `position` `%s`: "
              "CSS 2 §9.3.1 takes an ABSOLUTELY POSITIONED box out of normal flow, so §9.4.1's placement does "
              "not apply to it at all: its position is §9.3.2's `top`/`right`/`bottom`/`left` resolved against "
              "the containing block §10.1's third and fourth cases give it, and §10.6.4 solves the vertical "
              "pair the same way §10.3.7 solves the horizontal one. Both sections fall back on the STATIC "
              "POSITION — 'where the box would have been in normal flow' — for their `auto` cases, so this is "
              "not an alternative to §9.4.1's flow layout but a consumer of it. BUILD §9.4.1's vertical "
              "stacking first, then §10.3.7 and §10.6.4 over it",
              box_subject(el, nbuf, sizeof nbuf), box_subject_computed(el, "position", vbuf, sizeof vbuf));
    if (!fp_computed_is(el, "float", "none"))
        DFAILF("%s, computed `float` `%s`: "
              "CSS 2 §9.5 'Floats' positions a FLOATING box, and §9.4.1's rule does not: a float is shifted to "
              "the left or right edge of its containing block and then down past any earlier float it would "
              "overlap, under §9.5.1's nine constraints. Its own used width is a SHRINK-TO-FIT (§10.3.5) and "
              "core/layout/used_value.c COMPUTES ONE NOW, over core/layout/intrinsic_size.h's measurement of "
              "the box's content — so the extent is no longer the blocker and the POSITION is the whole of what "
              "is left. BUILD §9.5.1's nine constraints over the line boxes the float interacts with",
              box_subject(el, nbuf, sizeof nbuf), box_subject_computed(el, "float", vbuf, sizeof vbuf));
    fp_require_horizontal_tb(el);
    /* §9.4's TWO NORMAL-FLOW FORMATTING CONTEXTS ARE ALTERNATIVES, and which one places a box is its own
       inline-or-block LEVEL — §9.4.1 is written about a block-level box and §9.4.2 about the boxes on a line.
       A box on a line therefore leaves through its own section HERE, before the classification below, exactly
       as a float and an out-of-flow box leave through theirs above: the two rules at the end of this function
       are §9.4.1's and say nothing about a box on a line box. */
    if (fp_is_on_a_line_box(el)) return fp_line_box_origin(el);
    /* CSS 2.1 §17.5 Visual layout of table contents PLACES A ROW BOX AND A CELL BOX, and they leave HERE for
       the reason a box on a line box does: §9.4.1's two rules are not an approximation of §17.5's grid, they
       are a different algorithm over a different containing rectangle, so neither may fall through to them.
       THEY ARE TWO LINES AND ONE ANSWER — `fp_table_grid_origin` places a rectangle of grid cells and rules 1
       and 5 differ only in which rectangle — so a reader who takes the pair for two placements will build the
       third one twice. The other SIX table-internal boxes still crash below, each naming what §17.5 leaves it
       waiting on. */
    if (fp_computed_is(el, "display", "table-row")) return fp_table_row_origin(el);
    if (fp_computed_is(el, "display", "table-cell")) return fp_table_cell_origin(el);
    fp_require_placeable(el);

    /* §10.1's FIRST CASE, which is §9.4.1's base case as well: the root element's containing block is the ICB,
       "anchored at the canvas origin", so both of §9.4.1's rules reduce to this box's own two margins.
       VERTICALLY, "boxes are laid out one after the other, vertically, beginning at the top of a containing
       block" — there is no preceding sibling, so the box begins at the ICB's top, and §8.3.1 'Collapsing
       margins' states that "margins of the root element's box do not collapse", so the distance from that top
       to the border edge is the root's OWN used `margin-top` and not a collapsed one. */
    if (fp_is_root(n)) {
        p.x = fp_left_offset(el, fp_icb_width(el));
        /* §9.4.1's vertical rule at the top of the containing block, with §8.3.1's root-element exception. */
        p.y = used_value_px(el, "margin-top");
        return p;
    }

    /* §10.1's SECOND CASE, and §9.4.1's inductive step: the containing block is "the CONTENT EDGE of the
       nearest block container ancestor box", so this box's origin is that box's origin plus its own top and
       left border and padding — the two edges CSS 2 §8.1 nests between a border box and a content box — plus
       what §9.4.1 puts between the content edge and this box. HORIZONTALLY that is the same left-outer-edge
       touching the root arm above states, over the containing block's width rather than the ICB's. VERTICALLY
       it is core/layout/block_flow.h's walk: the used height of every preceding in-flow sibling with §8.3.1's
       collapsing between them, which is the one subproblem §10.6.3's content-based height was waiting on too.
       THE RECURSION TERMINATES AT THE ROOT, whose containing block is §10.1's first case — the ICB, which is
       no element's box — so `used_value_containing_block` answering NULL is exactly the arm above. */
    cb = used_value_containing_block(el);
    DCHECKF(cb != NULL,
           "%s, parent %s: "
           "CSS 2.1 §10.1 answered NULL — its first case, the initial containing block — for an element that is "
           "not the root. The two tests are the same test (a node whose parent is the Document), so they have "
           "come apart",
           box_subject(el, nbuf, sizeof nbuf), box_subject_node(n->parent, vbuf, sizeof vbuf));
    o = flow_border_box_origin(cb);
    p.x = css_px_add(css_px_add(o.x, used_value_leading_edge_px(cb, false)),
                     fp_left_offset(el, used_value_containing_block_width(el)));
    p.y = css_px_add(css_px_add(o.y, used_value_leading_edge_px(cb, true)), block_flow_child_top(el));
    return p;
}

/* CSS 2 §8.1 "Box dimensions"' PADDING BOX ORIGIN — the border box origin above moved inward by the ONE leading
   border on each axis. It is a second ENTRY and not a second answer: both coordinates come from
   `flow_border_box_origin`, so a box this component cannot place crashes there, by its own section, before a
   padding edge is ever asked for. */
FlowPoint flow_padding_box_origin(lxb_dom_element_t *el)
{
    FlowPoint p = flow_border_box_origin(el);

    p.x = css_px_add(p.x, used_value_leading_border_px(el, false));
    p.y = css_px_add(p.y, used_value_leading_border_px(el, true));
    return p;
}
