/* CSS 2 §9.4.1's block formatting context and CSS 2.1 §8.3.1's collapsing margins — the one walk that answers
   both §10.6.3's content-based height and a box's vertical position. See block_flow.h for the contract, for
   why the two are one component, and for what each unbuilt case is waiting on. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/dom/document.h"
#include "core/layout/block_flow.h"
#include "core/layout/used_value.h"

static char *bf_computed(lxb_dom_element_t *el, const char *name)
{
    char *v = css_computed_value(el, name);

    DCHECK(v != NULL, "the cascade produced no computed value for a property this engine models — every one of "
                      "them is in lexbor's registry with an initial value, so the last layer always answers");
    return v;
}

static bool bf_computed_is(lxb_dom_element_t *el, const char *name, const char *kw)
{
    char *v = bf_computed(el, name);
    bool same = strcmp(v, kw) == 0;

    free(v);
    return same;
}

static bool bf_length_is(lxb_dom_element_t *el, const char *name, const char *kw)
{
    CssLength len = css_computed_length(el, name);

    return len.kind == CSS_LENGTH_KEYWORD && strcmp(len.keyword, kw) == 0;
}

static bool bf_length_is_zero(lxb_dom_element_t *el, const char *name)
{
    CssLength len = css_computed_length(el, name);

    return len.kind == CSS_LENGTH_ABSOLUTE && len.px.px == 0.0;
}

static bool bf_is_root(const lxb_dom_node_t *n)
{
    return n->parent != NULL && n->parent->type == LXB_DOM_NODE_TYPE_DOCUMENT;
}

static bool bf_is_body(const lxb_dom_node_t *n)
{
    return n->owner_document != NULL &&
           document_body_of(lxb_dom_interface_node(n->owner_document)) == (lxb_dom_node_t *)n;
}

bool block_flow_display_is_block_container(const char *display)
{
    static const char *const BLOCK_CONTAINER[] = {
        "block", "flow-root", "list-item", "inline-block", "table-cell", "table-caption",
    };
    unsigned i;

    DCHECK(display != NULL, "the block-container question was asked about a NULL computed `display`");
    for (i = 0; i < sizeof(BLOCK_CONTAINER) / sizeof(BLOCK_CONTAINER[0]); i++)
        if (strcmp(BLOCK_CONTAINER[i], display) == 0) return true;
    return false;
}

/* ---- §8.3.1's ADJOINING RUN ------------------------------------------------------------------------------
   "When two or more margins collapse, the resulting margin width is the maximum of the collapsing margins'
   widths. In the case of negative margins, the maximum of the absolute values of the negative adjoining
   margins is deducted from the maximum of the positive adjoining margins. If there are no positive margins,
   the maximum of the absolute values of the adjoining margins is deducted from zero." Those three sentences
   are ONE reduction over two running maxima — the third is the second with the positive maximum left at its
   identity — so a run is two lengths, and it merges associatively, which is what lets the same structure carry
   a run up out of a child, down out of a child, and straight through one that collapses through.
   THE MAXIMA ARE `CssPx` AND NOT `double` because a margin may be a PERCENTAGE, which §8.3 resolves against
   the containing block's WIDTH — the viewport's, at the base of §10.1's chain — so which of two margins is
   larger is a question the environment could answer either way, and css_px_max carries BOTH operands' facts
   for exactly that reason. Deciding it on the modelled number is css_length.h's stated layering. */
typedef struct {
    CssPx pos;    /* the largest positive margin in the run */
    CssPx neg;    /* the largest ABSOLUTE value among the negative margins */
    bool  any;    /* whether any margin has joined it at all */
} BfRun;

static BfRun bf_run_empty(void)
{
    BfRun r;

    r.pos = css_px(0.0);
    r.neg = css_px(0.0);
    r.any = false;
    return r;
}

static BfRun bf_run_add(BfRun r, CssPx m)
{
    if (m.px < 0.0) r.neg = css_px_max(r.neg, css_px_scale(m, -1.0));
    else            r.pos = css_px_max(r.pos, m);
    r.any = true;
    return r;
}

static BfRun bf_run_of(CssPx m)
{
    return bf_run_add(bf_run_empty(), m);
}

static BfRun bf_run_merge(BfRun a, BfRun b)
{
    BfRun r;

    r.pos = css_px_max(a.pos, b.pos);
    r.neg = css_px_max(a.neg, b.neg);
    r.any = a.any || b.any;
    return r;
}

static CssPx bf_run_value(BfRun r)
{
    return css_px_sub(r.pos, r.neg);
}

/* ---- what a box is, for §8.3.1's two lists ---------------------------------------------------------------- */

