/* CSSOM VIEW §7 — Extensions to the HTMLElement Interface. See html_element_view.h for the split between the
   extents and the positions, for the three facts this engine cannot read and how every member finds out, and
   for why `scrollParent` is honestly absent. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/css/css_style_declaration.h"
#include "core/dom/document.h"
#include "core/dom/element.h"
#include "core/dom/element_view.h"
#include "core/dom/flat_tree.h"
#include "core/dom/node.h"
#include "core/dom/shadow_root.h"
#include "core/html/html_element.h"
#include "core/html/html_element_view.h"
#include "core/idl_args.h"
#include "core/layout/block_flow.h"
#include "core/layout/containment.h"
#include "core/layout/flow_position.h"
#include "core/layout/layout_job.h"
#include "core/layout/replaced_element.h"
#include "core/layout/used_value.h"
#include "solver/rest_unit.h"
#include <lexbor/core/base.h>   /* lexbor_malloc: the allocator the engine gives its refusal edge */

/* THE MEMBERS. §7 declares six and this file answers five; `scrollParent` is absent for the reason the header
   states in full, so it has no magic here rather than a magic with no case. */
typedef enum {
    HEV_OFFSET_PARENT = 0,
    HEV_OFFSET_TOP, HEV_OFFSET_LEFT,
    HEV_OFFSET_WIDTH, HEV_OFFSET_HEIGHT
} HtmlElementViewMember;

/* THE THREE QUESTIONS EVERY MEMBER OF §7 ASKS ABOUT THE ELEMENT, asked once here rather than by each. They are
   NOT §6's four: §7 contains no "if document is not the active document" step and no quirks-mode branch, so
   neither the element's document's realm nor its compat mode is an operand of anything below. What is asked is
   the has-a-box predicate, and the two elements §7 names by role. */
typedef struct {
    lxb_dom_node_t *node;     /* the element */
    JSContext      *rctx;     /* the ELEMENT's relevant realm — where its offsetParent's wrapper is minted */
    bool            has_box;  /* "the element has an associated box" — element_view.h's one predicate */
    bool            is_root;  /* "the element is the root element" */
    bool            is_body;  /* "the element is the body element" */
} HevTarget;

static void hev_target_of_element(lxb_dom_element_t *el, HevTarget *t)
{
    lxb_dom_node_t *doc;

    t->node = lxb_dom_interface_node(el);
    DCHECK(t->node->owner_document != NULL,
           "a CSSOM VIEW §7 member was reached on an element whose node has no owner document — every node this "
           "engine mints belongs to the document that created it, so a null owner is a tree built outside the "
           "DOM layer");
    doc = lxb_dom_interface_node(t->node->owner_document);
    t->rctx    = document_realm_of(t->node);
    DCHECK(t->rctx != NULL,
           "a CSSOM VIEW §7 member was reached on an element whose node document has no realm — every Document "
           "a page can hold a node of was built by one, and the only trees without a record are the solver's "
           "own scratch parses, which are handed to nobody");
    t->has_box = element_view_has_box(t->node);
    t->is_root = document_document_element_of(doc) == t->node;
    t->is_body = document_body_of(doc) == t->node;
    DCHECK(!(t->is_root && t->is_body),
           "one element answered as BOTH the root element and the body element. HTML makes the body element a "
           "CHILD of the root, so the two are never the same node and §7's step 1 lists them as two conditions "
           "rather than one");
}

