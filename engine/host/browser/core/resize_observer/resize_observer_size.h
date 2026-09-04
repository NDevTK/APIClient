/* RESIZE OBSERVER §2.3 — the `ResizeObserverSize` interface, the two-number record every box size in this
 * component is reported as. See resize_observer_size.c.
 *
 * IT IS ITS OWN COMPONENT because it is its own INTERFACE — `[Exposed=Window] interface ResizeObserverSize`
 * with two readonly attributes, appearing on the platform under its own name — and because it is what §3.4.8
 * "Calculate box size, given target and observed box" RETURNS, which makes it the unit three of §2.3's five
 * members are FrozenArrays OF and the unit §3.1's `lastReportedSizes` compares against. One problem per file.
 *
 * ITS TWO MEMBERS ARE `unrestricted double`, which is the IDL's own choice and not a detail: a box's extent
 * can legitimately be NaN or an infinity in this engine's arithmetic, and the restricted type would have made
 * §3.4.8 unable to report what it computed.
 *
 * EVERY MEMBER IS READONLY AND BOTH VALUES ARE DECIDED AT CONSTRUCTION, which is what makes the record's own
 * state one Array in an own slot rather than a C struct behind a class opaque: a size a flow computed must be
 * invisible to that flow's sibling and must survive a park to the cold tier, and an Array's writes are the
 * property writes the COW delta already captures (CLAUDE.md §PLATFORM DATA A FLOW QUEUES IS A JS VALUE). There
 * is no C record here, so there is no clone/finalizer/gc_mark triple to keep in step.
 */
#ifndef ENGINE_HOST_BROWSER_CORE_RESIZE_OBSERVER_RESIZE_OBSERVER_SIZE_H
#define ENGINE_HOST_BROWSER_CORE_RESIZE_OBSERVER_RESIZE_OBSERVER_SIZE_H

#include <stdbool.h>

#include "quickjs.h"

/* The AGENT's half: the class and the member ids, declared once. It registers the per-realm prototype install,
   so no host has a line to remember. */
void resize_observer_size_init(JSContext *ctx);
/* §3.7: THIS realm's ResizeObserverSize.prototype. A prototype held in one static would answer every
   document's `instanceof` out of whichever realm built it first. */
void resize_observer_size_install_proto(JSContext *ctx);
/* The interface object on one realm's global. */
void resize_observer_size_install(JSContext *ctx, JSValueConst global);
void resize_observer_size_free(JSRuntime *rt);

/* §3.4.8's "Let computedSize be a new ResizeObserverSize object", with both attributes already set — the
 * algorithm's own last step is "Return computedSize", so nothing outside this component ever holds one half
 * built.
 *
 * BOTH JSValues ARE CONSUMED, and both are `unrestricted double` values rather than C doubles for the reason
 * core/intersection_observer/intersection_observer_entry.h states at the same seam: a value a caller derived
 * from unknown external input crosses as ITSELF, and a C `double` parameter has nowhere to put one. §3.4.8's
 * own caller reads a used value out of core/layout, which carries the environment facts it is a function of
 * (core/frame/viewport.h's one seam mints the domain), so the two possibilities at this parameter are a Number
 * and a value that still forks control flow. The mint asserts exactly that pair.
 *
 * IT IS MINTED IN `ctx`, WHICH MUST BE THE REALM THE OBSERVER BELONGS TO — a [NewObject] belongs to the realm
 * whose interface produced it, and an entry handed to a callback out of a stranger's realm answers `instanceof`
 * false in the realm that is reading it. */
JSValue resize_observer_size_new(JSContext *ctx, JSValue inline_size, JSValue block_size);

/* IS THIS ONE — the class IS the brand, because this interface's instances are all minted here and a page has
   no constructor to mint another. Its one reader is the entry's own mint, which asserts that each of §2.3's
   three FrozenArray members was handed a size and not something shaped like one. */
bool resize_observer_size_is(JSValueConst v);

#endif
