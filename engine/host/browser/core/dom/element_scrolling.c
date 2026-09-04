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
#include "core/dom/perform_scroll.h"
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
 * below propagates it. A SCROLL POSITION is not that kind of value: it is a number this engine STORES and reads
 * back (core/frame/viewport.h holds the viewport's on a per-flow record), not a length derived from the
 * environment, so it carries no `CssEnvFact` to lose. IT IS ALSO THE TYPE EVERY CONSUMER SPEAKS —
 * `perform_scroll`, `viewport_window_scroll`, `element_view_scroll_position` — so converting at this boundary
 * rather than at three call sites is what keeps one answer.
 * WHAT THIS PARAGRAPH USED TO SAY IS RETIRED AND IS RESTATED IN CAPITALS SO NOBODY RE-DERIVES IT: EVERY SCROLL
 * POSITION IN THIS ENGINE IS DERIVED — THE VIEWPORT HAS ONE VALID POSITION AND NO ELEMENT HAS EVER BEEN
 * SCROLLED. The viewport's half of that is gone; the element's half is not, and it is the crash in
 * core/dom/perform_scroll.c rather than anything here that keeps it true. A REQUESTED position that is UNKNOWN
 * EXTERNAL INPUT is the one thing this `double` cannot carry, and core/dom/element_view.c's setter crashes
 * naming it rather than substituting a number. */
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

/* THE VIEWPORT'S EsBox. Its OVERFLOW DIRECTIONS used to be derived here, through a static that resolved
 * css-writing-modes-4 §8 "The Principal Writing Mode"' element; the derivation is unchanged and has MOVED to
 * core/layout/scrolling_area.h, which is where CSSOM VIEW §2's table lives and now answers both of its columns.
 * The argument for keeping it here was that §2's overflow directions were all anyone asked of §8, "so it is
 * answered here rather than in core/frame/viewport.h, whose §2 column is the viewport's SCROLLING AREA — a
 * rectangle this file does not touch and which that component derives as the initial containing block on both
 * axes, with no direction in it". That last clause is retired: §2's viewport row is built, the viewport's
 * scrolling area has a direction in it, and the fact has two readers — so a copy here would be one direction
 * with two answers, free to disagree about `<html><body dir=rtl>` in exactly the document where it matters. */
static void es_box_of_viewport(lxb_dom_node_t *doc, JSContext *dctx, EsBox *b)
{
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
        b->ending_at_hi[a] = scrolling_area_viewport_ending_edge_at_higher_coordinate(doc, vertical);
        b->cur[a] = viewport_window_scroll(dctx, vertical);
    }
}

/* §6.1's CLAMP, which is FOUR ROWS OVER ONE FACT and is written once for EVERY kind of scrolling box — see
 * element_scrolling.h for the two algorithms that state those rows and for why the operands are `double`.
 * IT IS NOT A FUNCTION OF `EsBox` any more, and that is what makes the claim above literally true rather than
 * nearly so: core/frame/viewport.c's §4 scroll() reaches its clamp with no `EsBox` in hand and used to write
 * the rightward/downward row again, guarded by an assert that the viewport's scrolling area equalled the
 * viewport. The assert was the two-sided half and it fired the day §2's viewport row was built, so the rows
 * moved to the ONE signature both callers can reach and the second copy went with it. */
static double es_clamp(const EsBox *b, int axis, double v)
{
    return element_scrolling_clamp_position(v, b->area[axis].px, b->extent[axis].px, b->ending_at_hi[axis]);
}

double element_scrolling_clamp_position(double v, double area_extent, double box_extent,
                                        bool ending_edge_at_higher_coordinate)
{
    double slack = area_extent - box_extent;

    DCHECK(slack >= 0.0,
           "CSSOM VIEW §2's scrolling area came out SMALLER than the box it belongs to, so §6.1's clamp has an "
           "empty range. §2's table takes the ending edge as an extreme OVER the beginning edge's own box, so "
           "the area contains that box on both axes by construction");
    if (ending_edge_at_higher_coordinate) return fmax(0.0, fmin(v, slack));
    return fmin(0.0, fmax(v, -slack));
}