/* THE FIVE QUESTIONS §7 ASKS OF ITS RECEIVER, FILLED ONCE THE BRAND HAS PASSED — which happens before this
   runs and not in it.

   THIS FUNCTION WAS THE BRAND CHECK, AND THE ARGUMENT IS KEPT BECAUSE IT IS THE REASON THIS SECTION IS A FILE
   OF ITS OWN AND A READER WILL RE-DERIVE IT. It read: the check is over HTMLElement rather than over Element,
   because `Object.getOwnPropertyDescriptor(HTMLElement.prototype,'offsetWidth').get.call(svgEl)` is a
   TypeError in every user agent while `svgEl.offsetWidth` is `undefined`, and a page that feature-detects by
   applying the getter tells the throw apart from both. Every clause of that is still true and the INTERFACE is
   what the five declarations now state: core/idl_args.h's idl_this_iface, naming
   core/html/html_element.h's own `html_element_is`, so Web IDL §3.7 Interfaces' implementation-check step 3 is
   performed at one place for all five. §6's members on Element.prototype brand with `element_is` in
   core/dom/element_view.c, which is the same split written as two declarations rather than as two bodies.

   IT MAY NOT COME BACK AS AN ASSERT: the receiver is the PAGE's input, and an abort there is this engine
   crashing on a line of ordinary JavaScript. The refusal that replaces it is §3.7.6's TypeError and lives in
   both builds. The assert below is a different claim — it is about this file's own install block, which either
   states the interface for a member or does not. */
static void hev_target(JSValueConst this_val, HevTarget *t)
{
    lxb_dom_element_t *el = element_of_value(this_val);

    DCHECK(el != NULL,
           "a CSSOM VIEW §7 member reached its body on a receiver that is not an element — every one of the "
           "five states html_element_is at its install, so reaching here means one was added without doing so");
    hev_target_of_element(el, t);
}

/* An HTML element's local name, ASCII-lowercase already for an HTML-namespace element. */
static bool hev_html_local_name_is(const lxb_dom_node_t *n, const char *name)
{
    size_t len = 0;
    const lxb_char_t *ln;

    DCHECK(n != NULL && n->type == LXB_DOM_NODE_TYPE_ELEMENT,
           "§7's td/th/table bullet names HTML ELEMENTS, and it was asked about a node that is not an element — "
           "the walk that asks it has already established the type in its own loop condition");
    if (n->ns != LXB_NS_HTML) return false;
    ln = lxb_dom_element_local_name(lxb_dom_interface_element((lxb_dom_node_t *)n), &len);
    return ln != NULL && strlen(name) == len && memcmp(ln, name, len) == 0;
}

/* ---- the inputs §7 branches on and this engine cannot read ---------------------------------------------- */

/* An operand of every member §7 states, including the two that read no ancestor. `zoom` is CSS Viewport §4
   "The zoom property": every number §7 reports is an UNSCALED one, and "the unscaled value of a CSS length
   relative to an element is the scaled value divided by the element's effective zoom", whose product runs over
   the whole flat-tree chain. */
static const char *const HEV_UNREADABLE_ALWAYS[] = { "zoom" };

/* DOES THIS ELEMENT DECLARE THE PROPERTY? The CASCADED value, which is a DETECTOR for an input this component
   cannot read and never a value it uses — its only outcome is the crash. */
static bool hev_declares(lxb_dom_element_t *el, const char *name)
{
    char *decl = cssom_cascaded_value(el, name);
    bool declared = decl != NULL;

    free(decl);
    return declared;
}

/* The flat-tree chain (css-shadow-1 §4.1), checked once before any of §7's facts is read for the input the walk
   below cannot see: a `zoom` declaration, since CSS Viewport §4's effective zoom is a product over "all flat tree
   ancestors". */
static void hev_require_readable_chain(lxb_dom_node_t *n)
{
    lxb_dom_node_t *a;
    size_t i;

    for (a = n; a != NULL && a->type == LXB_DOM_NODE_TYPE_ELEMENT; a = flat_tree_parent(a)) {
        lxb_dom_element_t *el = lxb_dom_interface_element(a);

        for (i = 0; i < sizeof HEV_UNREADABLE_ALWAYS / sizeof HEV_UNREADABLE_ALWAYS[0]; i++)
            if (hev_declares(el, HEV_UNREADABLE_ALWAYS[i]))
                DFAILF("an element on this CSSOM VIEW §7 chain DECLARES `%s`, whose computed value "
                       "core/css/css_computed_value.h does not derive, and EVERY member of §7 is a function of "
                       "it — the two that walk no ancestor included, which is why this one is asked whatever "
                       "the caller is asking. `zoom`: CSS Viewport §4 \"The zoom "
                       "property\" states \"the effective zoom of an element is the product of its computed "
                       "value of zoom and all flat tree ancestors' computed values of zoom\" and \"the unscaled "
                       "value of a CSS length relative to an element is the scaled value divided by the "
                       "element's effective zoom\", and every extent and coordinate §7 reports is an UNSCALED "
                       "one, so all five are off by that product and offsetParent's own "
                       "different-effective-zoom bullet can fire. BUILD this property's `Computed value:` line "
                       "in css_computed_value.c and record its shorthands in css_shorthand.c",
                       HEV_UNREADABLE_ALWAYS[i]);
    }
}

