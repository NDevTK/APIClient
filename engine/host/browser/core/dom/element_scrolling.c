/* CSSOM VIEW §6.1 "Element Scrolling Members" — the three algorithms §6's members end in. See
   element_scrolling.h for why they are a file of their own and for why every entry here returns `void`. */
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/dom/document.h"
#include "core/dom/element.h"
#include "core/dom/element_scrolling.h"
#include "core/dom/element_view.h"
#include "core/dom/shadow_root.h"
#include "core/frame/viewport.h"
#include "core/frame/window_proxy.h"
#include "core/layout/flow_position.h"
#include "core/layout/scroll_container.h"
#include "core/layout/scrolling_area.h"
#include "core/layout/used_value.h"

/* THE AXES, NAMED — §6.1 states every one of its steps twice, once per axis, and the two statements are the
   same sentence with `x`/`width`/`left` swapped for `y`/`height`/`top`. Writing them as an index is what lets
   the alignment below be ONE function that takes the axis, which is the only shape in which the block and
   inline branches cannot drift apart. */
#define ES_X 0
#define ES_Y 1

IDL_ENUM_VALUES_EXTERN(SCROLL_LOGICAL_POSITIONS, "start", "center", "end", "nearest");

ScrollLogicalPosition element_scrolling_logical_position(const char *keyword)
{
    int i;

    DCHECK(keyword != NULL, "a ScrollLogicalPosition was mapped from no keyword — §6's `block` and `inline` "
                            "dictionary members both declare `= \"start\"`/`= \"nearest\"`, so an absent member "
                            "arrives carrying its default rather than as nothing");
    for (i = 0; SCROLL_LOGICAL_POSITIONS[i] != NULL; i++)
        if (strcmp(SCROLL_LOGICAL_POSITIONS[i], keyword) == 0) return (ScrollLogicalPosition)i;
    DFAIL("a ScrollLogicalPosition keyword reached §6.1 that is none of the four `enum ScrollLogicalPosition { "
          "\"start\", \"center\", \"end\", \"nearest\" }` declares. Web IDL §3.2.18's enumeration conversion is "
          "part of the argument's TYPE and rejects every other string with a TypeError before a body runs, so "
          "this is the declaration and this list having come apart");
    return SCROLL_LOGICAL_START;
}

/* A SCROLLING BOX, RESOLVED — everything §6.1's two position algorithms read off one, gathered at the point the
 * box is identified so neither algorithm reaches back into a component for half of it.
 *
 * WHY THE EDGES ARE IN CLIENT COORDINATES. §6.1's determine the scroll-into-view position takes the target's
 * bounding border box from `getBoundingClientRect()`, which CSSOM VIEW §6 defines in CLIENT coordinates — the
 * viewport's own space, with the viewport's current scroll already subtracted — and then compares its edges
 * against the SCROLLING BOX's edges. Two rectangles compared edge to edge must be in one space, and that space
 * is the one the spec's own step 1 hands over. A box's edges are therefore its padding box less the window
 * scroll, exactly as core/dom/element_view.c's border area is.
 *
 * WHY A POSITION IS A `double` WHILE AN EDGE IS A `CssPx`. An edge is a LAYOUT length whose chain bottoms out in
 * the initial containing block, so it carries that environment fact (core/css/css_length.h) and the arithmetic
 * below propagates it. A SCROLL POSITION is not that kind of value: element_view.h's own rule is that a fact the
 * model DERIVES stays concrete, and every scroll position in this engine is derived — the viewport has one valid
 * position and no element has ever been scrolled. It is also the type the two things that consume it speak
 * (`viewport_scroll`, `element_view_scroll_position`), so converting at this boundary rather than at three
 * call sites is what keeps one answer. */
typedef struct {
    lxb_dom_element_t *el;    /* the scroll container, or NULL when this box is the VIEWPORT */
    JSContext *dctx;          /* the realm whose ACTIVE document this box belongs to */
    CssPx  lo[2], hi[2];      /* the box's own edges, in client coordinates */
    CssPx  area[2];           /* §2's SCROLLING AREA extent on each axis */
    CssPx  extent[2];         /* the box's own extent: an element's padding edge, or the viewport's ICB */
    double cur[2];            /* its current scroll position */
    bool   ending_at_hi[2];   /* §2's OVERFLOW DIRECTIONS: is the ending edge at the larger coordinate */
} EsBox;

