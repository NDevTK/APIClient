/* CSSOM VIEW §3.1 "Scrolling"'s PERFORM A SCROLL — "When a user agent is to perform a scroll of a scrolling
 * box box, to a given position position, an associated element or pseudo-element element and optionally a
 * scroll behavior behavior".
 *
 * WHY IT IS A FILE OF ITS OWN. It is §3 "Common Infrastructure", not a member of anything, and it has two
 * callers in two directories: CSSOM VIEW §4 "Extensions to the Window Interface"' `scroll()` step 12 reaches it
 * for the VIEWPORT (core/frame/viewport.c) and §6.1 "Element Scrolling Members"' scroll-an-element and
 * scroll-a-target-into-view reach it for either kind of box (core/dom/element_scrolling.c). It is also the ONE
 * WRITER of a scroll position in this engine — nothing else may move a box — which is what makes "where does a
 * scroll position live" a question with one answer instead of one per member.
 *
 * A SCROLL POSITION IS PER-FLOW STATE, AND THIS FILE DOES NOT HOLD IT. Two flows that scrolled one document
 * differently must read back different numbers, so the position rides the per-flow COW delta; and the delta
 * captures it for free when it is held as ORDINARY PROPERTY WRITES on a per-realm record, which is what
 * core/frame/viewport.c does with it (the same shape as CSSOM VIEW §13.1's resize latch beside it). This file
 * holds no state at all: it is the ALGORITHM, and each kind of box's owner holds the position, because the
 * owner is what already answers `scrollX`/`scrollY` and `scrollTop` and a second holder would be a second
 * answer to one fact.
 *
 * EVERY SCROLL THIS USER AGENT PERFORMS IS INSTANT, AND THAT IS §3.1's OWN SENTENCE RATHER THAN A GAP. Step 5
 * is a conjunction whose FIRST conjunct is about the user agent: "If the user agent honors the scroll-behavior
 * property and one of the following is true" — so a user agent that does not honor css-overflow-3 §4.1 "Smooth
 * Scrolling: the scroll-behavior Property" takes the other arm for EVERY request, `behavior: "smooth"`
 * included. This cascade declares no such property, so `scroll-behavior` computes to its initial value for
 * every element in every document this engine parses and there is nothing for the property to be honored FROM.
 * That is asserted rather than assumed: the day core/css models the property, perform_scroll.c's assert fires
 * and names the smooth scroll to build.
 *
 * SO THE PROMISE IS RESOLVED AND THIS ENTRY RETURNS `void`. §3.1 steps 3 and 4 mint a promise and return it,
 * and step 7.2 resolves it after step 6's "Wait until either the position has finished updating, or
 * scrollPromise has been resolved" — and an INSTANT scroll has finished updating when step 5 returns, so every
 * scroll in this engine resolves its promise before the algorithm ends. §6.1's and §4's callers therefore mint
 * a resolved promise for their own members, which is honest for that reason and no longer for the one
 * core/dom/element_scrolling.h used to give: THAT SAID NO BOX HAD EVER REACHED §3.1 AT ALL, which this file
 * retires. The day a scroll can take TIME — a smooth scroll — this entry grows a promise-returning shape and
 * its callers stop minting one; it is not the day an element can hold a position.
 *
 * AND "BOX HAS AN ONGOING SMOOTH SCROLL" IS FALSE FOR EVERY BOX, for the same reason and not for the old one.
 * Three of §6.1's steps and §4's step 10 read that disjunct beside "position is not the same as the current
 * scroll position"; §3.1 step 5 is the only thing that starts a smooth scroll and this user agent never takes
 * that arm, so steps 1 ("Abort any ongoing smooth scroll for box.") and 2 ("Resolve all pending scroll
 * Promises whose scroll container is box.") have an empty work-set by construction. Both are asserted at the
 * site. THE REASON THAT USED TO BE GIVEN WAS THAT NOTHING REACHED §3.1, and this file is what makes that
 * false. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_PERFORM_SCROLL_H
#define ENGINE_HOST_BROWSER_CORE_DOM_PERFORM_SCROLL_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"

/* §3.1's perform a scroll, over the scrolling box of `ctx`'s ACTIVE document.
 * `box` is the scroll container the position belongs to, or NULL for that document's VIEWPORT — the two kinds
 *   §2 "Terminology" gives a scrolling box, and the discriminator §13.2's own steps use.
 * `x`/`y` are §3.1's `position`, ALREADY CLAMPED into the box's scrolling area by the caller: §4's steps 7-8
 *   and §6.1's own two rows are the same four rows (core/dom/element_scrolling.h), and clamping here as well
 *   would be a second statement of them free to disagree with the one the caller already made.
 * `element` is §3.1's ASSOCIATED ELEMENT — §4 step 12 passes "document's root element as the associated
 *   element, if there is one, or null otherwise", §6.1 passes the element whose box this is. Exactly one step
 *   reads it (step 5's computed `scroll-behavior`), and that is the step this user agent does not take, so it
 *   is carried and unread; the assert that says so is what names the day it must be threaded further.
 * `behavior` is §4's `ScrollBehavior` keyword the caller's dictionary carried, "auto" when omitted.
 * The CALLER has already made §3.1's own preceding test — §4 step 10 and §6.1's step 5 both abort when the
 * position is the one the box already has — so this entry is reached only for a position that differs, which
 * is asserted rather than trusted. */