/* §9.4.1's own list of what establishes a new block formatting context — "floats, absolutely positioned
   elements, block containers (such as inline-blocks, table-cells, and table-captions) that are not block
   boxes, and block boxes with overflow other than visible (except when that value has been propagated to the
   viewport)" — plus css-display §2.1's `flow-root`, which is the same box under its own name.
   THE ROOT ELEMENT IS HERE FOR A DIFFERENT REASON AND THE CALLER ONLY EVER ASKS THE COMBINED QUESTION: it
   establishes nothing the other entries do, but §8.3.1's first exception states outright that "margins of the
   root element's box do not collapse", which is the same answer to the only question this predicate is asked —
   may a child's margin run escape through this box's edge. Keeping them apart would be two predicates whose
   callers all merge them again. */
static bool bf_no_collapse_through_edges(lxb_dom_element_t *el)
{
    lxb_dom_node_t *n = lxb_dom_interface_node(el);
    char *d;
    bool bfc;

    if (bf_is_root(n)) return true;
    if (bf_computed_is(el, "position", "absolute") || bf_computed_is(el, "position", "fixed")) return true;
    if (!bf_computed_is(el, "float", "none")) return true;
    d = bf_computed(el, "display");
    bfc = strcmp(d, "inline-block") == 0 || strcmp(d, "flow-root") == 0 ||
          strcmp(d, "table-cell") == 0 || strcmp(d, "table-caption") == 0;
    free(d);
    if (bfc) return true;
    if (bf_computed_is(el, "overflow-x", "visible") && bf_computed_is(el, "overflow-y", "visible")) return false;
    /* §9.4.1's own parenthesis — "except when that value has been propagated to the viewport" — is a question
       about the ROOT element and the BODY, and css-overflow §3.5 is the rule it is excepting. The root left
       through the first line above; the body has not, and the answer decides whether its margins collapse with
       its children's. */
    if (bf_is_body(n))
        DFAIL("CSS 2 §9.4.1 excepts an overflow 'propagated to the viewport' from the boxes that establish a "
              "block formatting context, and this is the BODY element with a computed overflow that is not "
              "`visible`. css-overflow §3.5 is that rule: 'UAs must apply the overflow-* values set on the root "
              "element to the viewport', and when the root's own used value is `visible` the value is taken "
              "from the body instead — after which 'the element from which the value is propagated must then "
              "have a used overflow value of visible', so this box would NOT establish a formatting context and "
              "its margins WOULD collapse with its children's. Whether that happened is a fact about the ROOT "
              "element's used overflow, and core/css/css_computed_value.c's computed_overflow implements "
              "css-overflow §3's visible-plus-scrollable pairing and not §3.5's propagation at all. BUILD §3.5 "
              "there — a rule that reads the root element's cascade from the body's computed value, which is "
              "the shape §7's inheritance already has — and this crash becomes the boolean it stands in for");
    return true;
}

/* §8.3.1's "no line boxes, no clearance, no padding and no border separate them", for ONE of a box's two
   horizontal edges. Clearance and line boxes are decided by the child walk below — a float and an inline-level
   child each crash there — so what is left at the edge itself is the padding and the border. */
static bool bf_edge_is_open(lxb_dom_element_t *el, bool top)
{
    CssLength b = css_computed_length(el, top ? "border-top-width" : "border-bottom-width");

    DCHECK(b.kind == CSS_LENGTH_ABSOLUTE,
           "a `border-*-width` computed to something that is not an absolute length. css-backgrounds-3 §3.3's "
           "`Computed value:` line is `absolute length, snapped as a border width` and every arm of that "
           "derivation produces one, so a percentage or a keyword here is a rule that did not run");
    return b.px.px == 0.0 && used_value_px(el, top ? "padding-top" : "padding-bottom").px == 0.0;
}

/* §8.3.1's `min-height` conjunct, which its third and fourth adjoining pairs both carry: "zero computed
   min-height". `min-height: auto` is the initial value css-sizing-3 §3.1.2 "Minimum Size Properties: the
   min-width and min-height properties" gives the property, and §3.2 "Sizing Values: the <length-percentage>,
   auto | none, min-content, max-content, and fit-content() values" resolves it: "unless otherwise defined by
   the relevant layout module, however, it resolves to a used value of 0". The one module that defines
   otherwise is css-flexbox-1 §4.5 "Automatic Minimum Size of Flex Items", and such an item never reaches this
   walk — core/layout/used_value.c classifies it and sends its size to its container's algorithm. */
static bool bf_min_height_is_zero(lxb_dom_element_t *el)
{
    return bf_length_is_zero(el, "min-height") || bf_length_is(el, "min-height", "auto");
}

/* ---- the child list --------------------------------------------------------------------------------------- */