static void es_box_of_element(lxb_dom_element_t *el, JSContext *dctx, EsBox *b)
{
    FlowPoint o = flow_padding_box_origin(el);
    int a;

    DCHECK(scroll_container_is(el),
           "CSSOM VIEW §6.1 was handed an element as a SCROLLING BOX that establishes none — css-overflow-3 "
           "§3.1's scroll-container question (core/layout/scroll_container.h) is what the ancestor walk selects "
           "on, so a box here that is not one is that walk and this constructor having come apart");
    b->el = el;
    b->dctx = dctx;
    /* css-overflow-3 §2.3 "Scrolling Overflow": "The visual 'viewport' of a scroll container (through which the
       scrollable overflow area can be viewed) COINCIDES WITH ITS PADDING BOX, and is called the scrollport."
       That is the rectangle §6.1 aligns against, and CSSOM VIEW §2's own scrolling-area table is stated over
       the same two padding edges — one rectangle, one derivation (core/layout/flow_position.h). */
    b->lo[ES_X] = css_px_sub(o.x, css_px(viewport_window_scroll(dctx, false)));
    b->lo[ES_Y] = css_px_sub(o.y, css_px(viewport_window_scroll(dctx, true)));
    for (a = ES_X; a <= ES_Y; a++) {
        bool vertical = (a == ES_Y);

        b->extent[a] = used_value_padding_edge_px(el, vertical);
        b->hi[a] = css_px_add(b->lo[a], b->extent[a]);
        b->area[a] = scrolling_area_extent_px(el, vertical);
        b->ending_at_hi[a] = scrolling_area_ending_edge_at_higher_coordinate(el, vertical);
        b->cur[a] = element_view_scroll_position(el, vertical);
    }
}

/* css-writing-modes-4 §8 "The Principal Writing Mode"' ELEMENT — the one whose used `writing-mode` and
 * `direction` the VIEWPORT's overflow directions are those of. §8: "The principal writing mode of the document
 * is determined by the used writing-mode, direction, and text-orientation values of the root element. This
 * writing mode is used, for example, to determine the direction of scrolling"; §8.1 "Propagation to the Initial
 * Containing Block": "The principal writing mode is propagated to the initial containing block and to the
 * viewport, thereby affecting … the scrolling direction of the viewport."
 * AND ITS HTML SPECIAL CASE IS THE WHOLE REASON THIS IS A FUNCTION RATHER THAN A READ OF THE ROOT. §8: "As a
 * special case for handling HTML documents, if the root element has a body child element whose display value is
 * not none, the used value of the writing-mode and direction properties on root element are taken from the
 * COMPUTED writing-mode and direction of the first such child element instead of from the root element's own
 * values." So `<html><body dir=rtl>` gives the VIEWPORT a leftward inline-end overflow direction while the root
 * element's own computed `direction` is still `ltr` — the propagation is on USED values and §8 says in so many
 * words that it "does not affect the computed values … of the root element itself". Reading the root's computed
 * value would answer the wrong direction for the single most common way an author writes a right-to-left page.
 * WHAT IS ASKED OF THE ANSWER IS §2's OVERFLOW DIRECTIONS AND NOTHING ELSE, so it is answered here rather than
 * in core/frame/viewport.h, whose §2 column is the viewport's SCROLLING AREA — a rectangle this file does not
 * touch and which that component derives as the initial containing block on both axes, with no direction in it. */
static lxb_dom_element_t *es_principal_writing_mode_element(lxb_dom_node_t *doc)
{
    lxb_dom_node_t *root = document_document_element_of(doc);
    lxb_dom_node_t *body = document_body_of(doc);

    DCHECK(root != NULL && root->type == LXB_DOM_NODE_TYPE_ELEMENT,
           "css-writing-modes-4 §8's principal writing mode was asked of a document with no ROOT ELEMENT, while "
           "§6.1's ancestor walk had already reached that document's viewport — a viewport exists only for a "
           "document a navigable is presenting, and such a document has a root element");
    /* "…a body child element whose display value is not none". `document_body_of` is HTML §3.1.7's body
       element, which is already the FIRST `body` or `frameset` child of the root; the display half is
       core/dom/element_view.h's one predicate, which reads the computed `display` of the element and of its
       ancestors and is the same question §8's clause asks. */
    if (body != NULL && element_view_has_box(body)) return lxb_dom_interface_element(body);
    return lxb_dom_interface_element(root);
}

