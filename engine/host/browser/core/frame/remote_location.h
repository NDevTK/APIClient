/* THE Location OF A DOCUMENT IN ANOTHER WASM INSTANCE — HTML §7.2.4 "The Location interface"'s cross-origin
 * arms (§7.2.4.1, §7.2.4.5, §7.2.4.6, §7.2.4.9, §7.2.4.10), over §7.2.1 "Security infrastructure for Window,
 * WindowProxy, and Location objects". See remote_location.c.
 *
 * IT IS A SECOND COMPONENT AND NOT A SECOND SPELLING OF core/frame/location.c. That file is §7.2.4's MEMBERS
 * over a Document THIS AGENT HOLDS: every one of them reads the document's address out of this heap, and its
 * §7.2.4 security check is a constant precisely because an instance is an ORIGIN-KEYED AGENT CLUSTER and so a
 * Document it hosts is same origin-domain with every realm that could be asking. This one is the object
 * §7.2.1.3.3 IsPlatformObjectSameOrigin answers FALSE for: it has no realm, no address and no members to read,
 * and the whole of it is §7.2.1.3.4 CrossOriginGetOwnPropertyHelper's two-entry filter plus §7.2.1.3.2
 * CrossOriginPropertyFallback's four names.
 *
 * THE TWO SETS ARE THE SAME SET, WHICH IS WHY THIS FILE MAY EXIST AT ALL. §7.2.4's arms split on
 * §7.2.1.3.3, and SECURITY.md keys an instance on `(browsing context group, origin)` — so "the current
 * settings object's origin is not same origin-domain with O's" and "this agent does not hold O's Document" are
 * one condition, and core/frame/window_proxy.c's proxy_realm asserts exactly that pairing at the boundary the
 * two components meet across. `document.domain` is a no-op here (core/dom/document_domain.c), so same origin
 * and same origin-domain are one question too.
 */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_REMOTE_LOCATION_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_REMOTE_LOCATION_H

#include <stdint.h>

#include "quickjs.h"

/* HTML §7.2.1.3.1 CrossOriginProperties ( O ) FOR A Location, verbatim: « { [[Property]]: "href",
   [[NeedsGetter]]: false, [[NeedsSetter]]: true }, { [[Property]]: "replace" } », in the standard's order.
 *
 * SPELLED ONCE, HERE, BECAUSE THIS IS THE OBJECT THE LIST DESCRIBES. Two components read it and each reads it
 * for a different half of one filter: this one EXPOSES exactly these names on a Location whose Document is a
 * peer's, and core/frame/location.c asserts at its own install that §7.2.4's IDL still declares both — a list
 * naming a member the interface does not have is the one way those two sections can drift, and a second copy
 * of the list is the only way that assert could pass while this object exposed something else. */
enum { LOCATION_XO_HREF, LOCATION_XO_REPLACE, LOCATION_XO_N };
extern const char *const LOCATION_CROSS_ORIGIN[LOCATION_XO_N];

/* Declared ONCE PER AGENT: the brand class, §7.2.1.3.1 CrossOriginProperties ( O )'s and §7.2.1.3.2
   CrossOriginPropertyFallback ( P )'s names, and the per-realm surface this REGISTERS (core/realm.h's
   intrinsic list). */
void remote_location_init(JSContext *ctx);
/* The AGENT's half undone — a ROW on core/platform.h's third column, which is why it takes the RUNTIME: the
   brand class, §7.2.1.3.1's and §7.2.1.3.2's interned names, the two pool entries §7.2.4's cross-origin
   members are, and the table of live objects are all registrations in a JSRuntime, and none of them is
   anything a realm owns. What makes the position safe is core/agent_state.h's own rule, met here rather than
   claimed: this component's finalizer reaches its record through JS_GetAnyOpaque and the table is emptied at
   the release, so the collection that runs afterwards frees every live object and scans nothing. */
void remote_location_free(JSRuntime *rt);

/* THE ONE Location FOR A PEER'S DOCUMENT — minted on the first ask and answered from a table on every ask
   after it, exactly as core/frame/window_proxy.c answers a peer's navigable with one WindowProxy.
   IDENTITY IS THE REASON IT IS A TABLE. `otherW.location === otherW.location` is a page-visible identity over
   one Document, and it is false the moment two reads each mint their own object. OWNED: the caller frees.
   `doc` is the world registry's handle for a document this agent does NOT hold — asserted, because a Document
   this agent hosts has a Location of its own (core/frame/location.h's location_object) and a second one here
   would be a second answer to a question that already has one. */
JSValue remote_location_of_document(JSContext *ctx, uint32_t doc);

#endif