typedef enum {
    BF_CHILD_NO_BOX = 0,   /* generates no box at all, or is out of flow: §10.6.3 ignores it */
    BF_CHILD_BLOCK         /* an in-flow block-level box this walk places */
} BfChildKind;

static bool bf_text_is_all_whitespace(const lxb_dom_node_t *n)
{
    const lxb_dom_character_data_t *cd = (const lxb_dom_character_data_t *)n;
    const lxb_char_t *d = cd->data.data;
    size_t len = cd->data.length, i;

    if (d == NULL) return true;
    for (i = 0; i < len; i++) {
        char c = (char)d[i];

        if (c != ' ' && c != '\t' && c != '\n' && c != '\f' && c != '\r') return false;
    }
    return true;
}

/* CSS 2.1 §9.2.2.1: "White space content that would subsequently be collapsed away according to the
   'white-space' property does not generate any anonymous inline boxes." So a run of white space between two
   block-level children is a box or is nothing, and which one it is is the INHERITED `white-space` of the
   element the anonymous inline box would belong to — the parent, since an anonymous box inherits from the box
   it is inside. §16.6's table gives the answer per value: `normal` and `nowrap` collapse a sequence of white
   space, `pre`, `pre-wrap` and `pre-line` preserve it (css-text-3 adds `break-spaces`), and a preserved run is
   real inline content with a real line box. */
bool block_flow_text_child_generates_box(lxb_dom_element_t *parent, const lxb_dom_node_t *n)
{
    char *ws;
    bool collapses;

    DCHECK(parent != NULL && n != NULL && n->type == LXB_DOM_NODE_TYPE_TEXT,
           "§9.2.2.1's white-space question was asked about something that is not a TEXT node inside an "
           "element — the rule is about a run of character data and the property it reads is the containing "
           "element's inherited `white-space`");

    if (!bf_text_is_all_whitespace(n))
        DFAIL("CSS 2 §9.4.2 'Inline formatting contexts' owns a TEXT run, and this block container has one "
              "beside its block-level children — so §9.2.1.1 wraps it in an ANONYMOUS BLOCK BOX ('if a block "
              "container box has a block-level box inside it, then we force it to have only block-level boxes "
              "inside it'), and that anonymous box's height is §10.6.3's first bullet, 'the bottom edge of the "
              "last line box'. WHAT IS MISSING IS ONE HALF OF THE MEASUREMENT AND NOT THE WHOLE OF IT, and "
              "saying otherwise sends the next reader to build what is here. The VERTICAL half is answered: "
              "CSS 2 §10.8.1 'Leading and half-leading''s `A` and `D` — 'a characteristic height above the "
              "baseline and a depth below it' — are core/css/font_metrics.h's, `AD` with them, and §10.8 "
              "'Line height calculations: the line-height and vertical-align properties''s "
              "step 1 reads an inline box's `line-height` through core/css/css_computed_value.h's "
              "`css_computed_line_height`, whose `normal` arm IS that `AD`. §10.8's step 2 aligns those boxes "
              "'according to their vertical-align property', and that is answered too: css-inline-3 §4.2 "
              "'Transverse Box Alignment: the vertical-align property' makes it a shorthand of "
              "`baseline-source`, `alignment-baseline` and `baseline-shift`, core/css/css_shorthand.c expands "
              "it and css_computed_value.c derives all three. So §10.8.1's leading (L = 'line-height' - AD, "
              "half above `A` and half below `D`) is arithmetic over values this engine holds. "
              "WHAT IS ABSENT IS THE HORIZONTAL half: the ADVANCE of an arbitrary glyph, which is what decides "
              "the break opportunities and therefore HOW MANY line boxes there are. font_metrics.h holds the "
              "advance of exactly two glyphs — css-values-4 §6.1.1's assumed '0' for `ch` and '水' for `ic` — "
              "and no measurement of a run of text, so the count of line boxes is the one term of §10.6.3's "
              "first bullet with nothing to derive it from, and a zero here would be an invented height every "
              "ancestor's geometry would then be stated over. BUILD the per-glyph advance beside `A` and `D` "
              "in core/css/font_metrics.h, then §9.4.2's line boxes over it with §10.8's calculation for their "
              "heights, then §9.2.1.1's anonymous block generation so this walk has a box to place");
    ws = bf_computed(parent, "white-space");
    collapses = strcmp(ws, "normal") == 0 || strcmp(ws, "nowrap") == 0;
    free(ws);
    if (collapses) return false;
    DFAIL("CSS 2.1 §9.2.2.1 generates no anonymous inline box for white space 'that would subsequently be "
          "collapsed away according to the white-space property', and this element's computed `white-space` "
          "PRESERVES it (§16.6 gives `pre`, `pre-wrap` and `pre-line` that line, and css-text-3 adds "
          "`break-spaces`) — so the white space between these block-level children is real inline content, "
          "§9.2.1.1 wraps it in an anonymous block box, and that box's height is a LINE BOX whose extent is the "
          "preserved run measured with a real font. BUILD §9.4.2's inline formatting context; this is the same "
          "missing component a non-white-space text run names above, reached through the one property that "
          "decides whether the run exists at all");
    return false;
}