/* §6.1's PERFORM-A-SCROLL STEP, for either kind of box — the last two steps of scroll an element to x,y ("If
 * position is the same as box's current scroll position, and box does not have an ongoing smooth scroll,
 * return a resolved `Promise` and abort the remaining steps." / "Perform a scroll of box to position, element
 * as the associated element and behavior as the scroll behavior.") and, word for word, scroll a target into
 * view's own step 2.3 ("If position is not the same as scrolling box's current scroll position, or scrolling
 * box has an ongoing smooth scroll") with its step 2.3.1.
 * STEP 2.3.1 IS ONE STEP AND NOT TWO, and this used to cite a SECOND sub-step beside it that §6.1 does not
 * have (the number is deliberately not spelled again: citegen.mjs reads a step number out of prose whether or
 * not it is quoted, and a retirement note that repeats the wrong one manufactures the finding it is retiring).
 * Step 2.3 holds an `<ol>` with a SINGLE `<li>`, and that item is a two-armed
 * `<dl class="switch">` — so a flat count of the arms reads one step as two, which is the same hazard §4's
 * clamp carries at viewport.c and the reason a cluster of sub-numbers is checked from its last member
 * backwards.
 * ITS TWO ARMS ARE WHAT DECIDES THE ASSOCIATED ELEMENT, and they are not the same answer. The element arm
 * passes "the element as the associated element" — the ancestor whose box this is, never the target. The
 * viewport arm runs three substeps of its own, of which the first two are "Let document be the viewport's
 * associated `Document`." and "Let root element be document's root element, if there is one, or null
 * otherwise." So the argument §3.1 step 5 reads is the ROOT ELEMENT there and the SCROLL CONTAINER here, and
 * one answer for both would be wrong on whichever side it was not written for.
 * `position` is already clamped. THE CRASH THAT USED TO LIVE HERE — AN ELEMENT CANNOT HOLD A SCROLL POSITION —
 * HAS MOVED TO THE COMPONENT THAT OWNS §3.1 (core/dom/perform_scroll.c), which is where the position store
 * belongs and therefore where its absence belongs: one site for one absence, and now the site that the diff
 * building the store will be editing anyway. */
static void es_perform_scroll(const EsBox *b, const double position[2], const char *behavior)
{
    /* "…AND BOX DOES NOT HAVE AN ONGOING SMOOTH SCROLL" is answered FALSE for every box, derived rather than
       skipped — and the derivation has CHANGED HANDS rather than gone away. IT USED TO BE THAT NOTHING IN THIS
       ENGINE HAD EVER REACHED §3.1 AT ALL; §3.1 is written now, and what keeps the disjunct false is its own
       step 5: a smooth scroll is started only by the arm this user agent never takes, because it does not
       honor css-overflow-3 §4.1's `scroll-behavior` property. core/dom/perform_scroll.h states it once and
       asserts it once. */
    if (position[ES_X] == b->cur[ES_X] && position[ES_Y] == b->cur[ES_Y]) return;
    if (b->el == NULL) {
        /* THE VIEWPORT ARM of step 2.3.1, with its own three substeps: the document is the one this box was
           built over and the associated element is that document's root element, or null where there is none.
           IT IS §3.1's SCROLLING-BOX ALGORITHM AND NOT ITS COORDINATED VIEWPORT ONE, for the reason
           core/dom/perform_scroll.h gives once for both of §3.1's callers — at this model's scale factor the
           two coincide, and the standard's own note under §4's step 12 records that user agents disagree about
           which of them a viewport scroll is.
           IT NO LONGER GOES THROUGH `viewport_scroll`, which is §4's THIRTEEN STEPS: routing §6.1 through them
           re-ran §4's own clamp over a position §6.1 had already clamped, so one request was clamped twice by
           two algorithms — idempotent, and still two statements of one decision. §6.1 says "perform a scroll",
           and that is now a thing this engine has. */
        perform_scroll(b->dctx, NULL, position[ES_X], position[ES_Y],
                       lxb_dom_interface_element(document_root_node(b->dctx)), behavior);
        return;
    }
    /* THE ELEMENT ARM — "with the element as the associated element". */
    perform_scroll(b->dctx, b->el, position[ES_X], position[ES_Y], b->el, behavior);
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
    es_perform_scroll(b, position, behavior);                        /* step 2.3 and its 2.3.1 */
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
    /* THE VIEWPORT HALF, and §3.1's ARRIVAL DID NOT RETIRE IT — it made it LOAD-BEARING, which is the opposite
       and is worth stating because the reverse reads as obvious. This comparison is about SLACK and never about
       whether a perform-a-scroll exists: each of §4's steps 7 and 8 has two arms and BOTH collapse onto the
       origin exactly when the slack is zero, so where the scrolling area equals the viewport no algorithm
       whatever can put a box anywhere else, and where it does not, §3.1 now can. It used to be the OUTER of two
       reasons, with NOTHING IN THIS ENGINE REACHES §3.1 standing behind it; that reason is gone and this one is
       the whole answer. See element_scrolling.h for why it needs no overflow direction of its own.
       `viewport_exists` is not asked here: where there is no viewport, core/frame/viewport.h answers both
       extents from the same modelled geometry and the comparison is false, which is the right answer — a
       document no navigable is presenting has no box for §3.1 to move. */
    if (viewport_scrolling_area_width(ctx)  > viewport_width(ctx))  return true;
    if (viewport_scrolling_area_height(ctx) > viewport_height(ctx)) return true;
    /* THE ELEMENT HALF, derived from the crash in core/dom/perform_scroll.c's instant scroll — which is where
       it moved when §3.1 was written, and it is the SAME crash rather than a new one: an element that reaches a
       real perform-a-scroll aborts for want of a place to put the position, so every element in this engine is
       at the origin core/dom/element_view.c's `scrollTop` getter step 8 derives. The diff that gives an element
       a scroll position is the diff that turns this line into a walk of the document's scroll containers. */
    return false;
}
