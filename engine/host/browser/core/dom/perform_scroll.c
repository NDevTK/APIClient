/* CSSOM VIEW §3.1 "Scrolling"'s perform a scroll. See perform_scroll.h for why it is a file of its own, why it
   holds no state, and why every scroll this user agent performs is an INSTANT one. */
#include <math.h>
#include <stdbool.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "core/css/css_computed_value.h"
#include "core/dom/perform_scroll.h"
#include "core/dom/scroll_events.h"
#include "core/frame/viewport.h"

/* §3.1 STEP 5's FIRST CONJUNCT, WHICH IS ABOUT THE USER AGENT AND NOT ABOUT THE REQUEST: "If the user agent
   honors the scroll-behavior property and one of the following is true". Both of the things that could make
   the SECOND conjunct true — a `behavior` of `smooth`, and an `element` whose computed `scroll-behavior` is
   smooth — are therefore unreachable while this is false, which is why neither operand is examined here.
   IT IS FALSE BY DERIVATION AND THE DERIVATION IS ASSERTED. css-overflow-3 §4.1 "Smooth Scrolling: the
   scroll-behavior Property" declares the property; this cascade declares no such property, so no element in
   any document this engine parses has a computed value for it and there is nothing to honor. The assert is
   two-sided: the day core/css models it, this fires at the one step that would then have a second arm. */
static bool ps_honors_scroll_behavior(void)
{
    DCHECK(!css_computed_models("scroll-behavior"),
           "css-overflow-3 §4.1 \"Smooth Scrolling: the scroll-behavior Property\" is in this cascade now, so "
           "CSSOM VIEW §3.1 \"Scrolling\"'s perform a scroll has a step 5 arm this engine does not run: \"then "
           "perform a smooth scroll of box to position\". BUILD the smooth scroll — a scroll that changes the "
           "box's position over TIME, so it is a flow that suspends between rendering opportunities and the "
           "one thing in this engine that makes §3.1's scrollPromise settle LATER than the call; step 1's "
           "abort-any-ongoing and step 2's resolve-all-pending stop being empty work-sets the same day, and "
           "the `element` argument this file carries unread is the operand of step 5's first bullet");
    return false;
}

/* §3.1 STEP 5's OTHER ARM — "otherwise, perform an instant scroll of box to position" — and step 6, which for
   an instant scroll ("Wait until either the position has finished updating, or scrollPromise has been
   resolved.") is over the moment this returns.
   RETURNS §3.1 STEP 7.1's OWN CONDITION, "If the scroll position changed as a result of this call": it is
   asked of the position BEFORE and AFTER, at the one place both are in hand, rather than inferred from the
   caller's own abort step — those are two different questions and only this one is about what happened.
   THE POSITION IS THE BOX OWNER'S, NEVER THIS FILE'S — perform_scroll.h states why. */
static bool ps_instant_scroll(JSContext *ctx, lxb_dom_element_t *box, double x, double y)
{
    double before_x, before_y;

    if (box != NULL) {
        DFAIL("CSSOM VIEW §3.1 \"Scrolling\"'s perform a scroll was reached for an ELEMENT's scrolling box, and "
              "an element in this engine has nowhere to put a scroll position. BUILD the store: it is per-flow "
              "state, because two flows that scrolled one element differently must read back different values, "
              "so it belongs in the COW delta — core/frame/viewport.c holds the VIEWPORT's as ordinary property "
              "writes on a per-realm record for exactly that reason, and an element's wants the same shape keyed "
              "by the element. core/dom/element_view.c's `scrollTop`/`scrollLeft` getter is the READER of the "
              "same fact, so the store and that getter are one diff: building this without it would move a box "
              "no member could report having moved");
        return false;
    }
    /* THE VIEWPORT'S POSITION, read and written through the component that owns it. `viewport_exists` is the
       caller's step (§4's own step 4, and §6.1's box constructor asserts it), so it is asserted and not
       re-asked — a viewport that vanished between the clamp and the write would be a navigable losing its
       document mid-algorithm. */
    DCHECK(viewport_exists(ctx),
           "§3.1's perform a scroll reached the VIEWPORT of a realm that is presenting no document — §4's own "
           "step 4 (\"If there is no viewport, return a resolved `Promise` and abort the remaining steps.\") "
           "and §6.1's scrolling-box constructor each answer that before this algorithm is entered");
    before_x = viewport_scroll_x(ctx);
    before_y = viewport_scroll_y(ctx);
    DCHECK(x != before_x || y != before_y,
           "§3.1's perform a scroll was asked to move the viewport to the position it is already at. Every "
           "caller has an abort step for exactly that — §4's step 10 and §6.1's scroll-an-element step 5 both "
           "return a resolved Promise there — so reaching this line means one of them was skipped, and step "
           "7.1's \"changed as a result of this call\" would then be answered by a scroll that did not happen");
    viewport_set_scroll_position(ctx, x, y);
    return x != before_x || y != before_y;
}

