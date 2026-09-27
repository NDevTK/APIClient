/* THE CookieChangeEvent INTERFACE — COOKIE STORE API §5.1 "The CookieChangeEvent interface". See
 * cookie_change_event.c.
 *
 *     [Exposed=Window,
 *      SecureContext]
 *     interface CookieChangeEvent : Event {
 *       constructor(DOMString type, optional CookieChangeEventInit eventInitDict = {});
 *       [SameObject] readonly attribute FrozenArray<CookieListItem> changed;
 *       [SameObject] readonly attribute FrozenArray<CookieListItem> deleted;
 *     };
 *     dictionary CookieChangeEventInit : EventInit {
 *       CookieList changed;
 *       CookieList deleted;
 *     };
 *
 * WHICH STANDARD THIS IS, BECAUSE IT MOVED. The Cookie Store API was developed in the W3C WICG and is now a
 * WHATWG Living Standard at https://cookiestore.spec.whatwg.org/, whose own intellectual-property boilerplate
 * records that move; both old addresses 404. That is core/cookie_store/cookie_store.h's argument and it is
 * repeated here because this file is in a DIFFERENT directory, so a reader who arrives at this interface
 * through core/events/event.c's subclass list never passes that header. The boilerplate is REPORTED and not
 * quoted, for the reason that header gives: front matter sits outside every numbered section, so a quotation of
 * it checked against §5.1 is truthfully not found there and stays a finding for ever at a comment that is right.
 *
 * WHAT THIS FILE IS. It is the whole of §5.1 — the constructor, the two attributes and the dictionary — plus
 * the TRUSTED mint Cookie Store API §7.4 "Process changes"' `fire a change event` needs.
 * THIS PARAGRAPH SAID THERE WAS NO `..._new_to_fire` ENTRY AND THAT NOTHING IN THIS ENGINE FIRED ONE, AND IT IS
 * REWRITTEN RATHER THAN DELETED BECAUSE THE RULE IT STATED IS WHY THE ENTRY ARRIVED WHEN IT DID: an entry with
 * no caller is the write-with-no-reader half of §A-FIELD-A-CONSUMER-DEFAULTS, and the two halves of that defect
 * conceal each other, so the firing mint arrives in the diff that DISPATCHES. That diff is the one that landed
 * §7.4 in core/cookie_store/process_changes.c, and `cookie_change_event_new_to_fire` below is its only caller.
 *
 * WHY §5.1 ALONE IS NOT A STUB, WHICH IS THE ONE ORDERING QUESTION THIS FILE HAS TO ANSWER. §NO-STUBS' hazard
 * is that installing a name flips a page's feature-detect TRUE and abandons a fallback that works today, so the
 * unit of landing is the smallest diff that makes the guard's TRUE branch SURVIVABLE. The true branch of
 * `"CookieChangeEvent" in window` is `new CookieChangeEvent(…)` and reading its two attributes, and all of that
 * COMPLETES here: the constructor converts the page's own dictionary and the attributes answer what it was
 * initialized to. That is exactly what the `onchange` handler cannot say — its true branch waits for an event
 * this engine does not yet dispatch — which is why the handler is the LAST member of this subproblem and this
 * interface is the first.
 *
 * THE TWO SLOTS ARE FROZEN ARRAYS BUILT ONCE, which is what `[SameObject]` means. Web IDL §3.2.27 "Frozen
 * arrays" opens "Values of frozen array types are represented by frozen JavaScript Array object references",
 * so the value IS an object reference and an attribute that built a fresh one per read would answer false to
 * `e.changed === e.changed`. §5.1's own prose is the whole of what they return — "The changed and deleted
 * attributes must return the value they were initialized to."
 *
 * AN ABSENT DICTIONARY MEMBER IS AN EMPTY FROZEN ARRAY AND NOT UNDEFINED. `CookieChangeEventInit` declares
 * `CookieList changed` with no `required` and no default, so a page that writes neither leaves both unstated —
 * and the attribute's type is a NON-NULLABLE `FrozenArray<CookieListItem>`, which undefined is not a value of.
 * « » is also the only value the standard ever puts in those slots for an empty change set: §7.4's "prepare
 * lists from changes" steps 1 and 2 are "Let changedList be « »" and "Let deletedList be « »".
 *
 * THE SLOTS ARE OWN PROPERTIES UNDER A PRIVATE SYMBOL, for the reason core/events/event.c gives: a slot written
 * as a property write is captured by the COW delta, so the event's state time-travels with the flow that built
 * it, and the symbol is a brand a page cannot forge.
 *
 * IT IS A REAL SUBCLASS: `CookieChangeEvent.prototype.__proto__ === Event.prototype`, so `e instanceof Event`
 * holds and `initEvent` works on one. The base half is event_new_derived's, and browser/idl_inheritance.h
 * already carries { "CookieChangeEvent", "Event", IDL_PROTO_INHERITS } — which `idl_interface_tag` asserts this
 * install against rather than merely agreeing with.
 *
 * IT HAS NO createEvent ROW: DOM §4.5's table is a closed legacy list and this interface is not in it, so there
 * is no factory maker here and no default instance for one to hand back.
 *
 * THE INTERFACE OBJECT IS A PER-REALM INTRINSIC, declared into core/realm.h's one list beside the prototype —
 * §3.7 Interfaces gives each realm its own interface OBJECT for the same reason it gives each its own
 * prototype. It reaches the global through core/idl_args' exposure-asking door, because this interface carries
 * BOTH a constructor and [SecureContext] and is the first in this engine to carry both; that header states why
 * the `if` a caller reaches for instead is the shape that aborts a dev build. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_COOKIE_CHANGE_EVENT_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_COOKIE_CHANGE_EVENT_H

#include "quickjs.h"

/* Declared ONCE PER AGENT, from core/events/event.c's subclass list — its prototype chains to that realm's
   Event.prototype, and core/realm.h runs the per-realm installs in declaration order. */
