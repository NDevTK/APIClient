/* PERMISSIONS §6.1 AND §6.2 — the `Permissions` interface and the Navigator extension that exposes it. See
 * permissions.c.
 *
 *   [Exposed=(Window)]
 *   partial interface Navigator { [SameObject] readonly attribute Permissions permissions; };
 *   [Exposed=(Window,Worker)]
 *   interface Permissions { Promise<PermissionStatus> query(object permissionDesc); };
 *   dictionary PermissionDescriptor { required DOMString name; };
 *
 * WHY A MISSING `navigator.permissions` COSTS MORE THAN A MISSING VALUE. `navigator.permissions.query(...)` on
 * an engine that has no `permissions` is a TypeError on a property of undefined — thrown out of whatever
 * `then`-chain the bundle wrote it in, taking the feature-detect, the granted branch, the prompt branch AND
 * the denied branch with it. Three worlds and their endpoints are lost to one absent object, which is why this
 * interface has to exist even though this engine hosts none of the powerful features it answers about.
 *
 * §6.2.1's ALGORITHM NEVER THROWS, and that is the one thing an implementation of it gets wrong. Every one of
 * its failure steps says "return a promise REJECTED with" — a not-fully-active document, a descriptor
 * conversion that throws, an unsupported name — and Web IDL §3.7.7 says the same of the argument conversion
 * itself for any operation whose return type is a promise. A page writes `navigator.permissions.query(d)
 * .catch(...)`, and a synchronous throw goes straight past the catch it wrote. So the promise is created
 * FIRST, before the first thing that can fail, and every failure settles it. */
#ifndef ENGINE_HOST_BROWSER_CORE_PERMISSIONS_PERMISSIONS_H
#define ENGINE_HOST_BROWSER_CORE_PERMISSIONS_PERMISSIONS_H

#include "quickjs.h"

/* Declared ONCE PER AGENT, from core/frame/navigator.c's own declaration — §6.1 is a partial interface OF
   Navigator, so the component that owns Navigator is the one place every host reaches this through, and no
   host has a line to forget. It declares §3's model and §6.3's interface under it. */
void permissions_init(JSContext *ctx);
void permissions_free(void);

/* §6.1's `[SameObject] readonly attribute Permissions permissions` — this realm's one Permissions object, the
   value navigator.c's getter answers with. [SameObject] is a property of WHERE IT IS KEPT rather than of a
   cache the getter holds: there is one object per realm, minted with the realm, so no flow can make its own
   first read into every sibling's baseline. OWNED: the caller frees. */
JSValue permissions_object(JSContext *ctx);

#endif
