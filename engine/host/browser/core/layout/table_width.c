/* CSS 2.1 §17.5.2 Table width algorithms: the 'table-layout' property, over core/layout/table_grid.h's grid and
   core/layout/table_column_width.h's per-column pair. See table_width.h for the contract, for which box each
   number is measured in, and for every place §17.5.2's own non-normative licence is taken. */
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

/* ---- §17.6 Borders' TWO MODELS, asked once, at the entry ---------------------------------------------------
   §17.6's `border-collapse` selects between §17.6.1 The separated borders model and §17.6.2 The collapsing
   border model, and every piece of arithmetic below is the FIRST one's: the cell spacing term is §17.6.1's
   `border-spacing`, which §17.6.2 gives no meaning to at all, and the column widths this component distributes
   over are sums of each cell's OWN computed `border-*-width`, which §17.6.2 replaces. */
static void tw_require_separated_borders(lxb_dom_element_t *table)
{
    char *collapse = tw_computed(table, "border-collapse");
    bool separated = strcmp(collapse, "separate") == 0;
    bool collapsed = strcmp(collapse, "collapse") == 0;
    char nbuf[160];

    free(collapse);
    DCHECK(separated || collapsed,
           "a `border-collapse` computed to something that is neither of the two keywords CSS 2.1 §17.6 "
           "Borders' `collapse | separate | inherit` grammar admits, and §17.6 gives it `Computed value: as "
           "specified` over a keyword-only value — so nothing between the declaration and here had a rule that "
           "could produce a third one");
    if (separated) return;
    DFAILF("%s: CSS 2.1 §17.5.2 Table width algorithms: the 'table-layout' property was asked for a table in "
           "CSS 2.1 §17.6.2 The collapsing border model, and every width below is CSS 2.1 §17.6.1 The separated "
           "borders model's. TWO TERMS ARE WRONG UNDER §17.6.2 AND NEITHER IS MERELY NARROWER. (1) The CELL "
           "SPACING: §17.6.1's `border-spacing` is what separates adjoining cell borders — \"The lengths specify "
           "the distance that separates adjoining cell borders\" — and §17.6.2 has no such distance, because "
           "\"borders are centered on the grid lines between the cells\" and the cells meet. (2) Each COLUMN's "
           "width, which core/layout/table_column_width.h measures as the BORDER BOX of the cells that occupy "
           "it, over each cell's own computed `border-*-width`: §17.6.2 resolves the borders of the two cells "
           "meeting at a grid line into ONE, so that sum charges the shared edge twice, and the section's own "
           "row-width equation charges the two OUTERMOST borders at HALF each. BUILD §17.6.2's used border "
           "widths — the winner §17.6.2.1 Border conflict resolution picks among the borders of the adjoining "
           "cell, row, row group, column, column group and table boxes — as a per-edge answer beside "
           "core/layout/table_column_width.h's four-term sum, and make the spacing term below zero under this "
           "model. TWO OF THOSE SIX BOXES CAN NOW BE NAMED: core/layout/table_column_box.h answers which column "
           "and column-group box occupies a grid column, which is CSS 2.1 §17.5's rules 3 and 4. THE CHOICE OF "
           "MODEL IS NOT WHAT IS MISSING EITHER: `border-collapse` is in "
           "core/css/css_computed_value.c's modelled set and was just read here",
           box_subject(table, nbuf, sizeof nbuf));
}

/* ---- §17.6.1's CELL SPACING -------------------------------------------------------------------------------
   §17.6.1 states the horizontal term twice and the two sentences together are the whole of this arithmetic.
   BETWEEN cells it is the property itself; at the TWO EDGES of the table it is stated separately — "The
   distance between the table border and the borders of the cells on the edge of the table is the table's
   padding for that side, plus the relevant border spacing distance" — so the spacing appears once more than
   there are gaps between columns, `ncols + 1` times in all, and the table's own padding is NOT part of it
   (§17.6.1's next sentence excludes padding and border from the table's width by name).
   A TABLE WITH NO COLUMNS HAS NO SUCH DISTANCE AT ALL, which is a derivation from those sentences rather than a
   floor chosen here: both are stated over the borders of CELLS, and a table with no columns has none, so there
   is neither a gap to fill nor an edge to stand off from. */
static CssPx tw_spacing_total(CssPx spacing, size_t ncols)
{
    if (ncols == 0) return css_px(0.0);
    return css_px_scale(spacing, (double) (ncols + 1));
}