/* ---- §7's `offsetParent` --------------------------------------------------------------------------------- */

/* CSSOM VIEW §7's "ancestor is a containing block of absolutely-positioned descendants (regardless of whether
   there are any absolutely-positioned descendants)" and "a containing block for fixed-positioned descendants":
   css-position-3 §2.1's establishment, the one predicate §10.1's walk asks (core/layout/used_value.h). */
static bool hev_establishes_abspos_containing_block(lxb_dom_element_t *el)
{
    return used_value_establishes_positioning_cb(el, false);
}

static bool hev_establishes_fixedpos_containing_block(lxb_dom_element_t *el)
{
    return used_value_establishes_positioning_cb(el, true);
}

/* The nearest flat-tree ancestor of `n` that establishes a fixed positioning containing block, or NULL. A
   boxless ancestor establishes none (containment "has no effect" without a principal box). */
static lxb_dom_element_t *hev_fixedpos_containing_block(const lxb_dom_node_t *n)
{
    lxb_dom_node_t *a;

    for (a = flat_tree_parent(n); a != NULL && a->type == LXB_DOM_NODE_TYPE_ELEMENT; a = flat_tree_parent(a))
        if (hev_establishes_fixedpos_containing_block(lxb_dom_interface_element(a)))
            return lxb_dom_interface_element(a);
    return NULL;
}

/* Whether the flat-tree ancestor `a` is hidden from `el` in §7's walk: DOM §4.8 "Interface ShadowRoot"'s
   closed-shadow-hidden without its mode condition — `a`'s root is a shadow root that is not a shadow-including
   inclusive ancestor of `el`. §7 says closed-shadow-hidden, which keeps an open tree's ancestor; Chrome
   157.0.8103.50, Firefox 160.0 and Safari 27.2 skip it whatever the mode (WPT
   shadow-dom/offsetParent-across-shadow-boundaries.html, the open-mode slot subtests, wpt.fyi stable runs of
   2026-11-11), so an offsetParent is always a shadow-including ancestor. */
static bool hev_hidden_from(const lxb_dom_node_t *a, const lxb_dom_node_t *el)
{
    lxb_dom_node_t *root = node_root((lxb_dom_node_t *)a);

    return shadow_root_is(root) && !shadow_root_is_shadow_including_inclusive_ancestor(root, el);
}

/* §7's own first list, shared by `offsetParent` and (in the ED) `scrollParent`: the four conditions under which
   the answer is null before any walking happens. The fourth is "The element's computed value of the position
   property is fixed and no ancestor establishes a fixed position containing block". */
static bool hev_null_offset_parent(const HevTarget *t)
{
    if (!t->has_box) return true;
    if (t->is_root)  return true;
    if (t->is_body)  return true;
    return css_computed_keyword_is(lxb_dom_interface_element(t->node), "position", CSS_KEYWORD_FIXED) &&
           hev_fixedpos_containing_block(t->node) == NULL;
}

/* §7's `offsetParent` algorithm, in the spec's own step order, over the flat tree. Returns the element or NULL.
   "The element is in a fixed position containing block" is read as: it is `position: fixed`, step 1 found an
   ancestor establishing one, and that ancestor is not hidden from it. The last clause is the same measurement
   as hev_hidden_from's: for a fixed element whose fixed containing block is in a shadow tree it is slotted into,
   the three engines answer the nearest absolute containing block the element can see (that file's "fixed
   element assigned to a slot within a fixed containing block" subtests), where §7's fixed bullet finds none.
   The different-effective-zoom bullet is unreachable: every effective zoom here is 1, which
   hev_require_readable_chain asserts. Running out of element parents is "no more parent of ancestor in the flat
   tree", since the flat tree holds the root element and no Document. */
