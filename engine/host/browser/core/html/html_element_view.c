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

/* WEB IDL §3.7.6 Attributes' BRAND CHECK, and it is over HTMLElement rather than over Element — which is the
   whole reason
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

/* ---- the inputs §7 branches on and this engine cannot read ---------------------------------------------- */

/* WHICH QUESTION EACH UNREADABLE PROPERTY IS AN OPERAND OF — AND THEY ARE TWO QUESTIONS THAT USED TO BE ASKED
   AS ONE. All four below are properties core/css/css_computed_value.h does not derive, and ONE list of them
   gated every member of §7 alike, so an ancestor DECLARING `transform` aborted `offsetHeight`. That is
   CLAUDE.md's two-question predicate exactly: the STRICTER question (`offsetParent`'s walk over positioning
   containing blocks) decided it, the LOOSER one (an extent, which walks no ancestor at all) paid, and the cost
   was silent because a refusal reads as correctness rather than as loss. The two are split here, each list
   stating what it is an operand OF, and the walk below is told which question its caller is asking. */

/* AN OPERAND OF THE POSITIONING QUESTION ONLY. Neither property reaches a used value except through
   css-position-3 §2.1 "Containing Blocks of Positioned Boxes"' positioning containing block, and each says so in
   its own section. css-transforms-1 §2 "The Transform Rendering Model": "For elements whose layout is governed
   by the CSS box model, the transform property does not affect the flow of the content surrounding the
   transformed element". css-will-change-1 §2 "Hinting at Future Behavior: the will-change property" states the
   whole of the other one: "The will-change property has no direct effect on the element it is specified on,
   beyond the creation of stacking contexts and containing blocks as specified above" — three normative effects,
   a stacking context and the two containing blocks, and nothing else. CSSOM VIEW §7 says it a third time for
   the two members that read no ancestor: an extent is returned "ignoring any transforms that apply to the
   element and its ancestors", so the transform never enters the number even where one applies. */
static const char *const HEV_UNREADABLE_POSITIONING[] = { "transform", "will-change" };

/* AN OPERAND OF EVERY QUESTION §7 ASKS, INCLUDING THE TWO MEMBERS THAT READ NO ANCESTOR. `contain` is in §2.1's
   list too and does NOT stop there, which is what keeps it here rather than above: css-contain-2 §3.1 "Size
   Containment" makes it a fact about the box's OWN size rather than about anyone's containing block — "The
   intrinsic sizes of the size containment box are determined as if the element had no content, following the
   same logic as when sizing as if empty" — so an extent is a function of it directly. `zoom` is CSS Viewport §4
   "The zoom property": every number §7 reports is an UNSCALED one, and "the unscaled value of a CSS length
   relative to an element is the scaled value divided by the element's effective zoom", whose product runs over
   the whole flat-tree chain. */
static const char *const HEV_UNREADABLE_ALWAYS[] = { "contain", "zoom" };

/* WHICH OF THE TWO A CALLER IS ASKING. It is a fact about the MEMBER and never about the element: `offsetParent`
   IS the positioning walk, `offsetTop`/`offsetLeft` are DEFINED by subtracting from its result, and
   `offsetWidth`/`offsetHeight` name it nowhere in either of their two steps. */
typedef enum {
    HEV_CHAIN_POSITIONING,  /* the member reads a §2.1 positioning containing block itself */
    HEV_CHAIN_EXTENT        /* the member reads only the border boxes of its own principal box's fragments */
} HevChainQuestion;

/* DOES THIS ELEMENT DECLARE THE PROPERTY? The CASCADED value, which is a DETECTOR for an input this component
   cannot read and never a value it uses — its only outcome is the crash. */
static bool hev_declares(lxb_dom_element_t *el, const char *name)
{
    char *decl = cssom_cascaded_value(el, name);
    bool declared = decl != NULL;

    free(decl);
    return declared;
}

/* IS §2.1's POSITIONING CONTAINING BLOCK AN OPERAND OF ANY USED VALUE THIS ELEMENT'S BORDER BOX IS DERIVED
   FROM? §2.1's opening sentence is the whole answer, and it is a SCOPE statement rather than a rule: "The
   containing block of a static, relative, or sticky box is as defined by its formatting context. For fixed and
   absolute boxes, it is defined as follows". So the properties §2.1 lists change a used value only where some
   box on the chain is `absolute` or `fixed`; for every other box the containing block comes from the formatting
   context, which §2.1 does not touch.
   THE ANCESTOR CHAIN IS A SOUND OVER-APPROXIMATION OF THE CONTAINING-BLOCK CHAIN, which is what this question
   needs of it and all it needs: §2.1's own "nearest ancestor box that establishes" and CSS 2.1 §10.1 "Definition
   of 'containing block'"' formatting-context cases both select an ANCESTOR, so no containing block in the chain
   is outside this walk. The error is one-sided — a false TRUE costs one crash naming a real absence, and there
   is no false FALSE. */