static BfChildKind bf_element_child(lxb_dom_element_t *el)
{
    char *d;
    bool block, inline_level, table;

    /* §10.6.3: "Only children in the normal flow are taken into account (i.e., floating boxes and absolutely
       positioned boxes are ignored…)". The absolutely positioned half is the rule RUNNING and not a gap: the
       box is out of flow, so it contributes nothing to this walk, and its own position is §9.3.2's offsets over
       a static position that this walk is what will one day supply. */
    if (bf_computed_is(el, "position", "absolute") || bf_computed_is(el, "position", "fixed"))
        return BF_CHILD_NO_BOX;
    if (!bf_computed_is(el, "float", "none"))
        DFAIL("CSS 2 §9.5 'Floats' positions this child, and §10.6.3's own parenthesis says a float is IGNORED "
              "when the container's height is computed — so the float alone would not stop this walk. What "
              "stops it is what a float does to its SIBLINGS: §9.5.2's `clear` on a later block-level box "
              "introduces CLEARANCE, §8.3.1 makes a margin with clearance NON-ADJOINING ('no line boxes, no "
              "clearance, no padding and no border separate them'), and §9.5.2 then shifts that box down past "
              "the float's bottom margin edge. One float therefore invalidates every collapse and every offset "
              "below it in this formatting context, and `clear` is not among the properties "
              "core/css/css_computed_value.c models, so there is nothing to read it through either. §10.6.7 "
              "wants the float as well, for a container that establishes a formatting context: 'if the element "
              "has any floating descendants whose bottom margin edge is below the element's bottom content "
              "edge, then the height is increased to include those edges'. BUILD §9.5.1's float placement, and "
              "record `clear` — CSS 2 §9.5.2 gives it `Computed value: as specified`, so it is a row of "
              "css_computed_models' as-specified arm and a row of css_shorthand_complete_for");
    d = bf_computed(el, "display");
    if (strcmp(d, "none") == 0) { free(d); return BF_CHILD_NO_BOX; }
    if (strcmp(d, "contents") == 0)
        DFAIL("css-display §3.1 gives this child `display: contents`, which 'does not generate any boxes "
              "itself, but its children and pseudo-elements still generate boxes and text runs as normal' — so "
              "the box tree and the ELEMENT tree are no longer the same shape here, and the children this walk "
              "must place are the child's children spliced into this list at its position. That splice is a "
              "BOX-TREE construction step and it belongs to every walk over children rather than to this one: "
              "core/layout/used_value.c's containing-block walk already steps OVER such an ancestor, and doing "
              "the same here by hand would be a second copy of one rule. BUILD css-display §3's box-tree "
              "flattening as the thing this walk iterates, so a `contents` element is invisible to every "
              "consumer at once");
    block = strcmp(d, "block") == 0 || strcmp(d, "flow-root") == 0 || strcmp(d, "list-item") == 0 ||
            strcmp(d, "flex") == 0 || strcmp(d, "grid") == 0;
    inline_level = strcmp(d, "inline") == 0 || strcmp(d, "inline-block") == 0 ||
                   strcmp(d, "inline-flex") == 0 || strcmp(d, "inline-grid") == 0 ||
                   strcmp(d, "inline-table") == 0;
    table = strcmp(d, "table") == 0 || strncmp(d, "table-", 6) == 0;
    free(d);
    if (block) return BF_CHILD_BLOCK;
    if (inline_level)
        DFAIL("this child's computed `display` makes it an INLINE-LEVEL box, so CSS 2 §9.4.2's inline "
              "formatting context places it on a LINE BOX and §9.2.1.1 wraps the run of them in an ANONYMOUS "
              "BLOCK BOX before this walk has anything block-level to place. Its height is then the line "
              "boxes', which needs the text measured with a real font. ONE OF THE TWO WAYS TO REACH THIS IS NOT "
              "AN AUTHOR'S DOING and should be checked first: core/css/css_style_declaration.c's UA_DEFAULT "
              "table carries §15.3.1's `display: none` rule entire and about two dozen `display: block` rows, "
              "and html.css has many more — `blockquote, pre, dl, dt, dd, hr, address, fieldset, figcaption, "
              "details, summary, dialog, legend, center, dir, menu, xmp, plaintext, listing` are block-level in "
              "every user agent and reach this line as `inline`, which is that table's own default for an "
              "element it does not name. ADD the rows, and BUILD §9.4.2 for the elements that are genuinely "
              "inline");
    if (table)
        DFAIL("this child generates a TABLE box, whose height is CSS 2.1 §17.5.3's and not §10.6.3's: the "
              "table's height is distributed over its ROWS and each row's height comes from its cells' content, "
              "so a table in this list has a height this walk cannot ask for. It also has a box STRUCTURE this "
              "engine does not build — §17.2's anonymous table-object generation inserts the missing row "
              "groups, rows and cells around whatever the author wrote — and a `table-row` or `table-cell` "
              "reaching this list directly is that generation not having run. BUILD §17.2, then §17.5.2 and "
              "§17.5.3's algorithms");
    DFAIL("this child's computed `display` is one CSS 2 §9.2's box types do not cover, so this walk cannot "
          "classify it: it is neither block-level, nor inline-level, nor a table box, nor `none`, nor "
          "`contents`. css-display §2's `<display-outside> <display-inside>` grammar admits pairs this engine "
          "has never had to answer for (`ruby`, `math`, a two-value `inline flow-root`), and each is a box type "
          "with its own formatting context. BUILD the classification in css_computed_value.c's computed_display, "
          "whose css-display §2.7 blockification already normalises the values this walk does understand");
    return BF_CHILD_NO_BOX;
}