static lxb_dom_element_t *hev_offset_parent(const HevTarget *t)
{
    lxb_dom_element_t *el = lxb_dom_interface_element(t->node), *fixed_cb;
    bool in_fixed_cb, is_static;
    lxb_dom_node_t *a;

    /* step 1 */
    if (hev_null_offset_parent(t)) return NULL;
    fixed_cb = css_computed_keyword_is(el, "position", CSS_KEYWORD_FIXED) ? hev_fixedpos_containing_block(t->node)
                                                                          : NULL;
    in_fixed_cb = fixed_cb != NULL && !hev_hidden_from(lxb_dom_interface_node(fixed_cb), t->node);
    is_static = css_computed_keyword_is(el, "position", CSS_KEYWORD_STATIC);
    /* step 2 */
    for (a = flat_tree_parent(t->node); a != NULL && a->type == LXB_DOM_NODE_TYPE_ELEMENT; a = flat_tree_parent(a)) {
        lxb_dom_element_t *anc = lxb_dom_interface_element(a);
        bool matches;

        if (hev_hidden_from(a, t->node)) {
            /* step 2's first substep */
            if (css_computed_keyword_is(anc, "position", CSS_KEYWORD_FIXED) && hev_fixedpos_containing_block(a) == NULL)
                return NULL;
            continue;
        }
        /* step 2's second substep */
        if (in_fixed_cb)
            matches = hev_establishes_fixedpos_containing_block(anc);
        else
            matches = hev_establishes_abspos_containing_block(anc) ||
                      document_body_of(lxb_dom_interface_node(t->node->owner_document)) == a ||
                      (is_static && (hev_html_local_name_is(a, "td") || hev_html_local_name_is(a, "th") ||
                                     hev_html_local_name_is(a, "table")));
        if (!matches) continue;
        /* §7 states its first two bullets over a BOX — a containing block is a box, and step 3 of `offsetTop`
           goes on to subtract this element's top PADDING EDGE — while `display: contents` gives an element none.
           css-display-3 makes `position` not apply to such an element, so the containing-block bullet cannot
           select one; the body and table-cell bullets are stated over the ELEMENT and can. */
        if (!element_view_has_box(a))
            DFAIL("CSSOM VIEW §7's offsetParent walk selected an ancestor that GENERATES NO BOX — a "
                  "`display: contents` body element, or a `display: contents` td/th/table — because those two "
                  "bullets are stated over the ELEMENT while the first is stated over a box. §7 does not say "
                  "which it means, and the two readings are observably different: returning it makes "
                  "`offsetTop` step 3 ask for the top padding edge of a box that does not exist, and skipping "
                  "it makes the answer some further ancestor. RESOLVE it against the spec (csswg-drafts) rather "
                  "than picking here, and state the answer at this line");
        return anc;
    }
    return NULL;
}

/* ---- §7's `offsetTop` and `offsetLeft` ------------------------------------------------------------------- */

/* CSS 2 §8.1's PADDING EDGE of a box on the leading side of one axis, as a coordinate in the initial containing
   block's space — the border box's origin plus that side's border width, and NOT plus the padding, which is the
   step from the padding edge to the CONTENT edge. §7's step 3 subtracts exactly this. */
static CssPx hev_padding_edge_origin(lxb_dom_element_t *el, bool vertical)
{
    FlowPoint o = flow_border_box_origin(el);
    CssLength b = css_computed_length(el, vertical ? "border-top-width" : "border-left-width");

    DCHECK(b.kind == CSS_LENGTH_ABSOLUTE,
           "§7's offsetTop/offsetLeft read a `border-*-width` whose computed value is not an absolute length. "
           "css-backgrounds-3 §3.3's `Computed value:` line is `absolute length, snapped as a border width` and "
           "every arm of that derivation produces one, so a percentage or a keyword here is a computed-value "
           "rule that did not run");
    return css_px_add(vertical ? o.y : o.x, b.px);
}