static void es_box_of_viewport(lxb_dom_node_t *doc, JSContext *dctx, EsBox *b)
{
    lxb_dom_element_t *pwm = es_principal_writing_mode_element(doc);
    int a;

    DCHECK(viewport_exists(dctx),
           "§6.1's ancestor walk reached the VIEWPORT of a realm that is presenting no document — the walk is "
           "entered from an element that has a box, and box existence is defined over exactly that");
    b->el = NULL;
    b->dctx = dctx;
    /* THE VIEWPORT'S EDGES IN CLIENT COORDINATES ARE THE ORIGIN AND ITS OWN SIZE, by the definition of the
       space: CSSOM VIEW §2's client coordinates are relative to the viewport's top-left. Its extent is CSS 2.1
       §10.1's INITIAL CONTAINING BLOCK, which "has the dimensions of the viewport" — core/frame/viewport.h. */
    for (a = ES_X; a <= ES_Y; a++) {
        bool vertical = (a == ES_Y);

        b->extent[a] = vertical ? viewport_icb_height(dctx) : viewport_icb_width(dctx);
        b->lo[a] = css_px(0.0);
        b->hi[a] = b->extent[a];
        b->area[a] = css_px(vertical ? viewport_scrolling_area_height(dctx)
                                     : viewport_scrolling_area_width(dctx));
        b->ending_at_hi[a] = scrolling_area_ending_edge_at_higher_coordinate(pwm, vertical);
        b->cur[a] = viewport_window_scroll(dctx, vertical);
    }
}

/* §6.1's CLAMP, which is FOUR ROWS OVER ONE FACT and is written once for both kinds of box. Scroll an element
 * to x,y states it for an element's box:
 *     "If box has rightward overflow direction — let x be max(0, min(x, element scrolling area width -
 *      element padding edge width)). If box has leftward overflow direction — let x be min(0, max(x, element
 *      padding edge width - element scrolling area width))"
 * and the two vertical rows are the same sentence on the other axis. §4's `scroll()` on a Window states the
 * viewport's rightward/downward pair identically over "the viewport's scrolling area width" and "the width of
 * the viewport", and §2 gives a scrolling box of A VIEWPORT OR ELEMENT the same two overflow directions — so
 * one function over `EsBox` is the spec's own generality rather than this file merging two rules.
 * THE COMPARISON RUNS ON THE EXAMPLE, which is the layering core/css/css_length.h states and which
 * core/dom/element_view.c's own overflow test already takes: both operands may be functions of the initial
 * containing block, so which is larger is a question the environment could answer either way, and it is decided
 * on the modelled viewport exactly as CSS 2.1 §10.3.3's slack test is. */
static double es_clamp(const EsBox *b, int axis, double v)
{
    double slack = b->area[axis].px - b->extent[axis].px;

    DCHECK(slack >= 0.0,
           "CSSOM VIEW §2's scrolling area came out SMALLER than the box it belongs to, so §6.1's clamp has an "
           "empty range. §2's table takes the ending edge as an extreme OVER the beginning edge's own box, so "
           "the area contains that box on both axes by construction");
    if (b->ending_at_hi[axis]) return fmax(0.0, fmin(v, slack));
    return fmin(0.0, fmax(v, -slack));
}

/* §6.1's PERFORM-A-SCROLL STEP, for either kind of box — the last two steps of scroll an element to x,y ("if
 * position is the same as box's current scroll position, and box does not have an ongoing smooth scroll, return
 * a resolved Promise and abort the remaining steps" / "perform a scroll of box to position") and, word for
 * word, scroll a target into view's own step 2.3 ("if position is not the same as scrolling box's current
 * scroll position, or scrolling box has an ongoing smooth scroll" / "perform a scroll of the element's
 * scrolling box to position").
 * ONE SITE FOR ONE ABSENCE. §6's `scrollTop` setter, its three scroll members and its `scrollIntoView` all end
 * here, and the crash below is the ONLY description in this engine of "an element cannot hold a scroll
 * position". A second copy of it would be a second description of one absence, and the one nobody deletes is
 * the one that goes on naming work that is already done.
 * `position` is already clamped. Returns true when the position is the one the box already has, which is
 * §6.1's own resolved-promise exit and is the answer for every box this engine can reach. */