static BfChildKind bf_child_kind(lxb_dom_element_t *parent, lxb_dom_node_t *n)
{
    switch (n->type) {
    case LXB_DOM_NODE_TYPE_ELEMENT:
        return bf_element_child(lxb_dom_interface_element(n));
    case LXB_DOM_NODE_TYPE_TEXT:
        return block_flow_text_child_generates_box(parent, n) ? BF_CHILD_BLOCK : BF_CHILD_NO_BOX;
    case LXB_DOM_NODE_TYPE_COMMENT:
    case LXB_DOM_NODE_TYPE_PROCESSING_INSTRUCTION:
    case LXB_DOM_NODE_TYPE_DOCUMENT_TYPE:
        /* CSS 2 §9.2 generates boxes for elements and for text; a comment, a processing instruction and a
           doctype are neither, and every user agent lays out none of the three. */
        return BF_CHILD_NO_BOX;
    default:
        DFAIL("a node type CSS 2 §9.2's box generation does not describe is a CHILD of a block container being "
              "laid out — the tree this walk iterates holds elements, text, comments, processing instructions "
              "and a doctype, and a CDATA section, a document or a fragment is not a child any parser this "
              "engine runs produces there. Find the writer that inserted it");
    }
    return BF_CHILD_NO_BOX;
}

/* ---- the walk ---------------------------------------------------------------------------------------------- */

/* ONE BOX'S CONTRIBUTION TO ITS PARENT'S FORMATTING CONTEXT, which is more than a height: §8.3.1's runs at the
   two edges may reach THROUGH the box, so a parent that saw only a number would place the next sibling against
   a margin that had already collapsed. */
typedef struct {
    CssPx content_h;         /* the walk's own answer: §10.6.3's distance from the top content edge */
    CssPx border_h;          /* the box's BORDER-box height — meaningless when it collapses through */
    BfRun top;               /* the run adjoining its top border edge, its own margin-top included */
    BfRun bottom;
    bool  collapse_through;  /* §8.3.1: the two runs are ONE run and the box places nothing */
} BfBox;

static BfBox bf_box(lxb_dom_element_t *el);

/* §9.4.1's placement rule over §8.3.1's runs, for the in-flow children of one block container. It answers this
   box's own contribution, and on the way it hands the caller the offset of `want`'s top border edge from this
   box's top CONTENT edge — the same running position read out at the child that asked for it, which is why
   there is one walk and not two. */
