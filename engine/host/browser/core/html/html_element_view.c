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
#include "core/dom/node.h"
#include "core/dom/shadow_root.h"
#include "core/html/html_element.h"
#include "core/html/html_element_view.h"
#include "core/idl_args.h"
#include "core/layout/flow_position.h"
#include "core/layout/used_value.h"

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

/* WEB IDL §3.7.5's BRAND CHECK, and it is over HTMLElement rather than over Element — which is the whole reason
   this section is a file of its own. `Object.getOwnPropertyDescriptor(HTMLElement.prototype,'offsetWidth').get
   .call(svgEl)` is a TypeError in every user agent and `svgEl.offsetWidth` is `undefined`, and a page that
   feature-detects by applying the getter tells the throw apart from both. A THROW and not an assert: the
   receiver is the PAGE's input, and an abort there is this engine crashing on a line of ordinary JavaScript. */
static bool hev_target(JSContext *ctx, JSValueConst this_val, const char *member, HevTarget *t)
{
    lxb_dom_element_t *el;

    if (!html_element_is(this_val)) {
        JS_ThrowTypeError(ctx, "HTMLElement.%s was reached on something that is not an HTML element", member);
        return false;
    }
    el = element_of_value(this_val);
    DCHECK(el != NULL, "an HTML element wrapper had no element behind it — html_element_is has already asked "
                       "node_of for an ELEMENT node in the HTML namespace, so the two answers cannot disagree");
    hev_target_of_element(el, t);
    return true;
}