void perform_scroll(JSContext *ctx, lxb_dom_element_t *box, double x, double y,
                    lxb_dom_element_t *element, const char *behavior)
{
    bool changed;

    DCHECK(ctx != NULL, "§3.1's perform a scroll was invoked with no realm — a scrolling box belongs to a "
                        "document, and which document is what the realm names");
    DCHECK(behavior != NULL,
           "§3.1's perform a scroll was invoked with no scroll behavior — §4 declares `ScrollBehavior` with a "
           "default of `auto` and the dictionary carries that default, so an absent member arrives as the "
           "keyword rather than as nothing");
    DCHECK(isfinite(x) && isfinite(y),
           "§3.1's perform a scroll was handed a non-finite position. CSSOM VIEW §3.2 \"WebIDL values\"' "
           "normalize non-finite values is the CALLER's step and the clamp after it is total, so a non-finite "
           "here is one of those two having been skipped");
    /* STEP 1 — "Abort any ongoing smooth scroll for box." — and STEP 2 — "Resolve all pending scroll
       Promises whose scroll container is box." Both have an EMPTY work-set by construction rather than by
       omission: step 5 below is the only thing that starts a smooth scroll, this user agent never takes that
       arm, and every promise this algorithm mints is resolved before it returns. The assert in
       `ps_honors_scroll_behavior` is the one that fires the day either becomes possible, so it is stated once
       there rather than restated twice here. */
    /* STEPS 3 AND 4 — "Let scrollPromise be a new `Promise`." / "Return scrollPromise, and run the remaining
       steps in parallel." This entry returns `void` and its callers mint a RESOLVED promise for their own
       members, because the remaining steps of an instant scroll complete before this call does. See
       perform_scroll.h; it is the instant scroll that makes that honest and not the absence of a caller. */
    /* STEP 5 */
    if (ps_honors_scroll_behavior()) {
        DFAIL("§3.1's perform a scroll took step 5's SMOOTH arm, which `ps_honors_scroll_behavior` above "
              "answers false for every request in this engine — the two have come apart");
    }
    (void)element;
    (void)behavior;
    changed = ps_instant_scroll(ctx, box, x, y);   /* step 5's other arm, and step 6 */
    /* STEP 7 — "If scrollPromise is still in the pending state" — is TRUE here, because step 4's promise is
       the one this call is resolving and nothing else can have. Its substep 7.2 resolves it, which is what the
       caller's resolved promise IS; its substep 7.1 is the line below. */
    if (changed) {
        /* CSSOM VIEW §13.2 "Scrolling"'s own producer, which is a DIFFERENT algorithm from the step below and
           runs first: "Whenever a viewport gets scrolled (whether in response to user interaction or by an
           API), the user agent must run these steps", ending in "Append (doc, \"scroll\") to doc's pending
           scroll events." A scroll that changed the position IS a viewport getting scrolled, which is why it
           is run HERE and not at §3.1's own step 7.1.
           IT IS ONLY EVER THE VIEWPORT, because `ps_instant_scroll` above aborts for an element's box — so
           §13.2's element-gets-scrolled steps have no caller and its visual-viewport ones have none either
           (nothing in this engine scrolls a VisualViewport: core/frame/visual_viewport.h derives a scale
           factor of 1, at which it covers the layout viewport and has no pan of its own). */
        scroll_events_viewport_scrolled(ctx);
        /* STEP 7.1 — "If the scroll position changed as a result of this call, emit the scrollend event." —
           which for §13.2 means making this box one that WAS SCROLLED; the scroll steps' own step 1 is what
           turns that into a pair, one rendering opportunity later. core/dom/scroll_events.h states why the
           two are not collapsed even though an instant scroll puts them in the same frame. */
        scroll_events_viewport_scrollend(ctx);
    }
}