/* §7's `offsetTop`/`offsetLeft`, in the spec's own three steps. Every number below is UNSCALED by construction
   rather than by undoing anything: CSS Viewport §4 makes the unscaled value "the scaled value divided by the
   element's effective zoom", every effective zoom in this model is 1, and a used value is already in CSS
   pixels. "Ignoring any transforms that apply to the element and its ancestors" is satisfied for the same kind
   of reason and is the ONE place §7 is easier than §6: §6's `getClientRects()` step 3 must APPLY the transforms
   and therefore crashes for want of a computed `transform`, while §7 must IGNORE them and a flow-laid-out
   border box already has none applied. */
static JSValue hev_offset_position(JSContext *ctx, const HevTarget *t, bool vertical)
{
    lxb_dom_element_t *el = lxb_dom_interface_element(t->node);
    lxb_dom_element_t *op;
    FlowPoint o;

    /* step 1 */
    if (t->is_body || !t->has_box) return element_view_length_long(ctx, css_px(0.0));
    /* step 2 */
    op = hev_offset_parent(t);
    o = flow_border_box_origin(el);
    if (op == NULL) return element_view_length_long(ctx, vertical ? o.y : o.x);
    /* Known interop divergence from step 3, taken deliberately: with the body element as offsetParent and not
       positioned, Gecko (`Element::GetOffsetRect`, which adds the body's own position) and Blink
       (`LayoutBoxModelObject::AdjustedPositionRelativeTo`, which adds a non-positioned body's location) measure
       from the body's container, not its padding edge, so a page's top-level element has `offsetTop` 8 under
       the default body margin where step 3 gives 0. No css/cssom-view WPT test pins it. This answers as step 2
       does, relative to the initial containing block, which is Blink's number; Gecko's also subtracts a body
       border. */
    if (document_body_of(lxb_dom_interface_node(t->node->owner_document)) == lxb_dom_interface_node(op) &&
        !hev_establishes_abspos_containing_block(op))
        return element_view_length_long(ctx, vertical ? o.y : o.x);
    /* step 3. Both edges contain the offset parent's border width, an atom named by its definition, so it
       cancels however many derivations the two edges took. */
    return element_view_length_long(ctx, css_px_sub(vertical ? o.y : o.x,
                                                    hev_padding_edge_origin(op, vertical)));
}

/* ---- §7's `offsetWidth` and `offsetHeight` --------------------------------------------------------------- */

/* §7's two steps, and step 2's own two sentences. "Return the unscaled width of the axis-aligned bounding box
   of the border boxes of ALL FRAGMENTS generated by the element's principal box, ignoring any transforms that
   apply to the element and its ancestors" — for a principal box that generates ONE fragment the bounding box of
   one border box IS that border box, with no comparison performed. The fragment count is
   core/dom/element_view.h's one derivation.
   Note that §7's step 1 does NOT exclude an inline box the way §6's client extents do — `span.clientWidth` is 0
   in every user agent and `span.offsetWidth` is the width of its line fragments — so the inline arm below is a
   missing capability rather than a zero, and writing a zero there would be the stub. */