static bool es_perform_scroll(const EsBox *b, const double position[2], const char *behavior)
{
    /* "…AND BOX DOES NOT HAVE AN ONGOING SMOOTH SCROLL" is answered FALSE for every box, derived rather than
       skipped: §3.1's perform a scroll is the only thing that starts one, the element route below crashes and
       the viewport route never changes a position, so no box in this engine has ever had one. The DFAIL is
       what keeps that derivation true. */
    if (position[ES_X] == b->cur[ES_X] && position[ES_Y] == b->cur[ES_Y]) return true;
    if (b->el == NULL) {
        /* §4's `scroll()` on the viewport, as the INTERNAL algorithm (§2 Terminology) — it re-runs the same
           clamp over the same two operands and asserts, at the step that decides, that the result is the
           position the viewport already has. Reaching this line therefore means the viewport's scrolling area
           has stopped being the initial containing block, and that assert is where it says so. */
        viewport_scroll(b->dctx, position[ES_X], position[ES_Y]);
        return true;
    }
    (void)behavior;
    DFAIL("CSSOM VIEW §6.1 \"Element Scrolling Members\" reached a real PERFORM A SCROLL of an ELEMENT's "
          "scrolling box — the position §6.1's clamp landed on is not the one the box has, so this is a scroll "
          "that must actually happen. TWO THINGS ARE MISSING AND NEITHER IS THE SCROLLING BOX ANY MORE: "
          "css-overflow-3 §3.1's scroll container is derived (core/layout/scroll_container.h) and §2's scrolling "
          "area is derived (core/layout/scrolling_area.h), which is what let this algorithm run at all. (1) THE "
          "SCROLL POSITION ITSELF, which an element must HOLD: it is per-flow state, because two flows that "
          "scrolled one element differently must read back different values, so it belongs in the COW delta with "
          "solver/dom_cow.h's capture at its accessor exactly as a browser component's own C record does — and "
          "core/dom/element_view.c's `scrollTop` getter step 8 reads it, so building one without the other "
          "would hand every flow the same number. (2) §3.1 \"Scrolling\"'s PERFORM A SCROLL, which is where the "
          "Promise these members answer with stops being a resolved one: its step 3 creates a promise that "
          "settles when the position has finished updating, its step 5 branches on the `behavior` argument this "
          "function already carries (`smooth` against `instant`, with `auto` deferring to the computed "
          "`scroll-behavior` of css-overflow-3 §4.1 \"Smooth Scrolling: the scroll-behavior Property\"), and "
          "its step 7.1 emits `scrollend`. BUILD the per-flow position first — the getter and this setter are "
          "one fact — then §3.1 over it. AND THE DIFF THAT DELETES THIS CRASH OWES ONE MORE THING: "
          "`element_scrolling_box_can_move` below derives its ELEMENT half FROM this crash, and three steps "
          "that drain or carry what a scroll produces read it (HTML §8.1.7.3 update-the-rendering step 9, HTML "
          "§7.4.6.5's scroll position data, and CSSOM VIEW §13.2's pending scroll events), so that half stops "
          "being a derivation the moment an element can hold a position and becomes a read of the real one");
    return true;
}