/* ---- THE TABLE BOX'S OWN HORIZONTAL PADDING AND BORDER ----------------------------------------------------
   §17.4 Tables in the visual formatting model puts these on the TABLE BOX and not on the wrapper — "all other
   values of non-inheritable properties are used on the table box and not the table wrapper box" — so they are
   read off the table element, and core/layout/table_wrapper.h is ASKED to confirm that rather than restated.
   THE PADDING GOES THROUGH core/layout/used_value.h AND THAT IS NOT THE CYCLE core/layout/table_column_width.c
   REFUSES. A percentage padding on a CELL resolves against the table's content width, which §17.5.2 is being
   run to produce; a percentage padding on the TABLE resolves against the table's own containing block, which
   CSS 2.1 §10.1 Definition of "containing block" answers without any of this. Two different bases, one of
   which exists already. */
static CssPx tw_table_edges(lxb_dom_element_t *table)
{
    static const char *const PADDINGS[2] = { "padding-left", "padding-right" };
    static const char *const BORDERS[2] = { "border-left-width", "border-right-width" };
    CssPx edges = css_px(0.0);
    int i;

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

/* ---- WHICH BOX A DECLARED `width` IS ON --------------------------------------------------------------------
   Two independent rules put it on the BORDER box and either one is enough: css-sizing-3 §3.3 "Box Edges for
   Sizing: the box-sizing property"'s `border-box`, and CSS 2.1 §17.6.1 The separated borders model's HTML
   sentence, "However, in HTML and XHTML1, the width of the <table> element is the distance from the left border
   edge to the right border edge." The second is a fact about the ELEMENT rather than about its `display`, which
   is why it is asked with lexbor's own namespace-qualified tag test: a `<div style="display: table">` is a CSS
   table and not an HTML `<table>`, and a `<table style="display: inline-table">` is still the HTML element the
   sentence is about. See table_width.h for why this is not spelled as a UA `box-sizing` declaration. */
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

/* ---- THE TABLE'S OWN `width`, AS A CONTENT WIDTH -----------------------------------------------------------
   Answers false for §17.5.2.2's `width: auto` arm and true with the declared width converted into the box
   §17.6.1 measures the table in. A PERCENTAGE resolves here and is not a cycle: CSS 2.1 §10.2 "Content width:
   the 'width' property" refers it to the CONTAINING BLOCK's width, which §17.5.2 takes as an input in its own
   words ("Input to the automatic table layout must only include the width of the containing block and the
   content of, and any CSS properties set on, the table and any of its descendants"). */
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
    if (w.kind == CSS_LENGTH_KEYWORD && strcmp(w.keyword, "auto") == 0) return false;
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
    /* css-values-4 §9.1 "Numeric Functions"' clamp at used-value time, and CSS 2.1 §10.2's own range in words
       ("negative values for 'width' are illegal"): a negative reaching here is a math function's top-level
       result, which §9.1 clamps rather than treating as an invalid declaration. */
    declared = css_px_max(declared, css_px(0.0));
    if (tw_declared_width_is_border_box(table)) declared = css_px_max(css_px_sub(declared, edges), css_px(0.0));
    *out = declared;
    return true;
}

/* ---- §17.5.2.2's CAPMIN -----------------------------------------------------------------------------------
   "The caption width minimum (CAPMIN) is determined by calculating for each caption the minimum caption outer
   width as the MCW of a hypothetical table cell that contains the caption formatted as "display: block". The
   greatest of the minimum caption outer widths is CAPMIN."
   THE SUBSTITUTION CHANGES NOTHING THIS ENGINE MEASURES, which is why it is not performed. A caption's computed
   `display` is `table-caption`, and core/layout/block_flow.h classifies `table-caption` and `block` alike as
   BLOCK CONTAINERS — so the same one of CSS 2.2 §9.4 "Normal flow"'s two formatting contexts lays the caption's
   content out either way, and core/layout/intrinsic_size.h answers the same pair for both. The hypothetical
   CELL contributes nothing of its own, and the rule for that is CSS 2 §9.2.1.1 Anonymous block boxes rather
   than §17.4, which is a different sentence about a different box: §9.2.1.1 states it of a box no element
   names — "The properties of anonymous boxes are inherited from the enclosing non-anonymous box." and
   "Non-inherited properties have their initial value." — while §17.4's parenthesis is about the SPLIT between
   a table element's two boxes ("Where the table element's values are not used on the table and table wrapper
   boxes, the initial values are used instead."), and §17.4's wrapper is not anonymous at all. So the
   hypothetical cell has no padding, no border and a `width` of `auto`, and §17.5.2.2's step 1 rule about a
   cell's own declared `width` compares against nothing.
   OUTER MEANS css-sizing-3 §2.2 "Intrinsic Size Contributions"' OUTER SIZE — "Intrinsic size contributions are
   based on the outer size of the box; for this purpose, auto margins are treated as zero" — and a caption's
   margins DO apply (CSS 2.1 §8.3 "Margin properties" excludes only table display types other than
   `table-caption`, `table` and `inline-table`), which is asked of the component that owns that line. */