static BfBox bf_layout(lxb_dom_element_t *el, lxb_dom_element_t *want, CssPx *want_top, bool *found)
{
    lxb_dom_node_t *n = lxb_dom_interface_node(el), *c;
    /* §8.3.1's THIRD and FOURTH adjoining pairs are different conditions and were one flag here, which got
       `height: 0` wrong in both directions. The THIRD — "bottom margin of a last in-flow child and bottom
       margin of its parent IF THE PARENT HAS AUTO COMPUTED HEIGHT" — is what lets a placed child's bottom run
       leave. The FOURTH — "top and bottom margins of a box … that has zero computed min-height, ZERO OR AUTO
       computed height, and no in-flow children" — is what makes the box itself collapse through, and a
       `height: 0` box does that while its bottom margin does not escape a child's.
       "AUTO COMPUTED HEIGHT" IS ASKED AS css-sizing-3 §3.2.1's BEHAVES AS AUTO, which is the current spec's
       own reading of exactly this prose: §3.2.1 exists "to have a common term for both when width/height
       computes to auto and when it is defined to behave as if auto were specified (as in the case of block
       percentage heights resolving against an indefinite size, see CSS2§10.5)", its note says legacy CSS2
       conditions phrased over a computed `auto` "should be interpreted as meaning behaves as auto", and its
       own test list is `margin-collapse-with-indefinite-block-size-001` through `-005` — these two pairs. The
       ZERO half stays literal: §3.2.1 speaks only about `auto`, so a `height: 0%` that RESOLVES is a resolved
       zero and not a case the section widens. */
    bool closed = bf_no_collapse_through_edges(el);
    bool open_top = !closed && bf_edge_is_open(el, true);
    bool open_bottom = !closed && bf_edge_is_open(el, false);
    bool min0 = bf_min_height_is_zero(el);
    bool esc_top = open_top;
    bool auto_h = used_value_height_behaves_as_auto(el);
    bool esc_bottom = open_bottom && min0 && auto_h;
    bool through_ok = open_top && open_bottom && min0 && (auto_h || bf_length_is_zero(el, "height"));
    CssPx pos = css_px(0.0);
    BfRun run = bf_run_empty();
    BfRun esc_run = bf_run_empty();
    bool escaping = esc_top, placed = false;
    BfBox out;

    for (c = n->first_child; c != NULL; c = c->next) {
        BfBox b;

        if (bf_child_kind(el, c) == BF_CHILD_NO_BOX) continue;
        b = bf_box(lxb_dom_interface_element(c));
        /* §8.3.1's second adjoining pair — "bottom margin of box and top margin of its next in-flow following
           sibling" — and its first, "top margin of a box and top margin of its first in-flow child": both are
           the run already open meeting this child's top run, and they are the same merge. */
        run = bf_run_merge(run, b.top);
        if (b.collapse_through) {
            /* §8.3.1's collapse-through note gives the box a TOP BORDER EDGE even though it places nothing, and
               it gives it in two cases: "if the element's margins are collapsed with its parent's top margin,
               the top border edge of the box is defined to be the same as the parent's" — which with an open
               top edge is this box's content edge, so the offset is the running position unchanged — and
               "otherwise … the position of the element's top border edge is the same as it would have been if
               the element had a non-zero bottom border", which is the run so far applied, this box's own top
               margin included and its bottom margin not. The note's next sentence says why the coordinate
               exists at all: "the top border edge position is only required for laying out DESCENDANTS of these
               elements" — and CSSOM VIEW §7's `offsetTop` on an empty `div` is a page asking for exactly
               that. */
            if (lxb_dom_interface_element(c) == want) {
                *want_top = escaping ? pos : css_px_add(pos, bf_run_value(run));
                *found = true;
            }
            run = bf_run_merge(run, b.bottom);
            continue;
        }
        if (escaping) {
            /* The run reached this box's top edge with nothing separating it, so it collapses with this box's
               OWN top margin and leaves. §8.3.1's collapse-through note states the consequence directly: "the
               top border edge of the box is defined to be the same as the parent's" — inside this box the child
               therefore begins at the top content edge and the margin contributes nothing. */
            esc_run = run;
            escaping = false;
        } else {
            pos = css_px_add(pos, bf_run_value(run));
        }
        if (lxb_dom_interface_element(c) == want) {
            *want_top = pos;
            *found = true;
        }
        pos = css_px_add(pos, b.border_h);
        run = b.bottom;
        placed = true;
    }

    out.top = bf_run_of(used_value_px(el, "margin-top"));
    out.bottom = bf_run_of(used_value_px(el, "margin-bottom"));
    out.border_h = css_px(0.0);
    out.collapse_through = false;
    if (placed) {
        if (esc_top) out.top = bf_run_merge(out.top, esc_run);
        if (esc_bottom) {
            /* §10.6.3's THIRD bullet: the height ends at "the bottom border edge of the last in-flow child
               whose top margin doesn't collapse with the element's bottom margin", because that child's bottom
               margin left through this edge. */
            out.bottom = bf_run_merge(out.bottom, run);
        } else {
            /* §10.6.3's SECOND bullet, and §10.6.7's rule for a formatting-context root: "the bottom edge of
               the bottom (possibly collapsed) margin of its last in-flow child". */
            pos = css_px_add(pos, bf_run_value(run));
        }
        /* §10.6.3's distance can be NEGATIVE — a last in-flow child with a negative bottom margin whose run
           does not escape is exactly that — and a box's CONTENT HEIGHT cannot be, which is css-sizing-3 §3.3
           "Box Edges for Sizing: the box-sizing property"'s own sentence ("as the content width and height
           cannot be negative, this computation is floored at zero"). So this floor belongs to the walk and
           holds with no limit declared at all. CSS 2.1 §10.7's CLAMP is a SEPARATE pass over the extent this
           walk returns and is core/layout/used_value.c's — it re-runs the rules with `min-height`/`max-height`
           substituted, and it runs on this value at every entry that consumes it. Calling this floor "§10.7's
           min-height running early" was true of the number and wrong about which section owns it, and it made
           the real clamp look already done. */
        out.content_h = css_px_max(pos, css_px(0.0));
        return out;
    }
    /* Nothing was placed: either there were no in-flow children at all — §10.6.3's FOURTH bullet, "zero,
       otherwise" — or every one of them collapsed through, in which case `run` holds their whole merged run and
       §8.3.1's own note applies: "a box's own margins collapse if … all of its in-flow children's margins (if
       any) collapse". */
    if (through_ok) {
        out.collapse_through = true;
        out.top = bf_run_merge(bf_run_merge(out.top, out.bottom), run);
        out.bottom = out.top;
        out.content_h = css_px(0.0);
        return out;
    }
    if (esc_top)         out.top = bf_run_merge(out.top, run);
    else if (esc_bottom) out.bottom = bf_run_merge(out.bottom, run);
    else                 pos = bf_run_value(run);
    out.content_h = css_px_max(pos, css_px(0.0));
    return out;
}

