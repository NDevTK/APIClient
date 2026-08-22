/* RFC 6265 HTTP STATE MANAGEMENT — §5.3's STORAGE MODEL and §5.4's COOKIE HEADER, as the ONE store an AGENT
 * has. Blink calls this file `core/loader/cookie_jar`, so this one is called that; see cookie_jar.c.
 *
 * WHY IT IS THE AGENT'S AND NOT A REALM'S. It was a realm's, held in a per-context slot beside the interface
 * prototypes, and that is a category error about what a cookie is:
 *
 *   - RFC 6265 §5.3 opens "The USER AGENT stores the following fields about each cookie", and §5.4 computes the
 *     cookie-string "from a COOKIE STORE and a REQUEST-URI" — one store, many request-URIs. A realm is not a
 *     party to either sentence.
 *   - HTML §3.1.4 states the consequence in its own words: "The cookie attribute's getter and setter
 *     synchronously access SHARED STATE. Since there is no locking mechanism, other browsing contexts in a
 *     multiprocess user agent can modify cookies while scripts are running." A per-realm store is a store no
 *     other browsing context can modify, which is the one property that paragraph says it does not have. The
 *     same section says the sharing is deliberate and is why paths are not a security feature: "Since the
 *     cookie attribute is accessible ACROSS FRAMES, the path restrictions on cookies are only a tool to help
 *     manage which cookies are sent to which parts of the site."
 *   - CLAUDE.md's SECURITY.md section says what an instance is: "AN INSTANCE IS AN ORIGIN-KEYED AGENT CLUSTER —
 *     (browsing-context group, origin) — because that IS the spec's heap boundary." Two same-origin documents
 *     in one browsing-context group are ONE agent, and a cookie is a fact about an ORIGIN's host, not about a
 *     document. So `frame.contentDocument.cookie = "s=1"` followed by `document.cookie` in the parent read two
 *     jars where a browser has one, and a bundle that stores a session token from an iframe and reads it from
 *     the top saw nothing — with no assert to say so, because each realm's answer was internally consistent.
 *
 * WHAT THE STORE IS KEYED BY, WHICH IS MORE THAN THE ORIGIN. §5.3 step 11 names the identity of a stored
 * cookie: "If the cookie store contains a cookie with the same NAME, DOMAIN, and PATH as the newly created
 * cookie" — one entry per (name, domain, path) triple, and the standard notes the algorithm maintains that as
 * an invariant. Those three are what this store's key is. They are NOT the origin: an origin is a (scheme,
 * host, port) tuple, and a cookie ignores the port entirely, admits a parent DOMAIN through §5.1.3's
 * domain-match, and carries a PATH the origin has no notion of. So "one jar per agent" is exact for the STORE
 * and would be wrong for the READ, and §5.4 is the read: it filters by domain-match, by path-match and by the
 * secure-only flag against ONE request-uri, and inside one agent it is the PATH that differs between documents.
 * Both halves are implemented here; see cookie_jar.c for what is modelled and what is not.
 *
 * WHAT CROSSES AN AGENT DOES NOT CROSS THIS. A DIFFERENT-ORIGIN document is a different instance, and its
 * cookies are its own store's — a read of one from here is a cross-instance read and is asserted, not served. */
#ifndef ENGINE_HOST_BROWSER_CORE_LOADER_COOKIE_JAR_H
#define ENGINE_HOST_BROWSER_CORE_LOADER_COOKIE_JAR_H

#include <stddef.h>

#include "quickjs.h"
#include "core/url/url.h"

/* THE AGENT'S DECLARATION — the store is built once per JSRuntime, at the PRE-BOOT BASELINE, which is what
   makes every flow's write to it a per-flow COW delta entry layered over one shared jar rather than one flow's
   creation becoming every sibling's baseline. */
void cookie_jar_init(JSContext *ctx);
/* Agent teardown: the store is the agent's, and it holds this runtime's strings. */
void cookie_jar_free(void);

/* §5.3 "RECEIVE A COOKIE" from `uri` for a "non-HTTP" API — the whole of what HTML §3.1.4's setter means by
   "act as it would when receiving a set-cookie-string for the document's URL via a non-HTTP API". `uri` is the
   REQUEST-URI: its host is §5.1.2's canonicalized request-host, its path is what §5.1.4's default-path is
   computed from, and its scheme is what §5.2.5's Secure attribute is measured against. A set-cookie-string the
   standard says to ignore leaves the store untouched. */
void cookie_jar_receive(JSContext *ctx, const UrlRecord *uri, const char *set_cookie, size_t len);

/* §5.4's COOKIE-STRING for `uri` for a "non-HTTP" API — the cookies of this store that domain-match, path-match
   and pass the secure-only test, sorted by §5.4 step 2 and serialized `name=value` joined by "; ".
   Returns an OWNED JS string. */
JSValue cookie_jar_cookie_string(JSContext *ctx, const UrlRecord *uri);

#endif