typedef struct {
    CssPx surround;   /* the caption's own horizontal padding and border */
    CssPx margins;    /* its horizontal margins, css-sizing-3 §2.2's `auto` read as zero */
} TwCaptionEdges;

/* ONE of the six terms, resolved under css-sizing-3 §5.2.1 "Intrinsic Contributions of Percentage-Sized
   Boxes". EVERY percentage here is CYCLIC by construction and there is no arm where it is not: a caption's
   containing block is §17.4's table wrapper box, whose width is the border-edge width of the table box, which
   is what §17.5.2 is being run to produce. §5.2.1 states the answer for exactly this case — "For the min size
   properties, as well as for margins and paddings (and gutters), a cyclic percentage is resolved against zero
   for determining intrinsic size contributions" — so this is the standard's own rule and not a floor chosen
   here, and it is NOT the rule for the same six properties at USED-value time, which is why this is a private
   reading inside an intrinsic measurement rather than a call to core/layout/used_value.h. */
static CssPx tw_caption_term(lxb_dom_element_t *caption, const char *name, bool margin)
{
    CssLength len = css_computed_length(caption, name);
    char nbuf[160];

    if (len.kind == CSS_LENGTH_ABSOLUTE) return len.px;
    /* §5.2.1's cyclic percentage, and css-values-4 §10.11 "Computed Value"'s residue that still carries one. */
    if (len.kind == CSS_LENGTH_PERCENTAGE || len.kind == CSS_LENGTH_CALCULATED) return css_px(0.0);
    /* §2.2's "auto margins are treated as zero", which is the ONLY keyword either grammar admits here: CSS 2.1
       §8.3's `<length> | <percentage> | auto` for a margin and §8.4's `<length> | <percentage>` for a padding,
       so a padding keyword is a cascade that answered outside its own grammar. */
    if (margin && len.kind == CSS_LENGTH_KEYWORD && strcmp(len.keyword, "auto") == 0) return css_px(0.0);
    DFAILF("%s: CSS 2.1 §17.5.2.2 Automatic table layout's CAPMIN is the caption's OUTER width, and its `%s` "
           "computed to a value neither CSS 2.1 §8.3 \"Margin properties\"' nor §8.4 \"Padding properties: "
           "'padding-top', 'padding-right', 'padding-bottom', 'padding-left', and 'padding'\"' grammar admits "
           "beside a length, a percentage and (for a margin) `auto`. Both sections give the computed value as "
           "the percentage as specified or the absolute length, so nothing in the cascade can produce this",
           box_subject(caption, nbuf, sizeof nbuf), name);
    return css_px(0.0);
}

static TwCaptionEdges tw_caption_edges(lxb_dom_element_t *caption)
{
    TwCaptionEdges e;

    DCHECK(css_property_applies(caption, "margin-left") && css_property_applies(caption, "margin-right"),
           "CSS 2.1 §17.5.2.2's CAPMIN is a caption's OUTER width and css-sizing-3 §2.2 \"Intrinsic Size "
           "Contributions\" makes an outer size include the margins, and CSS 2.1 §8.3 \"Margin properties\"' "
           "Applies-to line now says the horizontal margins do NOT apply to a caption box — so either that "
           "line changed or this box is not a caption, and CAPMIN would be reported by two margins too small");
    e.surround = css_px_add(css_px_add(tw_caption_term(caption, "padding-left", false),
                                       tw_caption_term(caption, "padding-right", false)),
                            css_px_add(tw_caption_term(caption, "border-left-width", false),
                                       tw_caption_term(caption, "border-right-width", false)));
    e.margins = css_px_add(tw_caption_term(caption, "margin-left", true),
                           tw_caption_term(caption, "margin-right", true));
    DCHECK(e.surround.px >= 0.0,
           "a caption's padding and border summed NEGATIVE. CSS 2.1 §8.4 states that padding values cannot be "
           "negative and css-backgrounds-3 §3.3 \"Line Thickness: the border-width properties\"' <line-width> "
           "is a non-negative <length>, so this is arithmetic that lost a sign rather than a document");
    return e;
}