/* §6.1's DETERMINE THE SCROLL-INTO-VIEW POSITION, ON ONE AXIS. §6.1 writes the block half and the inline half
 * as two copies of one paragraph — edges A/B against C/D, "element height" against "element width" — so they
 * are one function here and the axis is the argument.
 *
 * THE FOUR EDGES ARE PHYSICAL AND THE POSITIONS ARE FLOW-RELATIVE, which is the whole of what this function
 * translates. §6.1: "let scrolling box edge A be the BEGINNING EDGE in the block flow direction of scrolling
 * box, and let element edge A be target bounding border box's edge ON THE SAME PHYSICAL SIDE as that of
 * scrolling box edge A" — so the flow-relative question is asked once, of the BOX, and every comparison after
 * it is between two physical coordinates. `begin_lo` is that one answer: the beginning edge is at the smaller
 * coordinate exactly when the ENDING edge is at the larger one, which is §2's overflow direction
 * (core/layout/scrolling_area.h) and not a second derivation.
 *
 * THE RESULT IS A DELTA AND NOT A POSITION, because §6.1 says "align" and alignment is a statement about the
 * two rectangles' relative placement: the target's box is given in CLIENT coordinates, which move with the
 * scroll, so the position the box "would have" is its current position plus the distance the target must move.
 * ALIGNING TO THE ENDING EDGE IS THE SAME SUBTRACTION AS ALIGNING TO THE BEGINNING ONE — edge minus edge — and
 * that is why this is four cases and not eight: which physical side each of those edges is on is `begin_lo`'s
 * answer, already made.
 *
 * `nearest`'s TABLE IS TRANSCRIBED AND NOT SIMPLIFIED. Its four rows pair "element edge A is outside scrolling
 * box edge A" with an element-versus-box SIZE comparison, and the pairing is what makes a target taller than
 * the scrollport align its TOP while a shorter one that has fallen off the bottom aligns its BOTTOM. The rows
 * say nothing about the case where the two sizes are EQUAL, so an equal-sized target that is outside falls
 * through every row to "do nothing" — that is §6.1's own text, which is what this half of the engine traces to,
 * and a rule invented here to cover it would be a divergence with nothing to point at. */
static double es_align_delta(const EsBox *b, const CssPx elo[2], const CssPx ehi[2],
                             int axis, ScrollLogicalPosition pos)
{
    bool begin_lo = b->ending_at_hi[axis];
    /* Edge A is the BEGINNING edge's physical side and edge B the ENDING edge's, for both rectangles. */
    CssPx eA = begin_lo ? elo[axis] : ehi[axis], bA = begin_lo ? b->lo[axis] : b->hi[axis];
    CssPx eB = begin_lo ? ehi[axis] : elo[axis], bB = begin_lo ? b->hi[axis] : b->lo[axis];
    double esize = ehi[axis].px - elo[axis].px;
    double bsize = b->hi[axis].px - b->lo[axis].px;
    bool a_out, b_out;

    DCHECK(esize >= 0.0 && bsize >= 0.0,
           "§6.1's element height/width or scrolling box height/width came out negative — both are distances "
           "between a rectangle's two parallel edges, and each rectangle was built lo-then-hi from an origin "
           "plus a non-negative extent");
    switch (pos) {
    case SCROLL_LOGICAL_START:  return css_px_sub(eA, bA).px;
    case SCROLL_LOGICAL_END:    return css_px_sub(eB, bB).px;
    case SCROLL_LOGICAL_CENTER:
        /* "align the CENTRE of target bounding border box with the centre of scrolling box in scrolling box's
           block flow direction" — a midpoint is direction-free, so this arm needs no `begin_lo`. */
        return css_px_sub(css_px_scale(css_px_add(elo[axis], ehi[axis]), 0.5),
                          css_px_scale(css_px_add(b->lo[axis], b->hi[axis]), 0.5)).px;
    case SCROLL_LOGICAL_NEAREST: break;
    }
    /* "OUTSIDE" is beyond the box's edge in that edge's own outward direction: past the beginning edge on the
       beginning side, past the ending edge on the ending side. */
    a_out = begin_lo ? (elo[axis].px < b->lo[axis].px) : (ehi[axis].px > b->hi[axis].px);
    b_out = begin_lo ? (ehi[axis].px > b->hi[axis].px) : (elo[axis].px < b->lo[axis].px);
    if (a_out && b_out) return 0.0;                                   /* "Do nothing." */
    if ((a_out && esize < bsize) || (b_out && esize > bsize)) return css_px_sub(eA, bA).px;
    if ((a_out && esize > bsize) || (b_out && esize < bsize)) return css_px_sub(eB, bB).px;
    return 0.0;
}