/* DOES THIS BOX'S OWN CONTENT DECIDE ANYTHING ABOUT IT — the question that keeps a walk from looking inside a
   box that has already been sized, and it is not an optimisation. CSS 2.1 §10.6.3's content-based rule is
   stated for `height: auto` and for nothing else, and §8.3.1's collapse-through needs "a height of either 0 or
   auto", so those are exactly the two values for which what is inside the box changes what the box contributes
   to its parent's formatting context. For any other declared height the contribution is the used value
   core/layout/used_value.h already derives and two margins — which is why `<div style="height:100px">text</div>`
   stacks and measures with no font in sight, and why its own inline content is never reached.
   A PERCENTAGE IS ON WHICHEVER SIDE ITS CONTAINING BLOCK PUTS IT, and that is not a property of the
   declaration. css-sizing-3 §3.2.1 "“Behaving as auto”" makes "block percentage heights resolving against an
   indefinite size" behave as auto, so `height: 100%` inside an `auto`-height parent takes §10.6.3's walk while
   the same declaration inside a `height: 600px` parent is a resolved length that never looks inside the box.
   core/layout/used_value.h answers which, over §10.1's chain, reading computed values only — a size-computing
   answer here would run a layout to decide whether to run a layout. */
static bool bf_height_needs_content(lxb_dom_element_t *el)
{
    /* §10.6.3's walk, and §8.3.1's collapse-through for a box whose margins may meet through it. */
    return used_value_height_behaves_as_auto(el) || bf_length_is_zero(el, "height");
}

/* THE RECURSION'S ONE STEP, AND THE PLACE THE TWO COMPONENTS MEET. A box's BORDER-box height is what its
   parent's walk stacks, and where it comes from depends on one thing:
     - a box whose `height` computed to a LENGTH has a used value core/layout/used_value.h already derives, and
       re-deriving it here would be §10.6.3 answering a question §10.6.2 owns. It is asked for, and every arm
       used_value.c cannot compute — an intrinsic size, an out-of-flow box's constraint equation — crashes
       there naming its own section;
     - a box whose `height` is `auto` has the walk's own content height, and §8.1's box model turns it into a
       border-box height by adding the same four terms used_value.h computes for every other edge. ASKING
       used_value.h for it instead would re-enter this walk through its `auto` arm, so every level of the tree
       would be walked twice over and the cost would double with each: the entry that takes the content extent
       exists precisely so the conversion is stated once without the cycle. That entry is ALSO where CSS 2.1
       §10.7's clamp runs for this box, over the extent handed to it — the substituted pass is a declared
       length, so it needs no walk and the cycle stays broken.
   A box that COLLAPSES THROUGH has no border box to stack, which is what the flag is for. */
static BfBox bf_box(lxb_dom_element_t *el)
{
    CssPx sink = css_px(0.0);
    bool sunk = false;
    char *d = bf_computed(el, "display");
    bool container = block_flow_display_is_block_container(d);
    BfBox b;

    free(d);
    b.content_h = css_px(0.0);
    b.top = bf_run_of(used_value_px(el, "margin-top"));
    b.bottom = bf_run_of(used_value_px(el, "margin-bottom"));
    b.collapse_through = false;
    /* Not a block container: a flex or grid CONTAINER, whose height is its own spec's and which establishes an
       independent formatting context, so its margins are its own and nothing inside it is this walk's. The
       height is asked for and crashes in the section that owns it. */
    if (!container || !bf_height_needs_content(el)) {
        b.border_h = used_value_border_edge_px(el, true);
        return b;
    }
    b = bf_layout(el, NULL, &sink, &sunk);
    DCHECK(!sunk, "the child walk reported placing a box it was not looking for");
    if (b.collapse_through) return b;
    /* A box whose height BEHAVES AS AUTO (css-sizing-3 §3.2.1) is the one whose border-box height is this
       walk's own content height; a resolved percentage is a declared length like any other and goes back
       through used_value.h, which is where §10.4/§10.7's clamp runs for it. */
    b.border_h = used_value_height_behaves_as_auto(el)
                     ? used_value_border_edge_from_content_px(el, b.content_h, true)
                     : used_value_border_edge_px(el, true);
    return b;
}

