/* RESIZE OBSERVER §2.3 — the `ResizeObserverEntry` interface, the record §3.4.4 "Create and populate a
 * ResizeObserverEntry" builds and §3.4.5 hands the page's callback. See resize_observer_entry.c.
 *
 * IT IS ITS OWN COMPONENT because it is its own INTERFACE with its own five members, and because two of the
 * things it is a record OF belong to other standards again — a rectangle to Geometry Interfaces §3
 * (core/geometry/dom_rect.h) and each box size to §2.3's own second interface
 * (core/resize_observer/resize_observer_size.h). §3.4.4 and §3.4.8 COMPUTE; this holds what was computed and
 * answers the five members the IDL declares. One problem per file.
 *
 * EVERY MEMBER IS READONLY AND EVERY VALUE IS DECIDED AT CONSTRUCTION, which is what makes the record's own
 * state one Array in an own slot rather than a C struct behind a class opaque: an entry a flow built must be
 * invisible to that flow's sibling and must survive a park to the cold tier, and an Array's writes are the
 * property writes the COW delta already captures (CLAUDE.md §PLATFORM DATA A FLOW QUEUES IS A JS VALUE). There
 * is no C record here, so there is no clone/finalizer/gc_mark triple to keep in step.
 *
 * §2.3 DECLARES NO CONSTRUCTOR FOR THIS INTERFACE, and that is the whole difference from Intersection
 * Observer's entry beside it: `IntersectionObserverEntry` has a page-visible constructor taking a dictionary,
 * and `ResizeObserverEntry` has none at all — so the only producer anywhere is §3.4.4, and the interface object
 * a page finds is Web IDL §3.7.1 Interface object's non-constructible form.
 */
#ifndef ENGINE_HOST_BROWSER_CORE_RESIZE_OBSERVER_RESIZE_OBSERVER_ENTRY_H
#define ENGINE_HOST_BROWSER_CORE_RESIZE_OBSERVER_RESIZE_OBSERVER_ENTRY_H

#include <stdbool.h>

#include "quickjs.h"

/* The AGENT's half: the class and the member ids, declared once. It registers the per-realm prototype install,
   so no host has a line to remember, and it declares §2.3's OTHER interface (the size record) with it. */
void resize_observer_entry_init(JSContext *ctx);
/* §3.7: THIS realm's ResizeObserverEntry.prototype. */
void resize_observer_entry_install_proto(JSContext *ctx);
/* The interface object on one realm's global. */
void resize_observer_entry_install(JSContext *ctx, JSValueConst global);
void resize_observer_entry_free(JSRuntime *rt);

/* §3.4.4's WHOLE ALGORITHM'S RESULT — "Let this be a new ResizeObserverEntry", with the five members its
 * steps 2 to 7 set already decided by the caller.
 *
 * THE THREE FrozenArrays ARE MINTED HERE AND NOT PASSED IN, which is the one design decision in this header.
 * §2.3 declares `borderBoxSize`, `contentBoxSize` and `devicePixelContentBoxSize` as
 * `FrozenArray<ResizeObserverSize>`, and Web IDL §3.2.27 Frozen arrays makes that a FROZEN Array — a property
 * of the INTERFACE rather than of the computation, so a caller handing over a bare Array could hand over an
 * unfrozen one and a page would then be able to write the size back. The caller passes the three SIZES and
 * this component wraps and freezes each; §2.3's own note is why each array holds exactly one:
 *
 *     "In this spec, there will only be a single ResizeObserverSize returned in the FrozenArray, which will
 *      correspond to the dimensions of the first column."
 *
 * NAMED RESIDUAL — MULTI-FRAGMENT BOXES. That note is a narrowing this level of the standard states about
 * itself, and it is exactly what this component implements: one size per array. WHAT IS NOT COVERED is a
 * target broken into several fragments by a multi-column container, for which a future level extends the array
 * to carry a size per fragment. WHAT THE NEXT DIFF BUILDS is a per-fragment size list, and it can only be
 * built once core/layout produces fragments for a multi-column box — core/dom/element_view.h's
 * `element_view_fragment_kind` is where this engine's answer to "how many fragments does this box have" is
 * asked. HOW ITS ABSENCE WOULD SHOW: a page reading `entry.borderBoxSize.length` inside a `column-count`
 * container gets 1 where a browser implementing a later level gives it the column count.
 *
 * `target` is BORROWED; the rectangle and the three sizes are CONSUMED. The entry is minted in `ctx`, which
 * must be the realm the observer belongs to — a [NewObject] belongs to the realm whose interface produced it.
 */
JSValue resize_observer_entry_new(JSContext *ctx, JSValueConst target, JSValue content_rect,
                                  JSValue border_box_size, JSValue content_box_size,
                                  JSValue device_pixel_content_box_size);

#endif
