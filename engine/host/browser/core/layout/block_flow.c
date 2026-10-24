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
#include "core/timing/thread_work.h"
#include "core/layout/block_flow.h"
#include "core/layout/box_subject.h"
#include "core/layout/box_tree.h"
#include "core/layout/flex_container_baseline.h"
#include "core/layout/flex_cross_size.h"
#include "core/layout/flex_intrinsic_size.h"
#include "core/layout/flex_item.h"
#include "core/layout/grid_baseline.h"
#include "core/layout/grid_track.h"
#include "core/layout/flow_placement.h"
#include "core/layout/layout_job.h"
#include "core/layout/line_box.h"
#include "core/layout/list_marker.h"
#include "core/layout/replaced_element.h"
#include "core/layout/scroll_container.h"
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

static bool bf_length_is_zero(lxb_dom_element_t *el, const char *name)
{
    CssLength len = css_computed_length(el, name);

    return len.kind == CSS_LENGTH_ABSOLUTE && len.px.px == 0.0;
}

static bool bf_is_root(const lxb_dom_node_t *n)
{
    return n->parent != NULL && n->parent->type == LXB_DOM_NODE_TYPE_DOCUMENT;
}

bool block_flow_display_is_block_container(CssDisplay display)
{
    static const CssKeyword BLOCK_CONTAINER[] = {
        CSS_KEYWORD_BLOCK, CSS_KEYWORD_FLOW_ROOT, CSS_KEYWORD_LIST_ITEM, CSS_KEYWORD_INLINE_BLOCK,
        CSS_KEYWORD_TABLE_CELL, CSS_KEYWORD_TABLE_CAPTION,
    };
    unsigned i;

    for (i = 0; i < sizeof(BLOCK_CONTAINER) / sizeof(BLOCK_CONTAINER[0]); i++)
        if (css_display_spells(display, BLOCK_CONTAINER[i])) return true;
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

/* CSS 2.2 §9.4.1 "Block formatting contexts"' OWN LIST, WHOLE AND ALONE — "Floats, absolutely positioned
   elements, block containers (such as inline-blocks, table-cells, and table-captions) that are not block
   boxes, and block boxes with 'overflow' other than 'visible' (except when that value has been propagated to
   the viewport) establish new block formatting contexts for their contents" — plus css-display-3 §2.2 "Inner
   Display Layout Models: the flow, flow-root, table, flex, grid, and ruby keywords"' `flow-root`, which is the
   same box under its own name: that section says of it, in as many words, "it always establishes a new block
   formatting context for its contents".
   THE CITATION FOR `flow-root` READ `css-display §2.1` UNTIL THIS COMMIT and is corrected rather than dropped,
   because a reader who re-derives it from the keyword's own name will reach for the OUTER display roles again:
   §2.1 "Outer Display Roles for Flow Layout: the block, inline, and run-in keywords" does not contain the word
   at all — `flow-root` is an INNER display type, so it is §2.2's — and the number was unlevelled besides, which
   is the shielded form core/layout/block_flow.c's own overflow note below already warns about.
   IT IS ITS OWN PREDICATE AND THE ROOT ELEMENT IS NOT IN IT, which is a split of a bit that used to answer two
   questions. THE RETIRED ARGUMENT IS WRITTEN OUT RATHER THAN DELETED, BECAUSE A READER WILL RE-DERIVE IT: it
   said the root belongs here because CSS 2.1 §8.3.1 "Collapsing margins"' first exception — "margins of the
   root element's box do not collapse" — gives the same answer to the only question it was ever asked,
   `may a child's margin run escape through this box's edge`, and that
   `keeping them apart would be two predicates whose callers all merge them again`. Every
   word of that was true of every caller there was, and all of them were §8.3.1's. It stops being true the
   moment a caller asks WHICH BOXES SHARE ONE FORMATTING CONTEXT, because that question is answered by this
   section's list and the root element is on §8.3.1's list for a reason this section does not give. The two
   answers coincide at the root — a search for a context root has to stop there either way — so the split
   repairs no number; what it repairs is which SENTENCE a reader is standing on, which is the whole of what
   §9.5's population is decided by. RETIREMENT: this note goes when no caller of `bf_no_collapse_through_edges`
   remains, since the fold is then one predicate again and there is nothing to re-derive. */
bool block_flow_is_floated(lxb_dom_element_t *el)
{
    DCHECK(el != NULL, "CSS 2 §9.5.1's float question was asked of no element");
    if (css_computed_keyword_is(el, "position", CSS_KEYWORD_ABSOLUTE) || css_computed_keyword_is(el, "position", CSS_KEYWORD_FIXED)) return false;
    return !bf_computed_is(el, "float", "none");
}

static bool bf_establishes_block_formatting_context(lxb_dom_element_t *el)
{
    CssDisplay d;
    bool bfc;

    if (css_computed_keyword_is(el, "position", CSS_KEYWORD_ABSOLUTE) || css_computed_keyword_is(el, "position", CSS_KEYWORD_FIXED)) return true;
    if (block_flow_is_floated(el)) return true;
    d = css_computed_display(el);
    bfc = css_display_spells(d, CSS_KEYWORD_INLINE_BLOCK) || css_display_spells(d, CSS_KEYWORD_FLOW_ROOT) ||
          css_display_spells(d, CSS_KEYWORD_TABLE_CELL) || css_display_spells(d, CSS_KEYWORD_TABLE_CAPTION) ||
          /* CSS 2.1 §17.4 Tables in the visual formatting model states it of the wrapper in its own sentence —
             "The table wrapper box establishes a block formatting context" — and that box is what §9.4.1's
             stack holds for both spellings, so §8.3.1's run cannot reach through either. It is listed here
             rather than left to §9.4.1's own enumeration because §9.4.1 names "block containers … that are not
             block boxes" and a table wrapper is a block box that is not a block container, which is the one
             shape that list does not reach. */
          table_wrapper_generates(d);
    if (bfc) return true;
    if (css_computed_keyword_is(el, "overflow-x", CSS_KEYWORD_VISIBLE) &&
        css_computed_keyword_is(el, "overflow-y", CSS_KEYWORD_VISIBLE))
        return false;
    /* §9.4.1's own parenthesis — "except when that value has been propagated to the viewport" — and the rule
       it is excepting is css-overflow-3 §3.1.4 "Overflow Viewport Propagation", which ends "The element from
       which the value is propagated must then have a used overflow value of visible". A box whose USED
       overflow is `visible` is not on §9.4.1's list at all, so it establishes no formatting context and its
       margins DO collapse with its children's.
       THE ROOT CANNOT REACH THIS LINE — it left through the first test above, under §8.3.1's own exception for
       the root element's margins — so the only box this answers `false` for is the one §3.1.4's `html`-and-
       `body` arm names. IT IS ASKED UNCONDITIONALLY RATHER THAN UNDER A `body` TEST, and the deleted test is
       the point: which element propagates is §3.1.4's question, and a local `is this the body` here would be
       half of that rule written where it could disagree with the whole. core/layout/scroll_container.h holds
       it once and answers this, the scroll-container question and the content-clip question from it.
       WHAT STOOD HERE WAS A `DFAIL` WHOSE REMEDY CLAUSE WAS WRONG IN BOTH HALVES, recorded because a reader
       who re-derives it will re-propose it. It said to build §3.1.4 in core/css/css_computed_value.c's
       `computed_overflow`: wrong LAYER, because §3.1.4 gives a USED value and folding it into the cascade
       would make `getComputedStyle(document.body).overflowX` answer `"visible"` where a browser answers
       `"hidden"` (scroll_container.h states that derivation); and wrong TREE, because scroll_container.c had
       been reading §3.1.4 for two of its own questions since before the crash was written, so the premise
       that nothing implemented it was true only of the one file the clause happened to name. Its citation was
       unlevelled and its number was in no level — neither css-overflow-3 nor css-overflow-4 has a §3.5 — and
       its quotation read "the overflow-* values" where §3.1.4 reads "the overflow values", which is the same
       error written twice. RETIREMENT: this note goes when no css-overflow citation in this tree omits its
       level, since an unlevelled number is what let a section that exists in no document stand unplaced. */
    return !scroll_container_propagates_overflow_to_viewport(el);
}

/* CSS 2.1 §8.3.1 "Collapsing margins"' OWN QUESTION — may a child's margin run escape through this box's
   edge? — which is the list above PLUS the root element, for the reason §8.3.1 states itself and CSS 2.2
   §9.4.1 "Block formatting contexts" does not: "margins of the root element's box do not collapse". */
static bool bf_no_collapse_through_edges(lxb_dom_element_t *el)
{
    if (bf_is_root(lxb_dom_interface_node(el))) return true;
    return bf_establishes_block_formatting_context(el);
}

/* ---- CSS 2.2 §9.5 "Floats"' POPULATION FOR ONE BOX — see block_flow.h for the contract ---------------------
   THE BOX WHOSE CONTENTS' BLOCK FORMATTING CONTEXT `el`'s CONTENTS ARE IN. `el` ITSELF IS TESTED FIRST AND
   THAT IS NOT AN OFF-BY-ONE: §9.4.1's list establishes a context "for their contents", so a box on it holds
   its own contents' context and a box not on it hands them to the nearest ancestor that is.
   THE ROOT ELEMENT ENDS THE WALK WHATEVER ITS `display` IS — css-display-3 §2.8 "The Root Element's Principal
   Box": "its principal box always establishes an independent formatting context" — and the crash below is the
   only other way out, because a chain that leaves the element tree without passing a root is not a box tree
   any of §9.4.1's boxes are in. */
static lxb_dom_element_t *bf_context_root(lxb_dom_element_t *el)
{
    lxb_dom_node_t *n = lxb_dom_interface_node(el);
    char nbuf[160];

    for (;;) {
        lxb_dom_element_t *e = lxb_dom_interface_element(n);

        if (bf_is_root(n) || bf_establishes_block_formatting_context(e)) return e;
        DCHECKF(n->parent != NULL && n->parent->type == LXB_DOM_NODE_TYPE_ELEMENT,
                "%s: CSS 2.2 §9.4.1's walk for the block formatting context this box's contents are in left "
                "the element tree without reaching a root element, so there is no outermost context for it to "
                "stop at. css-display-3 §2.8 \"The Root Element's Principal Box\" makes the root's own box the "
                "one that \"always establishes an independent formatting context\", so a chain that misses it is "
                "a box in no formatting context at all — which is a DETACHED subtree being laid out, and "
                "core/dom/element_view.h's has-a-box predicate is the caller's step that declines one",
                box_subject_node(n, nbuf, sizeof nbuf));
        n = n->parent;
    }
}

/* The node after `n` in a pre-order walk of `root`'s subtree, entering `n`'s children when `descend`. */
static lxb_dom_node_t *bf_scan_next(lxb_dom_node_t *n, bool descend, lxb_dom_node_t *root)
{
    if (descend && n->first_child != NULL) return n->first_child;
    for (; n != root; n = n->parent)
        if (n->next != NULL) return n->next;
    return NULL;
}

/* §9.5's FLOAT, ANYWHERE IN ONE CONTEXT'S SUBTREE, IN DOCUMENT ORDER — which a pre-order walk IS, and which
   matters because every one of §9.5.1's nine rules that mentions another box says "earlier in the source
   document". The walk holds its place in the placement record when a resting drive spends its elements.
   THE THREE TESTS ARE IN THIS ORDER BECAUSE CSS 2.2 §9.7 IS NOT APPLIED TO THE COMPUTED `float` IN THIS ENGINE:
   an `position: absolute; float: left` element still reads `left`, so asking POSITION first keeps this walk
   right whichever way that gap is later closed. A SKIPPED SUBTREE IS SKIPPED WHOLE, which is §10.6.7's own
   example — "floats inside absolutely positioned descendants or other floats are not [taken into account]" —
   and why the float test is answered before the establishes-a-context test: a float is both. */
bool block_flow_context_first_float_ask(lxb_dom_element_t *el, lxb_dom_element_t **out)
{
    lxb_dom_element_t *root, *f = NULL, *up;
    lxb_dom_node_t *n, *rn, *fp;
    char fbuf[160], rbuf[160], ubuf[160];

    DCHECK(el != NULL && out != NULL,
           "CSS 2.2 §9.5's population was asked about a formatting context with no box or nowhere to answer");
    root = bf_context_root(el);
    if (flow_placement_first_float_peek(root, out)) return true;
    rn = lxb_dom_interface_node(root);
    if (!flow_placement_float_scan_peek(root, &n)) n = rn->first_child;
    while (n != NULL) {
        bool descend = true;

        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            lxb_dom_element_t *e = lxb_dom_interface_element(n);

            if (!layout_job_scan_take()) {
                flow_placement_float_scan_hold(root, n);
                return false;
            }
            /* §9.2 "Controlling box generation": `display: none` generates no box, so there is nothing here for
               §9.5 to float and nothing under it either. `contents` keeps its children's boxes, and this walk
               reaches them because it is over the element tree. */
            if (css_computed_display_is(e, CSS_KEYWORD_NONE) ||
                css_computed_keyword_is(e, "position", CSS_KEYWORD_ABSOLUTE) ||
                css_computed_keyword_is(e, "position", CSS_KEYWORD_FIXED)) {
                descend = false;
            } else if (block_flow_is_floated(e)) {
                f = e;
                break;
            } else if (bf_establishes_block_formatting_context(e)) {
                descend = false;
            }
        }
        n = bf_scan_next(n, descend, rn);
    }
    flow_placement_first_float_record(root, f);
    *out = f;
    if (f == NULL) return true;
    /* ONE FACT REACHED TWO WAYS, ASSERTED AGAINST ITSELF. The walk DOWN decides membership by skipping every
       subtree whose root is on CSS 2.2 §9.4.1 "Block formatting contexts"' list; the walk UP decides it by
       stopping at the first ancestor on that same list, so a float this search reports must name THIS context
       when asked from its own side. The float's own box establishes a context, so the question is asked of its
       PARENT: `bf_context_root` answers where a box's CONTENTS sit, and the float sits in its parent's. */
    fp = lxb_dom_interface_node(f)->parent;
    DCHECKF(fp != NULL && fp->type == LXB_DOM_NODE_TYPE_ELEMENT,
            "%s: CSS 2.2 §9.5's float was found with no element parent, so there is no box whose formatting "
            "context it participates in. The search starts at a context root and descends through element "
            "children only, so the float it reports always has one — unless the tree changed under the walk",
            box_subject(f, fbuf, sizeof fbuf));
    up = bf_context_root(lxb_dom_interface_element(fp));
    DCHECKF(up == root,
            "%s: CSS 2.2 §9.4.1's membership was reached two ways and they disagree — the walk DOWN from %s "
            "reported this float as belonging to that context, and the walk UP from its parent stops at %s "
            "instead. §9.5.1's own scope sentence is what both are reading (\"References to other elements in "
            "these rules refer only to other elements in the same block formatting context as the float\"), so "
            "one of the two readings of §9.4.1's list has been changed without the other",
            box_subject(f, fbuf, sizeof fbuf), box_subject(root, rbuf, sizeof rbuf),
            box_subject(up, ubuf, sizeof ubuf));
    return true;
}

lxb_dom_element_t *block_flow_context_first_float(lxb_dom_element_t *el)
{
    LayoutJobDrive d;
    lxb_dom_element_t *f = NULL;

    layout_job_drive_begin(&d);
    while (!block_flow_context_first_float_ask(el, &f)) (void)block_flow_drive_deferred(&d);
    layout_job_drive_end(&d);
    return f;
}

/* CSS 2 §8.1 "Box dimensions"' TWO EDGES between a box's TOP BORDER EDGE and its TOP CONTENT EDGE. §9.4.1's
   stack reaches each child at its top BORDER edge — that is what `block_flow_child_top_ask` reports and what this
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
    return bf_length_is_zero(el, "min-height") || css_computed_length_is(el, "min-height", CSS_KEYWORD_AUTO);
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
   anonymous inline box would belong to — THE TEXT NODE'S OWN DOM PARENT.
   THIS CLAUSE USED TO READ "the parent, SINCE AN ANONYMOUS BOX INHERITS FROM THE BOX IT IS INSIDE", and the
   answer it gives is right while its reason is exactly backwards — it is kept in its own words because it is
   the reason a reader re-derives, and re-deriving it is what would put the container back. css-display-3
   §2.5 "Box Generation: the none and contents keywords" is the refutation: the BOX a spliced run is inside is
   not the element it inherits from, because "as only the box tree is affected, any semantics based on the
   document tree, such as selector-matching, event handling, and property inheritance, are not affected". So
   the element is read off the node and never off the walk, and the two agree for every document with no
   `display: contents` element in it — which is why the wrong reason survived giving right answers.
   §16.6's table gives the answer per value: `normal` and `nowrap` collapse a sequence of white space,
   `pre`, `pre-wrap` and `pre-line` preserve it (css-text-3 adds `break-spaces`), and a preserved run is real
   inline content with a real line box.
   THIS PREDICATE ANSWERS §9.2.2.1 AND MEASURES NOTHING, which is the whole of its contract and was not always
   so: it used to CRASH for a run that generates a box, naming the glyph advance a line box needs. That put one
   component's missing input inside another component's classification, and the classification is right for
   both — the run either generates an anonymous inline box or it does not. The measurement crash now lives in
   core/layout/line_box.c, at the walk that would have to place the glyphs, which is where the advance is
   actually the missing operand and where the ONE line reporting it can name what to build. */
/* THE ELEMENT WHOSE `white-space` THIS READS IS THE TEXT NODE'S OWN DOM PARENT, DERIVED HERE AND NEVER PASSED
   IN. core/layout/block_flow.h states the reason in full and it is css-display-3 §2.5
   "Box Generation: the none and contents keywords"'s Note: the splice moves a node into another box's child
   sequence and moves no inheritance with it, so the container a walk is enumerating and the element this run
   inherits from are two different elements the moment a `display: contents` ancestor stands between them. */
