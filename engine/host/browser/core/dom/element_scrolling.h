/* CSSOM VIEW §6.1 "Element Scrolling Members" — the three ALGORITHMS §6's members end in, split out of
 * core/dom/element_view.c because they are not members of anything.
 *
 * §6 IS A LIST OF IDL MEMBERS AND §6.1 IS A LIST OF ALGORITHMS OVER BOXES, and the boundary is the spec's own.
 * §6's `scrollTop` setter, its `scroll()`/`scrollTo()`/`scrollBy()` and its `scrollIntoView()` each run eight or
 * nine steps of ARGUMENT and DOCUMENT questions — is this the root element, is the document in quirks mode, is
 * the body potentially scrollable, what did the dictionary carry — and then hand off to one of these three:
 *
 *   SCROLL AN ELEMENT (or pseudo-element) element to x,y — the clamp into §2's scrolling area and one
 *     perform-a-scroll. §6's setter's step 11 and its scroll members' step 11 both end here.
 *   DETERMINE THE SCROLL-INTO-VIEW POSITION of a target, with a block position, an inline position and a
 *     SCROLLING BOX — the alignment of the target's bounding border box against that box's four flow-relative
 *     edges.
 *   SCROLL A TARGET INTO VIEW — the walk over "each ancestor element or viewport that establishes a scrolling
 *     box, in order of innermost to outermost", running the algorithm above once per box.
 *
 * WHY THE SPLIT IS WORTH A FILE. The two scroll-into-view algorithms are the only things in this engine that
 * reason about a scrolling box that is NOT the receiver of the call: they walk out of the element, out of its
 * document, and (for a same-origin child navigable) out into the container element's own ancestors. That is a
 * different contract from every member of §6, which answers about ONE element, and mixing the two put a
 * multi-document walk inside a file whose every other function opens with the same four questions about one
 * element's node document.
 *
 * WHAT THIS ENGINE CAN ANSWER, AND WHY THE ANSWER IS A DERIVATION RATHER THAN A SHRUG. Both entries below
 * return `void`, and their caller answers the page with a RESOLVED promise — which is honest for exactly one
 * reason: every path through them either performs no scroll at all, or CRASHES. §3.1 "Scrolling"'s perform a
 * scroll is what returns a promise that settles later, and there is exactly one route to it for an element in
 * this engine and it is the DFAIL below. The viewport's route runs for real and CRASHES the same way: §4's
 * scroll() step 10 is a two-sided assert that the clamped position is the one the viewport already has, and
 * with §2's viewport row built that clamp CAN land elsewhere, so the viewport's route is now the second of the
 * two crashes rather than the no-op it used to be. WHAT STOOD HERE said it LANDS WHERE IT ALREADY IS, because
 * core/frame/viewport.h derived the viewport's scrolling area as the initial containing block and an area equal
 * to its box has one valid position; the diff that built §2's viewport row retired that premise. (Retired prose
 * is restated in CAPITALS and never in quotation marks — a quoted sentence beside a section number is a SPEC
 * quotation, and citegen.mjs reads it as one.) `scroll a target into view`'s own last
 * step — "resolve scrollPromise when all Promises in ancestorPromises have settled" — is therefore a resolved
 * promise over a set of resolved promises. The day an element can hold a scroll position, this file grows a
 * promise-returning shape and its callers stop minting one; the crash is what makes that a change nobody can
 * forget to make.
 *
 * A SMOOTH SCROLL IS NEVER ONGOING, AND THAT TOO IS DERIVED. Three of §6.1's steps read "or box has an ongoing
 * smooth scroll" as a second disjunct beside "position is not the same as the current scroll position", and
 * this engine answers the disjunct FALSE for every box — not because smooth scrolling is unimplemented but
 * because §3.1's perform a scroll is the ONLY thing that starts one, and no box in this engine has ever reached
 * it: the element route crashes and the viewport route never changes a position, which §3.1's own step 7.1
 * makes the condition for emitting `scrollend` too. The DFAIL is what keeps that true. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_ELEMENT_SCROLLING_H
#define ENGINE_HOST_BROWSER_CORE_DOM_ELEMENT_SCROLLING_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"

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

/* The four keywords, in this enum's order — exported so the ONE list is the declaration's `values` array AND
   the mapping below, rather than two lists that can disagree about an order nothing else states.
   THE EXTENT IS DELIBERATELY UNWRITTEN, and it used to say `[5]`. A hand-written extent on an `extern` array is
   a SECOND COPY of the list's length, and the direction it drifts in is the silent one: a definition supplying
   MORE entries than the extern declares is TRUNCATED to the declared size with only a warning, and the entry
   truncation drops is the LAST one — which for a sentinel-terminated list is the TERMINATOR. The bound written
   to make the scan safe is then exactly what makes it run off the end. Nothing outside this component needs the
   extent (the mapping below scans for the terminator, and the IDL declaration takes a pointer), so leaving the
   type incomplete means no second copy of the length exists to go stale. */
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
   scrolling after reaching container".
   `container` is NULL for §6's `container: "all"` and is the TARGET ITSELF for `"nearest"` — that is
   `scrollIntoView`'s own step 5.4 ("if the container dictionary member of options is 'nearest', set container
   to the element") and not a convention invented here.
   The caller has already run `scrollIntoView`'s step 7: the target has an associated box. */