static JSValue hev_offset_extent(JSContext *ctx, const HevTarget *t, bool vertical)
{
    lxb_dom_element_t *el = lxb_dom_interface_element(t->node);
    ElementViewFragments kind;

    /* step 1 */
    if (!t->has_box) return element_view_length_long(ctx, css_px(0.0));
    /* step 2's count, decided before its extent */
    kind = element_view_fragment_kind(el);
    if (kind == ELEMENT_VIEW_FRAGMENTS_LINE_BOXES) {
        /* §7's step 2 for an INLINE box: "the axis-aligned bounding box of the border boxes of ALL FRAGMENTS
           generated by the element's principal box", which CSS 2 §9.4.2 "Inline formatting contexts" makes one
           per line box the box spans. core/layout/flow_position.h answers that list, in the initial
           containing block's space, which ignores transforms as §7 asks; core/dom/element_view.h's
           `element_view_enclosing_box_px` is the union, the one §6's get the bounding box takes.
           STEP 2'S SECOND SENTENCE IS UNREACHABLE HERE, AND THE REASON WRITTEN HERE WAS RETIRED IN THE FILE IT
           NAMED. CSSOM VIEW §7: "If the element's principal box is an inline-level box which was \"split\" by a
           block-level descendant, also include fragments generated by the block-level descendants, unless they
           are zero width or height" — CSS 2 §9.2.1.1 "Anonymous block boxes"' second paragraph, quoted here
           WITHOUT its closing clause and attributed to §6 for as long as that stood. This line used to say
           core/layout/line_box.c crashed for such a box before any fragment of it existed, and that crash is
           GONE: its own comment records that §9.2.1.1's second paragraph is now RUN, the block-level box being
           the exclusive end of the run the walk stops at. What makes the sentence unreachable TODAY is a
           different crash at a different place — line_box.c refuses the ADDRESSING rather than the measurement,
           because a broken child sits in several of its container's anonymous block boxes at once and one run
           can name only one of them. The conclusion survived its argument, which is the worse of the two states:
           whoever builds that lookup must come back HERE, since this member would then answer one run's extent
           and §7 asks for the union over all of them. */
        FlowRect *frags = NULL;
        size_t n = flow_inline_fragment_rects(el, &frags), i;
        CssPx (*r)[4], box[4];

        DCHECK(n > 0, "an inline box with a box reported no line fragment; flow_position.h answers its origin "
                      "from the first");
        r = lexbor_malloc(n * sizeof *r);
        CHECK(r != NULL, "§7's offsetWidth/offsetHeight could not allocate its fragment list");
        for (i = 0; i < n; i++) {
            r[i][0] = frags[i].x;
            r[i][1] = frags[i].y;
            r[i][2] = frags[i].width;
            r[i][3] = frags[i].height;
        }
        free(frags);
        element_view_enclosing_box_px((const CssPx (*)[4])r, n, box);
        free(r);
        return element_view_length_long(ctx, vertical ? box[3] : box[2]);
    }
    /* A table element's principal box is CSS 2.1 §17.4's table wrapper box, one fragment, and its border box is
       its content box (the wrapper takes the initial border and padding): "The width of the table wrapper box
       is the border-edge width of the table box inside it", and its height is its stack of captions and table
       box. */
    if (kind == ELEMENT_VIEW_FRAGMENTS_TABLE)
        return element_view_length_long(ctx, vertical ? block_flow_table_wrapper_height(el)
                                                      : used_value_border_edge_px(el, false));
    /* step 2, for a principal box of one fragment */
    return element_view_length_long(ctx, used_value_border_edge_px(el, vertical));
}

/* ---- the members and the per-realm install --------------------------------------------------------------- */

/* The layout `member` reads, asked under the caller's resting drive on its entered job: true once
   `hev_member` below computes nothing that drive would defer, false ("not yet") with the question suspended.
   The asks are the ones each member's steps reach, in their order: `offsetParent` reads computed values only;
   `offsetTop`/`offsetLeft` the element's border-box origin and its offset parent's; `offsetWidth`/`offsetHeight`
   the line fragments of an inline box, a table wrapper's height, or the border edge. */
static bool hev_member_ask(const HevTarget *t, HtmlElementViewMember member)
{
    lxb_dom_element_t *el = lxb_dom_interface_element(t->node), *op;
    bool vertical = member == HEV_OFFSET_TOP || member == HEV_OFFSET_HEIGHT;
    FlowPoint o;
    FlowRect *frags = NULL;
    size_t n;
    CssPx px;
    bool ok;

    switch (member) {
    case HEV_OFFSET_PARENT:
        return true;
    case HEV_OFFSET_TOP:
    case HEV_OFFSET_LEFT:
        if (t->is_body || !t->has_box) return true;
        if (!flow_border_box_origin_ask(el, &o)) return false;
        op = hev_offset_parent(t);
        return op == NULL || flow_border_box_origin_ask(op, &o);
    case HEV_OFFSET_WIDTH:
    case HEV_OFFSET_HEIGHT:
        if (!t->has_box) return true;
        switch (element_view_fragment_kind(el)) {
        case ELEMENT_VIEW_FRAGMENTS_LINE_BOXES:
            ok = flow_inline_fragment_rects_ask(el, &frags, &n);
            free(frags);
            return ok;
        case ELEMENT_VIEW_FRAGMENTS_TABLE:
            return vertical ? block_flow_table_wrapper_height_ask(el, &px)
                            : used_value_border_edge_ask(el, false, &px);
        case ELEMENT_VIEW_FRAGMENTS_ONE:
            return used_value_border_edge_ask(el, vertical, &px);
        }
        DFAIL("§7's offsetWidth/offsetHeight was asked over a fragment kind core/dom/element_view.h does not "
              "declare");
        return true;
    }
    DFAIL("a CSSOM VIEW §7 member was asked with a magic no member of this file declares");
    return true;
}

