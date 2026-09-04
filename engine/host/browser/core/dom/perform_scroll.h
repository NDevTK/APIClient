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

#endif
