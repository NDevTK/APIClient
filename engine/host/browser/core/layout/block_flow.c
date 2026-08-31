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
#include "core/layout/line_box.h"
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
    BF_CHILD_BLOCK,        /* an in-flow block-level box this walk places */
    BF_CHILD_INLINE        /* in-flow inline-level content: §9.4.2's, not this walk's */
} BfChildKind;

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
static bool bf_text_is_all_whitespace(const lxb_dom_node_t *n)
{
    const lxb_dom_character_data_t *cd = (const lxb_dom_character_data_t *)n;
    const lxb_char_t *d = cd->data.data;
    size_t len = cd->data.length, i;

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
    if (!bf_text_is_all_whitespace(n)) return true;
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
    /* CSS 2.2 §9.2.2 "Inline-level elements and inline boxes": this child is inline-level, so it is not on
       §9.4.1's stack at all — §9.4.2's line boxes hold it, and which of the two formatting contexts this block
       container establishes is decided over the WHOLE child list rather than here (bf_content_kind). */
    if (inline_level) return BF_CHILD_INLINE;
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
        /* §9.2.2.1's anonymous inline box is INLINE-level — the box that text generates is an inline box, not
           a block-level one, and calling it block-level here is what used to make a text run look like
           something §9.4.1's stack could place. */
        return block_flow_text_child_generates_box(parent, n) ? BF_CHILD_INLINE : BF_CHILD_NO_BOX;
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
        switch (bf_child_kind(el, c)) {
        case BF_CHILD_NO_BOX: break;
        case BF_CHILD_BLOCK:  block = true; break;
        case BF_CHILD_INLINE: inl = true; break;
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
   siblings "separated only by collapsible whitespace and/or out-of-flow elements" as consecutive. `bf_child_kind`
   has already applied §9.2.2.1 (such a run is BF_CHILD_NO_BOX), so a run here is delimited by the first and the
   last child that generate an inline-level BOX, and a boxless child inside one is carried along with it because
   there is nothing of it to carry.
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

/* One past the LAST child of the run starting at `first` that generates an inline-level box — the exclusive
   end of §9.2.1.1's anonymous block box. `first` must itself generate one. */
static lxb_dom_node_t *bf_anon_run_end(lxb_dom_element_t *el, lxb_dom_node_t *first, lxb_dom_element_t *want)
{
    lxb_dom_node_t *c, *end = first->next;
    BfChildKind first_kind = bf_child_kind(el, first);

    DCHECK(first_kind == BF_CHILD_INLINE,
           "CSS 2.2 §9.2.1.1's anonymous block box was started at a child that generates no inline-level box. "
           "The section generates one only to wrap inline-level content — \"we assume that there is an "
           "anonymous block box around 'Some text'\" — so this box would be EMPTY, and §9.4.1's stack below "
           "would place a box the element tree never asked for and take a height from it");
    for (c = first; c != NULL; c = c->next) {
        BfChildKind kind = bf_child_kind(el, c);

        /* The run ends at the block-level box §9.2.1.1 makes the anonymous box's SIBLING rather than its
           content. Everything after the last inline-level child and before it generates no box at all, so it
           is left outside: the boundary is drawn where a box is, which is the only place it is observable. */
        if (kind == BF_CHILD_BLOCK) break;
        if (kind != BF_CHILD_INLINE) continue;
        if (c->type == LXB_DOM_NODE_TYPE_ELEMENT && lxb_dom_interface_element(c) == want)
            DFAIL("CSS 2 §9.4.1's vertical placement was asked for a box CSS 2.2 §9.2.1.1 puts INSIDE an "
                  "anonymous block box: it is an inline-level child of a block container that also holds a "
                  "block-level box, so its position is a position ALONG a line box in that anonymous box's "
                  "inline formatting context and not a distance down §9.4.1's stack. core/layout/"
                  "flow_position.c takes an inline-level box out through its own section before asking — the "
                  "same discrepancy the all-inline container asserts one branch up — so the two "
                  "classifications have come apart. It is decided HERE rather than left to the walk's "
                  "never-reached-this-box crash, because this box IS in the container's box tree and merely "
                  "not on its stack, which is a different fact and a different fix");
        end = c->next;
    }
    return end;
}

/* ONE ANONYMOUS BLOCK BOX over the run `[first, end)` of `parent`'s children, as §9.4.1's stack sees it.
   §10.6.3's FIRST BULLET is its height — it establishes an inline formatting context, by construction the only
   thing it can contain — and §8.3.1's fourth adjoining pair is the rest of its contribution. */
static BfBox bf_anon_box(lxb_dom_element_t *parent, lxb_dom_node_t *first, lxb_dom_node_t *end)
{
    bool any_line_box = false;
    char *d = bf_computed(parent, "display");
    bool container = block_flow_display_is_block_container(d);
    BfBox out;
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
    h = line_box_content_height(parent, first, end, &any_line_box);
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
   there is one walk and not two. */
static BfBox bf_layout(lxb_dom_element_t *el, lxb_dom_element_t *want, CssPx *want_top, bool *found,
                       BfAnonSink *anon)
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

    out.top = bf_run_of(used_value_px(el, "margin-top"));
    out.bottom = bf_run_of(used_value_px(el, "margin-bottom"));
    out.border_h = css_px(0.0);
    out.collapse_through = false;
    if (bf_content_kind(el) == BF_CONTENT_INLINE) {
        /* §9.4.2's INLINE FORMATTING CONTEXT. Nothing below this branch applies to it: §8.3.1's adjoining
           margins are stated over boxes that "both belong to in-flow BLOCK-LEVEL boxes participating in the
           same block formatting context", and there is no such box here — an inline box's vertical margins are
           not adjoining margins at all, so no run enters or leaves through this container's edges. */
        bool any_line_box = false;
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
        h = line_box_content_height(el, n->first_child, NULL, &any_line_box);
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
        BfChildKind kind = bf_child_kind(el, c);
        lxb_dom_element_t *ce = NULL;
        /* The run this iteration's box holds, meaningful for the ANONYMOUS box alone — `ce == NULL` is what
           says which box this is, and it is the same test the placement below already makes. */
        lxb_dom_node_t *anon_first = NULL, *anon_end = NULL;
        BfBox b;

        if (kind == BF_CHILD_NO_BOX) { c = c->next; continue; }
        if (kind == BF_CHILD_INLINE) {
            lxb_dom_node_t *end = bf_anon_run_end(el, c, want);

            DCHECK(end != c,
                   "CSS 2.2 §9.2.1.1's run ended where it began, so this walk would generate the same "
                   "anonymous block box for ever. The run starts at a child that generates an inline-level "
                   "box and therefore always contains at least that one");
            b = bf_anon_box(el, c, end);
            anon_first = c;
            anon_end = end;
            c = end;
        } else {
            ce = lxb_dom_interface_element(c);
            b = bf_box(ce);
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
    b = bf_layout(el, NULL, &sink, &sunk, NULL);
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
    b = bf_layout(el, NULL, &unused, &found, NULL);
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
    (void)bf_layout(cb, el, &top, &found, NULL);
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
    (void)bf_layout(el, NULL, &unused, &found, &sink);
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