/* The member's answer, on the job its asks answered on. */
static JSValue hev_member(JSContext *ctx, const HevTarget *t, HtmlElementViewMember member)
{
    lxb_dom_element_t *op;

    switch (member) {
    case HEV_OFFSET_PARENT:
        op = hev_offset_parent(t);
        /* `readonly attribute Element? offsetParent` — the wrapper is minted in the ELEMENT's relevant realm,
           which for an element is the realm of its node document, so `iframe.contentDocument.body.offsetParent`
           read through the PARENT's HTMLElement.prototype is an instance of the CHILD's Element. */
        return op ? node_wrap(t->rctx, lxb_dom_interface_node(op)) : JS_NULL;
    case HEV_OFFSET_TOP:    return hev_offset_position(ctx, t, true);
    case HEV_OFFSET_LEFT:   return hev_offset_position(ctx, t, false);
    case HEV_OFFSET_WIDTH:  return hev_offset_extent(ctx, t, false);
    case HEV_OFFSET_HEIGHT: return hev_offset_extent(ctx, t, true);
    }
    DFAIL("a CSSOM VIEW §7 member was read with a magic no member of this file declares");
    return JS_UNDEFINED;
}

/* §7's five members as one step machine owning a layout job, as core/dom/element_view.c's EV_CLIENT_STEP: the
   box tests' author matches (the element's and its ancestors', which the offsetParent walk and the chain check
   read too) are asked under a resting drive, then the member's layout (`hev_member_ask`), each activation one
   rest unit; the member is answered on the same job once both are. A selector the cascade cannot decide forks
   the flow (layout_job_fork). */
#define HEV_STAGES(X) \
    X(HEV_MEASURE, "CSSOM VIEW §7 \"Extensions to the HTMLElement Interface\" offsetParent / offsetTop / " \
                   "offsetLeft / offsetWidth / offsetHeight (the box tests and the member's layout under a " \
                   "resting drive, then the member on the same job)")
enum { IDL_STEP_STAGE_BASE(HEV_STAGES) HEV_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const HEV_STEPS[] = { HEV_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    LayoutJob *job;   /* owned, declared through layout_job_step_ops; NULL before the first activation */
} HevState;

static void hev_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    HevState *s = st;

    v->tree(ctx, (void **)&s->job, NULL, 0, &layout_job_step_ops);
}