void perform_scroll(JSContext *ctx, lxb_dom_element_t *box, double x, double y,
                    lxb_dom_element_t *element, const char *behavior);

/* A REQUESTED SCROLL POSITION ON ONE AXIS, BETWEEN THE MEMBER THAT READ IT AND THE STEP THAT PERFORMS A SCROLL.
 *
 * WHY IT IS HERE AND NOT IN EITHER MEMBER'S FILE. Two sections declare the same six scroll members over the
 * same two dictionary members — CSSOM VIEW §4 "Extensions to the Window Interface" for the VIEWPORT and §6
 * "Extensions to the Element Interface" for an element's box — and both end at the one algorithm this file
 * owns. A requested position is therefore ONE fact with two readers, and the decision it carries (below) is
 * one this engine may make only once: it WAS made twice, with §6's scroll members substituting the box's
 * current position at their read while §6's `scrollTop` setter crashed at its own, and the substitution's
 * justification cited the site that had already retired it. §3.1's perform a scroll is what every one of them
 * reaches, so the request that reaches it is declared beside it.
 *
 * CSSOM VIEW §3.2 "WebIDL values"' NORMALIZE NON-FINITE VALUES IS PART OF THE READ, not of the scroll: "When
 * asked to normalize non-finite values for a value x, if x is one of the three special floating point literal
 * values (Infinity, -Infinity or NaN), then x must be changed to the value 0". It is applied where the
 * member's own step applies it (CSSOM VIEW §4 "Extensions to the Window Interface" step 3, and §6's step 3 for
 * `scrollBy`), which is why the reader below performs it rather than `perform_scroll`.
 *
 * AN UNKNOWN IS CARRIED, NEVER SUBSTITUTED. §3.2.8's conversion crosses unknown external input AS ITSELF —
 * core/idl_args.h's `idl_concolic_rule` answers IDL_CONCOLIC_CROSSES for `unrestricted double` — so what a
 * member receives for `window.scrollTo(0, h)` with an opaque `h` is the page's own value. `unknown` is the
 * whole of what a body knows about it, and `px` is meaningful only where `unknown` is false: it is not an
 * example standing in for one, which is what `scroll_request_resolve` exists to enforce. */
typedef struct {
    bool unknown;
    double px;
} ScrollRequest;

/* A position this flow DETERMINED — the page's number after §3.2, or one the algorithm supplied itself. */
ScrollRequest scroll_request_px(double px);

/* ONE AXIS OF A SCROLL MEMBER'S REQUESTED POSITION, resolved to the coordinate the later steps use. Three ways
 * it arrives and each is the spec's or this engine's — none of them a zero standing in for a number:
 *   ABSENT is §4 step 1's "or the viewport's current scroll position on the x axis otherwise" and §6 step 1's
 *     identical sentence for an element, which is `current` and not a zero. A page writing
 *     `window.scrollTo({top: 40})` is asking for the x it already has, and answering 0 there would be a
 *     horizontal scroll it did not request. `relative` does NOT add to this arm: `scrollBy` with an absent
 *     member asks for no movement on that axis, which IS the current position.
 *   A NUMBER is the coordinate the page computed, with §3.2's normalize non-finite applied, plus `current`
 *     where the caller is the relative algorithm (§4's `scrollBy` steps 3-4, §6's the same).
 *   UNKNOWN EXTERNAL INPUT is carried, and a sum with an unknown addend is unknown — so `relative` needs no
 *     arm of its own here, and the addition becomes work for the concolic position `scroll_request_resolve`
 *     names rather than something this read can decide. */
ScrollRequest scroll_request_member(JSContext *ctx, JSValueConst member, double current, bool relative);

/* THE ONE PLACE ANY SCROLL MEMBER DECIDES WHAT AN UNKNOWN REQUESTED POSITION IS, reached only from a step that
 * PERFORMS A SCROLL. It is a crash and not a substitution, and that is what §3.1's arrival changed: passing the
 * box's CURRENT position in place of an unknown request made the write a no-op, which decided nothing while no
 * box could move, and now decides that the page's request had no effect over a domain most of whose members
 * land somewhere else. §RUN-DON'T-MATCH's rule against inventing a value cuts the same way against inventing
 * the absence of one. perform_scroll.c's crash names what to build.
 *
 * WHAT TO BUILD IS A CONCOLIC POSITION AND NOT A FORK, and the distinction is the whole of the design. A fork
 * asks a question with N answers; a scroll position is a VALUE with a domain, so the honest result of
 * `window.scrollTo(0, h)` or `documentElement.scrollTop = h` for an opaque `h` is a position that is
 * opaque-for-control-flow and carries the clamped example — after which `if (scrollY > 100)` forks at the
 * BRANCH, which is where CLAUDE.md's solver half puts a fork, and a page that never branches on it needs none.
 * A PREDECESSOR RECORDED THIS OBLIGATION AS A FORK, and that is written down as a correction rather than
 * silently replaced, because the next reader to re-derive "the setter owes a fork" will reach for the primitive
 * that cannot serve here: a plain C body has no machine state for a sibling to be snapshotted at, so a fork
 * from one crashes at the seam naming the operation — solver/decide.h says so at `solver_outcome` — and the
 * answer to that is `JS_CFUNC_STEP_DEF`, which is a large conversion bought for a question these sites do not
 * have. */
double scroll_request_resolve(ScrollRequest r);

#endif