static bool hev_chain_reads_positioning_cb(lxb_dom_node_t *n)
{
    lxb_dom_node_t *a;

    for (a = n; a != NULL && a->type == LXB_DOM_NODE_TYPE_ELEMENT; a = a->parent) {
        lxb_dom_element_t *el = lxb_dom_interface_element(a);

        if (hev_computed_is(el, "position", "absolute") || hev_computed_is(el, "position", "fixed"))
            return true;
    }
    return false;
}

/* THE CHAIN, CHECKED ONCE BEFORE ANY OF §7's FACTS IS READ. The absences are asked about together because they
   are the same failure — an input the walk below cannot see — and because all of them are decided over the same
   ancestor chain.
   THE SHADOW QUESTION IS FIRST BECAUSE IT IS WHAT MAKES THIS CHAIN THE RIGHT ONE, and it is asked for EVERY
   question rather than only the positioning one. §7 walks the FLAT TREE (CSS Scoping §4.1 "Flattening the DOM
   into an Element Tree"), and the node tree is that tree exactly when no element on the chain hosts a shadow
   root and the element's root is not one — a slotted element's flat-tree parent is its assigned SLOT, which the
   node tree does not have it under at all. That reaches an EXTENT too, because CSS Viewport §4's effective zoom
   is a product over "all flat tree ancestors" and a shadow tree's elements are not node-tree ancestors of a
   slotted element. Neither the flat tree nor DOM §4.8's `closed-shadow-hidden` exists in this engine, so a tree
   with a shadow root in it crashes naming both rather than being walked in the tree §7 does not name. */
static void hev_require_readable_chain(JSContext *ctx, lxb_dom_node_t *n, HevChainQuestion q)
{
    lxb_dom_node_t *a, *root = n;
    const char *positioning = NULL;
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
                  "closed-shadow-hidden over three conditions — 'A's root is a shadow root', 'A's root is not "
                  "a shadow-including inclusive ancestor of B', and 'A's root is a shadow root whose mode is "
                  "`closed` or A's root's host is closed-shadow-hidden from B' — which decide whether an "
                  "ancestor is visible to this element at all. THE THIRD CONDITION IS RECURSIVE, and reading a "
                  "mode alone is a WRONG answer rather than a narrower one: an element under an OPEN shadow "
                  "root whose HOST sits under a CLOSED one IS closed-shadow-hidden, and the mode says `open` "
                  "at every step of that walk. BUILD the flat-tree parent over DOM §4.2.2's assigned slot and "
                  "slottable, then closed-shadow-hidden as the disjunction the section states — "
                  "shadow_root_is_open's mode test OR the same predicate re-asked at shadow_root_host's host");
        for (i = 0; i < sizeof HEV_UNREADABLE_ALWAYS / sizeof HEV_UNREADABLE_ALWAYS[0]; i++)
            if (hev_declares(el, HEV_UNREADABLE_ALWAYS[i]))
                DFAILF("an element on this CSSOM VIEW §7 chain DECLARES `%s`, whose computed value "
                       "core/css/css_computed_value.h does not derive, and EVERY member of §7 is a function of "
                       "it — the two that walk no ancestor included, which is why this one is asked whatever "
                       "the caller is asking. `contain`: css-contain-2 §3.1 \"Size Containment\" makes it a fact "
                       "about the box's own size rather than about a containing block, \"The intrinsic sizes of "
                       "the size containment box are determined as if the element had no content, following the "
                       "same logic as when sizing as if empty\", so an EXTENT is derived from a value that was "
                       "never computed; it is also in css-position-3 §2.1 \"Containing Blocks of Positioned "
                       "Boxes\"' list, so it moves `offsetParent` as well. `zoom`: CSS Viewport §4 \"The zoom "
                       "property\" states \"the effective zoom of an element is the product of its computed "
                       "value of zoom and all flat tree ancestors' computed values of zoom\" and \"the unscaled "
                       "value of a CSS length relative to an element is the scaled value divided by the "
                       "element's effective zoom\", and every extent and coordinate §7 reports is an UNSCALED "
                       "one, so all five are off by that product and offsetParent's own "
                       "different-effective-zoom bullet can fire. BUILD this property's `Computed value:` line "
                       "in css_computed_value.c and record its shorthands in css_shorthand.c",
                       HEV_UNREADABLE_ALWAYS[i]);
        for (i = 0; positioning == NULL && i < sizeof HEV_UNREADABLE_POSITIONING /
                                               sizeof HEV_UNREADABLE_POSITIONING[0]; i++)
            if (hev_declares(el, HEV_UNREADABLE_POSITIONING[i])) positioning = HEV_UNREADABLE_POSITIONING[i];
        root = a;
    }
    if (root->parent != NULL && shadow_root_is(root->parent))
        DFAIL("CSSOM VIEW §7's walk is over the FLAT TREE and this element's root IS A SHADOW ROOT, so the walk "
              "would leave the tree at the shadow root rather than continuing through its HOST, which is where "
              "CSS Scoping §4.1's flattening puts the shadow tree's children. BUILD the flat-tree parent (the "
              "host, for the shadow root's own children) together with DOM §4.8's closed-shadow-hidden, which "
              "is the other half §7's every bullet is qualified by");
    /* THE POSITIONING LIST IS ASKED LAST BECAUSE ITS SECOND OPERAND IS THE WHOLE CHAIN. A declaration alone is
       not an unreadable INPUT here: it is one only where §2.1's containing block is consulted, which for an
       EXTENT is decided by the chain and not by this element. The walk above records the declaration and the
       scope question is asked once, and only where a declaration was found — so an ordinary page whose
       ancestors carry no such declaration never pays for the second walk. */
    if (positioning != NULL && (q == HEV_CHAIN_POSITIONING || hev_chain_reads_positioning_cb(n)))
        DFAILF("an element on this CSSOM VIEW §7 chain DECLARES `%s`, whose computed value "
               "core/css/css_computed_value.h does not derive, and this member READS css-position-3 §2.1 "
               "\"Containing Blocks of Positioned Boxes\"' positioning containing block — either because it is "
               "`offsetParent` or one of the two defined against it, or because a box on this chain is "
               "`absolute` or `fixed` and §2.1 therefore decides a used value the answer is built out of. §2.1's "
               "Note is the list: \"Properties that can cause a box to establish an absolute positioning "
               "containing block include position, transform, will-change, contain …\", and the same for a FIXED "
               "positioning containing block minus `position` — so with one of them declared, `offsetParent`'s "
               "two containing-block bullets and its `position: fixed` step 1 are decided by a value that was "
               "never derived. BUILD this property's `Computed value:` line in css_computed_value.c and record "
               "its shorthands in css_shorthand.c; §2.1's list ends in an ellipsis, so record the COMPLETE set "
               "of properties that establish either containing block while you are there rather than the three "
               "it names",
               positioning);
}