static bool hev_computed_is(lxb_dom_element_t *el, const char *name, const char *kw)
{
    char *v = css_computed_value(el, name);
    bool same;

    DCHECK(v != NULL, "the cascade produced no computed value for a property this engine models — every one of "
                      "them is in lexbor's registry with an initial value, so the last layer always answers");
    same = strcmp(v, kw) == 0;
    free(v);
    return same;
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

/* ---- the two inputs §7 branches on and this engine cannot read ------------------------------------------- */

/* css-position-3 §2.1 "Containing Blocks of Positioned Boxes" and CSS Viewport §4 "The zoom property" — the
   properties §7's walk and §7's "unscaled" are functions of and core/css/css_computed_value.h does not model.
   `position` is deliberately NOT in this list: it IS modelled, and it is the one this file reads. */
static const char *const HEV_UNREADABLE[] = { "transform", "will-change", "contain", "zoom" };

/* THE CHAIN, CHECKED ONCE BEFORE ANY OF §7's FACTS IS READ. Two absences are asked about together because they
   are the same failure — an input the walk below cannot see — and because both are decided over the same
   ancestor chain.
   THE SHADOW QUESTION IS FIRST BECAUSE IT IS WHAT MAKES THIS CHAIN THE RIGHT ONE. §7 walks the FLAT TREE (CSS
   Scoping §4.1 "Flattening the DOM into an Element Tree"), and the node tree is that tree exactly when no
   element on the chain hosts a shadow root and the element's root is not one — a slotted element's flat-tree
   parent is its assigned SLOT, which the node tree does not have it under at all. Neither the flat tree nor DOM
   §4.8's `closed-shadow-hidden` exists in this engine, so a tree with a shadow root in it crashes naming both
   rather than being walked in the tree §7 does not name. */
static void hev_require_readable_chain(JSContext *ctx, lxb_dom_node_t *n)
{
    lxb_dom_node_t *a, *root = n;
    size_t i;

    for (a = n; a != NULL && a->type == LXB_DOM_NODE_TYPE_ELEMENT; a = a->parent) {
        lxb_dom_element_t *el = lxb_dom_interface_element(a);

        if (shadow_root_of_element(ctx, el) != NULL)
            DFAIL("CSSOM VIEW §7's offsetParent walk is over 'the parent of the element in the FLAT TREE', and "
                  "an element on this chain HOSTS A SHADOW ROOT — so the flat tree and the node tree are "
                  "different trees here and this engine only has the second. CSS Scoping §4.1 'Flattening the "
                  "DOM into an Element Tree' is the construction: a shadow host's flat-tree children are its "
                  "shadow root's children, and a light child that is assigned to a slot appears under THAT "
                  "SLOT. Walking the node tree instead would answer a slotted element's offsetParent with an "
                  "ancestor the flat tree does not have it under — a WRONG element, not an absent one. §7's "
                  "second clause needs the other half as well: DOM §4.8 'Interface ShadowRoot' defines "
                  "closed-shadow-hidden ('A's root is a shadow root; A's root is not a shadow-including "
                  "inclusive ancestor of B; and A's root's mode is closed'), which decides whether an ancestor "
                  "is visible to this element at all. BUILD the flat-tree parent over DOM §4.2.2's assigned "
                  "slot and slottable, then closed-shadow-hidden over shadow_root.h's mode");
        for (i = 0; i < sizeof HEV_UNREADABLE / sizeof HEV_UNREADABLE[0]; i++) {
            char *decl = cssom_cascaded_value(el, HEV_UNREADABLE[i]);
            bool declared = decl != NULL;

            free(decl);
            if (declared)
                DFAIL("CSSOM VIEW §7 branches on facts this engine derives from computed values, and an element "
                      "on this chain DECLARES one of the properties whose computed value core/css/"
                      "css_computed_value.h does not model — `transform`, `will-change`, `contain` or `zoom`. "
                      "Each of them changes an answer below and none of them can be read. css-position-3 §2.1 "
                      "'Containing Blocks of Positioned Boxes' states the first three: 'properties that can "
                      "cause a box to establish an absolute positioning containing block include position, "
                      "transform, will-change, contain …' and the same for a FIXED positioning containing "
                      "block, minus `position` — so with one of them declared, `offsetParent`'s two "
                      "containing-block bullets and its `position: fixed` step 1 are all decided by a value "
                      "that was never derived. CSS Viewport §4 'The zoom property' states the fourth: 'the "
                      "effective zoom of an element is the product of its computed value of zoom and all flat "
                      "tree ancestors' computed values of zoom', and 'the unscaled value of a CSS length "
                      "relative to an element is the scaled value divided by the element's effective zoom' — so "
                      "with `zoom` declared, every UNSCALED extent and coordinate §7 reports is off by that "
                      "product and offsetParent's own different-effective-zoom bullet can fire. BUILD each "
                      "property's `Computed value:` line in css_computed_value.c and record its shorthands in "
                      "css_shorthand.c; §2.1's list ends in an ellipsis, so record the COMPLETE set of "
                      "properties that establish either containing block while you are there rather than the "
                      "three it names");
        }
        root = a;
    }
    if (root->parent != NULL && shadow_root_is(root->parent))
        DFAIL("CSSOM VIEW §7's walk is over the FLAT TREE and this element's root IS A SHADOW ROOT, so the walk "
              "would leave the tree at the shadow root rather than continuing through its HOST, which is where "
              "CSS Scoping §4.1's flattening puts the shadow tree's children. BUILD the flat-tree parent (the "
              "host, for the shadow root's own children) together with DOM §4.8's closed-shadow-hidden, which "
              "is the other half §7's every bullet is qualified by");
}

/* ---- §7's `offsetParent` --------------------------------------------------------------------------------- */

/* css-position-3 §2.1's "a box that establishes an ABSOLUTE POSITIONING CONTAINING BLOCK", which is §7's
   "ancestor is a containing block of absolutely-positioned descendants (regardless of whether there are any
   absolutely-positioned descendants)". §2.1's own list is `position, transform, will-change, contain …`, and
   the chain check above has already crashed if any of the last three is declared — so what is left is
   `position`, and CSS 2.1 §10.1's fourth case says which values: "the nearest ancestor with a position of
   'absolute', 'relative' or 'fixed'", i.e. every value but `static`. `sticky` is css-position-3's addition to
   that list and takes the same arm, which is why this is written as the complement of `static` rather than as
   three names. */
static bool hev_establishes_abspos_containing_block(lxb_dom_element_t *el)
{
    return !hev_computed_is(el, "position", "static");
}

/* §7's own first list, shared by `offsetParent` and (in the ED) `scrollParent`: the four conditions under which
   the answer is null before any walking happens. The fourth is the one this engine can state exactly — no
   element establishes a FIXED positioning containing block, because css-position-3 §2.1's list for that one is
   `transform, will-change, contain …` with `position` absent from it, and the chain check has crashed if any of
   those is declared. So a `position: fixed` element's offsetParent is null, full stop, and that is a derivation
   rather than a case left unwritten. */
static bool hev_null_offset_parent(const HevTarget *t)
{
    if (!t->has_box) return true;
    if (t->is_root)  return true;
    if (t->is_body)  return true;
    return hev_computed_is(lxb_dom_interface_element(t->node), "position", "fixed");
}

/* §7's `offsetParent` ALGORITHM, in the spec's own step order. Returns the element or NULL.
   THE ELEMENT IS NOT IN A FIXED POSITION CONTAINING BLOCK — every bullet below is qualified by that, and it is
   the derivation `hev_null_offset_parent` states: nothing in this model establishes one. So step 2's first
   bullet is unreachable, its second bullet's three conditions are the whole of the test, and its third
   (different effective zoom) is unreachable too because every effective zoom in this model is 1 (CSS Viewport
   §4). Neither is a remark: `hev_require_readable_chain` is where both are ASSERTED, and it is the one place
   they can be, because each is a statement about the whole ancestor chain rather than about this element.
   THE WALK IS OVER ELEMENTS. §7 says "the parent of the element in the flat tree", and the flat tree contains
   the document root's element and no Document node, so running out of element parents IS its "if there is no
   more parent of ancestor, terminate this algorithm and return null". */
static lxb_dom_element_t *hev_offset_parent(const HevTarget *t)
{
    lxb_dom_node_t *a;

    /* step 1 */
    if (hev_null_offset_parent(t)) return NULL;
    /* step 2 */
    for (a = t->node->parent; a != NULL && a->type == LXB_DOM_NODE_TYPE_ELEMENT; a = a->parent) {
        lxb_dom_element_t *anc = lxb_dom_interface_element(a);
        bool is_body = document_body_of(lxb_dom_interface_node(t->node->owner_document)) == a;
        bool table_cell = hev_html_local_name_is(a, "td") || hev_html_local_name_is(a, "th") ||
                          hev_html_local_name_is(a, "table");
        bool matches;

        /* Step 2's first substep — an ancestor that is closed-shadow-hidden and `position: fixed` with no
           ancestor establishing a fixed position containing block — is the shadow half, which the chain check
           has already crashed on, over the `position: fixed` half the derivation above settles. */
        matches = hev_establishes_abspos_containing_block(anc) || is_body ||
                  (table_cell && hev_computed_is(lxb_dom_interface_element(t->node), "position", "static"));
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
    /* step 3 */
    return element_view_length_long(ctx, css_px_sub(vertical ? o.y : o.x,
                                                    hev_padding_edge_origin(op, vertical)));
}

/* ---- §7's `offsetWidth` and `offsetHeight` --------------------------------------------------------------- */

/* §7's two steps, and step 2's own two sentences. "Return the unscaled width of the axis-aligned bounding box
   of the border boxes of ALL FRAGMENTS generated by the element's principal box, ignoring any transforms that
   apply to the element and its ancestors" — for a principal box that generates ONE fragment the bounding box of
   one border box IS that border box, with no comparison performed, which is why this member answers for every
   ordinary element while §6's `getBoundingClientRect` still crashes. The fragment count is
   core/dom/element_view.h's one derivation and each of its two arms crashes here naming §7's own text.
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
    if (kind == ELEMENT_VIEW_FRAGMENTS_LINE_BOXES)
        DFAIL("CSSOM VIEW §7's offsetWidth/offsetHeight step 2 is the AXIS-ALIGNED BOUNDING BOX of the border "
              "boxes of ALL FRAGMENTS the principal box generates, and an INLINE box has one fragment per LINE "
              "BOX it spans — plus step 2's own second sentence, 'if the element's principal box is an "
              "inline-level box which was split by a block-level descendant, also include fragments generated "
              "by the block-level descendants, unless they are zero width or height'. So this member's answer "
              "for an inline element is a union over line fragments, and there are no line boxes: CSS 2 §9.4.2 "
              "'Inline formatting contexts' needs the text MEASURED, and the half that is missing is the "
              "ADVANCE — core/css/font_metrics.h holds CSS 2 §10.8.1 'Leading and half-leading''s `A` and `D`, "
              "which say how TALL a line box is, and nothing says where the run breaks, which is what decides "
              "how many fragments this union is over. That is the same capability §6's getClientRects, §9's "
              "Range rectangles and CSS 2 §10.6.3's line-box arm are all waiting on. BUILD the per-glyph "
              "advance beside `A` and `D`, then §9.4.2's line boxes over it, then the union here");
    if (kind == ELEMENT_VIEW_FRAGMENTS_TABLE)
        DFAIL("CSSOM VIEW §7's offsetWidth/offsetHeight step 2 is over the fragments of the PRINCIPAL BOX, and "
              "an element whose computed `display` is `table` or `inline-table` has the box structure CSS 2 "
              "§17.2's anonymous table-object generation builds — a table box, an optional caption box and the "
              "anonymous container around them — which this engine does not build. Its EXTENTS are not §10's "
              "either: §17.5.2's two table layout algorithms own the table's width and §17.5.3 owns its height, "
              "which is why core/layout/used_value.c crashes for a table box before this member could measure "
              "one. BUILD §17.2, then §17.5's algorithms");
    /* step 2, for a principal box of one fragment */
    return element_view_length_long(ctx, used_value_border_edge_px(el, vertical));
}

/* ---- the members and the per-realm install --------------------------------------------------------------- */

static const char *hev_member_name(int magic)
{
    switch ((HtmlElementViewMember)magic) {
    case HEV_OFFSET_PARENT: return "offsetParent";
    case HEV_OFFSET_TOP:    return "offsetTop";
    case HEV_OFFSET_LEFT:   return "offsetLeft";
    case HEV_OFFSET_WIDTH:  return "offsetWidth";
    case HEV_OFFSET_HEIGHT: return "offsetHeight";
    }
    DFAIL("a CSSOM VIEW §7 member was named by a magic no member of this file declares — the magic IS the "
          "member, so an unknown one means a name was installed without a case to answer it");
    return "";
}

static JSValue js_hev_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    HevTarget t;
    lxb_dom_element_t *op;

    if (!hev_target(ctx, this_val, hev_member_name(magic), &t)) return JS_EXCEPTION;
    /* THE CHAIN CHECK RUNS FOR EVERY MEMBER, INCLUDING THE TWO THAT DO NOT WALK. `offsetWidth` reads no
       ancestor, but CSS Viewport §4 makes its UNSCALED value a function of every flat-tree ancestor's `zoom`,
       so the chain is an operand of it too — and the shadow half is what makes this chain the right one to
       have asked. */
    hev_require_readable_chain(t.rctx, t.node);
    switch ((HtmlElementViewMember)magic) {
    case HEV_OFFSET_PARENT:
        op = hev_offset_parent(&t);
        /* `readonly attribute Element? offsetParent` — the wrapper is minted in the ELEMENT's relevant realm,
           which for an element is the realm of its node document, so `iframe.contentDocument.body.offsetParent`
           read through the PARENT's HTMLElement.prototype is an instance of the CHILD's Element. */
        return op ? node_wrap(t.rctx, lxb_dom_interface_node(op)) : JS_NULL;
    case HEV_OFFSET_TOP:    return hev_offset_position(ctx, &t, true);
    case HEV_OFFSET_LEFT:   return hev_offset_position(ctx, &t, false);
    case HEV_OFFSET_WIDTH:  return hev_offset_extent(ctx, &t, false);
    case HEV_OFFSET_HEIGHT: return hev_offset_extent(ctx, &t, true);
    }
    DFAIL("a CSSOM VIEW §7 member was read with a magic no member of this file declares");
    return JS_UNDEFINED;
}

void html_element_view_install(JSContext *ctx, JSValueConst proto)
{
    idl_install_accessor(ctx, proto, "offsetParent", js_hev_get, HEV_OFFSET_PARENT, -1);
    idl_install_accessor(ctx, proto, "offsetTop",    js_hev_get, HEV_OFFSET_TOP,    -1);
    idl_install_accessor(ctx, proto, "offsetLeft",   js_hev_get, HEV_OFFSET_LEFT,   -1);
    idl_install_accessor(ctx, proto, "offsetWidth",  js_hev_get, HEV_OFFSET_WIDTH,  -1);
    idl_install_accessor(ctx, proto, "offsetHeight", js_hev_get, HEV_OFFSET_HEIGHT, -1);
}