static int js_hev_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                       JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    HevState *s = st;
    HtmlElementViewMember member = (HtmlElementViewMember)idl_step_magic(hdr);
    lxb_dom_element_t *el = element_of_value(hdr->this_val);
    bool matched, done = false;
    LayoutJobDrive d;
    HevTarget t;
    int rc;

    (void)argc; (void)argv; (void)cb_result; (void)out_cb; (void)out_argc;
    STEP_DISPATCH(HEV_STAGES, hdr->stage, hdr->def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(HEV_MEASURE);
    DCHECK(el != NULL, "a CSSOM VIEW §7 member reached its body on a receiver that is not an element — each of "
                       "the five states html_element_is at its declaration");
    if (s->job == NULL) s->job = layout_job_new();
    /* An entry the job's fork delivers to: its arm is recorded before the drive runs again. */
    rc = layout_job_fork(ctx, hdr, s->job);
    if (rc) return rc;
    rest_unit_begin(REST_UNIT_LAYOUT_DESCENDANTS);
    layout_job_enter(s->job);
    layout_job_drive_begin_resting(&d);
    matched = element_view_box_tests_ask(lxb_dom_interface_node(el));
    DCHECK(matched || layout_job_drive_spent(&d),
           "a CSSOM VIEW §7 member's box tests parked an author match with the drive's allowance unspent, so its "
           "next activation would ask the same thing over the same state for ever");
    if (matched) {
        hev_target(hdr->this_val, &t);
        /* The chain check runs for every member: `offsetWidth` reads no ancestor, yet CSS Viewport §4 makes its
           unscaled value a function of every flat-tree ancestor's `zoom`. */
        hev_require_readable_chain(t.node);
        while (block_flow_drive_deferred(&d)) {
            /* HTML §15.4.2's rule 5 label is measured ahead of the member's read, which takes it from the
               job's record. */
            if (replaced_element_label_ask(el) && hev_member_ask(&t, member)) {
                done = true;
                break;
            }
            DCHECK(layout_job_drive_spent(&d) || layout_job_drive_pending(&d),
                   "a CSSOM VIEW §7 member's layout answered \"not yet\" with no question suspended and no "
                   "allowance spent, so its next activation would ask the same thing over the same state for ever");
        }
        if (done) *presult = hev_member(ctx, &t, member);
    }
    layout_job_drive_end(&d);
    layout_job_leave(s->job);
    rest_unit_end();
    if (layout_job_declined(s->job)) {
        if (done) JS_FreeValue(ctx, *presult);
        rc = layout_job_fork(ctx, hdr, s->job);
        return rc ? rc : JS_STEP_YIELD;
    }
    if (!done) {
        layout_job_rested(s->job);
        return JS_STEP_YIELD;
    }
    return JS_STEP_DONE;
}

static const IdlStepDecl HEV_STEP = {
    js_hev_step, sizeof(HevState), hev_visit, NULL,
    "CSSOM VIEW §7 \"Extensions to the HTMLElement Interface\" offsetParent / offsetTop / offsetLeft / "
    "offsetWidth / offsetHeight",
    HEV_STEPS
};

static int g_id_hev[HEV_OFFSET_HEIGHT + 1] = { -1, -1, -1, -1, -1 };

void html_element_view_init(JSContext *ctx)
{
    int m;

    DCHECK(g_id_hev[HEV_OFFSET_PARENT] < 0,
           "html_element_view_init ran twice — §7's getters are declared once per agent");
    /* WEB IDL §3.7.6 "Attributes"' RECEIVER TEST, stated with each declaration and performed before the step
       runs. §7's members are a `partial interface HTMLElement`, so the predicate is core/html/html_element.h's
       own `html_element_is` — an element in the HTML namespace, which is what makes `svgEl.offsetWidth`
       `undefined` and `HTMLElement.prototype`'s getter applied to it a TypeError. */
    for (m = HEV_OFFSET_PARENT; m <= HEV_OFFSET_HEIGHT; m++) {
        g_id_hev[m] = idl_getter_id_step(ctx, &HEV_STEP, m);
        idl_this_iface(html_element_is, "HTMLElement");
    }
}

void html_element_view_install(JSContext *ctx, JSValueConst proto)
{
    DCHECK(g_id_hev[HEV_OFFSET_PARENT] >= 0,
           "§7's members were installed in a realm before html_element_view_init declared them");
    idl_install_accessor_step(ctx, proto, "offsetParent", g_id_hev[HEV_OFFSET_PARENT], -1);
    idl_install_accessor_step(ctx, proto, "offsetTop",    g_id_hev[HEV_OFFSET_TOP],    -1);
    idl_install_accessor_step(ctx, proto, "offsetLeft",   g_id_hev[HEV_OFFSET_LEFT],   -1);
    idl_install_accessor_step(ctx, proto, "offsetWidth",  g_id_hev[HEV_OFFSET_WIDTH],  -1);
    idl_install_accessor_step(ctx, proto, "offsetHeight", g_id_hev[HEV_OFFSET_HEIGHT], -1);
}

void html_element_view_free(void)
{
    /* The member ids are the agent's, and the pool they live in goes with the runtime. */
    for (int m = HEV_OFFSET_PARENT; m <= HEV_OFFSET_HEIGHT; m++) g_id_hev[m] = -1;
}