void cookie_change_event_init(JSContext *ctx);
void cookie_change_event_install_protos(JSContext *ctx);
/* Undone ONCE PER AGENT. The RUNTIME, not a realm: what it gives back is the agent's — a private Symbol, a
   class id and this interface's member declarations — and every prototype it built is in some realm's
   class-proto slot and goes with that realm. Reached from core/events/event.c's event_free_subclasses, which is
   core/platform.c's `event` row. */
void cookie_change_event_free(JSRuntime *rt);

/* COOKIE STORE API §7.4 "Process changes"' `To fire a change event named type with changes at target` steps 1
 * to 6 — everything that algorithm does before its step 7 dispatch, which is the caller's.
 *
 * The steps, and each is here: "Let event be the result of creating an Event using CookieChangeEvent" — DOM
 * §2.5 "Constructing events"' create-an-Event, so isTrusted is TRUE, which the page-facing constructor's own
 * mint deliberately is not; "Set event's type attribute to type", which §7.4 step 1.4 names as "change" and
 * which is why this entry takes no type argument — that step is the only caller a Window's CookieStore has, and
 * §5.2 "The ExtendableCookieChangeEvent interface"' `cookiechange` is a different interface; "Set event's
 * bubbles and cancelable attributes to false"; and steps 5 and 6 setting the two attributes to the lists
 * `prepare lists from changes` returned.
 *
 * `changed` AND `deleted` ARE CONSUMED on every path, and they arrive as PLAIN Arrays. Web IDL §3.2.27 "Frozen
 * arrays — FrozenArray< T >"' create-a-frozen-array is applied HERE rather than by the caller, for the reason
 * `[SameObject]` needs a stored object at all: what §5.1 requires of those two slots is this interface's
 * contract, so a caller free to hand over an unfrozen array is a caller free to hand over a mutable one and
 * make `e.changed.push(...)` work. Returns JS_EXCEPTION with the throw live. */
JSValue cookie_change_event_new_to_fire(JSContext *ctx, JSValue changed, JSValue deleted);

#endif
