/* BEACON §2.1 sendBeacon() Method — `partial interface Navigator`, Blink modules/beacon.
 *
 * ITS OWN COMPONENT AND NOT A MEMBER OF navigator.c, because it is not the same problem. That file is HTML
 * §8.10.1's CLIENT IDENTITY — a record of values a realm answers with. This is a REQUEST: it parses a URL
 * against the API base URL, runs Fetch §5.2's BodyInit extraction, and composes a POST. Putting it there would
 * pull core/fetch and solver/endpoint into the identity file for one member.
 *
 * DECLARED FROM navigator_init AND INSTALLED FROM navigator's OWN per-realm intrinsic, which is the rule
 * Permissions §6 already follows for the same reason: a host that has a Navigator has `navigator.sendBeacon`,
 * so a per-host line would be the hand-copied list core/realm.h exists to abolish. The install takes the
 * PROTOTYPE rather than the instance because §2.1 declares an OPERATION on the interface: a method defined on
 * the object would be an own property of `navigator` (which no browser has), would answer `undefined` from
 * `Navigator.prototype.sendBeacon`, and would be `delete`-able. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_NAVIGATOR_BEACON_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_NAVIGATOR_BEACON_H
#include "quickjs.h"

/* §2.1's member declaration, once per AGENT. Called from navigator_init. */
void navigator_beacon_init(JSContext *ctx);

/* Install `sendBeacon` on ONE realm's Navigator.prototype. Called from navigator's per-realm intrinsic, which
   is what guarantees the prototype exists and that every realm — the agent's first and every child
   navigable's — gets the member on ITS OWN prototype. */
void navigator_beacon_install(JSContext *ctx, JSValueConst proto);

void navigator_beacon_free(void);

#endif