/* §6.1's DETERMINE THE SCROLL-INTO-VIEW POSITION, whole — steps 1 to 11 over both axes, with the clamp §6.1's
   own "the scroll position scrolling box WOULD HAVE" implies (a scroll position is by definition one the box's
   scrolling area admits, which is the same four rows scroll an element to x,y states explicitly).
   STEP 1 IS `getBoundingClientRect()` AS THE INTERNAL ALGORITHM — §2 Terminology's rule — so a page that
   overwrote `Element.prototype.getBoundingClientRect` cannot change where its own `scrollIntoView` scrolls.
   §6.1's LAST STEP, "if the target element defines some scroll snap positions, the user agent MUST scroll snap
   the resulting position", is not written: css-scroll-snap-1 declares no property this cascade carries, so
   `scroll-snap-type` computes to its initial `none` for every element in every document this engine parses and
   the step's own condition ("defines some scroll snap positions") is false. That is asserted rather than
   assumed — the day the cascade models the property, the assert below fires and names the step to build. */
static void es_determine_position(lxb_dom_element_t *target, ScrollLogicalPosition block,
                                  ScrollLogicalPosition inline_pos, const EsBox *b, double out[2])
{
    CssPx r[4], elo[2], ehi[2];

    DCHECK(!css_computed_models("scroll-snap-type"),
           "css-scroll-snap-1 §4.1 \"Scroll Snapping Rules: the scroll-snap-type property\" is in this cascade "
           "now, so CSSOM VIEW §6.1's determine the scroll-into-view position has a step this function does not "
           "run: 'if target is an Element, and the target element defines some scroll snap positions, then the "
           "user agent must scroll snap the resulting position to one of that element's scroll snap positions "
           "if its nearest scroll container is a scroll snap container'. BUILD css-scroll-snap-1's snap "
           "positions and apply them to the position below");
    element_view_bounding_box_px(target, r);
    elo[ES_X] = r[0];  ehi[ES_X] = css_px_add(r[0], r[2]);
    elo[ES_Y] = r[1];  ehi[ES_Y] = css_px_add(r[1], r[3]);
    /* THE BLOCK POSITION DRIVES THE BLOCK AXIS AND THE INLINE POSITION THE INLINE ONE, and in a `horizontal-tb`
       writing mode the block axis IS the vertical one — which is the very fact
       `scrolling_area_ending_edge_at_higher_coordinate` asserts for every box it answers, so the mapping is
       made under an assert rather than beside one. */
    out[ES_Y] = es_clamp(b, ES_Y, b->cur[ES_Y] + es_align_delta(b, elo, ehi, ES_Y, block));
    out[ES_X] = es_clamp(b, ES_X, b->cur[ES_X] + es_align_delta(b, elo, ehi, ES_X, inline_pos));
}

void element_scrolling_scroll_element(lxb_dom_element_t *el, double x, double y, const char *behavior)
{
    JSContext *dctx;
    EsBox b;
    double position[2];

    DCHECK(el != NULL, "§6.1's scroll an element to x,y was invoked with no element");
    DCHECK(isfinite(x) && isfinite(y),
           "§6.1's scroll an element to x,y was handed a non-finite coordinate. CSSOM VIEW §3.2 \"WebIDL "
           "values\"' normalize non-finite values turns Infinity, -Infinity and NaN into 0, and it is the "
           "CALLER's step — §6's setter's step 2 and its scroll members' step 1 — so a non-finite here is that "
           "step having been skipped");
    dctx = document_active_realm_of(lxb_dom_interface_node(lxb_dom_interface_node(el)->owner_document));
    DCHECK(dctx != NULL,
           "§6.1's scroll an element to x,y was reached for an element whose node document is no navigable's "
           "ACTIVE document — §6's own steps 3 to 6 return before step 11 for exactly that document, so the "
           "caller's order has come apart");
    /* Step 1 — "let box be element's ASSOCIATED SCROLLING BOX". §6's step 10 has already terminated for an
       element that has none, so the constructor's own assert is the pairing between the two steps. */
    es_box_of_element(el, dctx, &b);
    /* Steps 2 and 3 — the clamp, and step 4's "let position be the scroll position box would have by aligning
       scrolling area x-coordinate x with the left of box", which for a clamped pair IS that pair. */
    position[ES_X] = es_clamp(&b, ES_X, x);
    position[ES_Y] = es_clamp(&b, ES_Y, y);
    /* Steps 5 and 6 */
    es_perform_scroll(&b, position, behavior);
}

