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
#include "solver/rest_unit.h"

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

/* A scrolling box, resolved: everything §6.1's two position algorithms read off one, gathered where the box
 * is identified.
 *
 * The edges are in client coordinates because §6.1's determine the scroll-into-view position takes the
 * target's bounding border box from `getBoundingClientRect()`, which CSSOM VIEW §6 defines in the viewport's
 * space, and compares it edge to edge with the box. A box's edges are therefore its padding box less the
 * window scroll, as core/dom/element_view.c's border area is.
 *
 * An edge is a `CssPx` because it is a layout length that may derive from the initial containing block, and
 * the arithmetic propagates that fact (core/css/css_length.h). A scroll position is a `double` because it is
 * a number this engine stores and reads back, carrying no environment fact, and it is the type
 * `perform_scroll`, `viewport_window_scroll` and `element_view_scroll_position` speak. A requested position
 * that is unknown external input cannot be a `double`; core/dom/element_view.c's setter crashes naming it. */
typedef ElementScrollingBox EsBox;

/* Every field of an element's box but its scrolling area, which a caller either walks in steps
   (`es_into_view_measure`) or asks whole (`es_box_of_element`). */
static void es_box_open_element(lxb_dom_element_t *el, JSContext *dctx, EsBox *b)
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
        b->ending_at_hi[a] = scrolling_area_ending_edge_at_higher_coordinate(el, vertical);
        b->cur[a] = element_view_scroll_position(el, vertical);
    }
}

static void es_box_of_element(lxb_dom_element_t *el, JSContext *dctx, EsBox *b)
{
    int a;

    es_box_open_element(el, dctx, b);
    for (a = ES_X; a <= ES_Y; a++) b->area[a] = scrolling_area_extent_px(el, a == ES_Y);
}

/* The viewport's EsBox. Its overflow directions are core/layout/scrolling_area.h's, which answers both columns
   of CSSOM VIEW §2's table, so the fact has one derivation for both of its readers. */
static void es_box_open_viewport(lxb_dom_node_t *doc, JSContext *dctx, EsBox *b)
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
        b->ending_at_hi[a] = scrolling_area_viewport_ending_edge_at_higher_coordinate(doc, vertical);
        b->cur[a] = viewport_window_scroll(dctx, vertical);
    }
}

/* §6.1's clamp: four rows over one fact, written once for every kind of scrolling box. The rows take plain
 * operands rather than an EsBox so that core/frame/viewport.c's §4 scroll(), which has no EsBox, reaches the
 * same function. */
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

/* §6.1's perform-a-scroll step, for either kind of box: the last two steps of scroll an element to x,y ("If
 * position is the same as box's current scroll position, and box does not have an ongoing smooth scroll,
 * return a resolved `Promise` and abort the remaining steps." / "Perform a scroll of box to position, element
 * as the associated element and behavior as the scroll behavior.") and, word for word, scroll a target into
 * view's own step 2.3 ("If position is not the same as scrolling box's current scroll position, or scrolling
 * box has an ongoing smooth scroll") with its step 2.3.1.
 * Step 2.3 holds one list item, a two-armed switch, and the arms decide the associated element: the element
 * arm passes "the element as the associated element" (the ancestor whose box this is, never the target), and
 * the viewport arm's substeps are "Let document be the viewport's associated `Document`." and "Let root
 * element be document's root element, if there is one, or null otherwise." `position` is already clamped.
 * An element's box cannot yet hold a scroll position; core/dom/perform_scroll.c, which owns §3.1, crashes
 * for it. */