void element_scrolling_scroll_target_into_view(lxb_dom_element_t *target, const char *behavior,
                                               ScrollLogicalPosition block, ScrollLogicalPosition inline_pos,
                                               lxb_dom_element_t *container);

/* CAN A SCROLLING BOX IN THIS REALM'S DOCUMENT BE AT A POSITION OTHER THAN THE ONE THIS ENGINE DERIVES —
 * CSSOM VIEW §3.1 "Scrolling"'s PERFORM A SCROLL ("when a user agent is to perform a scroll of a scrolling box
 * box, to a given position position"), asked as the CAPABILITY it is and answered by the component that owns
 * the algorithm.
 *
 * WHAT ASKS IT, AND WHY THE QUESTION IS WORTH EXPORTING. CSSOM VIEW §13.2 "Scrolling" gives every Document "an
 * associated list of pending scroll events, which stores pairs of (EventTarget, DOMString), initially empty",
 * and the ONLY thing that appends to it is a viewport or an element that "gets scrolled". Steps in two other
 * standards drain or carry what that append produces, and each has to know whether a box in this build can move
 * at all: HTML §8.1.7.3 "Processing model" update-the-rendering step 9 ("for each doc of docs, run the scroll
 * steps for doc"), and HTML §7.4.6.5 "Persisted history entry state"'s scroll position data. One fact, two
 * readers, one derivation.
 *
 * WHY IT IS NOT `realm_awaits(ctx, "scrollTo", …)`, WHICH IS WHAT THOSE READERS ASKED. A [[HasProperty]] on the
 * global answers whether a MEMBER IS INSTALLED, and no member is this capability — the two are not even
 * correlated. `Element.prototype.scrollTo` IS installed in this build (core/dom/element_view.c) and moves
 * nothing; CSSOM VIEW §4's `scroll`/`scrollTo`/`scrollBy` on the Window move nothing either, because their
 * whole body is §4's argument questions and a call to `viewport_scroll`, which cannot MOVE a viewport at all —
 * its steps 12-13 are unwritten and its step 10 crashes rather than pretending. (That clause used to read
 * "whose clamp lands on the position the viewport already has", which was the same conclusion resting on the
 * scrolling area being the initial containing block; the area is §2's real extreme now, so the reason is the
 * missing §3.1 and no longer the collapsed clamp.) So the name test was neither necessary nor sufficient, and its failure was
 * PRE-LOADED rather than latent: installing §4's three members satisfies it, so the readers would have fired
 * announcing a capability that had not arrived — a probe that reports a capability as PRESENT is worse than no
 * probe, because the next reader builds on it. core/timing/hr_time.c records the identical defect over
 * `crossOriginIsolated` and states the cure this follows: it is a component's answer and not a probe of the
 * global. A producer that is an INTERNAL ALGORITHM has no name on the global to probe for, so `realm_awaits`
 * is the wrong instrument for it by construction — core/realm.h says so at the mechanism.
 *
 * THE ANSWER IS DERIVED ON BOTH HALVES AND NEITHER HALF IS A STORED FLAG.
 *   THE VIEWPORT half is derived LIVE, from the clamp's own two operands: §4's `scroll()` steps 7 and 8 clamp a
 *   request into §2's scrolling area of the viewport, and whichever of each step's two arms the OVERFLOW
 *   DIRECTION selects, the range the clamp has to land in is empty exactly when that area equals the viewport —
 *   max(0, min(v, slack)) and min(0, max(v, -slack)) are both the origin at slack 0 and both reach past it at
 *   any larger slack. So the comparison below is the direction-independent half of the clamp and needs no
 *   direction of its own. It reads the clamp's own two functions, so a layout that extends the area past the
 *   initial containing block makes this answer true with nobody having to remember it — which is what happened:
 *   this used to argue that THE ICB IS THE VIEWPORT, SO THE CLAMP HAS NOWHERE TO LAND BUT THE POSITION THE
 *   VIEWPORT ALREADY HAS, and §2's viewport row retired that premise.
 *   THE ELEMENT half is derived FROM THE CRASH. §6.1's perform-a-scroll step in element_scrolling.c is the one
 *   site for "an element cannot hold a scroll position" and it DFAILs rather than moving one, so no element in
 *   this engine has ever been anywhere but the origin core/dom/element_view.c's `scrollTop` getter step 8
 *   derives. That DFAIL names this function, so the diff that deletes it is the diff that turns this half into
 *   a read of real state. */
bool element_scrolling_box_can_move(JSContext *ctx);

#endif
