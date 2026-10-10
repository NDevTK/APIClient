/* CSSOM VIEW §6.1 "Element Scrolling Members": the three algorithms §6's members end in, split out of
 * core/dom/element_view.c because they are not members of anything.
 *
 *   Scroll an element (or pseudo-element) element to x,y: the clamp into §2's scrolling area and one perform
 *     a scroll; §6's setter's step 11 and its scroll members' step 11 end here.
 *   Determine the scroll-into-view position of a target against one scrolling box.
 *   Scroll a target into view: the walk over "each ancestor element or viewport that establishes a scrolling
 *     box, in order of innermost to outermost", running the algorithm above once per box.
 *
 * The two scroll-into-view algorithms reason about scrolling boxes that are not the receiver of the call:
 * they walk out of the element, out of its document and, for a same-origin child navigable, into the
 * container element's ancestors. Every member of §6 answers about one element, which is why the walk is a
 * file of its own. */

/* No entry here returns a promise. Every scroll this user agent performs is an instant one: §3.1 step 5's
 * smooth arm is gated on honoring css-overflow-3 §4.1's `scroll-behavior`, which this cascade does not
 * declare, so §3.1 step 7.2 resolves each scroll's promise before the algorithm returns and §6.1's scroll a
 * target into view step 5 resolves a set of resolved promises. core/dom/perform_scroll.h states and asserts
 * that once. For the same reason no box ever has an ongoing smooth scroll, so the three steps of §6.1 that
 * read "or box has an ongoing smooth scroll" answer that disjunct false. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_ELEMENT_SCROLLING_H
#define ENGINE_HOST_BROWSER_CORE_DOM_ELEMENT_SCROLLING_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"
#include "core/css/css_length.h"
#include "core/layout/scrolling_area.h"

/* CSSOM VIEW §6's `enum ScrollLogicalPosition { "start", "center", "end", "nearest" }`, which §6.1's determine
   the scroll-into-view position branches over four ways on each axis. It is an enum here and a string at the
   boundary: core/idl_args.h has already checked the value against §3.2.18's enumeration list, so a spelling
   outside these four cannot arrive and the mapping crashes rather than defaulting. */
typedef enum {
    SCROLL_LOGICAL_START = 0,
    SCROLL_LOGICAL_CENTER,
    SCROLL_LOGICAL_END,
    SCROLL_LOGICAL_NEAREST
} ScrollLogicalPosition;

/* The four keywords, in this enum's order: the one list both the declaration's `values` array and the
   mapping below read. The extent is left unwritten because a hand-written extent on an `extern` array is a
   second copy of the length, and a definition with more entries is truncated to it with only a warning,
   dropping the sentinel the mapping scans for. */
extern const char *const SCROLL_LOGICAL_POSITIONS[];
ScrollLogicalPosition element_scrolling_logical_position(const char *keyword);

/* §6.1's CLAMP, WHICH IS ALSO §4's — the four rows that TWO algorithms state over ONE fact, written once.
   `v` is the requested position on this axis; `area_extent` is CSSOM VIEW §2 "Terminology"'s SCROLLING AREA
   extent on it; `box_extent` is the scrolling box's own (an element's padding edge, a viewport's initial
   containing block); and `ending_edge_at_higher_coordinate` is §2's OVERFLOW DIRECTION for that axis, which is
   core/layout/scrolling_area.h's one derivation and never a second one — a second would be free to disagree
   about `direction: rtl` in exactly the document where it matters.

   IT IS EXPORTED BECAUSE §6.1 IS NOT ITS ONLY READER, and that is the SPEC's own generality rather than this
   component merging two rules. §6.1's scroll an element to x,y states it over an element's box — "If box has
   rightward overflow direction / Let x be max(0, min(x, element scrolling area width - element padding edge
   width)). If box has leftward overflow direction / Let x be min(0, max(x, element padding edge width - element
   scrolling area width))" — and CSSOM VIEW §4 "Extensions to the Window Interface"' scroll() steps 7 and 8
   state the same two switches over the VIEWPORT's box: "Let x be max(0, min(x, viewport scrolling area width -
   viewport width))" against "Let x be min(0, max(x, viewport width - viewport scrolling area width))". §2 gives
   a scrolling box of A VIEWPORT OR ELEMENT the same two overflow directions, so those are one rule with two
   statements of it, and core/frame/viewport.c reads this rather than writing the rows again.

   THE ARITHMETIC RUNS ON THE EXAMPLE, which is why every operand is a `double` and not a `CssPx`: both extents
   may be functions of the initial containing block (core/css/css_length.h), so which is larger is a question
   the environment could answer either way, and it is decided on the modelled viewport exactly as CSS 2.1
   §10.3.3 "Computing widths and margins"' slack test is. */
double element_scrolling_clamp_position(double v, double area_extent, double box_extent,
                                        bool ending_edge_at_higher_coordinate);

