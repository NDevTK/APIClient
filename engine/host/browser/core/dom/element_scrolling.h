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
 * this engine and it is the DFAIL below. The viewport's route runs for real and lands where it already is
 * (core/frame/viewport.h derives the viewport's scrolling area as the initial containing block, so it has one
 * valid scroll position), so its promise is resolved when it is created. `scroll a target into view`'s own last
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

#endif