/* ONE caption's "minimum caption outer width". */
static CssPx tw_caption_outer_min(lxb_dom_element_t *caption)
{
    TwCaptionEdges e = tw_caption_edges(caption);
    IntrinsicInlineSizes sizes = intrinsic_inline_sizes(caption);
    CssPx inner = sizes.min_content;
    CssLength w = css_computed_length(caption, "width");
    char nbuf[160];

    /* §17.5.2.2 defines MCW as the width at which "the formatted content may span any number of lines but may
       not overflow the cell box", so a caption whose own declared `width` exceeds its min-content size raises
       the hypothetical cell's MCW to that width — the same comparison §17.5.2.2's step 1 makes for a real
       cell, and core/layout/table_column_width.c makes it with the same maximum.
       IT IS A RECORDED READING AND NOT THE ONLY ONE: css-sizing-3 §5.2 "Intrinsic Contributions" states a
       box's min-content contribution as "the size of the content box of a hypothetical auto-sized float that
       contains only that box", under which a declared width REPLACES the content-based size and the content
       simply overflows. CSS 2.1's own no-overflow sentence is the one this section is written in, so it is the
       one taken; the two differ only for a caption declaring a width narrower than its own longest word. */
    if (w.kind == CSS_LENGTH_ABSOLUTE) {
        CssPx declared = css_px_max(w.px, css_px(0.0));
        char *sizing = tw_computed(caption, "box-sizing");
        bool border_box = strcmp(sizing, "border-box") == 0;

        free(sizing);
        if (border_box) declared = css_px_max(css_px_sub(declared, e.surround), css_px(0.0));
        inner = css_px_max(inner, declared);
    } else if (w.kind == CSS_LENGTH_PERCENTAGE || w.kind == CSS_LENGTH_CALCULATED) {
        /* css-sizing-3 §5.2.1: a cyclic percentage in a non-replaced box's PREFERRED SIZE property "is
           treated, for the purpose of calculating the box's intrinsic size contributions only, as that
           property's initial value" — `auto` — which is this arm doing nothing. It is a DIFFERENT rule from
           the margins' and paddings' zero above, which is why the two are not one branch. */
        inner = sizes.min_content;
    } else if (!(w.kind == CSS_LENGTH_KEYWORD && strcmp(w.keyword, "auto") == 0)) {
        DFAILF("%s: CSS 2.1 §17.5.2.2 Automatic table layout's CAPMIN measures a caption whose computed `width` "
               "is neither a length, a percentage nor `auto`. css-sizing-3 §3.1.1 \"Preferred Size Properties: "
               "the width and height properties\" carries the intrinsic sizing keywords in that grammar and "
               "each names its own rule over the pair core/layout/intrinsic_size.h has just answered — BUILD "
               "each keyword's arm here rather than reading it as `auto`, which would drop an author's "
               "declaration out of a FLOOR under the whole table's width",
               box_subject(caption, nbuf, sizeof nbuf));
    }
    return css_px_add(css_px_add(inner, e.surround), e.margins);
}