/* ONE ITERATION of scroll a target into view's step 2 — the substeps run per scrolling box. */
static void es_visit(lxb_dom_element_t *target, const EsBox *b, const char *behavior,
                     ScrollLogicalPosition block, ScrollLogicalPosition inline_pos)
{
    double position[2];

    /* Step 2.1's SAME-ORIGIN CHECK is not asked here, and its absence is a derivation rather than an omission:
       SECURITY.md keys a WASM instance on `(browsing-context group, origin)`, so every Document whose element
       this walk can reach in this heap is same origin with every other one by construction. The one place the
       check has a false arm is the step OUT of a document, and that is where the walk asks it — a cross-origin
       parent's container element belongs to another instance and is not reachable at all. */
    es_determine_position(target, block, inline_pos, b, position);   /* step 2.2 */
    es_perform_scroll(b, position, behavior);                        /* steps 2.3.1 and 2.3.2 */
}

/* Step 2.4's STOP CONDITION — "if container is not null and either scrolling box is a shadow-including
   inclusive ancestor of container or is a viewport whose document is a shadow-including inclusive ancestor of
   container, abort any remaining iteration of this loop". The two disjuncts are one test over two node kinds,
   which is why the box's node is taken first and the DOM's own predicate asked once. */
static bool es_stop_after(const EsBox *b, lxb_dom_node_t *doc, lxb_dom_element_t *container)
{
    const lxb_dom_node_t *box_node = b->el != NULL ? lxb_dom_interface_node(b->el) : doc;

    if (container == NULL) return false;
    return shadow_root_is_shadow_including_inclusive_ancestor(box_node, lxb_dom_interface_node(container));
}

void element_scrolling_scroll_target_into_view(lxb_dom_element_t *target, const char *behavior,
                                               ScrollLogicalPosition block, ScrollLogicalPosition inline_pos,
                                               lxb_dom_element_t *container)
{
    lxb_dom_element_t *walk;
    lxb_dom_node_t *doc;
    JSContext *dctx;

    DCHECK(target != NULL, "§6.1's scroll a target into view was invoked with no target");
    DCHECK(element_view_has_box(lxb_dom_interface_node(target)),
           "§6.1's scroll a target into view was reached for an element with no associated box — "
           "`scrollIntoView`'s own step 7 returns a resolved Promise for exactly that element, so the caller's "
           "order has come apart");
    doc = lxb_dom_interface_node(lxb_dom_interface_node(target)->owner_document);
    walk = target;
    for (;;) {
        dctx = document_active_realm_of(doc);
        DCHECK(dctx != NULL,
               "§6.1's ancestor walk is inside a Document that is no navigable's ACTIVE document. The walk "
               "starts at an element WITH A BOX, which is defined over the document being presented, and it "
               "leaves a document only through a container element that is itself in a presented one");
        /* STEP 2's ORDER, "in order of INNERMOST TO OUTERMOST scrolling box", is the ancestor chain read
           upward. It is the FLAT TREE's chain (core/css/css_computed_value.h's `css_parent_element`) and not
           the node tree's, because a scrolling box is a BOX and the flat tree is where boxes nest: a shadow
           host's `overflow: auto` scrolls the boxes its shadow tree generates, and the node-tree parent of a
           slotted child is not the element whose scrolling box contains it.
           THE TARGET ITSELF IS NOT IN THE SET — §6.1 says "each ANCESTOR element or viewport" — which is why
           this loop advances before it tests. */
        for (walk = css_parent_element(walk); walk != NULL; walk = css_parent_element(walk)) {
            EsBox b;

            if (!scroll_container_is(walk)) continue;
            es_box_of_element(walk, dctx, &b);
            es_visit(target, &b, behavior, block, inline_pos);
            if (es_stop_after(&b, doc, container)) return;
        }
        {
            /* THE VIEWPORT IS THE OUTERMOST SCROLLING BOX OF EVERY DOCUMENT and is always one — §2 states the
               scrolling area of "a viewport or element" and gives the viewport its own column, so unlike an
               element it has no scroll-container question to pass. */
            EsBox b;

            es_box_of_viewport(doc, dctx, &b);
            es_visit(target, &b, behavior, block, inline_pos);
            if (es_stop_after(&b, doc, container)) return;
        }
        /* OUT OF THE DOCUMENT — HTML §7.3.1.3 "Child navigables"' CONTAINER, which is where the walk continues
         * and where step 2.1's same-origin check has a false arm.
         * THE THREE ANSWERS ARE THREE DIFFERENT FACTS AND ARE KEPT APART. A navigable with no PARENT is the
         * top-level traversable: the walk has visited every scrolling box there is and ENDS. A navigable with a
         * parent whose container ELEMENT is in this heap is a SAME-ORIGIN child — SECURITY.md keys an instance
         * on `(browsing-context group, origin)`, so the element being reachable IS the same-origin answer — and
         * the walk continues at that element. A navigable with a parent and NO local container element is a
         * CROSS-ORIGIN child whose container belongs to another instance, which is §6.1 step 2.1's "abort any
         * remaining iteration of this loop", and it is asserted to be that rather than assumed: the serialized
         * remote container is the positive statement that the element exists somewhere else. */
        {
            JSValueConst proxy = document_window_proxy(dctx);
            JSValue parent = window_proxy_parent_navigable(dctx, proxy);
            JSValue el_v;
            lxb_dom_element_t *ce;

            if (JS_IsUndefined(parent)) { JS_FreeValue(dctx, parent); return; }
            JS_FreeValue(dctx, parent);
            el_v = window_proxy_container(dctx, proxy);
            if (!element_of_value(el_v)) {
                DCHECK(window_proxy_remote_container(proxy) != NULL,
                       "§6.1's ancestor walk found a CHILD navigable with no container element in this heap and "
                       "no record of a remote one either. HTML §7.3.1.3 gives every child navigable a container "
                       "-- 'the navigable container whose content navigable is navigable' -- so the element is "
                       "in this instance or in another one, and a third answer is the two records having come "
                       "apart");
                JS_FreeValue(dctx, el_v);
                return;   /* step 2.1: not same origin — abort any remaining iteration */
            }
            ce = element_of_value(el_v);
            JS_FreeValue(dctx, el_v);
            /* The container element is an ancestor of the target in exactly §6.1's sense, so the walk resumes
               with IT rather than with its parent — an `<iframe style="overflow:auto">` is a scroll container
               whose scrolling box contains the child document's own box. */
            walk = ce;
            doc = lxb_dom_interface_node(lxb_dom_interface_node(ce)->owner_document);
            if (scroll_container_is(ce)) {
                EsBox b;

                es_box_of_element(ce, document_active_realm_of(doc), &b);
                es_visit(target, &b, behavior, block, inline_pos);
                if (es_stop_after(&b, doc, container)) return;
            }
        }
    }
}

