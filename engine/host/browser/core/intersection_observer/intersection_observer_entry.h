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
 * intersectionRect, isIntersecting, and target", plus §3.2.10 step 2.2.18's `isVisible` and `intersectionRatio`,
 * which that step passes and §3.2.6's own prose has not caught up with. The argument ORDER here is §3.2.10 step
 * 3.18's, which is the caller that exists.
 *
 * `time` is a DOMHighResTimeStamp already relative to `ctx`'s time origin — HR-TIME §4's relative high
 * resolution time, computed by the caller, because "relative to the time origin of the global object associated
 * with the IntersectionObserver instance" (§2.3) is a question about a realm and not about a moment.
 * `root_bounds` is the root intersection rectangle or JS_NULL, which is what §2.3's `readonly attribute
 * DOMRectReadOnly? rootBounds` declares and what a cross-origin-domain target answers. Every JSValue is
 * CONSUMED; `target` is borrowed. The entry is minted in `ctx`, which must be the realm the observer belongs
 * to.
 *
 * EIGHT JSValues AND NOT THREE C SCALARS, WHICH IS THE ONE DESIGN DECISION IN THIS HEADER. `time`,
 * `is_intersecting` and `is_visible` were a `double` and two `bool`s, which is exactly what §3.2.10's caller
 * has and exactly what §2.3's page-visible constructor does NOT: an initialiser's members cross Web IDL's
 * boundary as THEMSELVES when they carry unknown external input (core/idl_args.h's IDL_CONCOLIC_CROSSES — the
 * value has to keep forking control flow and stay solvable at a sink), so `new IntersectionObserverEntry({time:
 * u, isIntersecting: v, …})` has no C number and no C truth value to give. Coercing one is the de-tainting
 * placeholder core/geometry/dom_rect.c refused for the same reason at the same kind of record, and both of the
 * doors that would have done it say so at their own site: idl_args.h forbids a body calling JS_ToFloat64 on its
 * own argument, and idl_dict_bool ABORTS on an unknown because ToBoolean pins every one of them to `true`.
 * The eight members are never branched on by this engine — the getters hand back what was stored — so there is
 * nothing here that needs a C scalar, and a determined caller writes JS_NewFloat64 / JS_NewBool at its own
 * seam where the value it holds is real. */
JSValue intersection_observer_entry_new(JSContext *ctx, JSValue time, JSValue root_bounds, JSValue bounding,
                                        JSValue intersection, JSValue is_intersecting, JSValue is_visible,
                                        JSValue ratio, JSValueConst target);

#endif