/* ---- §7's `offsetParent` --------------------------------------------------------------------------------- */

/* CSSOM VIEW §7's "ancestor is a containing block of absolutely-positioned descendants (regardless of whether
   there are any absolutely-positioned descendants)". css-position-3 §2.1 "Containing Blocks of Positioned
   Boxes"' own Note lists what can establish one — "Properties that can cause a box to establish an absolute
   positioning containing block include position, transform, will-change, contain …" — and the chain check above
   has already crashed if any of the last three is declared, so what is left is `position`.
   WHICH OF ITS VALUES IS STATED BY css-position-3 §2 "Choosing A Positioning Scheme: position property", ONE
   SENTENCE AND NOT A DERIVATION OVER THREE NAMES: "Values other than static make the box a positioned box, and
   cause it to establish an absolute positioning containing block for its descendants". So this is written as
   the complement of `static`, which is what the sentence says, and `sticky` needs no separate arm. A PARAPHRASE
   IN QUOTATION MARKS STOOD HERE and cited §2.1 for it — "a box that establishes an ABSOLUTE POSITIONING
   CONTAINING BLOCK" occurs in no section of that standard, and the words that DO settle this are one section
   over, which is why the citation moved with them rather than only the marks coming off. */
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
    if (kind == ELEMENT_VIEW_FRAGMENTS_LINE_BOXES) {
        /* §7's step 2 for an INLINE box: "the axis-aligned bounding box of the border boxes of ALL FRAGMENTS
           generated by the element's principal box", which CSS 2 §9.4.2 "Inline formatting contexts" makes one
           per line box the box spans. core/layout/flow_position.h answers that list.
           THE BOUNDING BOX OF ONE FRAGMENT IS THAT FRAGMENT, with no comparison performed — the same
           derivation this member already makes for a principal box of one fragment, and the reason it can
           answer at all while §6's getBoundingClientRect crashes for a list of more than one.
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
        size_t n = flow_inline_fragment_rects(el, &frags);
        CssPx extent = vertical ? frags[0].height : frags[0].width;

        free(frags);
        if (n > 1)
            DFAIL("CSSOM VIEW §7's offsetWidth/offsetHeight step 2 is the AXIS-ALIGNED BOUNDING BOX over ALL "
                  "of a principal box's fragments, and CSS 2 §9.4.2 \"Inline formatting contexts\" split this "
                  "inline box across SEVERAL line boxes — \"when an inline box exceeds the width of a line "
                  "box, it is split into several boxes and these boxes are distributed across several line "
                  "boxes\". THE FRAGMENTS ARE BUILT AND THIS LINE USED TO SAY THEY WERE NOT: it named the "
                  "per-glyph ADVANCE as missing, which core/css/font_metrics.h answers for every Unicode "
                  "scalar value, and §9.4.2's line boxes as missing, which `line_box_inline_fragments` builds "
                  "over it — following it would have built both a second time. WHAT IS LEFT IS THE UNION, and "
                  "it is the SAME BUILD core/dom/element_view.c crashes for at §6's get-the-bounding-box steps "
                  "3 and 4: a bounding box over rectangles whose numbers are concolics derived from the "
                  "initial containing block, so the minimum and maximum may not be C branches on the examples "
                  "(that deletes the arm a responsive bundle explores) and both go through Geometry Interfaces "
                  "§3's NaN-safe derived edges. BUILD it ONCE over CssPx and let §6's two entries and this one "
                  "call it — three members, one union, or three chances to disagree about one rectangle");
        return element_view_length_long(ctx, extent);
    }
    if (kind == ELEMENT_VIEW_FRAGMENTS_TABLE)
        DFAIL("CSSOM VIEW §7's offsetWidth/offsetHeight step 2 is over the fragments of the PRINCIPAL BOX, and "
              "an element whose computed `display` is `table` or `inline-table` has the box structure CSS 2.1 "
              "§17.2.1 Anonymous table objects generates — a table box, an optional caption box and the "
              "anonymous container around them. TWO OF THOSE THREE ARE BUILT and this line used to say none "
              "was: core/layout/table_box.h answers §17.2.1's first two stages and the caption boxes CSS 2.1 "
              "§17.4 Tables in the visual formatting model puts beside the table box. The container is that "
              "section's TABLE WRAPPER BOX — \"the table generates a principal block box called the table "
              "wrapper box that contains the table box itself and any caption boxes\" — and no element in the "
              "tree names it — THOUGH §17.4's SPLIT IS ANSWERED AND THIS LINE USED TO ASK FOR IT: "
              "core/layout/table_wrapper.h says which declarations land on the wrapper and which on the table "
              "box, and whether the wrapper is block-level. Its EXTENTS are not §10's either, AND BOTH AXES "
              "ARE NOW ANSWERED — this line used to name the block axis as the half still missing, and then "
              "told its reader to BUILD it. CSS 2.1 §17.5.2 Table width algorithms: the 'table-layout' "
              "property owns the table's WIDTH and CSS 2.1 §17.5.3 Table height algorithms owns its HEIGHT, "
              "and BOTH are components (core/layout/table_width.h, core/layout/table_height.h) that "
              "core/layout/used_value.c routes a table box to on the declared arm and the `auto` arm alike — "
              "each section takes the declaration as an INPUT to its own comparison rather than as the used "
              "value, so there is no declared-height arm left for it to crash in; CSS 2.1 §17.4 Tables in the "
              "visual formatting model states the wrapper's own width over the first number. WHAT IS STILL MISSING IS STEP 2's OWN ENUMERATION AND NOT AN "
              "EXTENT: the principal box here is the WRAPPER, whose fragments are the table box and every "
              "caption box CSS 2.1 §17.4 Tables in the visual formatting model renders \"as normal block "
              "boxes inside the table wrapper box\", and this entry answers ONE extent. Assemble the "
              "wrapper's from those, the way CSSOM VIEW §6's entry has to "
              "assemble its rectangle list");
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
    /* THE CHAIN CHECK RUNS FOR EVERY MEMBER, INCLUDING THE TWO THAT DO NOT WALK — BUT NOT WITH THE SAME
       QUESTION, WHICH IS THE WHOLE OF WHAT THE ARGUMENT BELOW DECIDES. `offsetWidth` reads no ancestor, and
       CSS Viewport §4 still makes its UNSCALED value a function of every flat-tree ancestor's `zoom`, so the
       chain IS an operand of it and the shadow half is what makes this chain the right one to have asked.
       What is NOT an operand of it is css-position-3 §2.1's positioning containing block: §7 names it in
       `offsetParent`'s steps and in neither of the extents', so those two ask HEV_CHAIN_EXTENT and the chain
       decides for itself whether §2.1 is consulted anywhere on it. */
    hev_require_readable_chain(t.rctx, t.node,
                               (magic == HEV_OFFSET_WIDTH || magic == HEV_OFFSET_HEIGHT)
                                   ? HEV_CHAIN_EXTENT : HEV_CHAIN_POSITIONING);
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