bool block_flow_text_child_generates_box(const lxb_dom_node_t *n)
{
    lxb_dom_element_t *style;
    char *ws;
    bool collapses;

    DCHECK(n != NULL && n->type == LXB_DOM_NODE_TYPE_TEXT,
           "§9.2.2.1's white-space question was asked about something that is not a TEXT node — the rule is "
           "about a run of character data and the property it reads is that run's containing element's "
           "inherited `white-space`");
    DCHECK(n != NULL && n->parent != NULL && n->parent->type == LXB_DOM_NODE_TYPE_ELEMENT,
           "§9.2.2.1's white-space question was asked about a TEXT node with no ELEMENT parent. The property "
           "it reads is INHERITED, and css-display-3 §2.5 \"Box Generation: the none and contents "
           "keywords\"'s Note keeps inheritance on the document tree — "
           "\"any semantics based on the document tree, such as selector-matching, event handling, and "
           "property inheritance, are not affected\" — so the element this run inherits from is its own DOM "
           "parent and there is no other answer to fall back to. A text node whose parent is a document or a "
           "fragment is not a child any walk over a block container's content reaches");

    /* A run that is not entirely white space has content §9.2.2.1's sentence says nothing about collapsing
       away, so it generates a box whatever `white-space` says. */
    if (!block_flow_text_is_all_document_white_space(n)) return true;
    style = lxb_dom_interface_element(n->parent);
    ws = bf_computed(style, "white-space");
    collapses = strcmp(ws, "normal") == 0 || strcmp(ws, "nowrap") == 0;
    /* `pre-line` collapses spaces and tabs and preserves segment breaks (css-text-3 §4.1.3 "Segment Break
       Transformation Rules"), so a run of only spaces and tabs collapses away and one holding a line feed is a
       forced break with a line box. */
    if (strcmp(ws, "pre-line") == 0) {
        const lxb_dom_character_data_t *cd = (const lxb_dom_character_data_t *)n;

        collapses = cd->data.data == NULL || memchr(cd->data.data, '\n', cd->data.length) == NULL;
    }
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

/* CSS 2.2 §9.2.1 "Block-level elements and block boxes" over css-display-3 §2's pair: a block-level box whose
   inner type is a block container's (`flow`, `flow-root`, a list item's) or a flex or grid container's, which
   css-grid-1 §5.2 "Sizing Grid Containers" sizes "like a block box". A block-level table is §17.4's wrapper and
   `block ruby` is css-ruby-1's, so neither is here. */
static bool bf_display_is_block_level(CssDisplay d)
{
    return d.kind == CSS_DISPLAY_PAIR && d.outer == CSS_KEYWORD_BLOCK &&
           (d.inner == CSS_KEYWORD_FLOW || d.inner == CSS_KEYWORD_FLOW_ROOT || d.inner == CSS_KEYWORD_FLEX ||
            d.inner == CSS_KEYWORD_GRID);
}

/* CSS 2.2 §9.2.2 "Inline-level elements and inline boxes": an inline outer type over any inner type but ruby,
   whose inline box css-ruby-1 builds. */
static bool bf_display_is_inline_level(CssDisplay d)
{
    return d.kind == CSS_DISPLAY_PAIR && d.outer == CSS_KEYWORD_INLINE && d.inner != CSS_KEYWORD_RUBY;
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
    CssDisplay d = css_computed_display(el);
    bool block, inline_level;
    TableBoxKind table;
    char nbuf[160];

    if (css_display_spells(d, CSS_KEYWORD_NONE)) return BLOCK_FLOW_CHILD_NO_BOX;
    if (css_display_spells(d, CSS_KEYWORD_CONTENTS))
        DFAIL("css-display-3 §2.5 \"Box Generation: the none and contents keywords\" gives this child "
              "`display: contents`: \"The element itself does not generate any boxes, but its children and "
              "pseudo-elements still generate boxes and text sequences as normal.\" So the box tree and the "
              "ELEMENT tree are no longer the same shape here, and the children this walk must place are the "
              "child's children spliced into this list at its position. THE SPLICE IS BUILT — "
              "core/layout/box_tree.h is §2.5's \"the element must be treated as if it had been replaced in "
              "the element tree by its contents\" as a child sequence — so what is owed here is ROUTING this "
              "walk to it and NOT a second copy of the rule. "
              "THIS FILE'S OWN WALKS ARE ROUTED, WHICH IS WHY REACHING THIS LINE NOW MEANS A CALLER OUTSIDE "
              "THEM: `bf_content_next`, `bf_content_is_inline`, `bf_inline_box_breaks`, the run's start and step "
              "and `block_flow_run_generates_box` all take that sequence, so nothing this file enumerates can "
              "hand this classification a `contents` element any more. A caller that still composes a child "
              "list with `->first_child` and `->next`, or a box parent with `->parent`, is what gets here — "
              "and `block_flow_child_kind`'s own box-parent precondition names that caller directly. "
              "THE COUPLING THAT MADE THE ROUTING ONE LANDING RATHER THAN SEVERAL IS RECORDED BECAUSE A "
              "READER CONVERTING A REMAINING WALK MEETS IT AGAIN: `block_flow_run_generates_box` used to read "
              "a `BlockFlowRun`'s bounds as SIBLINGS of the container — answering TRUE for a bound whose "
              "parent is not the container, on the ground that such a bound is a FRAGMENT of an inline box "
              "§9.2.1.1 broke, which a SPLICED bound is not — so converting an iterator without the run's own "
              "step would put a spurious anonymous block box on §9.4.1's stack for every run bounded inside a "
              "`contents` child, which is a plausible box tree and not a crash");
    /* §9.7's SECOND arm. §9.3.1 states what it means for every caller of this classification at once —
       "Absolutely positioned boxes are taken out of the normal flow. This means they have no impact on the
       layout of later siblings" — and §10.6.3 says the same thing for the one walk in this file: "Only children
       in the normal flow are taken into account (i.e., floating boxes and absolutely positioned boxes are
       ignored…)". That is the rule RUNNING and not a gap; the box's OWN position is §9.3.2's offsets over a
       static position, which is a different question ANSWERED BY THE SAME WALK — `block_flow_static_child_top`
       reads the running position out at this child's content position without ever putting it on the stack,
       which is why this classification does not have to change for it. */
    if (css_computed_keyword_is(el, "position", CSS_KEYWORD_ABSOLUTE) || css_computed_keyword_is(el, "position", CSS_KEYWORD_FIXED)) {
        return BLOCK_FLOW_CHILD_NO_BOX;
    }
    /* §9.7's THIRD arm, ANSWERED AND NOT REFUSED — see block_flow.h. Every caller of this classification meets
       a float with a different missing capability, so the refusal belongs at each of their own lines: this
       file's stack names §9.5.2's clearance below, core/layout/line_box.c names §9.4.2's shortened line box,
       and core/layout/intrinsic_size.c names what a shortened line box does to a contribution. */
    if (block_flow_is_floated(el)) {
        return BLOCK_FLOW_CHILD_FLOAT;
    }
    block = bf_display_is_block_level(d);
    inline_level = bf_display_is_inline_level(d);
    /* CSS 2.1 §17.2 The CSS table model's box types, classified ONCE through the component that owns the
       vocabulary §17.2.1 Anonymous table objects' rules are written in (core/layout/table_box.h). A prefix
       test on "table-" answered this question for a long time and it answered it as ONE question; the two arms
       below are two DIFFERENT missing things, and a single predicate over both is what let one crash report
       them as one. */
    table = table_box_kind(d);
    if (block) return BLOCK_FLOW_CHILD_BLOCK;
    /* CSS 2.2 §9.2.2 "Inline-level elements and inline boxes": this child is inline-level, so it is not on
       §9.4.1's stack at all — §9.4.2's line boxes hold it, and which of the two formatting contexts this block
       container establishes is decided over the WHOLE child list rather than here (bf_content_is_inline). */
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
    DFAILF("%s: this child's computed `display` is a ruby box or a `run-in` one, which CSS 2 §9.2's box types do "
           "not cover. Build css-ruby-1 §2.2 \"Anonymous Ruby Box Generation\" and css-display-3 §5 \"Run-In "
           "Layout\" (core/layout/used_value.c's `uv_box_kind` names both) as box-tree steps",
           box_subject(el, nbuf, sizeof nbuf));
    return BLOCK_FLOW_CHILD_NO_BOX;
}

/* css-lists-3 §3.1's marker member, classified by §3.5's position — see block_flow.h for both arms. */
static BlockFlowChildKind bf_marker_child(lxb_dom_element_t *parent, BoxTreeChild c)
{
    char pbuf[160];

    DCHECK(c.pseudo == LXB_CSS_SELECTOR_PSEUDO_ELEMENT_MARKER,
           "CSS 2 §9.2's box generation was asked about a pseudo-element member other than ::marker, which is "
           "the one core/layout/box_tree.h yields — css-pseudo-4 §4.1's ::before and ::after need a computed "
           "`content`, and that property does not parse here");
    if (list_marker_position(parent) == LIST_MARKER_POSITION_OUTSIDE) return BLOCK_FLOW_CHILD_OUTSIDE_MARKER;
    DFAILF("%s has `list-style-position: inside`: css-lists-3 §3.5 makes its marker \"an inline element at the "
           "start of the list item’s contents\", which is an in-flow inline box on this item's first line, and "
           "§3.2's third arm fills it with \"a text sequence consisting of the specified marker string\". BUILD "
           "that string — css-counter-styles-3 §3.1 \"Counter algorithms: the system descriptor\" over the "
           "css-lists-3 §4.6 \"The Implicit list-item Counter\" value, which core/css/css_counter_style.h assigns "
           "to a later component — and a core/layout/line_box.c text item whose characters are not a DOM Text "
           "node's. The release arm lays the marker out as `outside`, contributing nothing in flow",
           box_subject(parent, pbuf, sizeof pbuf));
    (void) pbuf;
    return BLOCK_FLOW_CHILD_OUTSIDE_MARKER;
}

BlockFlowChildKind block_flow_child_kind(lxb_dom_element_t *parent, BoxTreeChild c)
{
    lxb_dom_node_t *n = c.node;

    DCHECK(parent != NULL && n != NULL,
           "CSS 2 §9.2's box generation was asked about a child with no node, or with no block container for "
           "it to be a child OF — the parent is not decoration here, §9.2.1's level and §9.7's three-property "
           "order are answers about the box this child is a child of");
    /* THIS MESSAGE USED TO NAME §9.2.2.1's WHITE-SPACE RULE AS THE REASON THE PARENT IS NEEDED, AND IT IS NOT
       ONE ANY MORE: that rule reads an INHERITED property, so it reads the TEXT NODE'S OWN parent, which
       core/layout/block_flow.h's predicate now derives for itself. The retired reason is recorded rather than
       dropped because it is the one a reader re-derives from the container being in hand — and because it is
       what the equality below is about to stop being able to assume. */
    DCHECK(box_tree_parent(c) == parent,
           "CSS 2 §9.2's box generation was asked about a node that is not a CHILD of the block container it "
           "was asked with. §9.2.1's level and §9.7's three-property order are stated over the box this node "
           "is a child of, so a node from elsewhere in the tree would be classified against a formatting "
           "context it is not in. "
           "THIS EQUALITY USED TO READ `n->parent == parent` AND IS REWRITTEN RATHER THAN DELETED, WHICH IS "
           "WHAT THE SPLICE DOES TO IT rather than retiring it: css-display-3 §2.5 \"Box Generation: the none "
           "and contents keywords\" makes a box-tree child of `parent` need not be a DOM child of it, so the "
           "DOM equality was the right precondition read through the wrong relation and the box-tree one is "
           "the same precondition stated over the tree §9.2 is actually about. A CALLER THAT COMPOSED "
           "`n->parent` FOR THE ARGUMENT IS EXACTLY WHAT THIS NOW CATCHES — a `contents` element is not a box "
           "and cannot be the `parent` any classification is stated against. §9.2.2.1's collapsing used to be "
           "named here as a third answer stated over `parent`'s child list and is no longer one of them");
    if (box_tree_child_is_pseudo(c)) return bf_marker_child(parent, c);
    switch (n->type) {
    case LXB_DOM_NODE_TYPE_ELEMENT:
        return bf_element_child(lxb_dom_interface_element(n));
    case LXB_DOM_NODE_TYPE_TEXT:
        /* §9.2.2.1's anonymous inline box is INLINE-level — the box that text generates is an inline box, not
           a block-level one, and calling it block-level here is what used to make a text run look like
           something §9.4.1's stack could place. */
        return block_flow_text_child_generates_box(n) ? BLOCK_FLOW_CHILD_INLINE : BLOCK_FLOW_CHILD_NO_BOX;
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

/* ---- CSS 2.2 §9.2.1.1 "Anonymous block boxes"' SECOND PARAGRAPH: WHICH INLINE BOXES BREAK ----------------
   The first paragraph of that section is the one every walk here already runs — a block container holding a
   block-level box is forced "to have only block-level boxes inside it". The SECOND is a different box tree and
   it is the one that decides whether the first even applies: "When an inline box contains an in-flow
   block-level box, the inline box (and its inline ancestors within the same line box) is broken around the
   block-level box (and any block-level siblings that are consecutive or separated only by collapsible
   whitespace and/or out-of-flow elements), splitting the inline box into two boxes (even if either side is
   empty), one on each side of the block-level box(es). The line boxes before the break and after the break are
   enclosed in anonymous block boxes, and THE BLOCK-LEVEL BOX BECOMES A SIBLING OF THOSE ANONYMOUS BOXES."
   SO THE BLOCK-LEVEL BOX LEAVES THE INLINE BOX AND JOINS THE BLOCK CONTAINER'S OWN BOX LIST, which is why this
   is a question the CONTAINER's classification has to ask and not one an inline box answers for itself. The
   section's own worked example is the whole derivation: with `p { display: inline }` and
   `span { display: block }` over `<P>…<SPAN>…</SPAN>…</P>`, "the resulting boxes would be a block box
   representing the BODY, containing an anonymous block box around C1, the SPAN block box, and another
   anonymous block box around C2" — three boxes on BODY's stack out of ONE child node, and a BODY that
   therefore CONTAINS a block-level box in exactly the sense §9.4.2 "Inline formatting contexts" means it ("an
   inline formatting context is established by a block container box that contains no block-level boxes").
   THE DESCENT IS THROUGH INLINE BOXES AND NOTHING ELSE, and §9.2.2 "Inline-level elements and inline boxes"
   is the whole of that test in one sentence: "A non-replaced element with a 'display' value of 'inline'
   generates an inline box", while "inline-level boxes that are not inline boxes (such as replaced inline-level
   elements, inline-block elements, and inline-table elements) are called atomic inline-level boxes because
   they participate in their inline formatting context as a single opaque box". An ATOMIC box is broken by
   nothing inside it — it establishes its own formatting context and keeps its block-level children — so
   `<span style="display:inline-block"><div></div></span>` breaks no ancestor and this walk must not enter it.
   THE RECURSION IS §9.2.1.1's OWN "and its inline ancestors within the same line box": a block-level box at
   any inline depth breaks every inline box above it up to the block container, so asking this of a container's
   DIRECT child answers for the whole chain beneath it. */
static bool bf_generates_inline_box(lxb_dom_element_t *el)
{
    return css_computed_display_is(el, CSS_KEYWORD_INLINE) && !replaced_element_of(el).replaced;
}

/* THE NODE FORM, so the three sites that ask it of a classified child do not each re-derive "is this an
   element" — a text node is §9.2.2.1's anonymous inline box, which has no `display` to read and no children
   to break around. */
bool block_flow_child_breaks_inline_box(lxb_dom_element_t *parent, BoxTreeChild n);

static bool bf_inline_box_breaks(lxb_dom_element_t *el)
{
    BoxTreeChild c;

    /* THE INLINE BOX'S OWN BOX-TREE CHILDREN, which is css-display-3 §2.5's sequence and not `el`'s DOM child
       list: a `contents` element INSIDE an inline box puts its own children in that inline box's content, so a
       block-level box under one still breaks the box it is spliced into. Walking `->next` here would have
       asked `block_flow_child_kind` about a node that generates no box. */
    for (c = box_tree_first_child(el); box_tree_child_exists(c); c = box_tree_next_sibling(el, c)) {
        switch (block_flow_child_kind(el, c)) {
        /* §9.2.1.1's "an in-flow block-level box", met. */
        case BLOCK_FLOW_CHILD_BLOCK:
            return true;
        case BLOCK_FLOW_CHILD_INLINE:
            if (block_flow_child_breaks_inline_box(el, c)) return true;
            break;
        /* NEITHER OF THESE IS "IN-FLOW", and the section says so about both in the same clause: it treats
           block-level siblings "that are consecutive or separated only by collapsible whitespace and/or
           out-of-flow elements" as one break, so §9.2.2.1's collapsed white space and §9.3.1's out-of-flow box
           are stepped over rather than counted, and §9.5's float is out of the flow by its own sentence
           ("since a float is not in the flow"). A float is therefore NOT a box that breaks an inline box, and
           reading it as one would split a line every real user agent keeps whole. */
        case BLOCK_FLOW_CHILD_NO_BOX:
        case BLOCK_FLOW_CHILD_FLOAT:
            break;
        /* css-lists-3 §3.5 places an outside marker "outside the principal block box", so it is not in-flow
           content of any box and breaks nothing. An inline box is never a list item, so this arm is a guard. */
        case BLOCK_FLOW_CHILD_OUTSIDE_MARKER:
            break;
        /* NO `default` ARM, for the reason core/layout/intrinsic_size.c states at its own switch: a fifth
           `BlockFlowChildKind` must be a COMPILE failure at every caller and not a case silently folded into
           "does not break". */
        }
    }
    return false;
}

bool block_flow_child_breaks_inline_box(lxb_dom_element_t *parent, BoxTreeChild c)
{
    lxb_dom_element_t *el;
    lxb_dom_node_t *n;

    DCHECK(block_flow_child_kind(parent, c) == BLOCK_FLOW_CHILD_INLINE,
           "CSS 2.2 §9.2.1.1's break was asked about a child that generates no in-flow inline-level box. The "
           "section's sentence is about an INLINE BOX being broken around a block-level box inside it, so a "
           "child with no such box is not a subject of it at all — and a caller that asked here without "
           "classifying first would be reading a `display` off a node §9.2 generates nothing for");
    /* An INLINE answer is a source-document node: the one pseudo member is classified as an outside marker. */
    n = box_tree_child_node(c);
    if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    el = lxb_dom_interface_element(n);
    return bf_generates_inline_box(el) && bf_inline_box_breaks(el);
}

/* IS `el` A BOX ON `container`'s BOX LIST, RATHER THAN A CHILD OF IT — one question asked at three sites,
   because a block container's BOX list and its ELEMENT child list stopped being the same list the day this
   file's enumeration began descending into a broken inline box. CSS 2.2 §9.2.1.1 "Anonymous block boxes" is
   the whole of the difference and states it in one sentence: "When an inline box contains an in-flow
   block-level box, the inline box (and its inline ancestors within the same line box) is broken around the
   block-level box (and any block-level siblings that are consecutive or separated only by collapsible
   whitespace and/or out-of-flow elements), splitting the inline box into two boxes (even if either side is
   empty), one on each side of the block-level box(es). The line boxes before the break and after the break
   are enclosed in anonymous block boxes, and the block-level box becomes a sibling of those anonymous
   boxes." A SIBLING OF BOXES ON `container`'s STACK IS A MEMBER OF `container`'s BOX LIST, however deep in
   the ELEMENT tree it sits — which is why no test over a PARENT POINTER can answer this question.
   EVERY INTERVENING ELEMENT MUST BREAK, AND THAT IS THE SECTION'S OWN PARENTHESIS RATHER THAN A STRICTER
   READING: "and its inline ancestors within the same line box" breaks the WHOLE chain, and
   `bf_inline_box_breaks` recurses through inline children, so a block-level box at any inline depth makes
   every inline box above it break. An ancestor that does not break is therefore not an inline box this
   section reaches through, and the box below it is on no list of `container`'s.
   THE TWO SHAPES IT REFUSES ARE THE TWO §10.1's WALK STEPS OVER AND THIS SECTION DOES NOT PUT HERE. CSS 2.2
   §9.2.2 "Inline-level elements and inline boxes"' ATOMIC inline-level box — a replaced `display: inline`
   element — KEEPS its own block-level children, because it "participate[s] in their inline formatting
   context as a single opaque box".
   THE `display: contents` HALF OF THAT SENTENCE IS RETIRED AND IS REWRITTEN RATHER THAN DELETED, because a
   reader who re-derives the chain from the element tree will re-add it: it said such an element is a splice
   `bf_element_child` refuses by name, "so no enumeration in this file reaches through one yet". The ascent is
   over BOX PARENTS now, so a `contents` element between `el` and `container` is not an intervening box at all
   — it is skipped by `box_tree_parent`, exactly as css-display-3 §2.5 "Box Generation: the none and contents
   keywords" replaces it by its contents — and the chain this loop tests is the chain of INLINE BOXES the
   section is about. Refusing it here would have been §2.5's splice read as §9.2.2's atomic box, which is the
   one confusion this file's own child classification keeps the two keywords apart to prevent. */
static bool bf_on_box_list_of(lxb_dom_element_t *el, lxb_dom_element_t *container)
{
    lxb_dom_element_t *q, *qp;

    DCHECK(el != NULL && container != NULL,
           "CSS 2.2 §9.2.1.1's box-list membership was asked with no box or no container. The question is "
           "about a PAIR — whether THIS box is on THAT container's list — so either half missing makes it a "
           "question about nothing rather than a question with an unknown answer");
    /* `q` RANGES OVER THE BOXES STRICTLY BETWEEN `el` AND `container`, each tested against its OWN box parent,
       which is the same enumeration the DOM-parent form made and is the reason `el` itself is never tested:
       the question is about the chain ABOVE it. */
    for (q = box_tree_parent(box_tree_child_of_node(lxb_dom_interface_node(el))); q != container; q = qp) {
        BoxTreeChild qc;

        /* An ascent that leaves the element tree was asked about a box in another subtree. */
        if (q == NULL) return false;
        qc = box_tree_child_of_node(lxb_dom_interface_node(q));
        qp = box_tree_parent(qc);
        if (qp == NULL) return false;
        if (block_flow_child_kind(qp, qc) != BLOCK_FLOW_CHILD_INLINE) return false;
        if (!block_flow_child_breaks_inline_box(qp, qc)) return false;
    }
    /* THE LOOP ENDS ONLY AT `container`, which the other exits return FALSE from — a walk that ran out of
       ancestors, or met a box the section does not break, was asked about a box on another list. */
    return true;
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

static bool bf_box(lxb_dom_element_t *el, BfBaseline pass, BfBox *out);

/* ---- WHICH FORMATTING CONTEXT THIS BLOCK CONTAINER ESTABLISHES ------------------------------------------
   CSS 2.2 §9.4.2 states the condition and §9.2.1 states the alternative in the same breath: a block container
   "either contains only block-level boxes or establishes an inline formatting context", and an inline
   formatting context "is established by a block container box THAT CONTAINS NO BLOCK-LEVEL BOXES". One
   block-level box settles the container's context — §9.2.1.1 "Anonymous block boxes": "then we FORCE IT TO
   HAVE ONLY BLOCK-LEVEL BOXES INSIDE IT" — so the scan stops at the first one: a long list of block-level
   children is answered at its first member, and only a container that holds none reads its whole list.
   Answers whether the container's children hold an inline-level box and no block-level box. */
static bool bf_content_is_inline(lxb_dom_element_t *el)
{
    BoxTreeChild c;
    bool inl = false;

    /* css-display-3 §2.5's SPLICED SEQUENCE, because this walk and `block_flow_next_block_box` are REQUIRED to
       agree — core/layout/intrinsic_size.c asserts that agreement by name — and a `contents` child would put
       its children on one walk's list and not the other's. */
    for (c = box_tree_first_child(el); box_tree_child_exists(c); c = box_tree_next_sibling(el, c)) {
        switch (block_flow_child_kind(el, c)) {
        case BLOCK_FLOW_CHILD_NO_BOX: break;
        /* css-lists-3 §3.5's outside marker box "is placed outside the principal block box", so it is neither
           a block-level nor an inline-level box of this content and leaves both bits as the source-document
           children set them. An inside marker would be inline content; classifying one aborts. */
        case BLOCK_FLOW_CHILD_OUTSIDE_MARKER: break;
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
        case BLOCK_FLOW_CHILD_BLOCK:  return false;
        case BLOCK_FLOW_CHILD_INLINE:
            inl = true;
            /* §9.2.1.1's SECOND PARAGRAPH IS PART OF THIS QUESTION AND NOT OF A LATER ONE. A broken inline box
               puts its in-flow block-level descendant on THIS container's box list, as a SIBLING of the
               anonymous boxes around the two halves — so the container "contains a block-level box" in exactly
               the sense §9.4.2's establishing condition means, and answering INLINE here would hand §9.4.2's
               line boxes a box no line box can hold. */
            if (block_flow_child_breaks_inline_box(el, c)) return false;
            break;
        }
    }
    return inl;
}

/* §9.4.2's OWN CONDITION, WHOLE — see the header for why it is exported rather than re-derived. Both conjuncts
   are the section's own sentence: the box must be a BLOCK CONTAINER, and it must CONTAIN NO BLOCK-LEVEL BOXES.
   The `display` half is asked FIRST and is not redundant with the classification below: `bf_content_is_inline` reads
   a CHILD LIST and a `display: inline` element with only text children has one that looks exactly like a block
   container's, so a caller that skipped it would be told that an inline box establishes the formatting context
   its own ancestor establishes — and would then place that context's line boxes against the inline box's
   content edge, which is not a rectangle §9.4.2 gives line boxes at all. */
bool block_flow_establishes_inline_context(lxb_dom_element_t *el)
{
    CssDisplay d;
    bool container;

    DCHECK(el != NULL, "CSS 2.2 §9.4.2's establishing condition was asked with no element");
    d = css_computed_display(el);
    container = block_flow_display_is_block_container(d);
    if (!container) return false;
    return bf_content_is_inline(el);
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

/* §9.2.1.1's box at `top`, appended to `container`'s anonymous boxes in core/layout/flow_placement.h's record
   as the stack lays it out; see block_flow.h for the entry's contract. */
static void bf_anon_record(lxb_dom_element_t *container, BlockFlowRun run, CssPx top, CssPx height)
{
    BlockFlowAnonBox box;

    box.run = run;
    /* §9.2.1.1's "the margins will be 0" with §10.3.3's constraint equation over an initial `width: auto` puts
       this box's content box's inline-start edge exactly on its container's — see block_flow.h. A literal zero
       and not a measured one, so it carries no environment fact for css_length.h to union. */
    box.content_x = css_px(0.0);
    box.content_y = top;
    box.height = height;
    flow_placement_anon_append(container, box);
}

/* ---- CSS 2.2 §9.2.1.1's BOX LIST, ENUMERATED IN CONTENT ORDER -------------------------------------------
   A block container's box list is NOT a partition of its child nodes, and §9.2.1.1's own worked example is
   where that stops being a technicality: with `p { display: inline }` and `span { display: block }`, "the
   resulting boxes would be a block box representing the BODY, containing an anonymous block box around C1, the
   SPAN block box, and another anonymous block box around C2" — THREE boxes out of ONE child node, because the
   inline box "is broken around the block-level box … splitting the inline box into two boxes (even if either
   side is empty)" and "the block-level box becomes a sibling of those anonymous boxes".
   SO THE ENUMERATION IS OVER POSITIONS IN THE CONTAINER'S CONTENT, and CONTENT is the container's descendants
   in document order with an in-flow INLINE BOX transparent — its children spliced in at its own position. That
   is §9.2.2 "Inline-level elements and inline boxes"' distinction and not a choice: "A non-replaced element
   with a 'display' value of 'inline' generates an inline box", while an `inline-block`, an `inline-table` or a
   replaced inline-level element is an ATOMIC inline-level box that "participate[s] in their inline formatting
   context as a single opaque box" and keeps its own block-level children.
   THE STEP DESCENDS ONLY INTO AN INLINE BOX THAT BREAKS, and that is an equivalence rather than an
   optimisation: this walk exists to find the positions at which the box list CHANGES, and an inline box with
   no in-flow block-level box inside it changes it nowhere — every position inside it is in the same box as
   every other. A consumer that must reach the CONTENT of such a box (a measurement, a fill) descends on its
   own; what it may not do is delimit on its own, which is why this is here and not there. */
static bool bf_content_child_is_breaking_inline_box(BoxTreeChild n)
{
    lxb_dom_element_t *parent;

    DCHECK(box_tree_child_exists(n), "CSS 2.2 §9.2.1.1's content order was stepped from no node");
    /* THE BOX THIS POSITION IS A CHILD OF, which css-display-3 §2.5's splice makes a different element from
       its DOM parent wherever a `contents` element stands between them — and `block_flow_child_kind` is
       stated over the BOX, so composing `n->parent` here would hand it a pair it now refuses. */
    parent = box_tree_parent(n);
    if (parent == NULL) return false;
    if (block_flow_child_kind(parent, n) != BLOCK_FLOW_CHILD_INLINE) return false;
    return block_flow_child_breaks_inline_box(parent, n);
}

static BoxTreeChild bf_content_next(lxb_dom_element_t *el, BoxTreeChild n)
{
    lxb_dom_node_t *root;

    DCHECK(el != NULL && box_tree_child_exists(n),
           "CSS 2.2 §9.2.1.1's content order was stepped with no container or no node");
    root = lxb_dom_interface_node(el);
    DCHECK(!box_tree_child_is_node(n, root),
           "CSS 2.2 §9.2.1.1's content order was stepped from the CONTAINER itself, which is not a position in "
           "its own content — the container brackets this sequence, it is not a member of it");
    /* THE DESCENT AND THE ASCENT ARE BOTH OVER core/layout/box_tree.h's SPLICED SEQUENCE. css-display-3 §2.5
       "Box Generation: the none and contents keywords" replaces a `contents` element by its contents, so such
       an element is not a position in anybody's content — the sequence steps THROUGH it in both directions,
       and neither half of this step may read a DOM pointer. The descent is `box_tree_first_child` rather than
       `->first_child` for the case of an inline box whose first child is spliced; the ascent asks each
       position's BOX parent, which is the container itself for a position inside a chain of `contents`
       elements and is what makes the chain cost no step of its own. */
    if (bf_content_child_is_breaking_inline_box(n)) {
        BoxTreeChild into = box_tree_first_child(lxb_dom_interface_element(box_tree_child_node(n)));

        if (box_tree_child_exists(into)) return into;
    }
    for (;;) {
        lxb_dom_element_t *p = box_tree_parent(n);
        BoxTreeChild nx;

        DCHECK(p != NULL,
               "CSS 2.2 §9.2.1.1's content order was stepped out of the top of the tree without ever reaching "
               "the container it was asked about, so the position it was stepped from is not in that "
               "container's content at all");
        nx = box_tree_next_sibling(p, n);
        if (box_tree_child_exists(nx)) return nx;
        if (lxb_dom_interface_node(p) == root) return nx;
        n = box_tree_child_of_node(lxb_dom_interface_node(p));
    }
}

/* The next block-level box at or after `at`. `*inline_seen`, when asked, is whether the positions passed on
   the way hold an inline-level box: they are the run before the returned box. */
static lxb_dom_node_t *bf_block_box_at(lxb_dom_element_t *el, BoxTreeChild at, bool *inline_seen)
{
    BoxTreeChild n;

    if (inline_seen != NULL) *inline_seen = false;
    for (n = at; box_tree_child_exists(n); n = bf_content_next(el, n)) {
        lxb_dom_element_t *box = box_tree_parent(n);

        DCHECK(box != NULL,
               "CSS 2.2 §9.2.1.1's content order reached a node with no BOX parent, so §9.2's box generation "
               "cannot be asked about it — every position in a container's content is a child of the "
               "container's own spliced sequence or of one inside an inline box in it");
        switch (block_flow_child_kind(box, n)) {
        /* §9.2.1.1's "an in-flow block-level box", whether it is a child of the container or was reached
           through an inline box it breaks. Either way it is a box on §9.4.1's stack: the section says so of
           the second case in as many words ("becomes a sibling of those anonymous boxes"), and of the first by
           forcing the container "to have only block-level boxes inside it". */
        case BLOCK_FLOW_CHILD_BLOCK:
            return box_tree_child_node(n);
        case BLOCK_FLOW_CHILD_INLINE:
            if (inline_seen != NULL) *inline_seen = true;
            break;
        case BLOCK_FLOW_CHILD_NO_BOX:
        case BLOCK_FLOW_CHILD_FLOAT:
            break;
        /* css-lists-3 §3.5: an outside marker is "placed outside the principal block box", so it is not a box
           on §9.4.1's stack. */
        case BLOCK_FLOW_CHILD_OUTSIDE_MARKER:
            break;
        /* NO `default` ARM, DELIBERATELY: `-Wswitch` is the forcing function, and it is this enumeration's to
           hold rather than each consumer's now that the box list is produced in one place. A fifth
           `BlockFlowChildKind` is a fifth kind of box in every container's list, and a member this walk cannot
           classify is a member every reader would silently drop — so the day one is added the answer must be a
           COMPILE failure here and not a wrong maximum somewhere else. */
        }
    }
    return NULL;
}

/* WHERE ONE OF §9.2.1.1's RUNS BEGINS — see block_flow.h for why this pair is one entry rather than the four
   copies of `run.after->next` beside `run.after->parent` that its callers used to compose for themselves. */
BlockFlowRunStart block_flow_run_start(lxb_dom_element_t *el, BlockFlowRun run)
{
    BlockFlowRunStart out;

    DCHECK(el != NULL, "CSS 2.2 §9.2.1.1's run was asked where it begins with no container to begin inside");
    if (run.after == NULL) {
        /* A RUN THAT FOLLOWS NO BREAK BEGINS AT THE CONTAINER'S OWN CONTENT and continues no fragment, so the
           box whose style its content carries is the container itself. The first position is the container's
           BOX-TREE first child: css-display-3 §2.5 "Box Generation: the none and contents keywords" puts a
           `contents` first child's own children there instead of it, and `->first_child` would have named a
           node that generates nothing for the run to begin at. */
        out.open = el;
        out.at = box_tree_first_child(el);
        return out;
    }
    out.open = box_tree_parent(box_tree_child_of_node(run.after));
    DCHECK(out.open != NULL,
           "CSS 2.2 §9.2.1.1's run was started after a node with no BOX parent, so there is no box for the "
           "run to be a fragment OF and no style for its content to be measured with — the node a run follows is "
           "in the container's own spliced sequence or in one inside an inline box the break itself split, and "
           "either way it has a box whose opening edge belongs to an earlier one of these boxes");
    /* AND THE POSITION AFTER IT IS A POSITION IN THE BOX TREE RATHER THAN A DOM SIBLING, which is the whole of
       what this entry stopped its four callers composing for themselves: §2.5's splice puts the next position
       after a node inside a `contents` element OUTSIDE that element, and `run.after->next` is a position in no
       box's content there. */
    out.at = box_tree_next_sibling(out.open, box_tree_child_of_node(run.after));
    return out;
}

/* THE NEXT POSITION IN THE RUN AFTER `at`, INSIDE THE FRAGMENT `open` — see block_flow.h. */
BoxTreeChild block_flow_run_next(lxb_dom_element_t *open, BoxTreeChild at)
{
    DCHECK(open != NULL && box_tree_child_exists(at),
           "CSS 2.2 §9.2.1.1's run was stepped with no open fragment, or from no position");
    return box_tree_next_sibling(open, at);
}

/* DOES §9.2.1.1 GENERATE AN ANONYMOUS BLOCK BOX FOR THIS RUN? The section generates one only to wrap
   inline-level content — "we assume that there is an anonymous block box around 'Some text'" — so a run
   holding none is not a box at all, and `<div><p></p></div>` has ONE box on its stack and not three.
   IT IS ASKED OVER THE CONTAINER'S OWN CHILDREN PLUS TWO FRAGMENT TESTS, and that is exact rather than
   approximate. A bound that sits INSIDE some child means the run holds a FRAGMENT of that child's inline box,
   and §9.2.1.1 makes a fragment a box in as many words — "splitting the inline box into two boxes (EVEN IF
   EITHER SIDE IS EMPTY)" — so either bound being deep settles it. Once neither is, every position in the run
   is a child of the container, and the depth-0 scan sees every inline-level box in it: reaching a deeper one
   means passing through an inline box at depth 0, which is itself inline-level.
   BOTH CONSUMERS ASK IT, which is the whole reason it is exported: §9.4.1's placement must not put an empty
   box on its stack, and css-sizing-3 §5.2's maximum would not notice one — so a walk that skipped it there
   would be a second box list that happens to give the same answer, free to stop doing so. */
bool block_flow_run_generates_box(lxb_dom_element_t *el, BlockFlowRun run)
{
    BoxTreeChild c;

    DCHECK(el != NULL, "CSS 2.2 §9.2.1.1's box generation was asked about a run with no container");
    /* A BOUND IS DEEP WHEN ITS BOX PARENT IS NOT THE CONTAINER, AND THAT IS NOT THE SAME TEST AS ITS DOM
       PARENT NOT BEING IT — which is the defect `bf_element_child`'s own crash names as the reason the run
       type and these walks are one landing. This test asks whether the run is a FRAGMENT of an inline box
       §9.2.1.1 broke, and the section makes such a fragment a box "even if either side is empty". A bound
       inside a `contents` element is NOT that: css-display-3 §2.5 "Box Generation: the none and contents
       keywords" gives that element no box to be a fragment of, and the bound is an ordinary member of the
       container's own spliced sequence. Reading the DOM pointer would have answered TRUE for it and put a
       spurious anonymous block box on §9.4.1's stack for every run so bounded — a plausible box tree rather
       than a crash. */
    if (run.after != NULL && box_tree_parent(box_tree_child_of_node(run.after)) != el) return true;
    if (run.end != NULL && box_tree_parent(box_tree_child_of_node(run.end)) != el) return true;
    /* NEITHER BOUND IS DEEP BY THE TWO TESTS ABOVE, so the run begins at a CHILD of the container and the
       `open` half of this answer is the container itself — which is why the scan below reads only `at`. Asking
       for the pair rather than composing the first position here is what keeps this walk and the two that
       MEASURE the same run agreeing about where it starts. */
    c = block_flow_run_start(el, run).at;
    /* An outside marker is not inline-level content (css-lists-3 §3.5), so it alone generates no box here. */
    for (; box_tree_child_exists(c) && !box_tree_child_is_node(c, run.end); c = block_flow_run_next(el, c))
        if (block_flow_child_kind(el, c) == BLOCK_FLOW_CHILD_INLINE) return true;
    return false;
}

static lxb_dom_node_t *bf_next_block_box(lxb_dom_element_t *el, lxb_dom_node_t *after, bool *inline_seen)
{
    DCHECK(el != NULL, "CSS 2.2 §9.2.1.1's box list was enumerated with no container");
    if (after == NULL) return bf_block_box_at(el, box_tree_first_child(el), inline_seen);
    return bf_block_box_at(el, bf_content_next(el, box_tree_child_of_node(after)), inline_seen);
}

lxb_dom_node_t *block_flow_next_block_box(lxb_dom_element_t *el, lxb_dom_node_t *after)
{
    DCHECK(el != NULL, "CSS 2.2 §9.2.1.1's box list was enumerated with no container");
    /* STRICTLY AFTER, WHICH IS WHAT MAKES THE ENUMERATION A LOOP RATHER THAN A SEARCH THAT CAN STAND STILL: a
       consumer seeds each call with the box the last one returned, so the two answers cannot be the same node
       and the walk cannot revisit a position. A NULL `after` is the start of the content and not "no
       constraint" — the two would differ for a container whose FIRST content position is a block-level box. */
    return bf_next_block_box(el, after, NULL);
}

/* §9.4.1's OWN question about the box list, asked ONCE rather than per run: has the box whose position was
   requested turned out to be INLINE-LEVEL, and therefore inside one of §9.2.1.1's anonymous boxes rather than
   on the stack? The runs partition the container's content, so an inline-level CHILD is in exactly one of them
   and no search is needed to know that — which is what the per-run scan this replaces was really asking.
   A BLOCK-LEVEL `want` REACHED THROUGH AN INLINE BOX IS NOT THIS CASE AND MUST NOT BE REFUSED: §9.2.1.1 makes
   it "a sibling of those anonymous boxes", so it IS on the stack and the walk below reaches it as an ordinary
   member of the box list. That is a capability this question would have taken away had it been asked over
   ancestry instead of over the child's own level. */
/* §9.5's FLOAT, REFUSED FOR THE TWO QUANTITIES IT CHANGES RATHER THAN FOR ITS PRESENCE, and each arm names
   §9.4.1's OWN missing capability rather than another caller's — this is the line a reader of this crash can act on.
   THE BLANKET REFUSAL THIS REPLACES IS WRITTEN OUT RATHER THAN DELETED, because a reader will re-derive it from
   the word FLOAT and because another component was relying on it. It fired for every container holding a float
   CHILD, on the ground that "there is no arm on this stack that is right by default", and it was wrong on BOTH
   axes at once. TOO WIDE: CSS 2.2 §9.5 "Floats" states this stack's answer outright — "Since a float is not in
   the flow, non-positioned block boxes created before and after the float box flow vertically as if the float
   did not exist" — and CSS 2.2 §10.6.3 "Block-level non-replaced elements in normal flow when 'overflow'
   computes to 'visible'" says the same of the height this walk returns, "Only children in the normal flow are
   taken into account (i.e., floating boxes and absolutely positioned boxes are ignored, and relatively
   positioned boxes are considered without their offset)". `bf_content_is_inline` above quotes the first of those and
   ANSWERS for a container with a float; this refusal read the same sentence and refused anyway, so one file
   disagreed with itself about one operand. TOO NARROW: it asked about a CHILD LIST where both surviving
   quantities are scoped to a FORMATTING CONTEXT, which core/layout/line_box.c's own §9.5 residual had already
   measured — a float in a sibling subtree passed it at every step. That record named this refusal as what had
   been keeping most of its population away, so removing it ARMS line_box.c's context check rather than widening
   a gap, and that file's record is rewritten in the same diff.
   WHAT SURVIVES IS TWO SECTIONS, EACH ASKED OVER ITS OWN POPULATION.
     - §9.5.2 "Controlling flow next to floats: the 'clear' property": "Values other than 'none' potentially
       introduce clearance . Clearance inhibits margin collapsing and acts as spacing above the margin-top of an
       element", and each value requires the box's top border edge "be below the bottom outer edge" of the float.
       So a box on this stack with `clear` set both MOVES and stops being adjoining, and the distance is measured
       from a box nothing here can place. `clear` IS A COMPUTED VALUE core/css/css_computed_value.c derives — its
       `Computed value: as specified` row, with CSS 2.1 §9.5.2's `both` restored to the parser, which CSS Page
       Floats 3's grammar had dropped — so the PROPERTY is not what is missing and the PLACEMENT is.
     - §10.6.7 "'Auto' heights for block formatting context roots": "if the element has any floating descendants
       whose bottom margin edge is below the element's bottom content edge, then the height is increased to
       include those edges". Its own opening scopes it — "In certain cases (see, e.g., sections 10.6.4 and 10.6.6
       above), the height of an element that establishes a block formatting context is computed as follows" — so
       it is asked of `bf_establishes_block_formatting_context` and of an `auto` height, and therefore NOT of the
       ROOT ELEMENT, which that predicate deliberately excludes (see its banner) and which takes §10.6.3's
       float-ignoring walk instead.
   THE THIRD CONSEQUENCE IS NOT REFUSED HERE AND MUST NOT BE, and it is the one a reader will look for: §9.4.2
   shortens the LINE BOXES beside a float, which moves this container's §10.6.3 case 1 and its baseline. Both
   reach core/layout/line_box.c, which refuses on the CONTEXT holding a float rather than on a child list, so a
   check here would report THIS line for that file's gap.
   IT IS `#if APICLIENT_DEV` for line_box.c's reason at its own context check: the float search is a SUBTREE WALK
   and it exists only to feed an abort a release build does not have.
   THE `clear` ARM CARRIES A NAMED RESIDUAL. NOT COVERED: §9.5.2's own two narrowings — "The 'clear' property does
   not consider floats inside the element itself or in other block formatting contexts", and each of its values
   is stated over floats "that resulted from elements earlier in the source document" — so this refuses a box
   whose only candidate float is its own descendant or is later in source order, where a browser computes a
   clearance of zero and this walk's untouched answer is already right. NEXT DIFF: §9.5.2's clearance itself over
   §9.5.1's placement, which core/layout/flow_position.c's crash names and which needs the float's bottom outer
   edge. HOW ITS ABSENCE SHOWS: this abort firing for a container in which no float precedes, in source order and
   in the same formatting context, any box whose computed `clear` is set. */
/* It answers false ("not yet") when the float search held its place for a resting drive. */
static bool bf_require_float_does_not_reach_this_stack(lxb_dom_element_t *el, bool auto_h)
{
#if APICLIENT_DEV
    lxb_dom_element_t *fl;
    lxb_dom_node_t *b, *cleared = NULL;
    bool root_height;
    char nbuf[160], fbuf[160], ebuf[160], vbuf[64];

    /* THE TWO ANTECEDENTS ARE ASKED BEFORE THE FLOAT IS LOOKED FOR, AND THE ORDER IS THE COST RATHER THAN THE
       LOGIC — both conditions are conjunctions with the float, so either order refuses the same documents.
       core/layout/block_flow.h prices the float search as a WALK OF THE WHOLE FORMATTING CONTEXT whenever the
       answer is NULL, and `bf_layout` runs once per box, so paying it unconditionally here would be that walk
       per box: quadratic in the document, in every DEV build, to prove an absence on the documents that have no
       float. Both antecedents are cheap and both are FALSE for almost every container — `clear`'s initial value
       is `none` (CSS 2.2 §9.5.2's own `Initial:` line) and §9.4.1's list holds few boxes — so the walk is
       reached only where one of the two sections could actually apply. */
    root_height = auto_h && bf_establishes_block_formatting_context(el);
    for (b = block_flow_next_block_box(el, NULL); b != NULL; b = block_flow_next_block_box(el, b)) {
        /* §9.5.2's `Applies to:` line is "block-level elements", so §9.2.1.1's ANONYMOUS block box has no
           `clear` to read and is not one of these — which is why the walk is over the BOX LIST's element
           members and the anonymous runs are skipped rather than asked. */
        if (b->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (!css_computed_keyword_is(lxb_dom_interface_element(b), "clear", CSS_KEYWORD_NONE)) {
            cleared = b;
            break;
        }
    }
    if (!root_height && cleared == NULL) return true;
    if (!block_flow_context_first_float_ask(el, &fl)) return false;
    if (fl == NULL) return true;
    if (root_height)
        DFAILF("%s: CSS 2.2 §10.6.7 \"'Auto' heights for block formatting context roots\" increases this container's "
               "own height to reach a float it holds — \"if the element has any floating descendants whose bottom "
               "margin edge is below the element's bottom content edge, then the height is increased to include "
               "those edges\" — and %s is such a descendant in this container's OWN formatting context, which "
               "is the scope §10.6.7's next sentence states (\"Only floats that participate in this block "
               "formatting context "
               "are taken into account\"). The edge it wants is the float's BOTTOM MARGIN EDGE, which needs the "
               "float "
               "PLACED: BUILD §9.5.1 \"Positioning the float: the 'float' property\"' placement, which "
               "core/layout/flow_position.c crashes for and whose own ordering note says what that placement needs "
               "first. A box that does NOT establish a formatting context does not reach this line and must not — "
               "§10.6.3's walk ignores floats by its own sentence, so its answer is already right",
               box_subject(el, nbuf, sizeof nbuf), box_subject(fl, fbuf, sizeof fbuf));
    /* `cleared` IS NON-NULL HERE BY THE RETURN ABOVE, so this abort is UNCONDITIONAL and is written as one: a
       `DCHECKF(cleared == NULL, …)` at this line would be a condition no state of the program can satisfy,
       which is a NON-check with a check's syntax rather than a weak guard. */
    DFAILF("%s, computed `clear` `%s`, in %s: CSS 2.2 §9.5.2 \"Controlling flow next to floats: the 'clear' "
           "property\" puts this box BELOW a float on the same stack — each of its three values \"Requires "
           "that the top border edge of the box be below the bottom outer edge\" of an earlier float — and "
           "%s is a float in this box's own formatting context. §9.5.2's next sentences are what make this "
           "§9.4.1's problem and not only this box's: \"Values other than 'none' potentially introduce "
           "clearance . Clearance inhibits margin collapsing and acts as spacing above the margin-top of an "
           "element\", and CSS 2.1 §8.3.1 \"Collapsing margins\" agrees from the other end by excepting a "
           "margin with clearance from its adjoining test — so one such box changes every collapse and every "
           "offset below it on this stack. The clearance is measured from the float's BOTTOM OUTER EDGE: "
           "BUILD §9.5.1 \"Positioning the float: the 'float' property\"' placement, which "
           "core/layout/flow_position.c crashes for. `clear` itself is RECORDED "
           "(a KEYWORD row of core/css/css_property_id.h and css_shorthand_complete_for, with CSS 2.1 "
           "§9.5.2's `both` restored to lexbor's parser), so the property is not the blocker",
           box_subject_node(cleared, nbuf, sizeof nbuf),
           box_subject_computed(cleared == NULL ? NULL : lxb_dom_interface_element(cleared),
                                "clear", vbuf, sizeof vbuf),
           box_subject(el, ebuf, sizeof ebuf), box_subject(fl, fbuf, sizeof fbuf));
#else
    (void) el;
    (void) auto_h;
#endif
    return true;
}

static void bf_require_want_on_the_stack(lxb_dom_element_t *el, lxb_dom_element_t *want)
{
    lxb_dom_node_t *n;

    if (want == NULL) return;
    n = lxb_dom_interface_node(want);
    /* THE GUARD IS THE CLASSIFICATION'S OWN PRECONDITION AND IS ASKED OVER THE BOX TREE, because
       `block_flow_child_kind` refuses a pair whose box relation is not the one it is stated over — and
       css-display-3 §2.5's splice makes a box-tree child of `el` need not be a DOM child of it, so the DOM
       equality would have skipped exactly the spliced boxes this refusal is about. */
    if (box_tree_parent(box_tree_child_of_node(n)) != el) return;
    if (block_flow_child_kind(el, box_tree_child_of_node(n)) != BLOCK_FLOW_CHILD_INLINE) return;
    DFAIL("CSS 2 §9.4.1's vertical placement was asked for a box CSS 2.2 §9.2.1.1 puts INSIDE an "
          "anonymous block box: it is an inline-level child of a block container that also holds a "
          "block-level box, so its position is a position ALONG a line box in that anonymous box's "
          "inline formatting context and not a distance down §9.4.1's stack. core/layout/"
          "flow_position.c takes an inline-level box out through its own section before asking — the "
          "same discrepancy the all-inline container asserts one branch up — so the two "
          "classifications have come apart. It is decided HERE rather than left to the walk's "
          "never-reached-this-box crash, because this box IS in the container's box tree and merely "
          "not on its stack, which is a different fact and a different fix");
}

/* ONE ANONYMOUS BLOCK BOX over one of §9.2.1.1's runs of `parent`'s CONTENT, as §9.4.1's stack sees it.
   §10.6.3's FIRST BULLET is its height — it establishes an inline formatting context, by construction the only
   thing it can contain — and §8.3.1's fourth adjoining pair is the rest of its contribution. */
static BfBox bf_anon_box(lxb_dom_element_t *parent, BlockFlowRun run, BfBaseline pass)
{
    bool any_line_box = false;
    CssDisplay d = css_computed_display(parent);
    bool container = block_flow_display_is_block_container(d);
    BfBox out;
    CssPx first_baseline = css_px(0.0), last_baseline = css_px(0.0);
    CssPx h;

    DCHECK(container,
           "CSS 2.2 §9.2.1.1 generates an anonymous block box inside a BLOCK CONTAINER BOX — its whole "
           "sentence is \"if a BLOCK CONTAINER BOX … has a block-level box inside it\" — and this box's parent "
           "is not one. There is then no §9.4.1 stack for the anonymous box to be a sibling on, and no "
           "enclosing non-anonymous box for it to inherit the font and `line-height` its line boxes are "
           "measured with");
    DCHECK(block_flow_run_generates_box(parent, run),
           "CSS 2.2 §9.2.1.1's anonymous block box was generated around NOTHING. The section generates one "
           "only to wrap inline-level content, so an empty one is a box with no reason to exist whose height "
           "§9.4.1's stack would nevertheless add to its container's — and the predicate that decides it is "
           "the one the enumeration exports, so a box here that it refuses is the walk and the generation "
           "having come apart");
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
    /* §9.2.1.1's ANONYMOUS BLOCK BOX IS THE CASE WHERE `style` ANSWERS BOTH QUESTIONS, which is why the
       width is left to be derived rather than stated: "non-inherited properties have their initial value", so
       CSS 2.1 §10.3.3's constraint equation with six zero terms makes this box's width its containing block's,
       which is `parent`'s content width — the very number `line_box_available_width_derived` reads. */
    h = line_box_content_height(parent, run, line_box_available_width_derived(),
                                &any_line_box, &first_baseline, &last_baseline);
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

/* ---- CSS 2.1 §10.3.7's and §10.6.4's STATIC POSITION, A READING OF §9.4.1's RUNNING POSITION --------------
   Both sections define it as the top (or left) margin edge "of a hypothetical box that would have been the
   first box of the element if its specified `position` value had been `static` and its specified `float` had
   been `none`", and §10.6.3 keeps the real box out of the stack that would place it ("absolutely positioned
   boxes are ignored"). The hypothetical box is never placed: the stack records its own state where each of
   §9.2.1.1's runs starts (flow_placement.h's FlowRunStart, keyed by the block-level box the run follows), and
   `block_flow_static_child_top_ask` reads the run its element falls in.
   The hypothetical box's `margin-top` is an operand its caller resolves by §10.6.3's own sentence ("if
   `margin-top`, or `margin-bottom` are `auto`, their used value is 0"): reading it through `used_value_px`
   would send an absolutely positioned box into §10.6.4's constraint equation, the section that asked. */

/* §8.3.1's collapse applied to the hypothetical box, then CSS 2 §8.1's own definition of a margin edge.
   THE BORDER EDGE IS WHERE §9.4.1's STACK WOULD REACH IT: the open run merged with the box's own top margin,
   applied to the running position — or, when that run is still ESCAPING through this container's top edge,
   §8.3.1's note's own answer, "the top border edge of the box is defined to be the same as the parent's".
   THE MARGIN EDGE IS THAT LESS THE MARGIN, WHICH IS §8.1 AND NOT A SECOND COLLAPSING RULE. §8.1 nests the
   margin edge outside the border edge by the margin whatever the margin collapsed with, so a box whose top
   margin escaped its container has a margin edge ABOVE that container's content edge — a negative distance,
   which is the sign CSS 2.1 §10.6.4 states outright ("the value is negative if the hypothetical box is above
   the containing block"). §10.3.7 states the inline-axis half of the same thing in its own words, "to the
   left of the containing block" rather than "above" it, which is the one place the two sections' wording
   differs and is why this entry quotes the one it answers for.
   AND THE ROUND TRIP IS WHY THE MARGIN EDGE IS THE RIGHT THING TO REPORT rather than the border edge: §10.3.7
   and §10.6.4 both set the offset TO this number and then place the box at `offset + margin` through their
   own constraint equation, so the margin cancels and the box lands exactly where the hypothetical one would
   have. Reporting the border edge would place it one margin too low in every case a margin is declared. */
static CssPx bf_static_margin_edge(CssPx pos, BfRun run, CssPx margin, bool escaping)
{
    CssPx border = escaping ? pos : css_px_add(pos, bf_run_value(bf_run_merge(run, bf_run_of(margin))));

    return css_px_sub(border, margin);
}

/* CSS 2.1 §17.4 "Tables in the visual formatting model"'s table wrapper box's children, the list §9.4.1's
   stack walks for it: "the table wrapper box … contains the table box itself and any caption boxes (in document
   order)", with §17.4.1 "Caption position and alignment"'s `caption-side` putting a caption "above the table
   box" (`top`) or "below the table box" (`bottom`). The table box is the NULL entry. The caller frees. */
static size_t bf_wrapper_boxes(lxb_dom_element_t *table, lxb_dom_element_t ***out)
{
    lxb_dom_element_t **caps = NULL, **v;
    size_t n = table_box_captions(table, &caps), i, k = 0;

    v = malloc((n + 1) * sizeof *v);
    CHECK(v != NULL, "out of memory listing one table wrapper box's children");
    for (i = 0; i < n; i++)
        if (css_computed_keyword_is(caps[i], "caption-side", CSS_KEYWORD_TOP)) v[k++] = caps[i];
    v[k++] = NULL;
    for (i = 0; i < n; i++)
        if (css_computed_keyword_is(caps[i], "caption-side", CSS_KEYWORD_BOTTOM)) v[k++] = caps[i];
    free(caps);
    *out = v;
    return k;
}

/* The table box's contribution to its wrapper's stack: §17.4 gives it the initial `margin-*` ("the computed
   values of properties … 'margin-*' … on the table element are used on the table wrapper box and not the table
   box"), so its runs are zero margins that still collapse with an adjoining caption's, and its height is its
   border edge (CSS 2.1 §17.5.3 "Table height algorithms" through core/layout/used_value.h). */
static BfBox bf_table_box(lxb_dom_element_t *table)
{
    BfBox b;

    b.top = bf_run_of(css_px(0.0));
    b.bottom = bf_run_of(css_px(0.0));
    b.collapse_through = false;
    b.content_h = used_value_border_edge_px(table, true);
    b.border_h = b.content_h;
    b.baseline = css_px(0.0);
    b.has_line_box = false;
    b.is_table_wrapper = false;
    return b;
}

/* The stack walk's loop state: the enumeration's place (`prev`, `brk`, `at_run`, and `wi` on a wrapper's
   stack), the running position, the open run, and the run escaping through the top edge. `run_inline` is
   whether the enumeration passed an inline-level box between `prev` and `brk`. `ifc` is the walk
   having taken §9.4.2's branch. */
typedef struct {
    lxb_dom_node_t *prev, *brk;
    size_t wi;
    bool   ifc, at_run, run_inline, escaping, placed;
    CssPx  pos;
    BfRun  run, esc_run;
} BfWalk;

/* Stage `w` and the baseline pair on `body`'s entry (layout_job.h's LayoutBfCursor), so the body's next run
   continues at the step `w` names. Answers false, the walk's "not yet". */
static bool bf_walk_stage(size_t body, lxb_dom_element_t *el, const BfWalk *w, const BfBox *out)
{
    LayoutBfCursor c;

    c.prev = w->prev;
    c.brk = w->brk;
    c.wi = w->wi;
    c.ifc = w->ifc;
    c.at_run = w->at_run;
    c.run_inline = w->run_inline;
    c.escaping = w->escaping;
    c.placed = w->placed;
    c.pos = w->pos;
    c.run_pos = w->run.pos;
    c.run_neg = w->run.neg;
    c.esc_pos = w->esc_run.pos;
    c.esc_neg = w->esc_run.neg;
    c.baseline = out->baseline;
    c.has_line_box = out->has_line_box;
    layout_job_bf_stage(body, el, &c);
    return false;
}

/* Whether `body` staged a walk on its last run; when it did, `w` and the baseline pair are that walk's. */
static bool bf_walk_resume(size_t body, lxb_dom_element_t *el, BfWalk *w, BfBox *out)
{
    LayoutBfCursor c;

    if (!layout_job_bf_resume(body, el, &c)) return false;
    w->prev = c.prev;
    w->brk = c.brk;
    w->wi = c.wi;
    w->ifc = c.ifc;
    w->at_run = c.at_run;
    w->run_inline = c.run_inline;
    w->escaping = c.escaping;
    w->placed = c.placed;
    w->pos = c.pos;
    w->run.pos = c.run_pos;
    w->run.neg = c.run_neg;
    w->esc_run.pos = c.esc_pos;
    w->esc_run.neg = c.esc_neg;
    out->baseline = c.baseline;
    out->has_line_box = c.has_line_box;
    return true;
}

/* §9.4.1's placement rule over §8.3.1's runs, for the in-flow children of one block container: the body of
   the LAYOUT_Q_BF_WALK question at `body`, which it stages on "not yet" and continues on its next run. It
   answers the box's own reduction and writes what its other readers take from it into flow_placement.h's
   record as it passes them: every child's top, where each run starts, the table box's top on a wrapper's
   stack and, for the `BF_BASELINE_NONE` pass, the anonymous block boxes.
   `pass` ASKS FOR CSS 2.2 §10.8.1's READING OF THAT SAME POSITION, reported on `BfBox`. It is a request and
   not a mode: what it decides is whether a `has_line_box` of false is a measurement or the initialisation,
   and it is a flag because answering it walks into a child whose own `height` already decided its size
   (`bf_height_needs_content`), which a height walk must not pay for. */
static bool bf_layout(lxb_dom_element_t *el, BfBaseline pass, size_t body, BfBox *result)
{
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
    BfWalk w;
    bool resumed;
    /* CSS 2.1 §17.4's wrapper box walks its own children, not the table element's (`bf_wrapper_boxes`). */
    bool wrapper;
    lxb_dom_element_t **wl = NULL;
    size_t wn = 0;
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
    {
        CssDisplay d = css_computed_display(el);

        wrapper = table_wrapper_generates(d);
    }
    w.prev = NULL;
    w.brk = NULL;
    w.wi = 0;
    w.ifc = false;
    w.at_run = true;
    w.run_inline = false;
    w.escaping = esc_top;
    w.placed = false;
    w.pos = css_px(0.0);
    w.run = bf_run_empty();
    w.esc_run = bf_run_empty();
    resumed = bf_walk_resume(body, el, &w, &out);
    if (wrapper) wn = bf_wrapper_boxes(el, &wl);
    /* A resumed walk took its branch on its first run. */
    if (!resumed) w.ifc = !wrapper && bf_content_is_inline(el);
    if (w.ifc) {
        /* §9.4.2's INLINE FORMATTING CONTEXT. Nothing below this branch applies to it: §8.3.1's adjoining
           margins are stated over boxes that "both belong to in-flow BLOCK-LEVEL boxes participating in the
           same block formatting context", and there is no such box here — an inline box's vertical margins are
           not adjoining margins at all, so no run enters or leaves through this container's edges. */
        bool any_line_box = false;
        CssPx first_baseline = css_px(0.0), last_baseline = css_px(0.0);
        CssPx h;

        /* The one run here starts at the top content edge with nothing met, which is where §10.3.7's reader
           puts a hypothetical block-level box that no box-generating content precedes (it refuses the case where
           some does: §9.2.1.1 would split this context around it). */
        {
            FlowRunStart start = { css_px(0.0), css_px(0.0), css_px(0.0), esc_top };

            flow_placement_run_record(el, NULL, start);
        }
        /* This container holds no block-level box, so §9.2.1.1 generates nothing and the one inline formatting
           context here is over the WHOLE child list — the same run form the anonymous box is measured through,
           with the container's own element supplying the style it already owns. */
        /* §10.8.1's MAIN ARM FOR THE SHAPE THAT HAS AN ELEMENT TO NAME IT. This box establishes the one inline
           formatting context, so "its last line box in the normal flow" is the last EXISTING line box of this
           very reduction and the distance is already measured from this box's top content edge — the frame
           `BfBox` states. It is taken from the same call that answers the height, never a second one. */
        {
            BlockFlowRun whole;

            /* §9.4.2's context with an ELEMENT to name it is the WHOLE of this container's content, which is
               the run with no break on either side of it — the same shape §9.2.1.1's enumeration yields for a
               container that holds no block-level box, stated here rather than reconstructed. */
            whole.after = NULL;
            whole.end = NULL;
            /* THE BOX HAS AN ELEMENT AND ITS OWN CONTENT BOX IS THE CONTAINING BLOCK OF ITS LINES, so
               §9.4.2's width is `el`'s and is left to be derived — which also keeps it unread for a run with
               no break position in it, the skip line_box.h states as load-bearing. */
            if (!line_box_atomics_ask(el, whole, line_box_available_width_derived()) ||
                !line_box_extents_ask(el, whole, line_box_available_width_derived()))
                return bf_walk_stage(body, el, &w, &out);   /* "not yet": deferred, or the fill rested */
            h = line_box_content_height(el, whole, line_box_available_width_derived(),
                                        &any_line_box, &first_baseline, &last_baseline);
        }
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
            *result = out;
            return true;
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
        *result = out;
        return true;
    }

    /* §9.4.1's stack is over the BOX LIST CSS 2.2 §9.2.1.1 forces this container to have, and that list is
       NOT a partition of its child nodes: its first paragraph makes each maximal run of inline-level content
       one ANONYMOUS BLOCK BOX, and its second breaks an inline box around an in-flow block-level box inside it,
       so ONE child can yield THREE boxes — "a block box representing the BODY, containing an anonymous block
       box around C1, the SPAN block box, and another anonymous block box around C2". The enumeration is
       core/layout/block_flow.h's own, in CONTENT order, and this walk consumes it rather than re-deriving it.
       `ce` is the element the box belongs to and is NULL for the anonymous one, which is the whole of the
       difference the placement below has to know about — everything else it reads is on `BfBox`.
       TWO BOXES COME OUT OF EACH TURN OF THE ENUMERATION and the body places ONE, so `anon` is which of the
       two this iteration is: the run BEFORE the break, then the break's own box. A run that §9.2.1.1 generates
       no box for is skipped rather than placed, which is that section's sentence and not an optimisation — an
       empty anonymous box would take a height off the stack and be reported to every consumer of the
       enumeration. */
    if (!resumed && !wrapper) w.brk = bf_next_block_box(el, NULL, &w.run_inline);
    for (;;) {
        lxb_dom_element_t *ce = NULL;
        BlockFlowRun anon_run;
        BfBox b;

        anon_run.after = w.prev;
        anon_run.end = w.brk;
        if (wrapper) {
            if (w.wi == wn) break;
            ce = wl[w.wi];
            if (ce == NULL) {
                b = bf_table_box(el);
            } else if (!bf_box(ce, pass, &b)) {
                free(wl);
                return bf_walk_stage(body, el, &w, &out);   /* "not yet": a question beneath was deferred */
            }
            w.wi++;
        } else if (w.at_run) {
            /* Where this run starts, for §10.3.7's and §10.6.4's static position, recorded before the
               generates-a-box escape below: a run that generates no box is the commonest position an
               out-of-flow child sits at (two block-level siblings with white space between them). */
            {
                FlowRunStart start = { w.pos, w.run.pos, w.run.neg, w.escaping };

                flow_placement_run_record(el, w.prev != NULL ? lxb_dom_interface_element(w.prev) : NULL, start);
            }
            if (!block_flow_run_generates_box(el, anon_run)) {
                /* §9.2.1.1 wraps every run of inline-level content in an anonymous block box, and the
                   enumeration that delimited this run classified each position it passed. */
#if APICLIENT_DEV
                {
                    char pbuf[160], bbuf[160];

                    DCHECKF(!w.run_inline,
                            "%s: the box-list enumeration passed an inline-level box in the run after %s and "
                            "before %s, and block_flow_run_generates_box answered that the run generates no "
                            "anonymous block box; the two classify the same positions with the same per-child "
                            "predicate", box_subject(el, nbuf, sizeof nbuf),
                            w.prev != NULL ? box_subject_node(w.prev, pbuf, sizeof pbuf) : "the content's start",
                            w.brk != NULL ? box_subject_node(w.brk, bbuf, sizeof bbuf) : "the content's end");
                }
#endif
                w.at_run = false;
                continue;
            }
            /* The anonymous box is one of a resting drive's boxes; a body's walk that has spent them stops. */
            if (!layout_job_bf_step(body)) return bf_walk_stage(body, el, &w, &out);
            if (!line_box_atomics_ask(el, anon_run, line_box_available_width_derived()) ||
                !line_box_extents_ask(el, anon_run, line_box_available_width_derived()))
                return bf_walk_stage(body, el, &w, &out);   /* "not yet": deferred, or the fill rested */
            b = bf_anon_box(el, anon_run, pass);
            w.at_run = false;
        } else if (w.brk != NULL) {
            ce = lxb_dom_interface_element(w.brk);
            if (!bf_box(ce, pass, &b))
                return bf_walk_stage(body, el, &w, &out);   /* "not yet": a question beneath was deferred */
            w.prev = w.brk;
            w.brk = bf_next_block_box(el, w.prev, &w.run_inline);
            w.at_run = true;
        } else {
            break;
        }
        /* §8.3.1's second adjoining pair — "bottom margin of box and top margin of its next in-flow following
           sibling" — and its first, "top margin of a box and top margin of its first in-flow child": both are
           the run already open meeting this child's top run, and they are the same merge. */
        w.run = bf_run_merge(w.run, b.top);
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
            CssPx through_top = w.escaping ? w.pos : css_px_add(w.pos, bf_run_value(w.run));

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
            /* The position goes to the record for every box the stack places, with no test of who will ask.
               An entry is keyed by a box on this container's CSS 2.2 §9.2.1.1 box list — which includes a
               block-level box inside a broken inline box, "a sibling of those anonymous boxes" — and §10.1's
               second case makes this container that box's containing block, so the frame the position is
               measured in is the frame every reader takes it in. */
            DCHECK(ce == NULL || bf_on_box_list_of(ce, el),
                   "CSS 2 §9.4.1's stack placed a box that is NOT on CSS 2.2 §9.2.1.1's box list for the "
                   "container being walked, so the position about to be recorded is a distance down a stack "
                   "this box is not on — and every reader of that entry would take it as a distance down the "
                   "stack of its own containing block. The enumeration and this membership test read the "
                   "SAME two sections over the SAME two elements, so they cannot disagree unless one of them "
                   "has been changed without the other");
            if (ce != NULL) flow_placement_record(ce, through_top);
            DCHECK(!(wrapper && ce == NULL),
                   "a table box collapsed through on its wrapper's stack; bf_table_box gives it no such run");
            /* §9.2.1.1's box reaches here when every line box in it is one §9.4.2 says "must be treated as NOT
               EXISTING for any other purpose", and the note above is what gives it a top border edge anyway.
               It is still a BOX — zero-height, at that edge — so it is reported like every other one. */
            if (ce == NULL && pass == BF_BASELINE_NONE) bf_anon_record(el, anon_run, through_top, b.border_h);
            w.run = bf_run_merge(w.run, b.bottom);
            continue;
        }
        if (w.escaping) {
            /* The run reached this box's top edge with nothing separating it, so it collapses with this box's
               OWN top margin and leaves. §8.3.1's collapse-through note states the consequence directly: "the
               top border edge of the box is defined to be the same as the parent's" — inside this box the child
               therefore begins at the top content edge and the margin contributes nothing. */
            w.esc_run = w.run;
            w.escaping = false;
        } else {
            w.pos = css_px_add(w.pos, bf_run_value(w.run));
        }
        /* §9.2.1.1's anonymous block box is reported at the same running position as its block-level siblings,
           which keeps them from disagreeing about where a margin collapsed. It has no border and no padding
           (§9.2.1.1's initial values), so this top border edge is also its top content and margin edge. */
        /* …AND THE SAME REPORT FOR A BOX THAT DID NOT COLLAPSE THROUGH — the collapse-through arm above
           states the reason and it is one reason, not two, and so does the membership assert. */
        DCHECK(ce == NULL || bf_on_box_list_of(ce, el),
               "CSS 2 §9.4.1's stack placed a box that is NOT on CSS 2.2 §9.2.1.1's box list for the "
               "container being walked — the identical assert on the collapse-through arm above states why "
               "the enumeration and this test cannot disagree");
        if (ce != NULL) flow_placement_record(ce, w.pos);
        /* On a wrapper's stack the NULL entry is the table box, the table element's other box. */
        if (wrapper && ce == NULL) flow_placement_table_box_record(el, w.pos);
        if (ce == NULL && !wrapper && pass == BF_BASELINE_NONE) bf_anon_record(el, anon_run, w.pos, b.border_h);
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

            out.baseline = css_px_add(css_px_add(w.pos, inner), b.baseline);
            out.has_line_box = true;
        }
        w.pos = css_px_add(w.pos, b.border_h);
        w.run = b.bottom;
        w.placed = true;
    }
    free(wl);
    /* §9.5.2's and §10.6.7's float refusals, asked once the stack is laid out so that a search a resting drive
       stops is staged at the loop's end and resumed there. `auto_h` is passed rather than re-read: a second
       read of §3.2.1's behaves-as-auto would be a second answer to one question. A wrapper's members are the
       table box and captions, each a formatting context root, so no float is on its stack. */
    if (!wrapper && !bf_require_float_does_not_reach_this_stack(el, auto_h)) return bf_walk_stage(body, el, &w, &out);

    if (w.placed) {
        if (esc_top) out.top = bf_run_merge(out.top, w.esc_run);
        if (esc_bottom) {
            /* §10.6.3's THIRD bullet: the height ends at "the bottom border edge of the last in-flow child
               whose top margin doesn't collapse with the element's bottom margin", because that child's bottom
               margin left through this edge. */
            out.bottom = bf_run_merge(out.bottom, w.run);
        } else {
            /* §10.6.3's SECOND bullet, and §10.6.7's rule for a formatting-context root: "the bottom edge of
               the bottom (possibly collapsed) margin of its last in-flow child". */
            w.pos = css_px_add(w.pos, bf_run_value(w.run));
        }
        /* §10.6.3's distance can be NEGATIVE — a last in-flow child with a negative bottom margin whose run
           does not escape is exactly that — and a box's CONTENT HEIGHT cannot be, which is css-sizing-3 §3.1
           "Sizing Properties"' own sentence: "the inner size is always floored at zero", an "inner size"
           being "The content-box size of a box" by css-sizing-3 §2 "Terminology". §3.1 IS THE SECTION AND
           §3.3 IS NOT, which is the correction core/layout/used_value.h argues out: §3.3's floor is a step
           of the `border-box` conversion and this walk performs no conversion at all. So this floor belongs
           to the walk and holds with no limit declared at all. CSS 2.1 §10.7's CLAMP is a SEPARATE pass over
           the extent this walk returns and is core/layout/used_value.c's — it re-runs the rules with
           `min-height`/`max-height` substituted, and it runs on this value at every entry that consumes it.
           Calling this floor "§10.7's min-height running early" was true of the number and wrong about which
           section owns it, and it made the real clamp look already done. */
        out.content_h = css_px_max(w.pos, css_px(0.0));
        *result = out;
        return true;
    }
    /* Nothing was placed: either there were no in-flow children at all — §10.6.3's FOURTH bullet, "zero,
       otherwise" — or every one of them collapsed through, in which case `run` holds their whole merged run and
       §8.3.1's own note applies: "a box's own margins collapse if … all of its in-flow children's margins (if
       any) collapse". */
    if (through_ok) {
        out.collapse_through = true;
        out.top = bf_run_merge(bf_run_merge(out.top, out.bottom), w.run);
        out.bottom = out.top;
        out.content_h = css_px(0.0);
        *result = out;
        return true;
    }
    if (esc_top)         out.top = bf_run_merge(out.top, w.run);
    else if (esc_bottom) out.bottom = bf_run_merge(out.bottom, w.run);
    else                 w.pos = bf_run_value(w.run);
    out.content_h = css_px_max(w.pos, css_px(0.0));
    *result = out;
    return true;
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
/* It answers false ("not yet") when a walk it ran answered false; `*result` is then unwritten. */
static bool bf_walk(lxb_dom_element_t *el, BfBaseline pass, BfBox *out);

static bool bf_box_compute(lxb_dom_element_t *el, BfBaseline pass, BfBox *result)
{
    CssDisplay d = css_computed_display(el);
    bool container = block_flow_display_is_block_container(d);
    bool grid = css_display_spells(d, CSS_KEYWORD_GRID) || css_display_spells(d, CSS_KEYWORD_INLINE_GRID);
    /* §17.4's wrapper is decided from the same string and BEFORE it is released, for the reason the two arms
       below are written apart: a table is block-LEVEL and is not a block CONTAINER (§9.2.1), so `container`
       cannot answer for it and the arm that handles a non-container is about flex and grid. */
    bool wrapper = table_wrapper_generates(d);
    char nbuf[160];
    BfBox b;

    b.content_h = css_px(0.0);
    b.top = bf_run_of(used_value_px(el, "margin-top"));
    b.bottom = bf_run_of(used_value_px(el, "margin-bottom"));
    b.collapse_through = false;
    b.baseline = css_px(0.0);
    b.has_line_box = false;
    b.is_table_wrapper = wrapper;
    /* CSS 2.1 §17.4 "Tables in the visual formatting model"'s table wrapper box, which §9.4.1's stack placed and
       which is not the table box. §17.4 uses the table element's `margin-*` on the wrapper (read above) and its
       border, padding and `height` on the table box, so the wrapper's border box is its content box, whose
       height is §10.6.3's over its own children: the captions and the table box (`bf_wrapper_boxes`). It reports
       no line box: §10.8.1's "last line box in the normal flow" does not reach into the captions' or the cells'
       own formatting contexts, and `is_table_wrapper` lets CSS 2.1 §17.5.3's cell-baseline reader name what it
       needs instead. */
    if (wrapper) {
        BfBox w;

        if (!bf_walk(el, BF_BASELINE_NONE, &w)) return false;
        b.content_h = w.content_h;
        b.border_h = w.content_h;
        *result = b;
        return true;
    }
    /* Not a block container: a flex or grid container, whose height is its own spec's and which establishes an
       independent formatting context, so its margins are its own and nothing inside it is this walk's. Its
       baseline is its own module's too, and it can be the last box on the stack holding one, so it is never
       skipped or substituted. A grid or flex container gives its first baseline (css-grid-1 §10.6 "Grid
       Container Baselines", css-flexbox-1 §8.5 "Flex Container Baselines") to either pass: Chrome 155 does,
       putting an `inline-block`'s baseline on the container's first row or line whatever follows it, where the
       last set would take the last. */
    if (!container) {
        DCHECKF(grid || flex_item_display_is_flex_container(d),
                "%s is neither a block container nor a grid or flex container, and reached the arm for those two",
                box_subject(el, nbuf, sizeof nbuf));
        if (pass != BF_BASELINE_NONE && grid) {
            if (!grid_container_first_baseline_ask(el, &b.has_line_box, &b.baseline)) return false;
        } else if (pass != BF_BASELINE_NONE) {
            if (!flex_container_first_baseline_ask(el, &b.has_line_box, &b.baseline)) return false;
        }
        b.border_h = used_value_border_edge_px(el, true);
        *result = b;
        return true;
    }
    if (!bf_height_needs_content(el)) {
        if (pass == BF_BASELINE_NONE) {
            b.border_h = used_value_border_edge_px(el, true);
            *result = b;
            return true;
        }
        /* The SIZE is settled and the LINE BOXES are not, so the walk runs for the baseline alone: everything
           else it answers is about a height this box does not take from its content, and taking any of it would
           be §10.6.3's rule running for a box §10.6.2 already sized. */
        b.border_h = used_value_border_edge_px(el, true);
        {
            BfBox inner;

            if (!bf_walk(el, pass, &inner)) return false;
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
        *result = b;
        return true;
    }
    if (!bf_walk(el, pass, &b)) return false;
    if (b.collapse_through) { *result = b; return true; }
    /* A box whose height BEHAVES AS AUTO (css-sizing-3 §3.2.1) is the one whose border-box height is this
       walk's own content height; a resolved percentage is a declared length like any other and goes back
       through used_value.h, which is where §10.4/§10.7's clamp runs for it. */
    if (used_value_height_behaves_as_auto(el)) {
        b.border_h = used_value_border_edge_from_content_px(el, b.content_h, true);
    } else {
        b.border_h = used_value_border_edge_px(el, true);
    }
    *result = b;
    return true;
}

/* A contribution as the layout job memoizes it (core/layout/layout_job.h's LayoutBfAnswer): BfBox with its
   two runs held as their extrema, which is the whole of what §8.3.1's reduction carries. */
static void bf_box_pack(const BfBox *b, LayoutBfAnswer *out)
{
    out->content_h = b->content_h;
    out->border_h = b->border_h;
    out->top_pos = b->top.pos;
    out->top_neg = b->top.neg;
    out->bottom_pos = b->bottom.pos;
    out->bottom_neg = b->bottom.neg;
    out->baseline = b->baseline;
    out->collapse_through = b->collapse_through;
    out->is_table_wrapper = b->is_table_wrapper;
    out->has_line_box = b->has_line_box;
}

static BfBox bf_box_unpack(const LayoutBfAnswer *a)
{
    BfBox b;

    b.content_h = a->content_h;
    b.border_h = a->border_h;
    b.top.pos = a->top_pos;
    b.top.neg = a->top_neg;
    b.bottom.pos = a->bottom_pos;
    b.bottom.neg = a->bottom_neg;
    b.baseline = a->baseline;
    b.collapse_through = a->collapse_through;
    b.is_table_wrapper = a->is_table_wrapper;
    b.has_line_box = a->has_line_box;
    return b;
}

/* A question's body: a contribution's (`bf_box_compute`) or a stack's (`bf_layout`, at its own entry). On
   "not yet" the job suspends it; otherwise the answer is memoized. The body is a thread-work span, counted as
   one question when it answers. */
static bool bf_question_run(LayoutQuestion q, size_t at, BfBox *out)
{
    BfBaseline pass = (BfBaseline)(q.code - 1u);
    LayoutBfAnswer a;
    ThreadWorkSpan tw;
    bool done;

    thread_work_begin(&tw);
    if (q.kind == LAYOUT_Q_BF_WALK) {
        done = bf_layout(q.element, pass, at, out);
        if (done && pass == BF_BASELINE_NONE) flow_placement_anon_close(q.element);
    } else {
        DCHECK(q.kind == LAYOUT_Q_BF_BOX, "a layout job handed block_flow.c a question of another kind");
        done = bf_box_compute(q.element, pass, out);
    }
    if (!done) {
        thread_work_end(&tw, THREAD_WORK_LAYOUT_BOX, 0);
        layout_job_suspend(at);
        return false;
    }
    thread_work_end(&tw, THREAD_WORK_LAYOUT_BOX, 1);
    bf_box_pack(out, &a);
    layout_job_bf_answer(at, &a);
    return true;
}

/* Ask `q` of the job: served from the memo, deferred ("not yet", `*out` unwritten), or run here. */
static bool bf_question_ask(LayoutQuestion q, BfBox *out)
{
    LayoutBfAnswer a;
    size_t at = 0;

    switch (layout_job_bf_ask(q, &a, &at)) {
    case LAYOUT_ASK_ANSWERED:
        *out = bf_box_unpack(&a);
        return true;
    case LAYOUT_ASK_DEFERRED:
        return false;
    case LAYOUT_ASK_OPENED:
        break;
    }
    return bf_question_run(q, at, out);
}

/* §9.4.1's contribution of one box, asked through the layout job: the question is (element, pass) and the
   job's memo serves every pass. It answers false ("not yet") when the job deferred this question or one its
   body asked; `*out` is then unwritten and the caller propagates. Its one caller is the stack walk. */
static bool bf_box(lxb_dom_element_t *el, BfBaseline pass, BfBox *out)
{
    LayoutQuestion q;

    DCHECK(el != NULL, "a box's contribution to §9.4.1's stack was asked for with no element");
    q = layout_question(LAYOUT_Q_BF_BOX, el, NULL, (unsigned) pass + 1u);
    LAYOUT_QUESTION_REPEAT_CHECK(q,
        "Both routes into this entry are broken by construction, so a repeat is one of them re-opened. "
        "Descending: the walk takes its boxes from `bf_block_box_at`, which returns only in-flow block-level "
        "boxes, because `bf_element_child` classifies a computed `position` of `absolute` or `fixed` as "
        "NO_BOX per CSS 2.1 §9.3.1 \"Choosing a positioning scheme: 'position' property\"; a chain through "
        "the abs-margin arm means that classification changed. Ascending: a percentage height resolving "
        "against a containing block whose height is the walk already open is refused by CSS 2.1 §10.7 "
        "\"Minimum and maximum heights: 'min-height' and 'max-height'\"'s second conjunct at the last gate of "
        "`used_value_containing_block_height`; a chain through it means that gate no longer refuses. Fix it at "
        "the rule that re-enters, never by a limit.");
    return bf_question_ask(q, out);
}

/* The stack of `el`'s own children for `pass`, asked through the layout job: run once per job whatever `el`'s
   `height` says, and continued where it stopped. Every reader of a container's stack asks here. */
static bool bf_walk(lxb_dom_element_t *el, BfBaseline pass, BfBox *out)
{
    LayoutQuestion q;

    DCHECK(el != NULL, "a §9.4.1 stack was asked for with no container");
    q = layout_question(LAYOUT_Q_BF_WALK, el, NULL, (unsigned) pass + 1u);
    LAYOUT_QUESTION_REPEAT_CHECK(q,
        "A container's stack asked, inside itself, for something only its finished stack answers: a child's "
        "position, its own content height or baseline, a run's start or its anonymous boxes. The asking rule "
        "reads its own output; fix it at the rule that re-enters, never by a limit.");
    return bf_question_ask(q, out);
}

bool block_flow_drive_deferred(LayoutJobDrive *d)
{
    LayoutQuestion q;
    size_t at;

    while (!layout_job_drive_spent(d) && layout_job_drive_next(d, &q, &at)) {
        BfBox r;

        (void)bf_question_run(q, at, &r);
    }
    return !layout_job_drive_spent(d);
}

/* A FLEX CONTAINER'S AUTO HEIGHT IS TWO DIFFERENT SECTIONS OF css-flexbox-1 AND §5.1 "Flex Flow Direction: the
 * flex-direction property" IS WHICH, so the question this entry answers is TWO questions and they take
 * opposite work. This split is the whole of what this function is: one crash for both arms was CORRECT for a
 * `row` container and WRONG for a `column` one, in the direction that costs the most — it named §9.6
 * "Cross-Axis Alignment" and the flex lines unconditionally, so a reader meeting it on a `column` container
 * was told to build §9.3 "Main Size Determination" and §9.4 "Cross Size Determination" for a number that
 * needs NO FLEX LINE AT ALL. BOTH OF §5.1's ARMS ARE NOW BUILT AND WHAT STILL CRASHES IS THE WRITING MODE,
 * which is a different question and is why the split is still worth stating.
 *
 * WHICH SIZE A *HEIGHT* IS, IS A FACT ABOUT THE WRITING MODE AND THEN ABOUT §5.1, in that order, and stating
 * only the second is one predicate answering two questions. A height is the BLOCK size only in a
 * `horizontal-tb` mode (css-writing-modes-4 §3.2 "Block Flow Direction: the writing-mode property"), which is
 * why that is asked FIRST and refused here rather than assumed — core/layout/flow_position.c refuses the same
 * value one question earlier for a box's PLACEMENT and names the same mapping. §5.1 then decides whether that
 * block size is the container's MAIN size or its CROSS size: §5.1 makes a `row` container's main axis "the
 * same orientation as the inline axis of the current writing mode", so its block axis is its CROSS axis, and
 * a `column` container's main axis is the block axis itself. core/layout/flex_item.h answers that mapping and
 * deliberately cannot see `row-reverse`/`column-reverse`, which are facts about ORDER and not about an axis.
 *
 * THE TWO ARMS SHARE NO STEP, which is why the dispatch is over the whole container and not a case discovered
 * part-way through — the same reason core/layout/flex_intrinsic_size.c asks §5.1 first for the mirror
 * question, a flex container's intrinsic INLINE size.
 *
 * IT RETURNS A BOOL SO THAT THE RELEASE ARM OF THE ONE REMAINING CRASH IS UNCHANGED, and that crash is now
 * the WRITING-MODE one rather than the `column` one. With the asserts compiled out a `DFAILF` is a no-op and
 * control falls through whatever follows it, so answering from the bottom of this function would hand a box in
 * a VERTICAL writing mode a number measured along the wrong dimension in release — where today it falls
 * through to CSS 2.1 §10.6.3's stack of block-level boxes, a DEFINED wrong number that is what that crash
 * already left behind and is not a dispatch's to choose. FALSE is therefore "this arm did not answer", and the
 * caller's fall-through is §10.6.3 exactly as before.
 *
 * RETIREMENT — THE CONDITION THAT STOOD HERE IS MET AND IS REWRITTEN RATHER THAN OBEYED, WHICH IS WORTH
 * RECORDING BECAUSE OBEYING IT WOULD HAVE BEEN A REGRESSION. It read: "this function goes when the `column`
 * arm is built; at that point there is no question left to dispatch on, the bool goes with the last crash".
 * The `column` arm IS built, and the clause was written when the `column` crash was the last one — so its own
 * reason ("the last crash") had a referent that moved, and a reader taking it literally would delete the bool
 * while the WRITING-MODE crash's release fall-through still depends on it. The condition was satisfiable by an
 * EDIT rather than by a CONSTRUCTION, which is the one shape a retirement clause must not have.
 * WHAT ENDS IT IS NAMED AS A CONSTRUCTION INSTEAD: this function goes when CSS 2.1 §10.6.3's stack and
 * core/layout/intrinsic_size.c's pair are each readable in the OTHER physical direction — the
 * dimension-parameterised walks the writing-mode arm below names — because at that point there is no refusal
 * left, every arm answers, and `block_flow_auto_height` can call the two sections' components directly. */
static bool bf_flex_auto_block_size(lxb_dom_element_t *el, CssPx *out)
{
    char nbuf[160], wbuf[64];

    if (!css_computed_keyword_is(el, "writing-mode", CSS_KEYWORD_HORIZONTAL_TB)) {
        DFAILF("%s, computed `writing-mode` `%s`: this FLEX CONTAINER's block axis is not the vertical one, so "
               "the HEIGHT this entry was asked for is not its block size and css-flexbox-1 §5.1 \"Flex Flow "
               "Direction: the flex-direction property\"' mapping below would send it to the wrong section — "
               "a `column` container in a vertical mode has its MAIN axis along the horizontal, so the arm "
               "that reads §9.2 \"Line Length Determination\" for it would be answering about a width. "
               "css-writing-modes-4 §3.2 \"Block Flow Direction: the writing-mode property\" is what makes "
               "the block flow direction a property at all. THE REMEDY THAT STOOD HERE SAID TO BUILD "
               "css-writing-modes-4 §7.4 \"Flow-Relative Mappings\" SO THIS DISPATCH COULD READ THE "
               "FLOW-RELATIVE AXES, AND THE CITATION WAS MIS-AIMED — §7.4 is the rule for WHOSE writing mode "
               "a flow-relative question is read against, and the TABLE is §6.4 \"Abstract-to-Physical "
               "Mappings\", held in core/css/css_logical.c and now exported for the axis question as "
               "`css_logical_axis_is_vertical`, with §5.1's composition in "
               "`flex_container_axis_is_vertical` (core/layout/flex_item.h). THE DISPATCH IS THEREFORE NO "
               "LONGER WHAT IS MISSING. What is missing is that the two arms it dispatches to are physical "
               "all the way down: css-writing-modes-4 §7.2 \"Dimensional Mapping\" puts CSS 2.1 §10.6's "
               "rules on the BLOCK size and §10.3's on the INLINE size, and this file measures a HEIGHT with "
               "§10.6's walk — which in a vertical writing mode is this box's inline size. BUILD the "
               "dimension-parameterised reading of that walk and the dispatch above becomes a call",
               box_subject(el, nbuf, sizeof nbuf),
               box_subject_computed(el, "writing-mode", wbuf, sizeof wbuf));
        return false;
    }

    if (flex_container_main_axis(el) == FLEX_MAIN_AXIS_BLOCK) {
        /* THE `column` ARM, WHICH IS NOW A CALL AND WAS THE CRASH. §5.1 has just made this container's block
           axis its MAIN axis, so the auto HEIGHT asked for is its MAIN size and neither §9.6 "Cross-Axis
           Alignment" nor a flex line has anything to say about it. §9.2 "Line Length Determination"' last step
           is the whole composition in one sentence — "Determine the main size of the flex container using the
           rules of the formatting context in which it participates. The automatic block size of a block-level
           flex container is its max-content size." — and the max-content MAIN size is css-flexbox-1 §9.9.1
           "Flex Container Intrinsic Main Sizes", which core/layout/flex_intrinsic_size.h answers in this axis.
           WHAT THE CRASH THAT STOOD HERE TOLD ITS READER TO BUILD IS WHAT WAS BUILT, and two clauses of it are
           worth keeping because a reader will re-derive them: §9.9.1's conformance sentence lets an
           implementation choose the Web-compatible Algorithm, §9.9.1.2 "Web-compatible Intrinsic Sizing
           Algorithm: Max-content Size and Min-content Single-line Size", whose max-content half is one sentence
           of arithmetic over the ITEMS with no line in it; and what differed from the INLINE-axis walk beside
           it was EVERY OPERAND'S AXIS and nothing else, so what landed is that walk with its main-axis property
           names and its measurement parameterised rather than a second copy of §9.9.3 "Flex Item Intrinsic Size
           Contributions".
           THE CONFORMANCE SENTENCE IS SCOPED AND THE OLD CRASH QUOTED IT CUT, which is recorded because the cut
           was harmless here and would not be one axis over: §9.9.1 reads "For max-content sizes, and for
           single-line min-content sizes, an implementation is conformant …", so the choice does NOT extend to a
           MULTI-LINE min-content size, which §9.9.1 hands to §9.9.1.3 "Multi-line Min-content Algorithm"
           unconditionally. Nothing on this path asks for one — a max-content main size is one algorithm for
           every container — so no multi-line arm is missing here and a reader must not add a refusal for one.
           IT IS A CONTENT EXTENT, which is what this function's contract returns and what §9.9.1.2's sum is:
           every operand is an item's own OUTER contribution inside the container's content box, so there is no
           edge to convert and css-sizing-3 §3.3 "Box Edges for Sizing: the box-sizing property"' conversion
           stays with the caller that exposes a used value, exactly as it does for the `row` arm below. */
        *out = flex_intrinsic_max_content_main_block_size(el);
        return true;
    }

    /* THE `row` ARM, WHICH IS A CALL AND NOT AN ALGORITHM. §5.1 has just made this container's block axis its
       CROSS axis, so the auto HEIGHT asked for is the cross size css-flexbox-1 §9.6 "Cross-Axis Alignment"'
       own step determines — "Determine the flex container's used cross size using the rules of the formatting
       context in which it participates. If a content-based cross size is needed, use the sum of the flex
       lines' cross sizes" — and core/layout/flex_cross_size.h runs that step over §9.4 "Cross Size
       Determination"' steps 7 and 8, which are what produce the lines' cross sizes it sums.
       THE CRASH THAT STOOD HERE TOLD ITS READER TO BUILD §9.3 "Main Size Determination" AND §9.4, and §9.3 is
       built (core/layout/flex_line.h) — a reader who follows the old sentence will build §9.7 "Resolving
       Flexible Lengths" a second time, which is the identical hazard the `column` arm above records about
       §9.9.3 "Flex Item Intrinsic Size Contributions". WHAT §9.3 DID NOT DO AND THIS DIFF DID is the CROSS
       axis: the two sections share no step, which is the same sentence this function's own dispatch rests
       on one level up.
       IT IS A CONTENT EXTENT, which is what this function's contract returns and what §9.6's sum is: a flex
       line sits inside the container's content box with nothing between them, so there is no edge to convert
       and css-sizing-3 §3.3 "Box Edges for Sizing: the box-sizing property"' conversion stays with the caller
       that exposes a used value, exactly as it does for CSS 2.1 §10.6.3's walk below. */
    *out = flex_cross_size_content_based(el);
    return true;
}

bool block_flow_auto_height_ask(lxb_dom_element_t *el, CssPx *out)
{
    CssDisplay d;
    bool container, flex, grid;
    BfBox b;

    DCHECK(el != NULL, "a content-based height was asked for with no element");
    d = css_computed_display(el);
    container = block_flow_display_is_block_container(d);
    flex = css_display_spells(d, CSS_KEYWORD_FLEX) || css_display_spells(d, CSS_KEYWORD_INLINE_FLEX);
    grid = css_display_spells(d, CSS_KEYWORD_GRID) || css_display_spells(d, CSS_KEYWORD_INLINE_GRID);
    /* THE FLEX DISPATCH IS NOT AN `if` IN FRONT OF THIS WALK, IT IS A ROUTE THAT MAY ANSWER. A `row`
       container's auto height is css-flexbox-1 §9.6 "Cross-Axis Alignment"' content-based cross size and
       returns here, and so does the `column` arm's §9.9.1 "Flex Container Intrinsic Main Sizes" max-content
       main size. The VERTICAL WRITING MODES still crash by name and return FALSE, and their fall-through into
       §10.6.3's stack below is the release arm this walk already had for them. */
    if (flex && bf_flex_auto_block_size(el, out)) return true;
    /* css-grid-1 §5.2 "Sizing Grid Containers": "the grid container's auto block size is its max-content
       size", the sum of its row sizes (core/layout/grid_track.h), not §10.6.3's stack of block-level children. */
    if (grid) return grid_container_max_content_block_size_ask(el, out);
    if (!container)
        DFAIL("CSS 2.1 §10.6.3's content-based height was asked for a box that is not a BLOCK CONTAINER — "
              "§9.2.1's box that 'either contains only block-level boxes or establishes an inline formatting "
              "context' — so there is no block formatting context inside it for this walk to run over. The "
              "caller is core/layout/used_value.c, which classifies the box type before asking, so the two "
              "lists have come apart");
    if (!bf_walk(el, BF_BASELINE_NONE, &b)) return false;
    *out = b.content_h;
    return true;
}

CssPx block_flow_auto_height(lxb_dom_element_t *el)
{
    LayoutJobDrive d;
    CssPx h;

    layout_job_drive_begin(&d);
    while (!block_flow_auto_height_ask(el, &h)) (void)block_flow_drive_deferred(&d);
    layout_job_drive_end(&d);
    return h;
}

bool block_flow_last_line_box_baseline_ask(lxb_dom_element_t *el, CssPx *baseline, bool *has)
{
    CssDisplay d;
    bool container;
    BfBox b;

    DCHECK(el != NULL, "CSS 2.2 §10.8.1's `inline-block` baseline was asked for with no element");
    DCHECK(baseline != NULL,
           "CSS 2.2 §10.8.1's `inline-block` baseline was asked for with nowhere to put the distance. The "
           "return value is only the sentence's \"no in-flow line boxes\" disjunct, so a caller holding it "
           "alone would know that a baseline EXISTS and have no coordinate to align the line to");
    d = css_computed_display(el);
    container = block_flow_display_is_block_container(d);
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
    DCHECK(has != NULL, "the last-line-box baseline ask was given nowhere to say whether a line box exists");
    if (!bf_walk(el, BF_BASELINE_LAST, &b)) return false;
    /* `bf_layout` measures both of its distances from this box's TOP CONTENT EDGE, which is the frame this
       entry's contract states, so there is no conversion here and none is hidden in the caller either. */
    *baseline = b.baseline;
    *has = b.has_line_box;
    return true;
}

bool block_flow_last_line_box_baseline(lxb_dom_element_t *el, CssPx *baseline)
{
    LayoutJobDrive d;
    bool has = false;

    layout_job_drive_begin(&d);
    while (!block_flow_last_line_box_baseline_ask(el, baseline, &has)) (void)block_flow_drive_deferred(&d);
    layout_job_drive_end(&d);
    return has;
}

bool block_flow_first_line_box_baseline_ask(lxb_dom_element_t *el, CssPx *baseline, bool *has)
{
    CssDisplay d;
    bool container;
    BfBox b;

    DCHECK(el != NULL, "CSS 2.1 §17.5.3's FIRST-line-box baseline was asked for with no element");
    DCHECK(baseline != NULL,
           "CSS 2.1 §17.5.3 Table height algorithms' FIRST-line-box baseline was asked for with nowhere to "
           "put the distance. The return value is only the section's \"if there is no such line box\" "
           "disjunct, so a caller holding it alone would know a baseline EXISTS and have no coordinate to "
           "align the row to");
    d = css_computed_display(el);
    container = block_flow_display_is_block_container(d);
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
    DCHECK(has != NULL, "the first-line-box baseline ask was given nowhere to say whether a line box exists");
    if (!bf_walk(el, BF_BASELINE_FIRST, &b)) return false;
    /* Same frame as the entry above and as `block_flow_auto_height`: the distance is from this box's own TOP
       CONTENT EDGE, so a caller measuring from a BORDER edge adds its own border and padding and this file
       adds neither. */
    *baseline = b.baseline;
    *has = b.has_line_box;
    return true;
}

bool block_flow_first_line_box_baseline(lxb_dom_element_t *el, CssPx *baseline)
{
    LayoutJobDrive d;
    bool has = false;

    layout_job_drive_begin(&d);
    while (!block_flow_first_line_box_baseline_ask(el, baseline, &has)) (void)block_flow_drive_deferred(&d);
    layout_job_drive_end(&d);
    return has;
}

bool block_flow_child_top_ask(lxb_dom_element_t *el, CssPx *out)
{
    lxb_dom_element_t *cb;
    CssPx top = css_px(0.0);
    BfBox unused;
    char nbuf[160], cbuf[160], pbuf[160];

    DCHECK(el != NULL, "a box's vertical placement was asked for with no element");
    cb = used_value_stacking_container(el);
    /* A NULL CONTAINING BLOCK HAS TWO PRODUCERS AND ONLY ONE OF THEM CAN REACH HERE, which is why this
       states which one rather than naming the root element outright. `used_value_containing_block` answers
       NULL for §10.1's FIRST case (the root element, whose containing block is the initial one) and — since
       the commit that built §10.1's FOURTH case — also for an absolutely positioned box with no ancestor
       establishing a containing block, which §10.1 likewise gives the initial containing block. The SECOND
       producer cannot arrive at this entry: §9.4.1's walk is over a block formatting context's IN-FLOW
       boxes, and §9.3.1 takes an absolutely positioned box out of flow before any of them see it. So the
       condition is unchanged and correct, and the message says the root element is what reaches it rather
       than what NULL means. RETIREMENT: this note goes when an out-of-flow box cannot be spelled as an
       argument to this entry — when the walk hands its own in-flow child list to it rather than an element
       any caller may name — because the second producer is then impossible by construction. */
    DCHECK(cb != NULL,
           "CSS 2 §9.4.1's placement was asked for a box with no containing block. §10.1 leaves that answer to "
           "TWO cases — its FIRST, the root element, and its FOURTH with no ancestor establishing one — and only "
           "the root element can reach this entry, because §9.4.1 walks a formatting context's IN-FLOW boxes and "
           "§9.3.1 has already taken an absolutely positioned box out of flow. So this is the root element, "
           "which core/layout/flow_position.c answers from §10.1 directly rather than by walking a parent's "
           "children: the caller's own root test and this one have come apart");
    /* CSS 2.2 §9.2.1.1 "Anonymous block boxes" PUTS A BOX ON A STACK ITS PARENT ELEMENT DOES NOT OWN, AND
       THIS TEST USED TO REFUSE IT. The condition was `cb == el->parent` and the message told its reader to
       BUILD THE BOX LIST §9.2.1.1 AND css-display-3 §2.5 DESCRIBE as the thing this walk iterates. §9.2.1.1's
       half of that list EXISTS — `block_flow_next_block_box` enumerates a container's boxes in content order
       and descends into an in-flow inline box the section breaks, and `bf_layout` stacks what it yields — so
       the demand was met and the refusal outlived it, which is the one failure mode a crash naming another
       mechanism has. WHAT THE TEST ASKS NOW IS THE SECTION'S OWN SENTENCE INSTEAD OF A PROXY FOR IT: is this
       box on that container's BOX list, which for a box inside a broken inline box is exactly what "the
       block-level box becomes a sibling of those anonymous boxes" makes it.
       §2.5's HALF IS STILL UNBUILT AND IS STILL REFUSED HERE, by the same test and without a second arm: a
       `display: contents` element generates no inline box, so `bf_on_box_list_of` cannot reach through one
       and the crash below names the splice. Nothing was widened past what §9.2.1.1 states.
       NAMED RESIDUAL — §9.2.1.1's RELATIVE-POSITIONING SENTENCE IS CARRIED BY NEITHER TERM OF THIS ORIGIN.
       WHAT IS NOT COVERED: the section's own last sentence of that paragraph — "When such an inline box is
       affected by relative positioning, any resulting translation also affects the block-level box contained
       in the inline box." This entry answers a distance down `cb`'s stack and core/layout/flow_position.c
       adds `cb`'s own border-box origin to it, so a translation applied to an inline box BETWEEN the two is
       in neither term. That is not the ordinary absence of CSS 2.2 §9.4.3 "Relative positioning" wearing a
       new name: an ordinary box inherits its ancestor's translation through that same origin recursion
       because the ancestor is ON the recursion's chain, and this box's broken inline ancestor is precisely
       the element §10.1's walk steps over, so it is on no chain any composition here climbs. WHAT THE NEXT
       DIFF BUILDS: §9.4.3's offset where a box's own origin is composed, PLUS this section's extra term for
       a box whose §9.2.1.1 ancestry crosses a relatively positioned inline box — the second does not fall
       out of the first. HOW ITS ABSENCE WOULD SHOW: a document whose block-level box sits inside a
       relatively positioned inline box reports the same border-box origin for that box with the inline's
       `top` and `left` set as without them. */
    DCHECKF(bf_on_box_list_of(el, cb),
           "%s, whose containing block is %s and whose parent is %s: "
           "§10.1's containing block for this box is not its parent element, which CSS 2.2 §9.2.1.1 "
           "Anonymous block boxes makes ORDINARY rather than impossible — a block-level box inside an inline "
           "box that section breaks \"becomes a sibling of those anonymous boxes\" on the block container's "
           "own box list, and `block_flow_next_block_box` reaches it there. THIS BOX IS ON NO SUCH LIST. So "
           "the chain between the two holds an element §10.1's walk steps over and §9.2.1.1 does not reach "
           "through, and there are exactly two of them. A `display: contents` element: css-display-3 §2.5 "
           "Box Generation: the none and contents keywords splices its children into the grandparent's box "
           "list — \"the element must be treated as if it had been replaced in the element tree by its "
           "contents\" — and `bf_element_child` refuses that value by name, so no enumeration in this file "
           "reaches through one. BUILD THAT SPLICE as the thing the enumeration iterates, beside §9.2.1.1's "
           "descent that already does it. OR CSS 2.2 §9.2.2 Inline-level elements and inline boxes' ATOMIC "
           "inline-level box, which \"participate[s] in their inline formatting context as a single opaque "
           "box\" and therefore KEEPS its block-level children: a box inside one is on no stack of this "
           "container's at all, so an atomic box on this chain is core/layout/used_value.c's "
           "containing-block walk having stepped over a box it must stop at, and the fix is THERE and not "
           "here. Deciding it HERE and not below is why the message can name which of the two it is: the "
           "walk below would report only that it never reached the box",
           box_subject(el, nbuf, sizeof nbuf), box_subject(cb, cbuf, sizeof cbuf),
           box_subject_node(lxb_dom_interface_node(el)->parent, pbuf, sizeof pbuf));
    /* §9.4.1's stack answers about every child and this entry is asked about one: the container's stack
       (`bf_walk`) records every position it passes in core/layout/flow_placement.h's record, so the first ask
       about any child of a container pays for every later ask about its siblings, and a position recorded
       before the stack stopped is final. */
    DCHECK(out != NULL, "a box's vertical placement was asked for with nowhere to put it");
    if (flow_placement_ask(el, &top)) {
        *out = top;
        return true;
    }
    /* A stack that answers "not yet" is still a walk the ask paid for; its retry is a second ask. */
    flow_placement_walked();
    /* A wrapper's members are the table box and its captions, which no inline box holds. */
    if (!table_wrapper_generates(css_computed_display(cb))) bf_require_want_on_the_stack(cb, el);
    if (!bf_walk(cb, BF_BASELINE_NONE, &unused)) return false;
    if (!flow_placement_peek(el, &top))
        DFAIL("CSS 2 §9.4.1's walk over this box's containing block placed every in-flow block-level child it "
              "found and NEVER REACHED THIS BOX, so there is no position to report. The walk skips exactly what "
              "§9.2 and §10.6.3 say generates no box or is out of flow — a `display: none` element, a comment, "
              "collapsing white space, an absolutely positioned child — and every one of those is a box the "
              "CALLER should have declined before asking: core/dom/element_view.h's has-a-box predicate covers "
              "the first and core/layout/flow_position.c's own §9.3 test covers the last. The two answers have "
              "come apart, and reporting a coordinate for a box that is not in this formatting context would be "
              "a number in the right units for a box that is not there");
    *out = top;
    return true;
}

/* ---- CSS 2.1 §10.3.7's and §10.6.4's HYPOTHETICAL BOX -----------------------------------------------------
   Both sections define the static position over "a hypothetical box that would have been the FIRST box of the
   element if its specified `position` value had been `static` and its specified `float` had been `none`", and
   the three entries below are that box's three facts: WHERE it would sit on §9.4.1's stack, WHOSE content
   edge that stack belongs to, and — inside the walk — what its own top margin is. */

/* §10.3.7's Note, asked as §9.2.1 asks it of every other box on the stack: "note that due to the rules in
   section 9.7, this hypothetical calculation might require also assuming a different computed value for
   `display`". core/css/css_computed_value.h answers the value; this turns it into §9.2.1's LEVEL, using the
   same list `bf_element_child` classifies a real child with, because it is the same question about the same
   box list. A box that is not block-level is on §9.4.2's LINE and its static position is a position ALONG one,
   which is a different algorithm and not a narrower answer. */
static bool bf_hypothetical_is_block_level(lxb_dom_element_t *el)
{
    CssDisplay d = css_hypothetical_static_display(el);
    bool block = bf_display_is_block_level(d) ||
                 /* §17.4 puts a TABLE WRAPPER BOX on this stack for `display: table`, exactly as
                    `bf_element_child` does for a real child. */
                 table_box_kind_generates_table_box(table_box_kind(d));

    return block;
}

/* §10.6.3's OWN SENTENCE for the hypothetical box's `margin-top` — "if `margin-top`, or `margin-bottom` are
   `auto`, their used value is 0" — and CSS 2 §8.3's basis for a percentage one, "calculated with respect to
   the WIDTH of the generated box's containing block. Note that this is true for `margin-top` and
   `margin-bottom` as well". The containing block is the HYPOTHETICAL one, which is `cb`'s content edge
   (§10.1's second case for a static box) and NOT the positioned ancestor's padding edge the real box resolves
   against — two different rectangles, and a percentage margin resolved against the wrong one is a length no
   box has.
   IT IS RESOLVED HERE RATHER THAN READ THROUGH `used_value_px` BECAUSE THAT ROUTE IS THE CALLER. An absolutely
   positioned box's `margin-top` is solved by §10.6.4's constraint equation, which is the section asking for
   this number, so asking that entry for it is a cycle with no base case. §10.6.3's sentence is what the
   hypothetical STATIC box's margin is, and it is one line. */
static CssPx bf_hypothetical_margin_top(lxb_dom_element_t *el, lxb_dom_element_t *cb)
{
    CssLength len = css_computed_length(el, "margin-top");

    if (len.kind == CSS_LENGTH_ABSOLUTE) return len.px;
    if (len.kind == CSS_LENGTH_PERCENTAGE || len.kind == CSS_LENGTH_CALCULATED)
        return css_length_resolve_pct(len, used_value_content_px(cb, false));
    DCHECK(css_length_is_keyword(len, CSS_KEYWORD_AUTO),
           "a `margin-top` computed to something CSS 2 §8.3's <margin-width> grammar does not admit — it is a "
           "length, a percentage or `auto`, and lexbor validates the declaration against exactly that");
    return css_px(0.0);
}

/* THE BLOCK-LEVEL BOX THE HYPOTHETICAL BOX'S RUN FOLLOWS, and whether the container's content order reaches
   the element at all. §9.2.1.1's runs partition the container's CONTENT, so a content position is named by
   the box list entry before it; NULL is the run before the first block-level box, which is also the whole
   content of a container that has none.
   IT WALKS THE CONTENT AND NOT THE CHILD LIST, for `bf_block_box_at`'s reason: §9.2.1.1 breaks an inline box
   around an in-flow block-level box inside it and makes that box "a sibling of those anonymous boxes", so a
   position inside a BREAKING inline box is a position in this container's own content. A position inside an
   inline box that does NOT break is not reached, and that is the honest answer rather than an omission — see
   the crash at the caller. */
static lxb_dom_node_t *bf_static_run_after(lxb_dom_element_t *cb, lxb_dom_node_t *target, bool *reached)
{
    BoxTreeChild n;
    lxb_dom_node_t *last = NULL;

    *reached = false;
    for (n = box_tree_first_child(cb); box_tree_child_exists(n); n = bf_content_next(cb, n)) {
        lxb_dom_element_t *box;

        if (box_tree_child_is_node(n, target)) { *reached = true; return last; }
        box = box_tree_parent(n);
        DCHECK(box != NULL,
               "CSS 2.2 §9.2.1.1's content order reached a node with no BOX parent, so §9.2's box generation "
               "cannot be asked about it");
        if (block_flow_child_kind(box, n) == BLOCK_FLOW_CHILD_BLOCK) last = box_tree_child_node(n);
    }
    return last;
}

lxb_dom_element_t *block_flow_static_position_containing_block(lxb_dom_element_t *el)
{
    lxb_dom_node_t *n;
    char nbuf[160];

    DCHECK(el != NULL && lxb_dom_interface_node(el)->parent != NULL &&
               lxb_dom_interface_node(el)->parent->type != LXB_DOM_NODE_TYPE_DOCUMENT,
           "the root element's static-position containing block was asked for; it is the initial containing "
           "block, no element's box, and its callers answer the root before asking");

    DCHECK(el != NULL, "CSS 2.1 §10.3.7's static-position containing block was asked for with no element");
    for (n = lxb_dom_interface_node(el)->parent; n != NULL && n->type == LXB_DOM_NODE_TYPE_ELEMENT;
         n = n->parent) {
        lxb_dom_element_t *a = lxb_dom_interface_element(n);
        CssDisplay d = css_computed_display(a);
        bool container = block_flow_display_is_block_container(d);

        if (container) return a;
    }
    DFAILF("%s: CSS 2.1 §10.3.7 states the static position against \"the containing block of a hypothetical "
           "box that would have been the first box of the element if its specified `position` value had been "
           "`static`\", and §10.1's SECOND case makes that \"the content edge of the nearest BLOCK CONTAINER "
           "ancestor box\" — and this element has no block container ancestor at all. The root element is one "
           "for every document this engine parses (§2.8 computes a root `display` of `contents` to `block`), "
           "so what reaches here is an element outside a rendered tree: a DOMParser fragment, or a subtree "
           "detached from its document. THE CALLER SHOULD HAVE DECLINED IT — core/dom/element_view.h's "
           "has-a-box predicate is where an unrendered element leaves — so the two answers have come apart",
           box_subject(el, nbuf, sizeof nbuf));
    return NULL;
}

bool block_flow_static_child_top_ask(lxb_dom_element_t *el, CssPx *out)
{
    lxb_dom_element_t *cb;
    lxb_dom_node_t *n, *after;
    BlockFlowRun before;
    FlowRunStart start;
    BfRun run;
    bool reached = false, held;
    char nbuf[160], cbuf[160];

    DCHECK(el != NULL, "CSS 2.1 §10.3.7's static position was asked for with no element");
    n = lxb_dom_interface_node(el);
    cb = block_flow_static_position_containing_block(el);
    /* §10.3.7's Note first, because every other step below is written about a box on §9.4.1's stack and this
       is what decides whether the hypothetical box is one. */
    if (!bf_hypothetical_is_block_level(el))
        DFAILF("%s: CSS 2.1 §10.3.7's hypothetical box — the element \"if its specified `position` value had "
               "been `static` and its specified `float` had been `none`\" — is INLINE-LEVEL, so §9.4.2's line "
               "boxes hold it and not §9.4.1's stack. Its static position is therefore a position ALONG a "
               "line box, which is core/layout/line_box.h's fragment and not this walk's running offset: the "
               "hypothetical box would sit at whatever inline position the line had reached, on whichever "
               "line box §9.4.2's fill had got to. §10.3.7's own Note is what makes this case exist — \"note "
               "that due to the rules in section 9.7, this hypothetical calculation might require also "
               "assuming a different computed value for `display`\" — since §9.7 blockifies the REAL box's "
               "`display` and the hypothetical one keeps the specified value "
               "(core/css/css_computed_value.h's `css_hypothetical_static_display`). BUILD it as "
               "core/layout/line_box.h's fill over a box list that CONTAINS this element, and report the "
               "fragment's origin: the fill is the same one §9.4.2 already runs, and what it does not have is "
               "a box for an element §9.3.1 took out of flow",
               box_subject(el, nbuf, sizeof nbuf));
    after = bf_static_run_after(cb, n, &reached);
    if (!reached)
        DFAILF("%s, whose static-position containing block is %s: CSS 2.2 §9.2.1.1's CONTENT ORDER for that "
               "container never reached this element, so there is no run for §10.3.7's hypothetical box to "
               "sit in. The order steps over a child's subtree except through an inline box the section "
               "BREAKS, so what reaches here is an element inside an inline box that breaks nowhere TODAY and "
               "would break around this very box in the hypothetical document — §9.2.1.1: \"when an inline box "
               "contains an in-flow block-level box, the inline box … is broken around the block-level box\". "
               "A `display: contents` ancestor is the other producer and `bf_element_child` names it. BUILD "
               "the box list over the HYPOTHETICAL document that §10.3.7 describes, in which this element's "
               "`position` is `static` — the same enumeration, asked of a child list one member longer",
               box_subject(el, nbuf, sizeof nbuf), box_subject(cb, cbuf, sizeof cbuf));
    /* §9.2.1.1's SPLIT, WHICH THIS WALK CANNOT MEASURE AND WILL NOT GUESS AT. A hypothetical block-level box
       inside a run of inline content breaks that run into TWO anonymous block boxes and sits between them, so
       its position is the running position plus the height of the FIRST of them — a height this walk measures
       for the WHOLE run and never for a prefix of it. Where nothing box-generating precedes the element in
       its run there is no first box and the position is the run's own start, which is the reading below and
       is the common case: an out-of-flow child between two block-level siblings, or at the start of a
       container's content, has only collapsible white space in front of it. */
    before.after = after;
    before.end = n;
    if (block_flow_run_generates_box(cb, before))
        DFAILF("%s, whose static-position containing block is %s: CSS 2.2 §9.2.1.1 would break the anonymous "
               "block box this element sits in — \"if a block container box … has a block-level box inside it "
               "…, then we force it to have only block-level boxes inside it\" — because §10.3.7's "
               "hypothetical "
               "box is BLOCK-LEVEL and inline-level content precedes it inside one run. Its static position "
               "is then this run's start plus the HEIGHT of the anonymous box that leading content becomes, "
               "and this walk measures the whole run as ONE box rather than the two the hypothetical document "
               "has. BUILD the split: `bf_anon_box` already measures a run and `block_flow_run_generates_box` "
               "already decides whether one is a box, so what is missing is the ENUMERATION yielding an extra "
               "break at this element — after which the position falls out of the same loop body every other "
               "box on the stack is placed by",
               box_subject(el, nbuf, sizeof nbuf), box_subject(cb, cbuf, sizeof cbuf));
    {
        lxb_dom_element_t *ae = after != NULL ? lxb_dom_interface_element(after) : NULL;
        BfBox w;

        /* A run start recorded before the stack stopped is final, as a position is. */
        held = flow_placement_run_peek(cb, ae, &start);
        if (!held) {
            if (!bf_walk(cb, BF_BASELINE_NONE, &w)) return false;
            held = flow_placement_run_peek(cb, ae, &start);
        }
    }
    if (!held)
        DFAILF("%s, whose static-position containing block is %s: CSS 2 §9.4.1's walk over that container "
               "visited every one of CSS 2.2 §9.2.1.1's runs and none of them was the one this element sits "
               "in. The run is named by the block-level box it FOLLOWS and the enumeration visits each run "
               "exactly once, so the two readings of the box list — `bf_static_run_after`'s and the walk's — "
               "have come apart, which they can only do if one of them classified a child differently from "
               "the other",
               box_subject(el, nbuf, sizeof nbuf), box_subject(cb, cbuf, sizeof cbuf));
    run.pos = start.run_pos;
    run.neg = start.run_neg;
    *out = bf_static_margin_edge(start.pos, run, bf_hypothetical_margin_top(el, cb), start.escaping);
    return true;
}

CssPx block_flow_static_child_top(lxb_dom_element_t *el)
{
    LayoutJobDrive d;
    CssPx top;

    layout_job_drive_begin(&d);
    while (!block_flow_static_child_top_ask(el, &top)) (void)block_flow_drive_deferred(&d);
    layout_job_drive_end(&d);
    return top;
}

static BlockFlowAnonBox *bf_anon_copy(const BlockFlowAnonBox *v, size_t n)
{
    BlockFlowAnonBox *c;

    if (n == 0) return NULL;
    c = malloc(n * sizeof *c);
    CHECK(c != NULL, "out of memory copying one container's anonymous block boxes");
    memcpy(c, v, n * sizeof *c);
    return c;
}

bool block_flow_anonymous_boxes_ask(lxb_dom_element_t *el, BlockFlowAnonBox **out, size_t *count)
{
    const BlockFlowAnonBox *held = NULL;
    size_t hn = 0;
    BfBox b;

    DCHECK(el != NULL && out != NULL && count != NULL,
           "CSS 2.2 §9.2.1.1's anonymous block boxes were asked for with no element or nowhere to put them");
    *out = NULL;
    *count = 0;
    /* §9.2.1.1's sentence has TWO conjuncts — "if a BLOCK CONTAINER BOX … HAS A BLOCK-LEVEL BOX INSIDE IT" —
       and the zero each gives is that sentence ANSWERING (see block_flow.h): an element that is no block
       container, and one establishing §9.4.2's context, are answered here; a container holding block-level
       boxes is answered by its stack, which wraps each run of inline-level content it meets. */
    if (!block_flow_display_is_block_container(css_computed_display(el))) return true;
    if (bf_content_is_inline(el)) return true;
    /* The census counts an ask at the attempt that answers it, as the origin's does. The stack runs whatever
       this container's `height` says: a container with a declared height is exactly the one whose text can
       overflow it. */
    if (!flow_placement_anon_ask(el, &held, &hn)) {
        flow_placement_anon_derived();
        if (!bf_walk(el, BF_BASELINE_NONE, &b)) return false;
        DCHECK(flow_placement_anon_peek(el, &held, &hn),
               "a container's stack answered without closing its anonymous boxes in the placement record; the "
               "stack question's body closes them when its BF_BASELINE_NONE pass answers");
    }
    *out = bf_anon_copy(held, hn);
    *count = hn;
    return true;
}

bool block_flow_table_box_top_ask(lxb_dom_element_t *table, CssPx *out)
{
    CssPx top = css_px(0.0);
    CssDisplay d = css_computed_display(table);
    bool wrapper = table_wrapper_generates(d);
    char nbuf[160];
    BfBox w;

    DCHECKF(wrapper, "%s: a table box's offset was asked of an element that generates no table wrapper box",
            box_subject(table, nbuf, sizeof nbuf));
    if (!flow_placement_table_box_peek(table, &top)) {
        if (!bf_walk(table, BF_BASELINE_NONE, &w)) return false;
        if (!flow_placement_table_box_peek(table, &top))
            DFAILF("%s: the table wrapper's stack never placed its table box", box_subject(table, nbuf, sizeof nbuf));
    }
    *out = top;
    return true;
}

CssPx block_flow_table_box_top(lxb_dom_element_t *table)
{
    LayoutJobDrive d;
    CssPx top;

    layout_job_drive_begin(&d);
    while (!block_flow_table_box_top_ask(table, &top)) (void)block_flow_drive_deferred(&d);
    layout_job_drive_end(&d);
    return top;
}

CssPx block_flow_table_wrapper_height(lxb_dom_element_t *table)
{
    bool wrapper = table_wrapper_generates(css_computed_display(table));
    char nbuf[160];
    LayoutJobDrive d;
    BfBox w;

    DCHECKF(wrapper, "%s: a table wrapper box's height was asked of an element that generates none",
            box_subject(table, nbuf, sizeof nbuf));
    layout_job_drive_begin(&d);
    while (!bf_walk(table, BF_BASELINE_NONE, &w)) (void)block_flow_drive_deferred(&d);
    layout_job_drive_end(&d);
    return w.content_h;
}

size_t block_flow_anonymous_boxes(lxb_dom_element_t *el, BlockFlowAnonBox **out)
{
    LayoutJobDrive d;
    size_t n = 0;

    layout_job_drive_begin(&d);
    while (!block_flow_anonymous_boxes_ask(el, out, &n)) (void)block_flow_drive_deferred(&d);
    layout_job_drive_end(&d);
    return n;
}