CssPx block_flow_auto_height(lxb_dom_element_t *el)
{
    CssPx unused = css_px(0.0);
    bool found = false;
    char *d;
    bool container, flex, grid;
    BfBox b;

    DCHECK(el != NULL, "a content-based height was asked for with no element");
    d = bf_computed(el, "display");
    container = block_flow_display_is_block_container(d);
    flex = strcmp(d, "flex") == 0 || strcmp(d, "inline-flex") == 0;
    grid = strcmp(d, "grid") == 0 || strcmp(d, "inline-grid") == 0;
    free(d);
    if (flex)
        DFAIL("css-flexbox §9.4 determines a FLEX CONTAINER's auto cross size and CSS 2.1 §10.6.3 does not: the "
              "container collects its items into FLEX LINES, each line's cross size is the largest hypothetical "
              "cross size of the items on it, and the container's content-box cross size is the sum of the "
              "lines'. None of that is a stack of block-level boxes with collapsing margins, so this walk would "
              "answer a number from the wrong algorithm. BUILD css-flexbox §9's layout algorithm, which needs "
              "the container's own used main size first — the same §10.3.3 subproblem core/layout/used_value.c "
              "already solves, one level up");
    if (grid)
        DFAIL("css-grid §11 sizes a GRID CONTAINER's ROWS and its auto height is the sum of the row track sizes "
              "plus the gutters, not CSS 2.1 §10.6.3's stack of block-level children: a grid item is placed in "
              "a TRACK, and two items in the same row do not stack at all. BUILD css-grid §11's track sizing "
              "algorithm");
    if (!container)
        DFAIL("CSS 2.1 §10.6.3's content-based height was asked for a box that is not a BLOCK CONTAINER — "
              "§9.2.1's box that 'either contains only block-level boxes or establishes an inline formatting "
              "context' — so there is no block formatting context inside it for this walk to run over. The "
              "caller is core/layout/used_value.c, which classifies the box type before asking, so the two "
              "lists have come apart");
    b = bf_layout(el, NULL, &unused, &found);
    DCHECK(!found, "the content-height walk reported placing the box it was not looking for");
    return b.content_h;
}

CssPx block_flow_child_top(lxb_dom_element_t *el)
{
    lxb_dom_element_t *cb;
    CssPx top = css_px(0.0);
    bool found = false;

    DCHECK(el != NULL, "a box's vertical placement was asked for with no element");
    cb = used_value_containing_block(el);
    DCHECK(cb != NULL,
           "CSS 2 §9.4.1's placement was asked for a box whose containing block is CSS 2.1 §10.1's FIRST case, "
           "the initial containing block — that is the ROOT ELEMENT, and core/layout/flow_position.c answers it "
           "from §10.1 directly rather than by walking a parent's children. The caller's own root test and this "
           "one have come apart");
    DCHECK(lxb_dom_interface_node(cb) == lxb_dom_interface_node(el)->parent,
           "§10.1's containing block for this box is NOT its parent element, so §9.4.1's walk over that block's "
           "own children can never reach it. The two ways to get here are the two that walk crashes on for their "
           "own reasons — a `display: contents` ancestor, whose children css-display §3 splices into the "
           "grandparent's box list, and an ancestor that generates no block container box at all — so the walk "
           "below would raise one of those messages a step late. Decide it HERE, where the discrepancy is");
    (void)bf_layout(cb, el, &top, &found);
    if (!found)
        DFAIL("CSS 2 §9.4.1's walk over this box's containing block placed every in-flow block-level child it "
              "found and NEVER REACHED THIS BOX, so there is no position to report. The walk skips exactly what "
              "§9.2 and §10.6.3 say generates no box or is out of flow — a `display: none` element, a comment, "
              "collapsing white space, an absolutely positioned child — and every one of those is a box the "
              "CALLER should have declined before asking: core/dom/element_view.h's has-a-box predicate covers "
              "the first and core/layout/flow_position.c's own §9.3 test covers the last. The two answers have "
              "come apart, and reporting a coordinate for a box that is not in this formatting context would be "
              "a number in the right units for a box that is not there");
    return top;
}
