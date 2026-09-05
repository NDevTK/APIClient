/* COOKIE STORE API §3 The CookieStore interface — the asynchronous read of the cookie store this agent already
 * has, over RFC 6265's jar in core/loader/cookie_jar.c.
 *
 * WHICH STANDARD THIS IS, BECAUSE IT MOVED. The Cookie Store API was developed in the W3C WICG and is now a
 * WHATWG Living Standard: "Cookie Store API Standard", https://cookiestore.spec.whatwg.org/, whose own
 * boilerplate records the move — "This Living Standard was originally developed in the W3C WICG". Its old
 * WICG address 404s. Every citation in this component is to the WHATWG document's section numbers.
 *
 * WHAT THIS COMPONENT IS AND IS NOT. It is §3.1's `get` and §3.2's `getAll` — the QUERY half, §7.1 "Query
 * cookies" — and the §6.1 Window member that reaches them. It is NOT §3.3's `set` or §3.4's `delete`, and that
 * absence is a SUBPROBLEM ORDER rather than an oversight: §7.2's step 12.3 needs a registrable-domain-suffix
 * predicate that exists in this tree but is private to another component, and §7.1 needs nothing that is not
 * built. See the file header and the named residual in cookie_store.c. A page that calls `cookieStore.set` finds it absent and throws, which is what §NO STUBS
 * asks for and is the forcing function for the diff that builds §7.2.
 *
 * WHY THE JAR AND NOT A STORE OF ITS OWN. §2.2 "Cookie store" says the object is a view: this API and
 * `document.cookie` read ONE store, so a cookie written through either is a cookie the other reads. Two stores
 * would be two timelines wearing one name — the defect cookie_jar.h's header argues at length about one level
 * down, where the store was a realm's and an iframe's session token was invisible to its parent.
 *
 * THE VALUES IT HANDS A PAGE ARE CONCOLIC, through the same seam `document.cookie` uses and for the same
 * reason: a cookie is external input. See cs_item in cookie_store.c for which of the two strings is wrapped
 * and why the other is not. */
#ifndef ENGINE_HOST_BROWSER_CORE_COOKIE_STORE_COOKIE_STORE_H
#define ENGINE_HOST_BROWSER_CORE_COOKIE_STORE_COOKIE_STORE_H

#include "quickjs.h"

/* Declared ONCE PER AGENT; the per-realm install is declared from here through core/realm.h's one list. */
void cookie_store_init(JSContext *ctx);
/* Agent teardown: the class and the declared ids are the agent's. */
void cookie_store_free(void);

#endif
