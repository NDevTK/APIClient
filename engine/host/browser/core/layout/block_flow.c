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
#include "core/layout/box_subject.h"
#include "core/layout/line_box.h"
#include "core/layout/table_box.h"
#include "core/layout/table_wrapper.h"
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
   for exactly that reason. Deciding it on the modelled number is css_length.h's stated layering.
   A RUN IS EXACTLY THOSE TWO MAXIMA AND CARRIES NO "has any margin joined" FLAG, because §8.3.1 never asks:
   an empty run and a run of zero margins both reduce to zero and both merge identically, so the two are the
   same fact and a field distinguishing them is written everywhere and read nowhere. What DOES depend on
   whether anything was placed is the walk's own `placed`, which is about BOXES and not about margins. */
typedef struct {
    CssPx pos;    /* the largest positive margin in the run */
    CssPx neg;    /* the largest ABSOLUTE value among the negative margins */
} BfRun;

static BfRun bf_run_empty(void)
{
    BfRun r;

    r.pos = css_px(0.0);
    r.neg = css_px(0.0);
    return r;
}

static BfRun bf_run_add(BfRun r, CssPx m)
{
    if (m.px < 0.0) r.neg = css_px_max(r.neg, css_px_scale(m, -1.0));
    else            r.pos = css_px_max(r.pos, m);
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
          strcmp(d, "table-cell") == 0 || strcmp(d, "table-caption") == 0 ||
          /* CSS 2.1 §17.4 Tables in the visual formatting model states it of the wrapper in its own sentence —
             "The table wrapper box establishes a block formatting context" — and that box is what §9.4.1's
             stack holds for both spellings, so §8.3.1's run cannot reach through either. It is listed here
             rather than left to §9.4.1's own enumeration because §9.4.1 names "block containers … that are not
             block boxes" and a table wrapper is a block box that is not a block container, which is the one
             shape that list does not reach. */
          table_wrapper_generates(d);
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

/* CSS 2 §8.1 "Box dimensions"' TWO EDGES between a box's TOP BORDER EDGE and its TOP CONTENT EDGE. §9.4.1's
   stack reaches each child at its top BORDER edge — that is what `block_flow_child_top` reports and what this
   walk's own running position is — while §9.4.2 stacks the line boxes INSIDE a box from its top CONTENT edge,
   so a distance measured in one frame needs exactly this to be read in the other.
   NO FLOOR IS APPLIED AND NONE IS NEEDED: CSS 2 §8.4 "Padding properties" says outright that "unlike margin
   properties, values for padding values cannot be negative", and css-backgrounds-3 §3.3 "Line Thickness: the
   border-width properties" gives a border width a `<line-width>`, so neither operand can be. A floor here
   would be a rule the two properties already carry, restated where it could drift. */
static CssPx bf_content_top_from_border_edge(lxb_dom_element_t *el)
{
    CssLength b = css_computed_length(el, "border-top-width");

    DCHECK(b.kind == CSS_LENGTH_ABSOLUTE,
           "a `border-*-width` computed to something that is not an absolute length. css-backgrounds-3 §3.3's "
           "`Computed value:` line is `absolute length, snapped as a border width` and every arm of that "
           "derivation produces one, so a percentage or a keyword here is a rule that did not run");
    return css_px_add(b.px, used_value_px(el, "padding-top"));
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

/* §9.2's box generation, §9.3.1's and §9.5's flow membership and §9.2.1's level, for ONE child — see
   block_flow.h for the four values and for why a FLOAT is one of them rather than a refusal inside here. */
/* THE CHARACTERS §9.2.2.1's SENTENCE IS ABOUT — "white space content that WOULD SUBSEQUENTLY BE COLLAPSED AWAY
   according to the 'white-space' property" — which makes the set css-text-3 §4.1.1 "Phase I: Collapsing and
   Transformation"'s COLLAPSIBLE one and NOT HTML's ASCII whitespace. The two differ by exactly one character
   and the difference is a dropped glyph rather than a nicety:
     U+0020 SPACE and U+0009 TAB are §4.1.1's "collapsible spaces and tabs";
     U+000A LINE FEED is the SEGMENT BREAK — css-text-3 §4 "White Space Processing & Control Characters": "when
       an HTML document is represented as a DOM tree each line feed (U+000A) is treated as a segment break";
     U+000D CARRIAGE RETURN is NOT a segment break in the DOM ("unlike HTML, the DOM does not give any
       particular meaning to carriage returns") and §4 says what it is instead: "carriage returns (U+000D) are
       treated identically to spaces (U+0020) IN ALL RESPECTS";
     U+000C FORM FEED IS NOT WHITE SPACE HERE AT ALL, and it used to be in this list. §4's control-character
       rule covers it — "control characters (Unicode category Cc) OTHER THAN tabs (U+0009), line feeds
       (U+000A), carriage returns (U+000D) and sequences that form a segment break must be rendered as a
       VISIBLE GLYPH which the UA must synthesize if the glyphs found in the font are not visible" — so it is
       CONTENT, it generates an anonymous inline box, and [UAX14] additionally gives it line breaking class BK,
       which css-text-3 §5.5 "Line Breaking Details" makes a FORCED LINE BREAK. Counting it as collapsible
       removed the box silently: the run was classified as generating none, so neither the line box walk nor the
       intrinsic size walk was ever handed the character that would have crashed for it. */
bool block_flow_text_is_all_document_white_space(const lxb_dom_node_t *n)
{
    const lxb_dom_character_data_t *cd = (const lxb_dom_character_data_t *)n;
    const lxb_char_t *d;
    size_t len, i;

    DCHECK(n != NULL && n->type == LXB_DOM_NODE_TYPE_TEXT,
           "css-text-3 §4's character question was asked about something that is not a TEXT node — the answer "
           "is a scan of character data, and the cast below reads a node's data pointer through a struct only "
           "a CharacterData node has");
    d = cd->data.data;
    len = cd->data.length;
    if (d == NULL) return true;
    for (i = 0; i < len; i++) {
        char c = (char)d[i];

        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return false;
    }
    return true;
}

/* CSS 2.2 §9.2.2.1 "Anonymous inline boxes": "White space content that would subsequently be collapsed away
   according to the 'white-space' property does not generate any anonymous inline boxes." So a run of white
   space is a box or is nothing, and which one it is is the INHERITED `white-space` of the element the
   anonymous inline box would belong to — the parent, since an anonymous box inherits from the box it is
   inside. §16.6's table gives the answer per value: `normal` and `nowrap` collapse a sequence of white space,
   `pre`, `pre-wrap` and `pre-line` preserve it (css-text-3 adds `break-spaces`), and a preserved run is real
   inline content with a real line box.
   THIS PREDICATE ANSWERS §9.2.2.1 AND MEASURES NOTHING, which is the whole of its contract and was not always
   so: it used to CRASH for a run that generates a box, naming the glyph advance a line box needs. That put one
   component's missing input inside another component's classification, and the classification is right for
   both — the run either generates an anonymous inline box or it does not. The measurement crash now lives in
   core/layout/line_box.c, at the walk that would have to place the glyphs, which is where the advance is
   actually the missing operand and where the ONE line reporting it can name what to build. */
bool block_flow_text_child_generates_box(lxb_dom_element_t *parent, const lxb_dom_node_t *n)
{
    char *ws;
    bool collapses;

    DCHECK(parent != NULL && n != NULL && n->type == LXB_DOM_NODE_TYPE_TEXT,
           "§9.2.2.1's white-space question was asked about something that is not a TEXT node inside an "
           "element — the rule is about a run of character data and the property it reads is the containing "
           "element's inherited `white-space`");

    /* A run that is not entirely white space has content §9.2.2.1's sentence says nothing about collapsing
       away, so it generates a box whatever `white-space` says. */
    if (!block_flow_text_is_all_document_white_space(n)) return true;
    ws = bf_computed(parent, "white-space");
    collapses = strcmp(ws, "normal") == 0 || strcmp(ws, "nowrap") == 0;
    free(ws);
    /* THE COLLAPSED ARM IS §9.2.2.1's SENTENCE AND NOT A SHORTCUT PAST ONE, and it is exact only because of
       what §16.6 makes `normal` and `nowrap` do: a collapsible run between two BLOCK-LEVEL boxes is removed
       entirely, so there is no anonymous inline box and nothing to place. Inside an INLINE formatting context
       the same run may survive as a single space, and core/layout/line_box.c is where that difference is
       decided — it is a fact about the whole formatting context (css-text-3 §4.1.2 "Phase II: Trimming and
       Positioning" removes a collapsible sequence only at the beginning or the end of a LINE), which is not a
       question a per-node predicate can answer and must not pretend to. */
    return !collapses;
}

/* CSS 2.2 §9.7 "Relationships between 'display', 'position', and 'float'" IS THE ORDER THESE THREE QUESTIONS
   ARE ASKED IN, and it is the section's whole content: "if 'display' has the value 'none', then 'position' and
   'float' do not apply. In this case, the element generates no box"; "otherwise, if 'position' has the value
   'absolute' or 'fixed', the box is absolutely positioned, the computed value of 'float' is 'none'"; "otherwise,
   if 'float' has a value other than 'none', the box is floated". ASKING THEM IN ANOTHER ORDER ANSWERS A
   DIFFERENT DOCUMENT and this walk used to: it read `float` before `display`, so `display: none; float: left` —
   an ordinary way to turn a floated rule off — reported a FLOAT for an element §9.7 says generates no box at
   all, and the crash that stood at that test fired for a box no section of CSS 2 places anywhere. */
static BlockFlowChildKind bf_element_child(lxb_dom_element_t *el)
{
    char *d = bf_computed(el, "display");
    bool block, inline_level;
    TableBoxKind table;
    char nbuf[160];

    if (strcmp(d, "none") == 0) { free(d); return BLOCK_FLOW_CHILD_NO_BOX; }
    if (strcmp(d, "contents") == 0)
        DFAIL("css-display-3 §2.5 \"Box Generation: the none and contents keywords\" gives this child "
              "`display: contents`: \"The element itself does not generate any boxes, but its children and "
              "pseudo-elements still generate boxes and text sequences as normal.\" So the box tree and the "
              "ELEMENT tree are no longer the same shape here, and the children this walk must place are the "
              "child's children spliced into this list at its position. That splice is a BOX-TREE construction "
              "step and it belongs to every walk over children rather than to this one: "
              "core/layout/used_value.c's containing-block walk already steps OVER such an ancestor, and doing "
              "the same here by hand would be a second copy of one rule. BUILD THE SPLICE §2.5 STATES — \"For "
              "the purposes of box generation and layout, the element must be treated as if it had been "
              "replaced in the element tree by its contents (including both its source-document children and "
              "its pseudo-elements, such as ::before and ::after pseudo-elements, which are generated "
              "before/after the element's children as normal).\" — as the thing this walk iterates, so a "
              "`contents` element is invisible to every consumer at once");
    /* §9.7's SECOND arm. §9.3.1 states what it means for every caller of this classification at once —
       "Absolutely positioned boxes are taken out of the normal flow. This means they have no impact on the
       layout of later siblings" — and §10.6.3 says the same thing for the one walk in this file: "Only children
       in the normal flow are taken into account (i.e., floating boxes and absolutely positioned boxes are
       ignored…)". That is the rule RUNNING and not a gap; the box's OWN position is §9.3.2's offsets over a
       static position, which is a different question this walk is what will one day answer. */
    if (bf_computed_is(el, "position", "absolute") || bf_computed_is(el, "position", "fixed")) {
        free(d);
        return BLOCK_FLOW_CHILD_NO_BOX;
    }
    /* §9.7's THIRD arm, ANSWERED AND NOT REFUSED — see block_flow.h. Every caller of this classification meets
       a float with a different missing capability, so the refusal belongs at each of their own lines: this
       file's stack names §9.5.2's clearance below, core/layout/line_box.c names §9.4.2's shortened line box,
       and core/layout/intrinsic_size.c names what a shortened line box does to a contribution. */
    if (!bf_computed_is(el, "float", "none")) {
        free(d);
        return BLOCK_FLOW_CHILD_FLOAT;
    }
    block = strcmp(d, "block") == 0 || strcmp(d, "flow-root") == 0 || strcmp(d, "list-item") == 0 ||
            strcmp(d, "flex") == 0 || strcmp(d, "grid") == 0;
    inline_level = strcmp(d, "inline") == 0 || strcmp(d, "inline-block") == 0 ||
                   strcmp(d, "inline-flex") == 0 || strcmp(d, "inline-grid") == 0 ||
                   strcmp(d, "inline-table") == 0;
    /* CSS 2.1 §17.2 The CSS table model's box types, classified ONCE through the component that owns the
       vocabulary §17.2.1 Anonymous table objects' rules are written in (core/layout/table_box.h). A prefix
       test on "table-" answered this question for a long time and it answered it as ONE question; the two arms
       below are two DIFFERENT missing things, and a single predicate over both is what let one crash report
       them as one. */
    table = table_box_kind(d);
    free(d);
    if (block) return BLOCK_FLOW_CHILD_BLOCK;
    /* CSS 2.2 §9.2.2 "Inline-level elements and inline boxes": this child is inline-level, so it is not on
       §9.4.1's stack at all — §9.4.2's line boxes hold it, and which of the two formatting contexts this block
       container establishes is decided over the WHOLE child list rather than here (bf_content_kind). */
    if (inline_level) return BLOCK_FLOW_CHILD_INLINE;
    /* CSS 2.1 §17.4 Tables in the visual formatting model ANSWERS THIS CLASSIFICATION, and it used to abort
       here. The box §9.4.1's stack places for a `display: table` element is not the table box at all: "the
       table generates a principal block box called the table wrapper box that contains the table box itself
       and any caption boxes (in document order)", and "The table wrapper box is a 'block' box if the table is
       block-level" — so it is an ordinary block-level box on this stack, exactly like a `<div>`.
       AN `inline-table` LEFT THROUGH `inline_level` ABOVE AND THAT IS THE SAME SENTENCE'S OTHER HALF — its
       wrapper is "an 'inline-block' box if the table is inline-level", which is inline-level, so §9.4.2's line
       boxes hold it and core/layout/line_box.c is where its own missing number is named. Neither spelling
       needs an arm here any more.
       WHAT THIS DOES NOT DECIDE is how TALL that wrapper is, which is `bf_box`'s question and crashes there —
       naming §17.5 with the wrapper's own structure in hand instead of naming a box type this walk could not
       classify. §9.2.1 is why one test could not answer both: "Except for table boxes, which are described in
       a later chapter, and replaced elements, a block-level box is also a block container box", so a table is
       block-LEVEL without being a block CONTAINER, and a walk that asked only the second stopped here. */
    if (table_box_kind_generates_table_box(table)) return BLOCK_FLOW_CHILD_BLOCK;
    if (table != TABLE_BOX_NOT_A_TABLE_BOX)
        DFAILF("%s: "
              "this child is a TABLE-INTERNAL box — a row, a cell, a row group, a column, a column group or a "
              "caption — sitting directly in a BLOCK CONTAINER's child list, which is not where CSS 2.1 §17.2 "
              "The CSS table model puts any of them. That is not a box type to classify here, it is §17.2.1 "
              "Anonymous table objects' THIRD stage not having run, and that stage changes THIS LIST rather "
              "than anything inside a table: \"For each proper table child C in a sequence of consecutive "
              "proper table children, if C is misparented then generate an anonymous 'table' or 'inline-table' "
              "box T around C and all consecutive siblings of C that are proper table children. (If C's parent "
              "is an 'inline' box, then T must be an 'inline-table' box; otherwise it must be a 'table' box.)\" "
              "— with \"A 'table-row' is misparented if its parent is neither a row group box nor a 'table' or "
              "'inline-table' box\", and the same sentence for a column, a row group, a column group and a "
              "caption. A CELL reaches its row through that stage's FIRST rule: \"For each 'table-cell' box C "
              "in a sequence of consecutive internal table and 'table-caption' siblings, if C's parent is not "
              "a 'table-row' then generate an anonymous 'table-row' box around C and all consecutive siblings "
              "of C that are 'table-cell' boxes.\" "
              "SO THE THING TO BUILD IS THE CHILD LIST, NOT A CASE IN THIS SWITCH. Once the third stage runs, "
              "the anonymous table box it generates is what this walk sees here — one block-level box — and "
              "the misparented children are inside it where core/layout/table_box.h's first two stages already "
              "classify them. It is the same list css-display-3 §2.5 Box Generation: the none and contents "
              "keywords' `contents` splice is spliced into, and the arm above says so for that value too, so "
              "both belong at the top of this walk and neither belongs at a call site. `table_box.h` "
              "deliberately does not hold this stage: its subject is a list that is NOT a table's, and a table "
              "component generating the table it is asked about would be that rule answering itself",
              box_subject(el, nbuf, sizeof nbuf));
    DFAIL("this child's computed `display` is one CSS 2 §9.2's box types do not cover, so this walk cannot "
          "classify it: it is neither block-level, nor inline-level, nor a table box, nor `none`, nor "
          "`contents`. css-display §2's `<display-outside> <display-inside>` grammar admits pairs this engine "
          "has never had to answer for (`ruby`, `math`, a two-value `inline flow-root`), and each is a box type "
          "with its own formatting context. BUILD the classification in css_computed_value.c's computed_display, "
          "whose css-display §2.7 blockification already normalises the values this walk does understand");
    return BLOCK_FLOW_CHILD_NO_BOX;
}

BlockFlowChildKind block_flow_child_kind(lxb_dom_element_t *parent, lxb_dom_node_t *n)
{
    DCHECK(parent != NULL && n != NULL,
           "CSS 2 §9.2's box generation was asked about a child with no node, or with no block container for "
           "it to be a child OF — the parent is not decoration here, §9.2.2.1's white-space rule reads its "
           "inherited `white-space` to decide whether a run of character data is a box at all");
    DCHECK(n->parent == lxb_dom_interface_node(parent),
           "CSS 2 §9.2's box generation was asked about a node that is not a CHILD of the block container it "
           "was asked with. Every answer here is stated over `parent`'s child list — §9.2.2.1's collapsing, "
           "§9.2.1's level, §9.7's three-property order — so a node from elsewhere in the tree would be "
           "classified against a formatting context it is not in");
    switch (n->type) {
    case LXB_DOM_NODE_TYPE_ELEMENT:
        return bf_element_child(lxb_dom_interface_element(n));
    case LXB_DOM_NODE_TYPE_TEXT:
        /* §9.2.2.1's anonymous inline box is INLINE-level — the box that text generates is an inline box, not
           a block-level one, and calling it block-level here is what used to make a text run look like
           something §9.4.1's stack could place. */
        return block_flow_text_child_generates_box(parent, n) ? BLOCK_FLOW_CHILD_INLINE
                                                             : BLOCK_FLOW_CHILD_NO_BOX;
    case LXB_DOM_NODE_TYPE_COMMENT:
    case LXB_DOM_NODE_TYPE_PROCESSING_INSTRUCTION:
    case LXB_DOM_NODE_TYPE_DOCUMENT_TYPE:
        /* CSS 2 §9.2 generates boxes for elements and for text; a comment, a processing instruction and a
           doctype are neither, and every user agent lays out none of the three. */
        return BLOCK_FLOW_CHILD_NO_BOX;
    default:
        DFAIL("a node type CSS 2 §9.2's box generation does not describe is a CHILD of a block container being "
              "laid out — the tree this walk iterates holds elements, text, comments, processing instructions "
              "and a doctype, and a CDATA section, a document or a fragment is not a child any parser this "
              "engine runs produces there. Find the writer that inserted it");
    }
    return BLOCK_FLOW_CHILD_NO_BOX;
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
    /* THE BASELINE THE PASS WAS ASKED FOR, IN THE SAME FRAME AS `content_h` — the distance from this box's own
       TOP CONTENT EDGE — so the struct carries one origin and a reader cannot take one distance for the other.
       THEY ARE A MEASUREMENT ONLY WHERE THE WALK WAS ASKED FOR ONE (`bf_layout`'s `pass` argument), AND
       THE ONE READER IS THE WALK ITSELF, under that same flag. A `false` from a walk that was not asked would
       be indistinguishable from §9.4.2's own "this box has no line box", which is a real and different answer
       — so the flag gates the READ and not merely the write, and there is no path on which the two can be
       confused. `bf_layout` is asked for the baseline exactly when its caller was, all the way down.
       WHICH baseline it is, is the PASS's and not a second field, and that is a statement about §9.4.1's stack
       rather than about storage: the first line box on the stack and the last are in DIFFERENT boxes, so
       carrying both would mean walking every box for a distance one of the two callers throws away. One field
       under one pass keeps the frame single and keeps `has_line_box` meaning the same thing for both. */
    CssPx baseline;
    bool  has_line_box;
    /* CSS 2.1 §17.4 Tables in the visual formatting model's TABLE WRAPPER BOX, reported because ONE reader
       needs to tell it from every other box on the stack and reading `display` a second time to do so would be
       one property answered from two places. CSS 2.1 §17.5.3 Table height algorithms is that reader: a cell's
       baseline is "the baseline of the first in-flow line box in the cell, OR THE FIRST IN-FLOW TABLE-ROW IN
       THE CELL, whichever comes first", so a wrapper reached before any line box is the OTHER arm of that
       sentence and not a box to walk past. It is written on every path, like the two above. */
    bool  is_table_wrapper;
} BfBox;

/* WHICH BASELINE §9.4.1's STACK IS BEING REDUCED TO, which is a three-way question and never a flag: CSS 2.2
   §10.8.1 "Leading and half-leading" asks for the LAST line box in the normal flow, css-inline-3 §4.2.1
   "Alignment Baseline Source: the baseline-source longhand"'s `first` keyword and CSS 2.1 §17.5.3 "Table
   height algorithms"' cell baseline ("the baseline of the first in-flow line box in the cell") ask for the
   FIRST, and §10.6.3's height asks for neither and must not pay for one. */
typedef enum {
    BF_BASELINE_NONE = 0,
    BF_BASELINE_LAST,
    BF_BASELINE_FIRST
} BfBaseline;

static BfBox bf_box(lxb_dom_element_t *el, BfBaseline pass);

/* ---- WHICH FORMATTING CONTEXT THIS BLOCK CONTAINER ESTABLISHES ------------------------------------------
   CSS 2.2 §9.4.2 states the condition and §9.2.1 states the alternative in the same breath: a block container
   "either contains only block-level boxes or establishes an inline formatting context", and §9.4.2's own first
   sentence is that an inline formatting context "is established by a block container box THAT CONTAINS NO
   BLOCK-LEVEL BOXES". So the question is answered over the WHOLE child list, ONCE, before either algorithm
   runs — never per child, which is what a walk that classified as it stacked would be doing.
   A MIXED CONTAINER IS NOT A THIRD ANSWER TO §9.2.1's QUESTION, and that is the whole content of §9.2.1.1
   "Anonymous block boxes": "if a block container box (such as that generated for the DIV above) has a
   block-level box inside it (such as the P above), then we FORCE IT TO HAVE ONLY BLOCK-LEVEL BOXES INSIDE IT".
   So the presence of one block-level child settles the CONTAINER's formatting context — §9.4.1's stack, below —
   and what the inline-level children get is not a formatting context for the container but a box of their own
   inside it.
   IT IS STILL A FOURTH ANSWER TO THIS CLASSIFICATION'S QUESTION, because the forcing has to be RUN and only a
   mixed child list runs it. A walk that folded MIXED into BLOCK answered §9.2.1 correctly and then could not
   say whether §9.2.1.1 had generated anything — which is exactly what a caller enumerating those boxes needs
   to know before it walks a child list at all. */
typedef enum {
    BF_CONTENT_NONE = 0,   /* no in-flow children: §10.6.3's fourth bullet, and §9.4.2 creates no line box */
    BF_CONTENT_BLOCK,      /* ONLY block-level boxes: §9.4.1's stack below, and §9.2.1.1 generates nothing */
    BF_CONTENT_INLINE,     /* NO block-level box: §9.4.2's context, established by this ELEMENT's own box */
    BF_CONTENT_MIXED       /* both: §9.4.1's stack below, over the box list §9.2.1.1's forcing produces */
} BfContent;

static BfContent bf_content_kind(lxb_dom_element_t *el)
{
    lxb_dom_node_t *n = lxb_dom_interface_node(el), *c;
    bool block = false, inl = false;

    for (c = n->first_child; c != NULL; c = c->next) {
        switch (block_flow_child_kind(el, c)) {
        case BLOCK_FLOW_CHILD_NO_BOX: break;
        /* §9.4.2's condition is "a block container box that CONTAINS NO BLOCK-LEVEL BOXES" and §9.2.1.1's
           forcing is triggered by one — and a float is neither, because it is not in this container's flow at
           all: §9.5's "since a float is not in the flow, non-positioned block boxes created before and after
           the float box flow vertically as if the float did not exist". §9.2.1.1's own splitting paragraph
           reads the same way from the other end, treating block-level siblings "that are consecutive or
           separated only by collapsible whitespace and/or out-of-flow elements" as one break. So a float
           decides NOTHING here, and this classification ANSWERS for a container that has one; what a float
           does to the answer is a fact about the WIDTH of line boxes and about §9.5.2's clearance, which is
           each walk's own question at its own line. */
        case BLOCK_FLOW_CHILD_FLOAT:  break;
        case BLOCK_FLOW_CHILD_BLOCK:  block = true; break;
        case BLOCK_FLOW_CHILD_INLINE: inl = true; break;
        }
    }
    if (block) return inl ? BF_CONTENT_MIXED : BF_CONTENT_BLOCK;
    if (inl) return BF_CONTENT_INLINE;
    return BF_CONTENT_NONE;
}

/* §9.4.2's OWN CONDITION, WHOLE — see the header for why it is exported rather than re-derived. Both conjuncts
   are the section's own sentence: the box must be a BLOCK CONTAINER, and it must CONTAIN NO BLOCK-LEVEL BOXES.
   The `display` half is asked FIRST and is not redundant with the classification below: `bf_content_kind` reads
   a CHILD LIST and a `display: inline` element with only text children has one that looks exactly like a block
   container's, so a caller that skipped it would be told that an inline box establishes the formatting context
   its own ancestor establishes — and would then place that context's line boxes against the inline box's
   content edge, which is not a rectangle §9.4.2 gives line boxes at all. */
bool block_flow_establishes_inline_context(lxb_dom_element_t *el)
{
    char *d;
    bool container;

    DCHECK(el != NULL, "CSS 2.2 §9.4.2's establishing condition was asked with no element");
    d = bf_computed(el, "display");
    container = block_flow_display_is_block_container(d);
    free(d);
    if (!container) return false;
    return bf_content_kind(el) == BF_CONTENT_INLINE;
}

/* ---- CSS 2.2 §9.2.1.1 "Anonymous block boxes" -------------------------------------------------------------
   THE BOX THIS WALK ITERATES IS NOT THE ELEMENT LIST. "In a document like this: <DIV> Some text <P>More text
   </DIV> … we assume that there is an ANONYMOUS BLOCK BOX around 'Some text'", and generally "if a block
   container box … has a block-level box inside it …, then we force it to have only block-level boxes inside
   it." So each maximal run of inline-level children is ONE box, a sibling of the block-level boxes on either
   side, and §9.4.1's stack below places it exactly like any other child — which is why this is a box-tree step
   and not a case inside the placement.
   WHERE THE WHITE SPACE GOES IS DECIDED BEFORE THIS FUNCTION AND BY §9.2.2.1 "Anonymous inline boxes": "white
   space content that would subsequently be collapsed away according to the 'white-space' property does not
   generate any anonymous inline boxes." The space in `<div><p>x</p>  <p>y</p></div>` is therefore not
   inline-level content at all — there is nothing between the two block boxes to wrap, and no anonymous block
   box is generated there. §9.2.1.1's own splitting paragraph leans on the same fact when it treats block-level
   siblings "separated only by collapsible whitespace and/or out-of-flow elements" as consecutive.
   `block_flow_child_kind` has already applied §9.2.2.1 (such a run is BLOCK_FLOW_CHILD_NO_BOX), so a run here
   is delimited by the first and the last child that generate an inline-level BOX, and a boxless child inside
   one is carried along with it because there is nothing of it to carry.
   THE BOX HAS NO STYLE OF ITS OWN, which is what makes it a `BfBox` with no element rather than a synthesised
   one: "the properties of anonymous boxes are inherited from the enclosing non-anonymous box …. Non-inherited
   properties have their initial value. For example, the font of the anonymous box is inherited from the DIV,
   but the margins will be 0." Every property §9.4.1's placement would read of this box is therefore either the
   PARENT's — the font and `line-height` its line boxes are measured with, which core/layout/line_box.h takes
   the parent element for — or a constant: zero margins, zero padding, no border, `min-height: auto`,
   `height: auto`, static position, `float: none`, `overflow: visible`. Those constants are what let the
   §8.3.1 answers below be written out rather than derived: an anonymous block box never establishes a new
   block formatting context and has nothing at either edge to separate a margin, so §8.3.1's fourth adjoining
   pair holds of it whenever "it does not contain a line box" does, and its own contribution to a margin run is
   a zero.
   §9.2.1.1's LAST PARAGRAPH NEEDS NO CODE AND IS RECORDED HERE SO THE NEXT READER DOES NOT GO LOOKING:
   "anonymous block boxes are ignored when resolving percentage values that would refer to it: the closest
   non-anonymous ancestor box is used instead." core/layout/used_value.c's containing-block walk steps over
   ELEMENTS, and an anonymous block box is not one, so the closest non-anonymous ancestor is the only box it
   can reach. The day that walk iterates boxes instead, the rule becomes a step in it. */

/* §9.2.1.1's BOXES, COLLECTED AS §9.4.1's STACK GENERATES THEM — see block_flow.h for the entry's contract and
   for why the position reported is the stack's own running offset rather than a second derivation of it. A
   NULL sink is the walk running for one of its other two answers, which is every caller but that entry. */
typedef struct {
    BlockFlowAnonBox *v;
    size_t n, cap;
} BfAnonSink;

static void bf_anon_record(BfAnonSink *s, lxb_dom_node_t *first, lxb_dom_node_t *end, CssPx top, CssPx height)
{
    if (s == NULL) return;
    DCHECK(first != NULL && first != end,
           "CSS 2.2 §9.2.1.1's anonymous block box was reported over an EMPTY run. The section generates one "
           "only to wrap inline-level content, and the walk sets this run at the child that starts it — so an "
           "empty one here is the placement having reported a box the generation above it did not make");
    if (s->n == s->cap) {
        size_t cap = s->cap != 0 ? s->cap * 2 : 4;
        BlockFlowAnonBox *v = realloc(s->v, cap * sizeof(*v));

        CHECK(v != NULL, "the anonymous block boxes of one block container could not be allocated");
        s->v = v;
        s->cap = cap;
    }
    s->v[s->n].first = first;
    s->v[s->n].end = end;
    /* §9.2.1.1's "the margins will be 0" with §10.3.3's constraint equation over an initial `width: auto` puts
       this box's content box's inline-start edge exactly on its container's — see block_flow.h. A literal zero
       and not a measured one, so it carries no environment fact for css_length.h to union. */
    s->v[s->n].content_x = css_px(0.0);
    s->v[s->n].content_y = top;
    s->v[s->n].height = height;
    s->n++;
}

/* §9.2.1.1's RUN, DELIMITED AND NOTHING ELSE — see block_flow.h for the contract and for why the delimitation
   is exported while `block_flow_anonymous_boxes` may not be reached from an intrinsic pass. */
lxb_dom_node_t *block_flow_anonymous_box_end(lxb_dom_element_t *el, lxb_dom_node_t *first)
{
    lxb_dom_node_t *c, *end;

    DCHECK(el != NULL && first != NULL, "CSS 2.2 §9.2.1.1's run was delimited with no container or no child");
    DCHECK(block_flow_child_kind(el, first) == BLOCK_FLOW_CHILD_INLINE,
           "CSS 2.2 §9.2.1.1's anonymous block box was started at a child that generates no inline-level box. "
           "The section generates one only to wrap inline-level content — \"we assume that there is an "
           "anonymous block box around 'Some text'\" — so this box would be EMPTY, and every caller would then "
           "hold a box the element tree never asked for: §9.4.1's stack would take a height from it and "
           "css-sizing-3 §5.2's maximum would take a width");
    end = first->next;
    for (c = first; c != NULL; c = c->next) {
        BlockFlowChildKind kind = block_flow_child_kind(el, c);

        /* The run ends at the block-level box §9.2.1.1 makes the anonymous box's SIBLING rather than its
           content. Everything after the last inline-level child and before it generates no box at all, or is
           out of flow, so it is left outside: the boundary is drawn where an in-flow box is, which is the only
           place it is observable. A FLOAT inside the run is carried along by the same reading — §9.2.1.1's own
           splitting paragraph steps over "collapsible whitespace and/or out-of-flow elements" — and it is the
           MEASUREMENT of the run, not this delimitation, that then has to answer for it. */
        if (kind == BLOCK_FLOW_CHILD_BLOCK) break;
        if (kind != BLOCK_FLOW_CHILD_INLINE) continue;
        end = c->next;
    }
    return end;
}

/* §9.4.1's OWN reading of that run: the same delimitation, plus the one thing only a PLACEMENT can ask — has
   the box whose position was requested turned out to be inside an anonymous box rather than on the stack. */
static lxb_dom_node_t *bf_anon_run_end(lxb_dom_element_t *el, lxb_dom_node_t *first, lxb_dom_element_t *want)
{
    lxb_dom_node_t *end = block_flow_anonymous_box_end(el, first), *c;

    if (want == NULL) return end;
    for (c = first; c != end; c = c->next)
        if (c->type == LXB_DOM_NODE_TYPE_ELEMENT && lxb_dom_interface_element(c) == want &&
            block_flow_child_kind(el, c) == BLOCK_FLOW_CHILD_INLINE)
            DFAIL("CSS 2 §9.4.1's vertical placement was asked for a box CSS 2.2 §9.2.1.1 puts INSIDE an "
                  "anonymous block box: it is an inline-level child of a block container that also holds a "
                  "block-level box, so its position is a position ALONG a line box in that anonymous box's "
                  "inline formatting context and not a distance down §9.4.1's stack. core/layout/"
                  "flow_position.c takes an inline-level box out through its own section before asking — the "
                  "same discrepancy the all-inline container asserts one branch up — so the two "
                  "classifications have come apart. It is decided HERE rather than left to the walk's "
                  "never-reached-this-box crash, because this box IS in the container's box tree and merely "
                  "not on its stack, which is a different fact and a different fix");
    return end;
}

/* ONE ANONYMOUS BLOCK BOX over the run `[first, end)` of `parent`'s children, as §9.4.1's stack sees it.
   §10.6.3's FIRST BULLET is its height — it establishes an inline formatting context, by construction the only
   thing it can contain — and §8.3.1's fourth adjoining pair is the rest of its contribution. */
static BfBox bf_anon_box(lxb_dom_element_t *parent, lxb_dom_node_t *first, lxb_dom_node_t *end,
                         BfBaseline pass)
{
    bool any_line_box = false;
    char *d = bf_computed(parent, "display");
    bool container = block_flow_display_is_block_container(d);
    BfBox out;
    CssPx first_baseline = css_px(0.0), last_baseline = css_px(0.0);
    CssPx h;

    free(d);
    DCHECK(container,
           "CSS 2.2 §9.2.1.1 generates an anonymous block box inside a BLOCK CONTAINER BOX — its whole "
           "sentence is \"if a BLOCK CONTAINER BOX … has a block-level box inside it\" — and this box's parent "
           "is not one. There is then no §9.4.1 stack for the anonymous box to be a sibling on, and no "
           "enclosing non-anonymous box for it to inherit the font and `line-height` its line boxes are "
           "measured with");
    DCHECK(first != NULL && first != end,
           "CSS 2.2 §9.2.1.1's anonymous block box was generated around NOTHING. The section generates one "
           "only to wrap inline-level content, so an empty one is a box with no reason to exist whose height "
           "§9.4.1's stack would nevertheless add to its container's");
    /* "Non-inherited properties have their initial value … the margins will be 0", so both of §8.3.1's runs at
       this box's edges are a zero margin. A zero is not the same as no margin at all: it still joins the run
       and it still collapses with its neighbours, and `bf_run_value` of a zero-margin run is zero either way. */
    out.top = bf_run_of(css_px(0.0));
    out.bottom = bf_run_of(css_px(0.0));
    out.collapse_through = false;
    /* §10.8.1's BASELINE COMES OUT OF THE SAME READING AND IS NEVER ASKED FOR SEPARATELY HERE, because this box
       has no `height` of its own to decide anything: §9.2.1.1 gives it the initial value, so the reduction runs
       for the height on every path and the baseline is a second distance down the lines it already walked.
       ITS FRAME NEEDS NO CONVERSION: "the margins will be 0" and the other non-inherited properties are at
       their initial values, so this box's border edge, padding edge and content edge are ONE rectangle and the
       distance line_box.h measures from the top CONTENT edge is the distance the stack outside measures from
       the top BORDER edge. That is the same identity `bf_anon_record` states for the position. */
    h = line_box_content_height(parent, first, end, &any_line_box, &first_baseline, &last_baseline);
    out.baseline = pass == BF_BASELINE_FIRST ? first_baseline : last_baseline;
    out.has_line_box = any_line_box;
    /* §9.2.1.1's anonymous block box wraps a run of INLINE-LEVEL children; a table wrapper is block-level
       (§17.4: "a 'block' box if the table is block-level"), so it is never inside one. */
    out.is_table_wrapper = false;
    if (!any_line_box) {
        /* §8.3.1's own note, every conjunct of which is a constant for this box except the last two: "a box's
           own margins collapse if the 'min-height' property is zero, and it has neither top or bottom borders
           nor top or bottom padding, and it has a 'height' of either 0 or 'auto', and IT DOES NOT CONTAIN A
           LINE BOX, and all of its in-flow children's margins (if any) collapse." §9.4.2 is what makes the
           fourth true here — a line box holding nothing but empty inline boxes "must be treated as NOT
           EXISTING for any other purpose" — and it is also what makes the fifth vacuous, since an inline box
           with a non-zero margin is one of the things that would have made the line box exist. */
        out.collapse_through = true;
        out.top = bf_run_merge(out.top, out.bottom);
        out.bottom = out.top;
        out.content_h = css_px(0.0);
        out.border_h = css_px(0.0);
        return out;
    }
    DCHECK(h.px >= 0.0,
           "CSS 2.2 §10.8's step 3 produced a NEGATIVE height for the line boxes inside an anonymous block "
           "box. It is \"the distance between the uppermost box top and the lowermost box bottom\", and every "
           "box on the line contributes its own `line-height`, which css-inline-3 §5.1 makes `<number [0,∞]>` "
           "/ `<length-percentage [0,∞]>` and says outright that \"negative values are illegal\"");
    out.content_h = h;
    /* §8.1's box model with all four surrounding terms at their initial value: no padding and no border, so
       the anonymous box's BORDER box — what the stack below advances by — is exactly its content box. */
    out.border_h = h;
    return out;
}

/* §9.4.1's placement rule over §8.3.1's runs, for the in-flow children of one block container. It answers this
   box's own contribution, and on the way it hands the caller the offset of `want`'s top border edge from this
   box's top CONTENT edge — the same running position read out at the child that asked for it, which is why
   there is one walk and not two.
   `baseline` ASKS FOR CSS 2.2 §10.8.1's THIRD READING OF THAT SAME POSITION, reported on `BfBox`. It is a
   REQUEST and not a mode: nothing else about the walk changes, and every arm below writes the pair whether it
   was asked or not — what the flag decides is whether a `has_line_box` of false is a measurement or merely the
   initialisation, which is why the flag gates the read at every level and travels down unchanged. It is not
   free, and that is the whole reason it is a flag: answering it means walking INTO a child whose own `height`
   already decided its size (`bf_height_needs_content`), so a height walk that always asked would look inside
   boxes §10.6.3 never needs to open, doubling the tree walked at every level and reaching sections that crash. */
static BfBox bf_layout(lxb_dom_element_t *el, lxb_dom_element_t *want, CssPx *want_top, bool *found,
                       BfAnonSink *anon, BfBaseline pass)
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
    char nbuf[160];

    out.top = bf_run_of(used_value_px(el, "margin-top"));
    out.bottom = bf_run_of(used_value_px(el, "margin-bottom"));
    out.border_h = css_px(0.0);
    out.collapse_through = false;
    /* §10.8.1's pair starts where §9.4.1's stack starts and where §9.4.2's line boxes start — the top content
       edge, with nothing met — so a box that places nothing reports NOT MET rather than a coordinate. Every
       arm below either overwrites both or leaves both, and no arm writes one without the other. */
    out.baseline = css_px(0.0);
    out.has_line_box = false;
    /* This is the box the WALK is reducing, never a child of it — the flag is a fact `bf_box` decides about a
       child from its own `display`, and `bf_layout`'s subject reaches it through that function. */
    out.is_table_wrapper = false;
    if (bf_content_kind(el) == BF_CONTENT_INLINE) {
        /* §9.4.2's INLINE FORMATTING CONTEXT. Nothing below this branch applies to it: §8.3.1's adjoining
           margins are stated over boxes that "both belong to in-flow BLOCK-LEVEL boxes participating in the
           same block formatting context", and there is no such box here — an inline box's vertical margins are
           not adjoining margins at all, so no run enters or leaves through this container's edges. */
        bool any_line_box = false;
        CssPx first_baseline = css_px(0.0), last_baseline = css_px(0.0);
        CssPx h;

        DCHECK(want == NULL,
               "CSS 2 §9.4.1's placement was asked for a box whose containing block establishes §9.4.2's "
               "INLINE formatting context, so the box on the line is inline-level and its position is a "
               "position ALONG a line box rather than a distance down a stack. core/layout/flow_position.c "
               "takes an inline-level box out through its own section before asking, so the two "
               "classifications have come apart — and answering a vertical offset here would be a coordinate "
               "in the right units measured against the wrong axis");
        /* This container holds no block-level box, so §9.2.1.1 generates nothing and the one inline formatting
           context here is over the WHOLE child list — the same run form the anonymous box is measured through,
           with the container's own element supplying the style it already owns. */
        /* §10.8.1's MAIN ARM FOR THE SHAPE THAT HAS AN ELEMENT TO NAME IT. This box establishes the one inline
           formatting context, so "its last line box in the normal flow" is the last EXISTING line box of this
           very reduction and the distance is already measured from this box's top content edge — the frame
           `BfBox` states. It is taken from the same call that answers the height, never a second one. */
        h = line_box_content_height(el, n->first_child, NULL, &any_line_box, &first_baseline, &last_baseline);
        /* WHICH of the two the caller asked for — this box establishes the ONE inline formatting context here,
           so both of §9.4.2's ends are inside it and the pass is the whole of the choice. */
        out.baseline = pass == BF_BASELINE_FIRST ? first_baseline : last_baseline;
        out.has_line_box = any_line_box;
        if (any_line_box) {
            DCHECK(h.px >= 0.0,
                   "CSS 2.2 §10.8's step 3 produced a NEGATIVE line box height. It is \"the distance between "
                   "the uppermost box top and the lowermost box bottom\", and every box on the line "
                   "contributes `A' + D'` = its own `line-height`, which css-inline-3 §5.1 makes "
                   "`<number [0,∞]>` / `<length-percentage [0,∞]>` and says outright that \"negative values "
                   "are illegal\" — so a negative maximum is a declaration that should have been dropped by "
                   "css-values-4 §5.1's range restriction reaching the arithmetic");
            out.content_h = h;
            return out;
        }
        /* §9.4.2: a line box holding nothing but empty inline boxes "must be treated as ZERO-HEIGHT … and must
           be treated as NOT EXISTING for any other purpose", and §8.3.1 asks exactly that in both places it
           mentions line boxes — its adjoining test excepts them by name ("note that certain zero-height line
           boxes (see 9.4.2) are ignored for this purpose") and its collapse-through note requires that the box
           "does not contain a line box". So this container behaves for §8.3.1 as one with no line box at all,
           and `<div><span></span></div>` collapses through exactly as `<div></div>` does. */
        if (through_ok) {
            out.collapse_through = true;
            out.top = bf_run_merge(out.top, out.bottom);
            out.bottom = out.top;
        }
        out.content_h = css_px(0.0);
        return out;
    }

    /* §9.4.1's stack is over the BOX list CSS 2.2 §9.2.1.1 forces this container to have, not over its child
       nodes: a block-level child is one box and a maximal run of inline-level children is one ANONYMOUS BLOCK
       BOX. `ce` is the element the box belongs to and is NULL for the anonymous one, which is the whole of the
       difference the placement below has to know about — everything else it reads is on `BfBox`. */
    c = n->first_child;
    while (c != NULL) {
        BlockFlowChildKind kind = block_flow_child_kind(el, c);
        lxb_dom_element_t *ce = NULL;
        /* The run this iteration's box holds, meaningful for the ANONYMOUS box alone — `ce == NULL` is what
           says which box this is, and it is the same test the placement below already makes. */
        lxb_dom_node_t *anon_first = NULL, *anon_end = NULL;
        BfBox b;

        if (kind == BLOCK_FLOW_CHILD_NO_BOX) { c = c->next; continue; }
        /* THE FLOAT'S REFUSAL IS HERE, AT THE WALK THAT WOULD PLACE IT, and it names §9.4.1's OWN missing
           capability rather than any other caller's — this is the line a reader of this crash can act on.
           CSS 2 §9.5 "Floats" positions this child, and §10.6.3's own parenthesis says a float is IGNORED when
           the container's height is computed, so the float alone would not stop this walk. What stops it is
           what a float does to its SIBLINGS: §9.5.2's `clear` on a later block-level box introduces CLEARANCE,
           §8.3.1 makes a margin with clearance NON-ADJOINING ("no line boxes, no clearance, no padding and no
           border separate them"), and §9.5.2 then shifts that box down past the float's bottom margin edge.
           One float therefore invalidates every collapse and every offset below it in this formatting context,
           and `clear` is not among the properties core/css/css_computed_value.c models, so there is nothing to
           read it through either. §10.6.7 wants the float as well, for a container that establishes a
           formatting context: "if the element has any floating descendants whose bottom margin edge is below
           the element's bottom content edge, then the height is increased to include those edges". */
        if (kind == BLOCK_FLOW_CHILD_FLOAT)
            DFAILF("CSS 2 §9.5 \"Floats\" takes this child off §9.4.1's stack and then changes where every box "
                   "BELOW it sits: §9.5.2 \"Controlling flow next to floats: the 'clear' property\"' clearance "
                   "makes a later block-level box's margin non-adjoining and shifts it past the float's bottom "
                   "margin edge, and §10.6.7's own rule pulls this container's height down to a floating "
                   "descendant's edge. So there is no arm on this stack that is right by default. BUILD "
                   "§9.5.1 \"Positioning the float: the 'float' property\"'s placement, and record `clear` — "
                   "CSS 2 §9.5.2 gives it `Computed value: as specified`, so it is a row of "
                   "css_computed_models' as-specified arm and a row of css_shorthand_complete_for. "
                   "core/layout/line_box.c and core/layout/intrinsic_size.c each name §9.5.1 as the same "
                   "absent capability under their own section's reason. %s",
                   box_subject_node(c, nbuf, sizeof nbuf));
        if (kind == BLOCK_FLOW_CHILD_INLINE) {
            lxb_dom_node_t *end = bf_anon_run_end(el, c, want);

            DCHECK(end != c,
                   "CSS 2.2 §9.2.1.1's run ended where it began, so this walk would generate the same "
                   "anonymous block box for ever. The run starts at a child that generates an inline-level "
                   "box and therefore always contains at least that one");
            b = bf_anon_box(el, c, end, pass);
            anon_first = c;
            anon_end = end;
            c = end;
        } else {
            ce = lxb_dom_interface_element(c);
            b = bf_box(ce, pass);
            c = c->next;
        }
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
            CssPx through_top = escaping ? pos : css_px_add(pos, bf_run_value(run));

            /* §8.3.1's collapse-through note requires that the box "does not contain a line box", so a box that
               reached this arm cannot be the one holding §10.8.1's last line box. That is one fact stated in
               two places rather than two facts, and asserting it here is what keeps them from parting: the
               note's conjunct is decided over the box's own children and `has_line_box` is decided over the
               same reduction, so a box answering both would be §9.4.2's "not existing for any other purpose"
               read one way for the collapse and the other way for the baseline. */
            DCHECK(pass == BF_BASELINE_NONE || !b.has_line_box,
                   "CSS 2.2 §8.3.1's collapse-through note lists \"it does not contain a line box\" among the "
                   "conjuncts that let a box's own two margins collapse, and this box COLLAPSED THROUGH while "
                   "reporting a line box for §10.8.1's baseline. One of the two readings is wrong about "
                   "§9.4.2's zero-height line box, and the margin it already collapsed is a position every "
                   "box below this one on the stack has been placed against");
            if (ce != NULL && ce == want) {
                *want_top = through_top;
                *found = true;
            }
            /* §9.2.1.1's box reaches here when every line box in it is one §9.4.2 says "must be treated as NOT
               EXISTING for any other purpose", and the note above is what gives it a top border edge anyway.
               It is still a BOX — zero-height, at that edge — so it is reported like every other one. */
            if (ce == NULL) bf_anon_record(anon, anon_first, anon_end, through_top, b.border_h);
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
        /* §9.2.1.1's anonymous block box is never the box asked for BY ELEMENT — it has no element, so no
           `want` can name it — and `bf_anon_run_end` has already crashed for a `want` that is INSIDE one. It is
           REPORTED here instead, at the same running position and out of the same `pos`, which is what keeps an
           anonymous box and its block-level siblings from disagreeing about where a margin collapsed. The box
           has no border and no padding (§9.2.1.1's initial values), so this top BORDER edge is also its top
           content edge and its top MARGIN edge. */
        if (ce != NULL && ce == want) {
            *want_top = pos;
            *found = true;
        }
        if (ce == NULL) bf_anon_record(anon, anon_first, anon_end, pos, b.border_h);
        /* CSS 2.2 §10.8.1's "LAST line box in the normal flow", TAKEN OFF THE SAME `pos` THE PLACEMENT ABOVE
           JUST USED. §9.4.1 lays the boxes out "one after the other, vertically, beginning at the top of a
           containing block", so the last of them to hold a line box holds THE last line box — which is why
           this is an overwrite per box that has one and not a search, and why nothing here decides anything
           about ORDER that the stack has not already decided. UNDER `BF_BASELINE_FIRST` THE SAME SENTENCE READ
           FROM THE OTHER END makes the FIRST such box the one that holds it, so the overwrite becomes a
           first-write and no second traversal is needed for it — `out.has_line_box` is the only state the two
           readings differ over.
           THE FRAME CONVERSION IS §8.1's AND IS THE ONLY ARITHMETIC: `pos` is this box's TOP BORDER EDGE and
           `b.baseline` is measured from its TOP CONTENT EDGE, so the border and the padding between them
           are added here. §9.2.1.1's anonymous block box has neither — "the margins will be 0" and every other
           non-inherited property is at its initial value — so its two edges are one and its term is a literal
           zero, the same identity `bf_anon_record` above relies on for the position. */
        /* CSS 2.1 §17.5.3 Table height algorithms' OTHER ARM, refused where it would otherwise be walked past
           in silence. The cell baseline that section defines is "the baseline of the first in-flow line box in
           the cell, or the first in-flow table-row in the cell, WHICHEVER COMES FIRST" — so a table wrapper
           standing on this stack ahead of every line box IS the answer, and it is not one this walk can give:
           a table's line boxes are inside its cells' own formatting contexts, so `bf_box` reports no line box
           for it (which is right for §10.8.1, whose "last line box in the normal flow" does not reach into a
           nested formatting context) and a first-baseline pass would sail past it to a LATER line box.
           BUILD it as §17.5.3's own recursion — the first ROW's baseline, "the maximum distance between the
           top of the cell box and the baseline over all cells that have 'vertical-align: baseline'" applied to
           that row, which core/layout/table_height.h computes the row heights of and which CSS 2.2 §10.8 asks
           for in the same words for an atomic inline ("The baseline of an 'inline-table' is the baseline of
           the first row of the table") — and report it on `BfBox` beside `baseline`, in this same frame.
           IT IS ONLY EVER ASKED ON THE FIRST-BASELINE PASS AND ONLY BEFORE A LINE BOX IS MET, which is what
           keeps an ordinary `<td>text<table>…</table></td>` out of it: that cell's first in-flow line box does
           come first, so §17.5.3's own "whichever comes first" is decided by the line box and the wrapper
           below it is never consulted. */
        if (pass == BF_BASELINE_FIRST && !out.has_line_box && b.is_table_wrapper)
            DFAILF("%s: CSS 2.1 §17.5.3 Table height algorithms' CELL BASELINE is \"the baseline of the first "
                   "in-flow line box in the cell, or the first in-flow table-row in the cell, whichever comes "
                   "first\", and CSS 2 §9.4.1's stack reached a TABLE WRAPPER BOX (printed above) before any "
                   "line box — so the SECOND arm is the one that applies here and this walk has only the "
                   "first. It must not skip the wrapper: a table's line boxes live in its cells' own "
                   "formatting contexts, so taking a line box further down the stack would answer with a "
                   "baseline from a box that is not the first in-flow thing in this cell at all, and the row "
                   "height built on it would be too SHORT with nothing to say so. BUILD §17.5.3's own "
                   "recursion — the first ROW's baseline, which is that section's \"maximum distance between "
                   "the top of the cell box and the baseline over all cells that have 'vertical-align: "
                   "baseline'\" over the rows core/layout/table_height.h already answers — and report it on "
                   "`BfBox` beside `baseline` in this same frame. CSS 2.2 §10.8 \"Line height calculations: "
                   "the 'line-height' and 'vertical-align' properties\" wants the identical number for an "
                   "atomic inline (\"The baseline of an 'inline-table' is the baseline of the first row of the "
                   "table\"), so ONE component answers both and neither reader derives it",
                   box_subject(ce, nbuf, sizeof nbuf));
        if (pass != BF_BASELINE_NONE && b.has_line_box &&
            !(pass == BF_BASELINE_FIRST && out.has_line_box)) {
            CssPx inner = ce != NULL ? bf_content_top_from_border_edge(ce) : css_px(0.0);

            out.baseline = css_px_add(css_px_add(pos, inner), b.baseline);
            out.has_line_box = true;
        }
        pos = css_px_add(pos, b.border_h);
        run = b.bottom;
        placed = true;
    }

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
   A box that COLLAPSES THROUGH has no border box to stack, which is what the flag is for.
   §10.8.1's BASELINE IS A THIRD ANSWER AND IT SPLITS THE FIRST BULLET, which is the whole reason `baseline` is
   an argument here rather than a property of the box. The size question above is settled for a declared height
   WITHOUT looking inside; where this box's own LINE BOXES ARE is a different question with a different answer,
   and a box with a declared `height` is exactly the one whose last line box can sit below its own content edge
   (§10.8.1 asks for that baseline, not for a clamped one). So when the baseline is asked for, the walk RUNS
   whatever `height` says — `block_flow_anonymous_boxes` runs it for the same reason and states it in the same
   words — and only the SIZE is still taken from used_value.h. */
static BfBox bf_box(lxb_dom_element_t *el, BfBaseline pass)
{
    CssPx sink = css_px(0.0);
    bool sunk = false;
    char *d = bf_computed(el, "display");
    bool container = block_flow_display_is_block_container(d);
    /* §17.4's wrapper is decided from the same string and BEFORE it is released, for the reason the two arms
       below are written apart: a table is block-LEVEL and is not a block CONTAINER (§9.2.1), so `container`
       cannot answer for it and the arm that handles a non-container is about flex and grid. */
    bool wrapper = table_wrapper_generates(d);
    char nbuf[160];
    BfBox b;

    free(d);
    b.content_h = css_px(0.0);
    b.top = bf_run_of(used_value_px(el, "margin-top"));
    b.bottom = bf_run_of(used_value_px(el, "margin-bottom"));
    b.collapse_through = false;
    b.baseline = css_px(0.0);
    b.has_line_box = false;
    b.is_table_wrapper = wrapper;
    /* CSS 2.1 §17.4 Tables in the visual formatting model's TABLE WRAPPER BOX, which is what §9.4.1's stack
       placed and which is NOT the table box. Its two margins are already read above and that is §17.4's rule
       running rather than an accident: "The computed values of properties 'position', 'float', 'margin-*',
       'top', 'right', 'bottom', and 'left' on the table element are used on the table wrapper box and not the
       table box" — core/layout/table_wrapper.h is that split, asked once. What the same sentence takes AWAY is
       every other non-inherited value: `border-*`, `padding-*` and `height` are used on the TABLE BOX, and the
       wrapper gets the initial value instead — "(Where the table element's values are not used on the table
       and table wrapper boxes, the initial values are used instead.)" So the wrapper has NO border and NO
       padding, and ITS BORDER BOX IS ITS CONTENT BOX.
       THAT CONTENT HEIGHT IS §10.6.3's OVER ITS OWN IN-FLOW CHILDREN, and §17.4 says what they are: the
       wrapper "contains the table box itself and any caption boxes (in document order)". WITH NO CAPTION THE
       WALK IS ONE BOX LONG AND HAS A CLOSED FORM, which is why this arm is arithmetic rather than a recursion:
       §10.6.3's distance runs "to the bottom edge of the bottom (possibly collapsed) margin of its last in-flow
       child", the only child is the table box, and §17.4 has just given that box the INITIAL `margin-*` — so
       there is no margin to collapse and the distance IS the table box's border-box height. The wrapper adds
       nothing to it on either side.
       SO THE NUMBER IS `used_value_border_edge_px` ON THE TABLE ELEMENT, AND THE CRASH THAT STOOD HERE SAID
       THAT WOULD BE "A MEASUREMENT OF THE WRONG BOX". That was right about the general case and wrong about
       this one, and the difference is worth stating because the two boxes are genuinely different: §17.4 puts
       `border-*`, `padding-*` and `height` on the TABLE BOX, so that call measures the TABLE BOX's border edge
       — which is exactly what this arm wants, because the wrapper's own edge coincides with it whenever the
       wrapper has no other child. It stops coinciding the moment a CAPTION exists, and that case crashes
       below rather than borrowing this identity.
       WHAT MAKES IT ANSWERABLE AT ALL IS §17.5.3, WHICH IS NOW BUILT: `used_value_border_edge_px` on a table
       box reaches CSS 2.1 §17.5.3 Table height algorithms through core/layout/table_height.h — the sum of the
       row heights plus the vertical cell spacing — over §17.5.2's used column widths. The crash here was
       waiting on that section and on nothing else in the no-caption case. */
    if (wrapper) {
        lxb_dom_element_t **captions = NULL;
        size_t ncaptions = table_box_captions(el, &captions);

        free(captions);
        /* IT IS A `CHECK` AND NOT A `DCHECK`, WHICH IS A DECISION AND NOT A DEFAULT — CLAUDE.md's own rule is
           that when unsure it is a DCHECK, so the reason has to be positive. A `DFAILF` here is compiled out
           in release and execution FALLS THROUGH to the arm below, which returns the TABLE BOX's border edge
           — and with a caption in the wrapper that is the wrapper's height MINUS every caption's margin box,
           which is a real number for a real box and is not this one. What makes that production-fatal rather
           than merely wrong is WHERE IT GOES. This height becomes `block_flow_child_top`'s running offset,
           which becomes core/layout/flow_position.c's coordinates, which become CSSOM VIEW's
           `getBoundingClientRect`, the scrolling area, and core/intersection_observer/intersection_observer.c's
           chain — and this engine's OUTPUT is findings derived from what it observed. A fabricated rectangle
           there is a plausible datum indistinguishable from a measurement, which is the defect this project
           names at the field level and refuses; it is the DATA INTEGRITY case CLAUDE.md's CHECK list carries,
           so it must not PROCEED in release either.
           THE COST IS STATED BECAUSE IT IS VISIBLE: engine/wpt.mjs reads the marker, so this site reports in
           the `fatal` column and not in `gap`, which that gate calls THE WORK QUEUE. It is still the work
           queue — the wrapper's own CHILD BOX LIST is what to build — and the column says only that shipping
           past it is not an option. THE ARM BELOW HAS THE SAME SHAPE FOR A FLEX OR GRID CONTAINER and is NOT promoted
           here: that is another lane's box type, its wrong number is the same element's own border edge rather
           than a different box's, and moving its aborts between columns without a measurement to attribute
           them to would be this diff spending someone else's signal. It is named, not changed. */
        if (ncaptions != 0)
            CHECK_FAILF("%s: CSS 2.1 §17.4 Tables in the visual formatting model's TABLE WRAPPER BOX has %zu "
                   "CAPTION BOX(ES) in it beside the table box — \"the table generates a principal block box "
                   "called the table wrapper box that contains the table box itself and any caption boxes (in "
                   "document order)\" — so §10.6.3's walk over its in-flow children has more than one child "
                   "and the table box's own border edge is no longer the whole of the wrapper's height. "
                   "THE CAPTION'S OWN NUMBERS ARE NO LONGER WHAT IS MISSING, AND THE CRASH THAT STOOD HERE "
                   "SAID THEY WERE: it named the caption's used WIDTH and the table wrapper box as \"an "
                   "anonymous box no element in this tree names\". core/layout/used_value.c answers both now — "
                   "§10.1's walk reports the wrapper as a box (§17.2.1 Anonymous table objects decides a "
                   "caption's box parent, and §17.4 gives the wrapper's width outright as \"the border-edge "
                   "width of the table box inside it, as described by section 17.5.2\"), so "
                   "`used_value_px(caption, \"width\")` is §10.3.3's constraint equation and "
                   "`used_value_px(caption, \"height\")` is §10.6.3's ordinary content-based height. "
                   "WHAT IS MISSING IS THE WRAPPER'S OWN CHILD BOX LIST, WHICH IS NOT AN ELEMENT'S CHILD "
                   "LIST — and that is why this walk cannot simply be run. §17.4's wrapper contains \"the "
                   "table box itself and any caption boxes\": the captions are children of the table ELEMENT, "
                   "the table box is that SAME element wearing its other box, and the rows are not in the "
                   "wrapper at all — so `bf_element_child` over `el`'s children reaches neither the right set "
                   "nor the right boxes, and `bf_layout` has no list to stack. BUILD THAT LIST as the thing "
                   "§9.4.1's stack iterates. It is the SAME box-tree step this file already crashes for twice "
                   "over — CSS 2.2 §9.2.1.1 Anonymous block boxes' runs, which `block_flow_anonymous_boxes` "
                   "delimits but `bf_layout` still walks as elements, and css-display-3 §2.5 Box Generation: "
                   "the none and contents keywords' `contents` splice, which `bf_element_child` names by "
                   "hand — so it is one construction with three rules and not three constructions. Once the "
                   "walk iterates boxes, this arm is §8.3.1's ordinary collapsing stack over the captions and "
                   "the table box, with `caption-side` (§17.4.1 Caption position and alignment) deciding only "
                   "their ORDER, which a SUM does not ask, and §17.4's own initial-value sentence guaranteeing "
                   "the table box brings no margin to collapse with theirs. DO NOT SUM THE HEIGHTS HERE "
                   "MEANWHILE: a second copy of §8.3.1's run algebra beside `bf_layout`'s is two "
                   "implementations of one section, free to disagree about a collapsed margin on exactly the "
                   "documents where the difference shows",
                   box_subject(el, nbuf, sizeof nbuf), ncaptions);
        /* §10.8.1's BASELINE IS NOT THIS ARM'S AND MUST NOT BE SKIPPED INTO. A wrapper reports NO line box —
           §10.8.1's "last line box in the normal flow" does not reach into the cells' own formatting contexts
           — and `is_table_wrapper` above is what lets the ONE reader that needs the other answer (CSS 2.1
           §17.5.3 Table height algorithms' cell baseline, whose second arm is "the first in-flow table-row in
           the cell") crash in `bf_layout` naming it, at the point where it knows whether a line box came
           first. Nothing is decided here, which is why there is no `pass` test in this arm. */
        b.border_h = used_value_border_edge_px(el, true);
        return b;
    }
    /* Not a block container: a flex or grid CONTAINER, whose height is its own spec's and which establishes an
       independent formatting context, so its margins are its own and nothing inside it is this walk's. The
       height is asked for and crashes in the section that owns it. */
    if (!container) {
        /* ITS BASELINE IS ITS OWN MODULE'S AND MUST NOT BE GUESSED. This box is on §9.4.1's stack and can
           therefore be the LAST box holding a line box, so an answer of "no line box here" is not a skip — it
           would hand §10.8.1's sentence the baseline of some EARLIER box, or its own bottom margin edge, and
           both are real coordinates on a real line that nothing downstream can tell from a measured one. */
        if (pass != BF_BASELINE_NONE)
            DFAILF("CSS 2.2 §10.8.1 \"Leading and half-leading\" is walking §9.4.1's stack for the baseline of "
                   "an enclosing `inline-block` — \"the baseline of its last line box in the normal flow\" — and "
                   "reached a box on that stack that is NOT a block container (%s), so it holds no line box of "
                   "§9.4.2's and its baseline is defined by its own module: css-flexbox-1 §8.5 \"Flex Container "
                   "Baselines\" for a flex container, whose own words are that \"the baselines of a flex "
                   "container are determined as follows\" over its startmost and endmost FLEX LINES, and "
                   "css-grid-1 §10.6 \"Grid Container Baselines\" for a "
                   "grid container. Those two are the whole of what can arrive: `bf_element_child` above puts "
                   "exactly `block`, `flow-root`, `list-item`, `flex` and `grid` on this stack, the first three "
                   "are block containers, and a TABLE box crashes there instead — CSS 2.1 §17.4 Tables in the "
                   "visual formatting model's table wrapper box is what §9.4.1 would stack, and §17.5.2 Table "
                   "width algorithms: the 'table-layout' property is missing before its §17.5.3 Table height "
                   "algorithms baseline is. NEITHER SKIPPING NOR SUBSTITUTING "
                   "THE BOTTOM MARGIN EDGE IS AVAILABLE HERE: this box may be the LAST one on the stack that has "
                   "a baseline at all, so either would put a real coordinate on a real line that no reader can "
                   "distinguish from a measured one. BUILD the module's own baseline and report it on `BfBox` "
                   "beside `last_baseline`, in the same frame — the distance from this box's own top content "
                   "edge — and the placement above needs no arm added for it. IT IS REACHED LATER THAN IT "
                   "LOOKS, so a fixture aimed at it must clear one earlier gate: core/layout/line_box.c "
                   "collects the enclosing `inline-block` as an atomic run item with its MARGIN-BOX INLINE "
                   "SIZE, which for a `width: auto` box is CSS 2.2 §10.3.9 \"'Inline-block', non-replaced "
                   "elements in normal flow\"'s shrink-to-fit over css-sizing-3 §5.2 \"Intrinsic "
                   "Contributions\" — and core/layout/intrinsic_size.c crashes there for a BLOCK-LEVEL element "
                   "child, before any baseline is asked for. So this abort shows on an `inline-block` with a "
                   "DECLARED inline size whose §9.4.1 stack holds a flex or grid container; the same box with "
                   "`width: auto` stops one component earlier, naming §5.2 instead",
                   box_subject(el, nbuf, sizeof nbuf));
        b.border_h = used_value_border_edge_px(el, true);
        return b;
    }
    if (!bf_height_needs_content(el)) {
        if (pass == BF_BASELINE_NONE) {
            b.border_h = used_value_border_edge_px(el, true);
            return b;
        }
        /* The SIZE is settled and the LINE BOXES are not, so the walk runs for the baseline alone: everything
           else it answers is about a height this box does not take from its content, and taking any of it would
           be §10.6.3's rule running for a box §10.6.2 already sized. */
        b.border_h = used_value_border_edge_px(el, true);
        {
            BfBox inner = bf_layout(el, NULL, &sink, &sunk, NULL, pass);

            DCHECK(!sunk, "the child walk reported placing a box it was not looking for");
            /* §8.3.1's fourth adjoining pair needs "zero or auto computed height", which is exactly what
               `bf_height_needs_content` just answered NO to — so a box on this arm cannot collapse through, and
               the two answers being discarded here are discarded because the section makes them unreachable
               rather than because this arm has no use for them. */
            DCHECK(!inner.collapse_through,
                   "CSS 2.2 §8.3.1's collapse-through note requires a box with \"zero or auto computed "
                   "height\", and this box's `height` is neither — `bf_height_needs_content` answered false "
                   "for it one line above. The walk nevertheless reported it collapsing through, so the "
                   "note's conjunct and css-sizing-3 §3.2.1's behaves-as-auto have come apart, and the run "
                   "this box would have merged is one every sibling below it is placed against");
            b.baseline = inner.baseline;
            b.has_line_box = inner.has_line_box;
        }
        return b;
    }
    b = bf_layout(el, NULL, &sink, &sunk, NULL, pass);
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
    b = bf_layout(el, NULL, &unused, &found, NULL, BF_BASELINE_NONE);
    DCHECK(!found, "the content-height walk reported placing the box it was not looking for");
    return b.content_h;
}

bool block_flow_last_line_box_baseline(lxb_dom_element_t *el, CssPx *baseline)
{
    CssPx unused = css_px(0.0);
    bool found = false;
    char *d;
    bool container;
    BfBox b;

    DCHECK(el != NULL, "CSS 2.2 §10.8.1's `inline-block` baseline was asked for with no element");
    DCHECK(baseline != NULL,
           "CSS 2.2 §10.8.1's `inline-block` baseline was asked for with nowhere to put the distance. The "
           "return value is only the sentence's \"no in-flow line boxes\" disjunct, so a caller holding it "
           "alone would know that a baseline EXISTS and have no coordinate to align the line to");
    d = bf_computed(el, "display");
    container = block_flow_display_is_block_container(d);
    free(d);
    /* §9.2.1 states the alternative this walk chooses between — a block container "either contains only
       block-level boxes or establishes an inline formatting context" — over a BLOCK CONTAINER BOX, and
       §10.8.1's own sentence is about an `inline-block`, which §9.2.1 makes one. A box that is neither has no
       §9.4.2 line boxes of its own and no §9.4.1 stack this walk can run, so there is nothing here to measure
       rather than a measurement to decline. */
    DCHECK(container,
           "CSS 2.2 §10.8.1's `inline-block` baseline was asked of a box that is not a BLOCK CONTAINER. "
           "§9.2.1's \"either contains only block-level boxes or establishes an inline formatting context\" is "
           "stated over exactly that box, and core/layout/line_box.c has classified the box type — and taken "
           "out the REPLACED `inline-block`, which has no baseline at all — before asking, so the two lists "
           "have come apart");
    b = bf_layout(el, NULL, &unused, &found, NULL, BF_BASELINE_LAST);
    DCHECK(!found, "the baseline walk reported placing a box it was not looking for");
    /* `bf_layout` measures both of its distances from this box's TOP CONTENT EDGE, which is the frame this
       entry's contract states, so there is no conversion here and none is hidden in the caller either. */
    *baseline = b.baseline;
    return b.has_line_box;
}

bool block_flow_first_line_box_baseline(lxb_dom_element_t *el, CssPx *baseline)
{
    CssPx unused = css_px(0.0);
    bool found = false;
    char *d;
    bool container;
    BfBox b;

    DCHECK(el != NULL, "CSS 2.1 §17.5.3's FIRST-line-box baseline was asked for with no element");
    DCHECK(baseline != NULL,
           "CSS 2.1 §17.5.3 Table height algorithms' FIRST-line-box baseline was asked for with nowhere to "
           "put the distance. The return value is only the section's \"if there is no such line box\" "
           "disjunct, so a caller holding it alone would know a baseline EXISTS and have no coordinate to "
           "align the row to");
    d = bf_computed(el, "display");
    container = block_flow_display_is_block_container(d);
    free(d);
    /* THE SAME §9.2.1 ALTERNATIVE THE LAST-BASELINE ENTRY ASSERTS, over the same box and for the same reason —
       a box that is neither of §9.2.1's two shapes has no §9.4.2 line boxes of its own and no §9.4.1 stack to
       walk. CSS 2.1 §9.2.1 Block-level elements and block boxes is also what makes this entry's own caller
       legitimate: "non-replaced inline blocks and NON-REPLACED TABLE CELLS are block containers but not
       block-level boxes", so a cell is exactly the box §17.5.3 measures a baseline inside. */
    DCHECK(container,
           "CSS 2.1 §17.5.3 Table height algorithms' cell baseline was asked of a box that is not a BLOCK "
           "CONTAINER. §9.2.1's \"either contains only block-level boxes or establishes an inline formatting "
           "context\" is stated over exactly that box, and §9.2.1 puts a non-replaced table cell inside it by "
           "name — so a box here that is not one is core/layout/table_box.h's cell classification and this "
           "list having come apart, and a REPLACED cell (an `<img style=\"display:table-cell\">`) reaching "
           "this walk is CSS 2.1 §17.2 The CSS table model's \"replaced elements with these 'display' values "
           "are treated as their given display types during layout\" needing an arm §17.5.3 does not write");
    b = bf_layout(el, NULL, &unused, &found, NULL, BF_BASELINE_FIRST);
    DCHECK(!found, "the first-baseline walk reported placing a box it was not looking for");
    /* Same frame as the entry above and as `block_flow_auto_height`: the distance is from this box's own TOP
       CONTENT EDGE, so a caller measuring from a BORDER edge adds its own border and padding and this file
       adds neither. */
    *baseline = b.baseline;
    return b.has_line_box;
}

CssPx block_flow_child_top(lxb_dom_element_t *el)
{
    lxb_dom_element_t *cb;
    CssPx top = css_px(0.0);
    bool found = false;
    char nbuf[160], cbuf[160], pbuf[160];

    DCHECK(el != NULL, "a box's vertical placement was asked for with no element");
    cb = used_value_containing_block(el);
    DCHECK(cb != NULL,
           "CSS 2 §9.4.1's placement was asked for a box whose containing block is CSS 2.1 §10.1's FIRST case, "
           "the initial containing block — that is the ROOT ELEMENT, and core/layout/flow_position.c answers it "
           "from §10.1 directly rather than by walking a parent's children. The caller's own root test and this "
           "one have come apart");
    DCHECKF(lxb_dom_interface_node(cb) == lxb_dom_interface_node(el)->parent,
           "%s, whose containing block is %s and whose parent is %s: "
           "§10.1's containing block for this box is NOT its parent element, so §9.4.1's walk over that block's "
           "own children can never reach it. WHAT REACHES HERE IS AN ANCESTOR THE CONTAINING-BLOCK WALK STEPS "
           "OVER, and there are two of them, each naming a BOX-TREE construction step this walk does not "
           "perform. A `display: contents` ancestor: css-display-3 §2.5 Box Generation: the none and contents "
           "keywords splices its children into the grandparent's box list — \"the element must be treated as if "
           "it had been replaced in the element tree by its contents\" — so those children are boxes in this "
           "block's list that this walk over ELEMENT children never visits. An `inline` ancestor holding this "
           "in-flow BLOCK-LEVEL box: CSS 2 §9.2.1.1 Anonymous block boxes breaks the inline around it, and "
           "\"the block-level box becomes a sibling of those anonymous boxes\" — a sibling in this same block "
           "container's box list, again reached by no walk over element children. BUILD THE BOX LIST §9.2.1.1 "
           "AND §2.5 DESCRIBE as the thing this walk iterates; `bf_element_child`'s own `contents` crash names "
           "the first half and this is where both halves are noticed. Deciding it HERE and not below is why "
           "the message can name which of the two it is: the walk below would report only that it never "
           "reached the box",
           box_subject(el, nbuf, sizeof nbuf), box_subject(cb, cbuf, sizeof cbuf),
           box_subject_node(lxb_dom_interface_node(el)->parent, pbuf, sizeof pbuf));
    (void)bf_layout(cb, el, &top, &found, NULL, BF_BASELINE_NONE);
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

size_t block_flow_anonymous_boxes(lxb_dom_element_t *el, BlockFlowAnonBox **out)
{
    BfAnonSink sink;
    CssPx unused = css_px(0.0);
    bool found = false;
    char *d;
    bool container;

    DCHECK(el != NULL, "CSS 2.2 §9.2.1.1's anonymous block boxes were asked for with no element");
    DCHECK(out != NULL,
           "CSS 2.2 §9.2.1.1's anonymous block boxes were asked for with nowhere to put them. A count alone "
           "names no run and no position, so a caller holding one would know that a box exists and have no way "
           "to reach the formatting context inside it — which is the only thing this entry is asked for");
    sink.v = NULL;
    sink.n = 0;
    sink.cap = 0;
    *out = NULL;
    /* §9.2.1.1's sentence has TWO conjuncts — "if a BLOCK CONTAINER BOX … HAS A BLOCK-LEVEL BOX INSIDE IT" —
       and both are asked here rather than left to the walk, because a zero is that sentence ANSWERING. See
       block_flow.h for what each shape of zero means; every one of them is a positive statement about which
       box holds this element's inline-level content, never this component declining to look. */
    d = bf_computed(el, "display");
    container = block_flow_display_is_block_container(d);
    free(d);
    if (!container) return 0;
    if (bf_content_kind(el) != BF_CONTENT_MIXED) return 0;
    /* §9.4.1's STACK IS RUN, because that is where §9.2.1.1's boxes are generated and where their positions
       come from. It is run whatever this container's `height` says — `bf_height_needs_content` decides whether
       a box's own content decides ITS SIZE, which is a different question from where the boxes inside it are,
       and a container with a declared height is exactly the one whose text can overflow it. */
    (void)bf_layout(el, NULL, &unused, &found, &sink, BF_BASELINE_NONE);
    DCHECK(!found, "the anonymous-box walk reported placing a box it was not looking for");
    DCHECK(sink.n > 0,
           "CSS 2.2 §9.2.1.1's forcing generated NO anonymous block box inside a container whose child list "
           "holds BOTH a block-level box and inline-level content. Those are the section's own two conjuncts, "
           "the classification above and the walk decide them over the same child list with the same "
           "per-child predicate, and the walk wraps every maximal inline-level run it meets — so an empty "
           "answer is one classification having been asked twice and answered differently");
    *out = sink.v;
    return sink.n;
}