/* §6.1's "To SCROLL AN ELEMENT (or pseudo-element) element to x,y optionally with a scroll behavior behavior".
   `x` and `y` are the requested position in CSS pixels, before §6.1's own clamp. `behavior` is §4's
   ScrollBehavior keyword the caller's dictionary carried; it is consumed by §3.1's perform a scroll.
   THE CALLER HAS ALREADY RUN §6's STEP 10 — the element has an associated box, it has an associated scrolling
   box, and it has overflow — which is what makes step 1's "let box be element's associated scrolling box" have
   an answer. Both facts are asserted here rather than trusted. */
void element_scrolling_scroll_element(lxb_dom_element_t *el, double x, double y, const char *behavior);

/* §6.1's "To SCROLL A TARGET INTO VIEW target … with a scroll behavior behavior, a block flow direction
   position block, an inline base direction position inline, and an optional containing Element to stop
   scrolling after reaching container", as a cursor over step 2's ancestor walk: each `next` does one rest
   unit of work (opening a box, a run of its scrolling area's descendants, or steps 2.2 to 2.4 over it) and
   answers whether the walk continues, so a step machine can rest inside a box's measurement. The cursor holds only DOM nodes of the running flow, which a fork shares by address.
   `container` is NULL for §6's `container: "all"` and is the target itself for `"nearest"` (`scrollIntoView`'s
   step 5.4). The caller has already run `scrollIntoView`'s step 7: the target has an associated box. */
typedef enum {
    ES_INTO_VIEW_ANCESTORS = 1,   /* the ancestor elements of `walk` within `doc` */
    ES_INTO_VIEW_VIEWPORT,        /* `doc`'s viewport */
    ES_INTO_VIEW_CONTAINER,       /* out of `doc`, through its navigable's container */
    ES_INTO_VIEW_DONE
} ElementScrollingIntoViewPhase;

/* A scrolling box as §6.1's two position algorithms read it: its edges in client coordinates (CSSOM VIEW
   §6's `getBoundingClientRect()` space), its own extent, its scrolling area, its current scroll position
   and its overflow directions. element_scrolling.c states why edges are `CssPx` and positions `double`. */
typedef struct {
    lxb_dom_element_t *el;    /* the scroll container, or NULL when this box is the viewport */
    JSContext *dctx;          /* the realm whose active document this box belongs to */
    CssPx  lo[2], hi[2];      /* the box's own edges, in client coordinates */
    CssPx  area[2];           /* §2's scrolling area extent on each axis */
    CssPx  extent[2];         /* the box's own extent: an element's padding edge, or the viewport's ICB */
    double cur[2];            /* its current scroll position */
    bool   ending_at_hi[2];   /* §2's overflow directions: is the ending edge at the larger coordinate */
} ElementScrollingBox;

typedef struct {
    lxb_dom_element_t *target, *walk, *container;
    lxb_dom_node_t    *doc;
    ScrollLogicalPosition block, inline_pos;
    ElementScrollingIntoViewPhase phase;   /* where the walk continues once no box is open */
    /* The box being measured: its scrolling area is walked one rest unit of descendants per `next`, axis by
       axis, before steps 2.2 to 2.4 run over it. */
    bool                box_open;
    int                 axis;
    ElementScrollingBox box;
    ScrollingAreaWalk   area;
} ElementScrollingIntoView;

void element_scrolling_into_view_begin(ElementScrollingIntoView *w, lxb_dom_element_t *target,
                                       ScrollLogicalPosition block, ScrollLogicalPosition inline_pos,
                                       lxb_dom_element_t *container);
bool element_scrolling_into_view_next(ElementScrollingIntoView *w, const char *behavior);

/* Can a scrolling box in this realm's document be at a position other than the one this engine derives:
 * CSSOM VIEW §3.1 "Scrolling"'s perform a scroll ("when a user agent is to perform a scroll of a scrolling box
 * box, to a given position position"), asked as a capability of the component that owns the algorithm. Its
 * readers are steps that carry or restore what a scroll produces: HTML §6.6.6 "Focus management APIs"'
 * `focus()` step 4 and HTML §7.4.6.5 "Persisted history entry state"'s scroll position data.
 *
 * It is not `realm_awaits(ctx, "scrollTo", …)`: whether a member is installed on the global says nothing about
 * whether the algorithm can move a box, and an internal algorithm has no name there to probe (core/realm.h;
 * core/timing/hr_time.c's `crossOriginIsolated` is the same case). */

/* Both halves are derived, neither is a flag. The viewport half compares the two extents §4's `scroll()`
 * steps 7 and 8 clamp between: max(0, min(v, slack)) and min(0, max(v, -slack)) both land on the origin at
 * slack 0 and both reach past it at any larger slack, so the comparison needs no overflow direction. It is an
 * upper bound by design: it says a box could be elsewhere, so a reader's assert fires when the possibility
 * arrives. The element half follows from core/dom/perform_scroll.c's instant scroll, which DFAILs for an
 * element's box, so every element is at the origin core/dom/element_view.c's `scrollTop` getter step 8
 * derives; the diff that removes that DFAIL turns this half into a walk of the document's scroll containers. */
bool element_scrolling_box_can_move(JSContext *ctx);

#endif
