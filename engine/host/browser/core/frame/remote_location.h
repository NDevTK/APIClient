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

#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"
#include "core/frame/window_proxy.h"   /* CrossOriginProperty — §7.2.1.3.1's record, shared by both its arms */

/* HTML §7.2.1.3.1 CrossOriginProperties ( O ) FOR A Location, verbatim: « { [[Property]]: "href",
   [[NeedsGetter]]: false, [[NeedsSetter]]: true }, { [[Property]]: "replace" } », in the standard's order.
 *
 * THE FLAGS ARE PART OF THE LIST AND NOT A SECOND ONE. It was two bare names, which answers "which members
 * does this object expose" and cannot answer "is this ACCESS to that member permitted" — the question
 * §7.2.1.1 Integration with IDL decides, and the one where the two entries DISAGREE: `href`'s [[NeedsGetter]]
 * is FALSE, so `Location.prototype`'s href SETTER goes through with a cross-origin receiver and its GETTER is
 * a "SecurityError". A name-only list makes those one answer.
 *
 * SPELLED ONCE, HERE, BECAUSE THIS IS THE OBJECT THE LIST DESCRIBES. Two components read it and each reads it
 * for a different half of one filter: this one EXPOSES exactly these names on a Location whose Document is a
 * peer's, and core/frame/location.c asserts at its own install that §7.2.4's IDL still declares both — a list
 * naming a member the interface does not have is the one way those two sections can drift, and a second copy
 * of the list is the only way that assert could pass while this object exposed something else. */
enum { LOCATION_XO_HREF, LOCATION_XO_REPLACE, LOCATION_XO_N };
extern const CrossOriginProperty LOCATION_CROSS_ORIGIN[LOCATION_XO_N];

/* IS THIS OBJECT A Location §7.2.1.3.3 IsPlatformObjectSameOrigin ANSWERS FALSE FOR — the brand §7.2.1.1
   Integration with IDL's step 1 needs, and half of what that step asks.
   THE OTHER HALF NEEDS NO BRAND, and that is a fact about this engine rather than a shortcut. §7.2.1.1 step 1
   returns for an object that is not a Window or Location, and its step 3 returns for a Location that IS same
   origin-domain — so for a Location THIS AGENT HOLDS the two arms reach the same answer, and an instance is an
   ORIGIN-KEYED AGENT CLUSTER (SECURITY.md), which is the header paragraph above stating that every such
   Location is same origin-domain with every realm that could be asking. There is therefore no observation that
   separates "not brand-tested" from "brand-tested and permitted" for core/frame/location.c's object, and a
   second brand asked at that step would be a test whose answer nothing reads.
   Side-effect-free. */
bool remote_location_is(JSValueConst v);

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