static void es_perform_scroll(const EsBox *b, const double position[2], const char *behavior)
{
    /* The ongoing-smooth-scroll disjunct is false for every box: §3.1 step 5 starts a smooth scroll only on
       the arm this user agent never takes, because it does not honor css-overflow-3 §4.1's `scroll-behavior`
       (core/dom/perform_scroll.h states and asserts it). */
    if (position[ES_X] == b->cur[ES_X] && position[ES_Y] == b->cur[ES_Y]) return;
    if (b->el == NULL) {
        /* The viewport arm of step 2.3.1: the document is the one this box was built over and the associated
           element is its root element, or null. It is §3.1's scrolling-box algorithm and not its coordinated
           viewport one, for core/dom/perform_scroll.h's reason (at this model's scale factor the two
           coincide). It calls §3.1 directly rather than §4's scroll(), whose own clamp would re-clamp a
           position §6.1 has already clamped. */
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

void element_scrolling_into_view_begin(ElementScrollingIntoView *w, lxb_dom_element_t *target,
                                       ScrollLogicalPosition block, ScrollLogicalPosition inline_pos,
                                       lxb_dom_element_t *container)
{
    DCHECK(target != NULL, "§6.1's scroll a target into view was invoked with no target");
    DCHECK(element_view_has_box(lxb_dom_interface_node(target)),
           "§6.1's scroll a target into view was reached for an element with no associated box — "
           "`scrollIntoView`'s own step 7 returns a resolved Promise for exactly that element, so the caller's "
           "order has come apart");
    w->target = target;
    w->walk = target;
    w->doc = lxb_dom_interface_node(lxb_dom_interface_node(target)->owner_document);
    w->container = container;
    w->block = block;
    w->inline_pos = inline_pos;
    w->phase = ES_INTO_VIEW_ANCESTORS;
    w->box_open = false;
}

/* Step 2.2 to step 2.4 for one scrolling box, and whether the walk ends after it. */
static bool es_into_view_visit(ElementScrollingIntoView *w, const EsBox *b, const char *behavior)
{
    es_visit(w->target, b, behavior, w->block, w->inline_pos);
    if (!es_stop_after(b, w->doc, w->container)) return true;
    w->phase = ES_INTO_VIEW_DONE;
    return false;
}

/* Begin the descendant walk of the open box's scrolling area on `w->axis`. The viewport's row is the one
   core/frame/viewport.c's `viewport_scrolling_area_*` asks, over the same initial containing block. */
static void es_area_begin(ElementScrollingIntoView *w)
{
    bool vertical = w->axis == ES_Y;

    if (w->box.el != NULL) scrolling_area_walk_element(&w->area, w->box.el, vertical);
    else                   scrolling_area_walk_viewport(&w->area, w->doc, w->box.extent[w->axis], vertical);
}

/* Open a scrolling box (`el` NULL for the viewport) and begin its first area walk. */
static void es_into_view_open(ElementScrollingIntoView *w, lxb_dom_element_t *el, JSContext *dctx)
{
    if (el != NULL) es_box_open_element(el, dctx, &w->box);
    else            es_box_open_viewport(w->doc, dctx, &w->box);
    w->axis = ES_X;
    w->box_open = true;
    es_area_begin(w);
}

/* One rest unit of the open box's area walks, or, once both are complete, the visit. The descendants are
   the rest unit's items (REST_UNIT_LAYOUT_DESCENDANTS), and the bracket measures them against the slice. */
static bool es_into_view_measure(ElementScrollingIntoView *w, const char *behavior)
{
    if (!scrolling_area_walk_done(&w->area)) {
        size_t k, n = rest_unit_items(REST_UNIT_LAYOUT_DESCENDANTS);

        rest_unit_begin(REST_UNIT_LAYOUT_DESCENDANTS);
        for (k = 0; k < n && !scrolling_area_walk_done(&w->area); k++) scrolling_area_walk_step(&w->area);
        rest_unit_end();
        return true;
    }
    /* The viewport's area is the bare number core/frame/viewport.c answers. */
    w->box.area[w->axis] = w->box.el != NULL ? scrolling_area_walk_extent(&w->area)
                                             : css_px(scrolling_area_walk_extent(&w->area).px);
    if (w->axis == ES_X) {
        w->axis = ES_Y;
        es_area_begin(w);
        return true;
    }
    w->box_open = false;
    return es_into_view_visit(w, &w->box, behavior);
}

bool element_scrolling_into_view_next(ElementScrollingIntoView *w, const char *behavior)
{
    JSContext *dctx;

    DCHECK(w->phase != ES_INTO_VIEW_DONE, "§6.1's ancestor walk was continued after it ended");
    if (w->box_open) return es_into_view_measure(w, behavior);
    dctx = document_active_realm_of(w->doc);
    DCHECK(dctx != NULL,
           "§6.1's ancestor walk is inside a Document that is no navigable's ACTIVE document. The walk starts at "
           "an element with a box, which is defined over the document being presented, and it leaves a "
           "document only through a container element that is itself in a presented one");
    if (w->phase == ES_INTO_VIEW_ANCESTORS) {
        /* "Each ancestor element or viewport that establishes a scrolling box", innermost first, over the flat
           tree's chain (core/css/css_computed_value.h's `css_parent_element`), because a scrolling box is a box
           and the flat tree is where boxes nest. The target itself is not in the set, so the walk advances
           before it tests. */
        for (w->walk = css_parent_element(w->walk); w->walk != NULL; w->walk = css_parent_element(w->walk)) {
            if (!scroll_container_is(w->walk)) continue;
            es_into_view_open(w, w->walk, dctx);
            return true;
        }
        w->phase = ES_INTO_VIEW_VIEWPORT;
    }
    if (w->phase == ES_INTO_VIEW_VIEWPORT) {
        /* The viewport is the outermost scrolling box of every document and always one: §2 gives it its own
           column of the scrolling-area table, so it has no scroll-container question to pass. */
        w->phase = ES_INTO_VIEW_CONTAINER;
        es_into_view_open(w, NULL, dctx);
        return true;
    }
    DCHECK(w->phase == ES_INTO_VIEW_CONTAINER, "§6.1's ancestor walk resumed in a phase it does not have");
    /* Out of the document, through HTML §7.3.1.3 "Child navigables"' container. No parent is the top-level
       traversable, and the walk has visited every scrolling box. A parent whose container element is in this
       heap is a same-origin child (SECURITY.md keys an instance on its origin), and the walk continues at
       that element. A parent with no local container element is a cross-origin child: CSSOM VIEW §6.1's scroll
       a target into view step 2.1, "abort any remaining iteration of this loop", asserted by the remote
       container's record. */
    {
        JSValueConst proxy = document_window_proxy(dctx);
        JSValue parent = window_proxy_parent_navigable(dctx, proxy);
        JSValue el_v;
        lxb_dom_element_t *ce;

        if (JS_IsUndefined(parent)) {
            JS_FreeValue(dctx, parent);
            w->phase = ES_INTO_VIEW_DONE;
            return false;
        }
        JS_FreeValue(dctx, parent);
        el_v = window_proxy_container(dctx, proxy);
        if (!element_of_value(el_v)) {
            DCHECK(window_proxy_remote_container(proxy) != NULL,
                   "§6.1's ancestor walk found a CHILD navigable with no container element in this heap and no "
                   "record of a remote one either. HTML §7.3.1.3 gives every child navigable a container -- 'the "
                   "navigable container whose content navigable is navigable' -- so the element is in this "
                   "instance or in another one, and a third answer is the two records having come apart");
            JS_FreeValue(dctx, el_v);
            w->phase = ES_INTO_VIEW_DONE;
            return false;
        }
        ce = element_of_value(el_v);
        JS_FreeValue(dctx, el_v);
        /* The container element is an ancestor of the target in §6.1's sense, so the walk resumes with it: an
           `<iframe style="overflow:auto">` is a scroll container whose box contains the child document's. */
        w->walk = ce;
        w->doc = lxb_dom_interface_node(lxb_dom_interface_node(ce)->owner_document);
        w->phase = ES_INTO_VIEW_ANCESTORS;
        if (scroll_container_is(ce)) es_into_view_open(w, ce, document_active_realm_of(w->doc));
    }
    return true;
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
    /* The viewport half is a question about slack: each of §4's steps 7 and 8 has two arms and both collapse
       onto the origin exactly when the scrolling area equals the viewport, so then no algorithm can put the
       box anywhere else, and otherwise §3.1 can. It needs no overflow direction (element_scrolling.h).
       Where there is no viewport, core/frame/viewport.h answers both extents from the same modelled geometry
       and the comparison is false, which is right: a document nothing presents has no box to move. */
    if (viewport_scrolling_area_width(ctx)  > viewport_width(ctx))  return true;
    if (viewport_scrolling_area_height(ctx) > viewport_height(ctx)) return true;
    /* The element half: an element that reaches a real perform a scroll aborts in core/dom/perform_scroll.c
       for want of a place to put the position, so every element is at the origin core/dom/element_view.c's
       `scrollTop` getter step 8 derives. Giving an element a scroll position turns this line into a walk of
       the document's scroll containers. */
    return false;
}