/* CSSOM VIEW §3.1 "Scrolling"'s PERFORM A SCROLL, ASKED AS A CAPABILITY — see element_scrolling.h for the
 * three readers, for why this is a component's answer rather than a `realm_awaits` over a name on the global,
 * and for the derivation each half rests on. */
bool element_scrolling_box_can_move(JSContext *ctx)
{
    DCHECK(ctx != NULL,
           "CSSOM VIEW §3.1's perform-a-scroll capability was asked without a realm — the viewport half is a "
           "fact about the viewport THIS realm's document is presented in, and core/frame/viewport.h answers "
           "for the realm it is passed and never for a remembered one");
    /* THE VIEWPORT HALF, derived from §4's clamp's own operands. `viewport_scroll` clamps a request to
       max(0, min(x, area - viewport)) on each axis, so the clamp can land anywhere but the origin exactly
       when the scrolling area is larger than the viewport. `viewport_exists` is not asked here: where there is
       no viewport, core/frame/viewport.h answers both extents from the same modelled geometry and the
       comparison is false, which is the right answer — a document no navigable is presenting has no box for
       §3.1 to move. */
    if (viewport_scrolling_area_width(ctx)  > viewport_width(ctx))  return true;
    if (viewport_scrolling_area_height(ctx) > viewport_height(ctx)) return true;
    /* THE ELEMENT HALF, derived from the crash in `es_perform_scroll` above: an element that reached a real
       perform-a-scroll aborts there rather than moving, so every element in this engine is at the origin
       core/dom/element_view.c's `scrollTop` getter step 8 derives, and there is nothing for §13.2's list to
       have been appended for. */
    return false;
}
