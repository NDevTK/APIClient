/* INTERSECTION OBSERVER §2.3 — the `IntersectionObserverEntry` interface, the record §3.2.6 queues and §3.2.5
 * hands the page's callback. See intersection_observer_entry.c.
 *
 * IT IS ITS OWN COMPONENT because it is its own INTERFACE with its own constructor and its own dictionary, and
 * because the thing it is a record OF — a rectangle — belongs to a third standard again (Geometry Interfaces
 * §3, core/geometry/dom_rect.h). §3.2.10 computes; this holds what was computed and answers the eight members
 * the IDL declares. One problem per file.
 *
 * EVERY MEMBER IS READONLY AND EVERY VALUE IS DECIDED AT CONSTRUCTION, which is what makes the record's own
 * state one Array in an own slot rather than a C struct behind a class opaque: an entry a flow queued must be
 * invisible to that flow's sibling and must survive a park to the cold tier, and an Array's writes are the
 * property writes the COW delta already captures (CLAUDE.md §PLATFORM DATA A FLOW QUEUES IS A JS VALUE). There
 * is no C record here, so there is no clone/finalizer/gc_mark triple to keep in step.
 */
#ifndef ENGINE_HOST_BROWSER_CORE_INTERSECTION_OBSERVER_INTERSECTION_OBSERVER_ENTRY_H
#define ENGINE_HOST_BROWSER_CORE_INTERSECTION_OBSERVER_INTERSECTION_OBSERVER_ENTRY_H

#include <stdbool.h>

#include "quickjs.h"

/* The AGENT's half: the class and the member ids, declared once. It registers the per-realm prototype install,
   so no host has a line to remember. */
void intersection_observer_entry_init(JSContext *ctx);
/* §3.7: THIS realm's IntersectionObserverEntry.prototype. */
void intersection_observer_entry_install_proto(JSContext *ctx);
/* The interface object on one realm's global. */
void intersection_observer_entry_install(JSContext *ctx, JSValueConst global);
void intersection_observer_entry_free(JSRuntime *rt);

/* §3.2.6 step 1 — "CONSTRUCT an IntersectionObserverEntry, passing in time, rootBounds, boundingClientRect,
 * intersectionRect, isIntersecting, and target", plus §3.2.10 step 3.18's `isVisible` and `intersectionRatio`,
 * which that step passes and §3.2.6's own prose has not caught up with. The argument ORDER here is §3.2.10 step
 * 3.18's, which is the caller that exists.
 *
 * `time` is a DOMHighResTimeStamp already relative to `ctx`'s time origin — HR-TIME §4's relative high
 * resolution time, computed by the caller, because "relative to the time origin of the global object associated
 * with the IntersectionObserver instance" (§2.3) is a question about a realm and not about a moment.
 * `root_bounds` is the root intersection rectangle or JS_NULL for a cross-origin-domain target; the three
 * rectangles and `ratio` are CONSUMED, `target` is borrowed. The entry is minted in `ctx`, which must be the
 * realm the observer belongs to. */
JSValue intersection_observer_entry_new(JSContext *ctx, double time, JSValue root_bounds, JSValue bounding,
                                        JSValue intersection, bool is_intersecting, bool is_visible,
                                        JSValue ratio, JSValueConst target);

#endif