static CssPx tw_capmin(lxb_dom_element_t *table)
{
    lxb_dom_element_t **captions = NULL;
    CssPx capmin = css_px(0.0);
    size_t n, i;

    /* §17.4 Tables in the visual formatting model puts the caption boxes in the WRAPPER and
       core/layout/table_box.h answers which they are; this component does not re-derive them. §17.5.2.2
       Automatic table layout closes CAPMIN over them — "The greatest of the minimum caption outer widths is
       CAPMIN" — and over no captions that is a maximum of nothing, which is zero and is what every comparison
       below wants. */
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

/* ---- THE DISTRIBUTION -------------------------------------------------------------------------------------
   §17.5.2.2's rule 1 ends "If the used width is greater than MIN, the extra width should be distributed over
   the columns", and §17.5.2.1's final paragraph says the same of the fixed algorithm ("If the table is wider
   than the columns, the extra space should be distributed over the columns"). Neither says HOW, and both
   sections offer a family rather than one answer, so THE READING IS RECORDED HERE: the extra is split in
   proportion to each column's own headroom — its maximum less its minimum — because that is the preference the
   columns themselves state, and EQUALLY where every column's headroom is zero. The fixed algorithm has no
   maximum at all and passes `head` as NULL, which takes the same equal split.
   A COLUMN MAY END WIDER THAN ITS MAXIMUM, which is correct rather than an overflow: a table declared wider
   than its content has to put the surplus somewhere, and §17.5.2.2's MAX is an upper operand in the comparison
   that chose the used width, never a cap on a column afterwards.
   THE LAST COLUMN TAKES THE REMAINDER rather than its own share, so the shares sum to `extra` by construction
   and the boundary assert in `table_widths` compares two numbers that were built to be equal.
   THE RATIO GOES THROUGH `css_px_div` AND NOT THROUGH C DIVISION, because a column's headroom can be a function
   of the environment (a `rem`-sized cell, a viewport-derived containing block) and css_length.h's quotient
   carries BOTH operands' facts — a bare `double` divide would decide the split on the modelled viewport and
   delete the arm another viewport takes. */
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

/* ---- §17.5.2.2 Automatic table layout's FINAL RULES ---------------------------------------------------------
   The four steps are core/layout/table_column_width.h's; what is left is the two rules under "Column and
   caption widths influence the final table width as follows", plus the sums MIN and MAX those rules are stated
   over ("the minimum width required by all the columns plus cell spacing or borders (MIN)"). */
static void tw_auto_layout(lxb_dom_element_t *table, const TableGrid *grid, CssPx spacing,
                           bool has_declared, CssPx declared, TableUsedWidths *out)
{
    TableColumnWidth *head = NULL;
    CssPx spacing_total, min_sum, max_sum, capmin, used;
    size_t n, i;

    n = table_column_widths(table, grid, &head);
    DCHECK(n == grid->ncols,
           "CSS 2.1 §17.5.2.2's four steps answered a different number of columns than the grid they were run "
           "over holds — that entry answers `grid->ncols` and nothing else, so the two have come apart");
    DCHECK(head != NULL || n == 0,
           "CSS 2.1 §17.5.2.2's four steps answered a non-zero column count with no array — that entry stores "
           "NULL only for a grid with no columns");
    spacing_total = tw_spacing_total(spacing, n);
    min_sum = spacing_total;
    max_sum = spacing_total;
    for (i = 0; i < n; i++) {
        min_sum = css_px_add(min_sum, head[i].min);
        max_sum = css_px_add(max_sum, head[i].max);
    }
    capmin = tw_capmin(table);
    if (has_declared) {
        /* RULE 1: "If the 'table' or 'inline-table' element's 'width' property has a computed value (W) other
           than 'auto', the used width is the greater of W, CAPMIN, and the minimum width required by all the
           columns plus cell spacing or borders (MIN)." */
        used = css_px_max(css_px_max(declared, capmin), min_sum);
    } else {
        /* RULE 2: "If the 'table' or 'inline-table' element has 'width: auto', the used width is the greater of
           the table's containing block width, CAPMIN, and MIN. However, if either CAPMIN or the maximum width
           required by the columns plus cell spacing or borders (MAX) is less than that of the containing
           block, use max(MAX, CAPMIN)."
           THE "HOWEVER" IS READ LITERALLY, AS A DISJUNCTION OVER THE TWO NAMED QUANTITIES, and that reading is
           RECORDED rather than smoothed: the sentence says "either CAPMIN or ... (MAX) is less than", so a
           table whose MAX exceeds its containing block while CAPMIN does not still takes the override and comes
           out at max(MAX, CAPMIN) — wider than its containing block, holding its content unbroken. A user agent
           that shrink-to-fits instead (clamping the containing block between MIN and MAX) is equally
           conforming, because §17.5.2.2 is non-normative from "The remainder of this section is non-normative"
           onward and says outright that a UA "can use any other algorithm even if it results in different
           behavior". What is NOT available is treating the ambiguity as a gap: both readings answer, and a
           crash here would refuse a document either one lays out. */
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
   it only depends on the table's width, the width of the columns, and borders or cell spacing." That sentence
   is the whole of why this arm reads no intrinsic size at all, and why it is a DIFFERENT algorithm rather than
   a mode of §17.5.2.2 Automatic table layout: its step 2 is stated over the FIRST ROW alone — "Cells in
   subsequent rows do not affect column widths" — so a component that measured every cell would answer a
   different width for the same document while looking like it had merely been thorough.
   IT IS REACHED ONLY WITH A DECLARED WIDTH, which §17.5.2.1's own second paragraph requires: "A value of 'auto'
   (for both 'display: table' and 'display: inline-table') means use the automatic table layout algorithm." */
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
    /* THE COLUMN COUNT THIS ALGORITHM DIVIDES SPACE OVER IS THE SECTION'S OWN AND IT IS NOT THE GRID'S ALONE.
       §17.5.2.1 says so where it says what happens when they differ: "If a subsequent row has more columns than
       the greater of the number determined by the table-column elements and the number determined by the first
       row, then additional columns may not be rendered." — so the table-column elements DETERMINE a number, and
       the table's count is at least it. core/layout/table_grid.h grows `ncols` only to cover the cells it
       places, so `<colgroup span="5">` over two-cell rows leaves this algorithm dividing the table's space over
       TWO columns where a browser divides it over five, and step 3's "Any remaining columns equally divide the
       remaining horizontal table space" then hands each of them more than twice its width. THAT IS WHY THIS
       CRASH IS UNCONDITIONAL HERE AND CONDITIONED ON A DECLARED WIDTH IN §17.5.2.2 Automatic table layout: an
       `auto` column contributes nothing to a FLOOR, and it takes a full share of an equal DIVISION. */
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
    /* STEP 1: "A column element with a value other than 'auto' for the 'width' property sets the width for that
       column." WHICH grid columns such a box covers is CSS 2.1 §17.5 Visual layout of table contents' rules 3
       and 4, which is core/layout/table_column_box.h.
       "A COLUMN ELEMENT" IS THE COLUMN BOX AND NOT THE GROUP, AND THAT NARROWNESS IS RECORDED RATHER THAN
       GUESSED AT. §17.2 The CSS table model makes 'table-column' and 'table-column-group' two box types, and
       this section names only the first — here and again in its own later sentence, "the number determined by
       the table-column elements". A `<colgroup width="100">` over columns of its own therefore sets nothing in
       THIS algorithm and its columns fall to steps 2 and 3, which is the section read literally; §17.5.2.2
       Automatic table layout's step 4 is where a column group's width is given an effect, and it gives it a
       different one (a floor under the SUM of the minima, not a width).
       NO BOX CONVERSION IS OWED, WHICH SEPARATES THIS FROM STEP 2's CELL BELOW. A column width is a border-box
       width (core/layout/table_column_width.h's recorded choice), and in this border model a column box has
       neither a border nor a padding to differ by: CSS 2.1 §17.6.1 The separated borders model states "Rows,
       columns, row groups and column groups cannot have borders (i.e., user agents must ignore the border
       properties for those elements)." and §17.5's own opening states "Internal table elements generate
       rectangular boxes with content and borders. Cells have padding as well." — padding for the cells alone.
       So the declared number is already the column's whole width. */
    for (i = 0; i < n; i++) {
        CssLength cw;
        CssPx width;

        if (boxes.cols[i].column == NULL) continue;
        cw = css_computed_length(boxes.cols[i].column, "width");
        if (cw.kind == CSS_LENGTH_KEYWORD && strcmp(cw.keyword, "auto") == 0) continue;
        if (cw.kind == CSS_LENGTH_ABSOLUTE) {
            width = cw.px;
        } else if (cw.kind == CSS_LENGTH_PERCENTAGE || cw.kind == CSS_LENGTH_CALCULATED) {
            /* §17.5.2.2's "A percentage value for a column width is relative to the table width." — resolvable
               HERE and a cycle there, for this algorithm's own first reason: "the horizontal layout of the
               table does not depend on the contents of the cells; it only depends on the table's width, the
               width of the columns, and borders or cell spacing", and that width is the declared one, already
               in hand. It is the identical arm step 2 below takes for a first-row cell. */
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
    /* STEP 2: "Otherwise, a cell in the first row with a value other than 'auto' for the 'width' property
       determines the width for that column. If the cell spans more than one column, the width is divided over
       the columns." THE DIVISION IS EQUAL, which is a recorded reading: the sentence names no distribution, and
       §17.5.2.2's own spanning step ("If possible, widen all spanned columns by approximately the same amount")
       is the nearest thing the chapter says about dividing one cell's width over several columns.
       A CELL WHOSE COLUMN WAS ALREADY SET DOES NOT SET IT AGAIN — "Otherwise" and "a cell", singular, make the
       first cell reaching a column the one that determines it, and core/layout/table_grid.h reports the first
       row's cells in their own tree order. */
    for (k = 0; k < grid->ncells && n > 0; k++) {
        const TableGridCell *cell = &grid->cells[k];
        CssLength cw;
        CssPx width = css_px(0.0);
        size_t c;

        if (cell->row != 0) continue;
        /* §17.2.1 Anonymous table objects' anonymous cell names no element and therefore declares nothing;
           §17.5.2.1's step 2 is stated over a cell "with a value other than 'auto'", which it cannot have. */
        if (cell->element == NULL) continue;
        cw = css_computed_length(cell->element, "width");
        if (cw.kind == CSS_LENGTH_KEYWORD && strcmp(cw.keyword, "auto") == 0) continue;
        if (cw.kind == CSS_LENGTH_ABSOLUTE) {
            width = cw.px;
        } else if (cw.kind == CSS_LENGTH_PERCENTAGE || cw.kind == CSS_LENGTH_CALCULATED) {
            /* "A percentage value for a column width is relative to the table width", and under this algorithm
               the table's width is the DECLARED one and is already in hand — which is exactly what makes the
               percentage resolvable here and a cycle in §17.5.2.2, where the table's width is the output. */
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
        /* A column width is a BORDER-BOX width (core/layout/table_column_width.h's recorded choice), so a cell
           sized in its content box has its own padding and border added — the one four-term sum both of
           §17.5.2's algorithms share — and one sized in its border box floors at that same sum, which is
           css-sizing-3 §3.3's own "the content box width and height are calculated by subtracting the border
           and padding … and flooring the result at zero" read from the outside. */
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
    /* STEP 3: "Any remaining columns equally divide the remaining horizontal table space (minus borders or cell
       spacing)." The table space is the declared width; "minus borders or cell spacing" is §17.6.1's spacing
       term, which the table's content width holds and the columns do not. */
    avail = css_px_max(css_px_sub(css_px_sub(declared, spacing_total), assigned_sum), css_px(0.0));
    if (remaining > 0) {
        CssPx share = css_px_scale(avail, 1.0 / (double) remaining);

        for (i = 0; i < n; i++) if (!assigned[i]) out->columns[i] = share;
    }
    sum = spacing_total;
    for (i = 0; i < n; i++) sum = css_px_add(sum, out->columns[i]);
    /* "The width of the table is then the greater of the value of the 'width' property for the table element
       and the sum of the column widths (plus cell spacing or borders). If the table is wider than the columns,
       the extra space should be distributed over the columns." The second sentence is what step 3 has already
       spent the surplus on when there WAS a remaining column; it still has work to do when there was not, which
       is a table all of whose columns the first row declared. */
    out->content = css_px_max(declared, sum);
    tw_distribute(out->columns, NULL, n, css_px_sub(out->content, sum));
    free(assigned);
    table_column_boxes_release(&boxes);
}

void table_widths(lxb_dom_element_t *table, const TableGrid *grid, TableUsedWidths *out)
{
    CssBorderSpacing spacing;
    CssPx edges, declared = css_px(0.0), check_sum, slack;
    char *display, *layout;
    bool is_table_box, fixed, has_declared;
    size_t i;
    char nbuf[160];

    DCHECK(table != NULL && grid != NULL && out != NULL,
           "CSS 2.1 §17.5.2's table width was asked for with no element, no grid or nowhere to put it. The grid "
           "is the OPERAND every column in the section is stated over, and nothing before "
           "core/layout/table_grid.h says which cells occupy which column");
    DCHECK(grid->cells != NULL || grid->ncells == 0,
           "CSS 2.1 §17.5.2 was handed a grid with no cell array and a non-zero cell count — `table_grid_build` "
           "stores NULL only for a table whose rows generate no cell, so the two have been carried apart since "
           "it answered");
    display = tw_computed(table, "display");
    is_table_box = table_box_kind_generates_table_box(table_box_kind(display));
    free(display);
    if (!is_table_box)
        DFAILF("%s: CSS 2.1 §17.5.2 Table width algorithms: the 'table-layout' property is stated over a TABLE "
               "box — both of its final rules name \"the 'table' or 'inline-table' element\" — and this box's "
               "computed `display` (printed above) is a different one of CSS 2.1 §17.2 The CSS table model's "
               "box types. The width of a table-internal box is NOT this question and must not be answered "
               "from it: a CELL's is the used width of the columns it occupies, which this component "
               "distributes and reports in `TableUsedWidths.columns` and which "
               "`table_cell_used_border_box` above takes out of them; a ROW's and a ROW GROUP's is NOT the "
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
    tw_require_separated_borders(table);
    /* §17.6.1's `Computed value: two absolute lengths` — core/css/css_computed_value.h answers the pair whole
       because neither the text entry nor the single-length one can carry it. Only the horizontal half is a
       term of a WIDTH; the vertical half is CSS 2.1 §17.5.3 Table height algorithms'. */
    spacing = css_computed_border_spacing(table);
    DCHECK(spacing.horizontal.px >= 0.0,
           "CSS 2.1 §17.6.1 The separated borders model states \"Lengths may not be negative\" of "
           "`border-spacing`, and core/css/css_computed_value.c asserts that where it computes the pair — so a "
           "negative reaching here is arithmetic that lost a sign rather than a document");
    edges = tw_table_edges(table);
    layout = tw_computed(table, "table-layout");
    fixed = strcmp(layout, "fixed") == 0;
    DCHECK(fixed || strcmp(layout, "auto") == 0,
           "a `table-layout` computed to something that is neither of the two keywords CSS 2.1 §17.5.2 Table "
           "width algorithms: the 'table-layout' property's `auto | fixed | inherit` grammar admits, and that "
           "section gives it `Computed value: as specified` over a keyword-only value — so nothing between the "
           "declaration and here had a rule that could produce a third one");
    free(layout);
    has_declared = tw_declared_content_width(table, edges, &declared);
    /* THE DISPATCH, WHICH IS TWO QUESTIONS AND NOT ONE. §17.5.2 makes the fixed algorithm the one case a UA may
       not substitute for ("except when the fixed layout algorithm is selected"); §17.5.2.1 Fixed table layout
       then hands a `width: auto` table straight back to the automatic algorithm in its own sentence. So
       `table-layout` alone does not decide, and a component that dispatched on it alone would run the fixed
       algorithm with no table space for its step 3 to divide. See table_width.h for the optional §10.3.3 arm
       the same paragraph offers and this component declines. */
    if (fixed && has_declared) tw_fixed_layout(table, grid, spacing.horizontal, declared, out);
    else tw_auto_layout(table, grid, spacing.horizontal, has_declared, declared, out);
    /* THE SPACING IS PART OF THE ANSWER AND IS STORED FROM THE ONE READ THE LAYOUT RAN UNDER — see
       table_width.h for why a consumer cannot recover it from `content` and the columns. It is written HERE,
       past both algorithms, so neither of them can store a different one. */
    out->spacing = spacing.horizontal;
    DCHECK(out->ncols == grid->ncols && (out->columns != NULL || out->ncols == 0),
           "CSS 2.1 §17.5.2 answered a different number of used column widths than the grid it was run over "
           "holds, or a non-zero count with no array — every consumer of this answer indexes it by the grid's "
           "own column number, so the two must not come apart");
    check_sum = tw_spacing_total(spacing.horizontal, out->ncols);
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
    /* §17.6.1's DEFINITION OF THE TABLE'S WIDTH, ASSERTED RATHER THAN ASSUMED: "The width of the table is the
       distance from the left inner padding edge to the right inner padding edge (including the border spacing
       but excluding padding and border)" — which IS the used column widths plus that spacing, so the two fields
       of this answer are one fact and a consumer may place cells from `columns` and size the box from `content`
       without the two describing different boxes. A ZERO-COLUMN TABLE IS THE ONE CASE WHERE THEY LEGITIMATELY
       DIFFER: there is no cell edge to stand off from, so the sum is zero while the table still takes the
       greater of its own declared `width` and CAPMIN. THE TOLERANCE IS THE DISTRIBUTION'S OWN ROUNDING —
       `tw_distribute` gives the last column the remainder so the shares sum to the surplus by construction, but
       the per-column additions there and this re-summation are two different orders of the same terms and IEEE
       addition is not associative. */
    DCHECK(out->ncols == 0 ||
               slack.px <= 1e-9 * (1.0 + (out->content.px < 0.0 ? -out->content.px : out->content.px)),
           "CSS 2.1 §17.6.1 The separated borders model defines a table's width as the distance from its left "
           "inner padding edge to its right inner padding edge including the border spacing, which is exactly "
           "the used column widths plus that spacing — and this answer's two halves disagree by more than the "
           "distribution's own rounding. A consumer placing cells from the columns and sizing the box from the "
           "content width would lay them out in two different rectangles");
    DCHECK(out->content.px >= 0.0,
           "CSS 2.1 §17.5.2 gave a table a NEGATIVE used width. Every operand of both algorithms' final "
           "comparison is a floored declared length, a containing block width, CAPMIN or a sum of non-negative "
           "column widths");
}

/* See table_width.h for the reading the N-1 spacings encode and for why this is a BORDER-box number. */
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
    /* The N-1 spacings a spanning cell covers, added only when there are any — `border-spacing` is not an
       operand of a cell that spans one column, and multiplying it by zero would union its environment facts
       into an answer it contributes nothing to (core/css/css_length.h's domain rides every operand). */
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
