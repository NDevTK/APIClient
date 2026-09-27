/* COOKIE STORE API §3 The CookieStore interface — the asynchronous read of the cookie store this agent already
 * has, over RFC 6265's jar in core/loader/cookie_jar.c.
 *
 * WHICH STANDARD THIS IS, BECAUSE IT MOVED. The Cookie Store API was developed in the W3C WICG and is now a
 * WHATWG Living Standard at https://cookiestore.spec.whatwg.org/, whose own intellectual-property boilerplate
 * records that move. Both old addresses 404 — wicg.github.io/cookie-store and w3c.github.io/webappsec-cookie-
 * store — so a reader correcting this URL from memory would break every citation below. Every citation in
 * this component is to the WHATWG document's section numbers.
 * THE BOILERPLATE IS PARAPHRASED HERE AND NOT QUOTED, AND THAT IS A RULE RATHER THAN A PREFERENCE. A
 * standard's front and back matter sits OUTSIDE every numbered section, so a section-keyed corpus cannot hold
 * it: a quoted run of it standing under this banner's §3 citation is checked against §3, is truthfully not
 * found there, and stays a finding for ever at a comment that is correct. That is the cry-wolf direction
 * CLAUDE.md rates worse than no citation at all — a permanent red teaches a reader to skim the category every
 * real finding sits in. So a sentence worth keeping from un-numbered matter is REPORTED, never quoted; only
 * text a section owns is quoted. Measured: this exact run was the single finding the cookiestore index
 * surfaced when it was added, and nothing at the site was wrong.
 *
 * WHAT THIS COMPONENT IS AND IS NOT. It is the whole of §3's four METHODS — §3.1's `get` and §3.2's `getAll`
 * over §7.1 "Query cookies", §3.3's `set` over §7.2 "Set a cookie" and §3.4's `delete` over §7.3 "Delete a
 * cookie" — and the §6.1 Window member that reaches them. What it is NOT is the CHANGE-EVENT half: §5.1 "The
 * CookieChangeEvent interface", §7.4 "Process changes" and the `onchange` handler are absent TOGETHER, which is
 * a SUBPROBLEM ORDER rather than an oversight — installing the handler without §7.4 would flip a page's
 * `"onchange" in cookieStore` true and abandon nothing, because `CookieStore : EventTarget` already answers
 * `addEventListener("change", f)` and no change is dispatched to either. See the named residual in
 * cookie_store.c for what the next diff builds.
 * THIS PARAGRAPH TWICE SAID A WRITER WAS UNBUILT AND IS REWRITTEN RATHER THAN DELETED, BECAUSE A READER WHO
 * RE-DERIVES THE SUBPROBLEM ORDER FROM THE QUERY HALF LANDING FIRST WILL RE-ADD EITHER CLAIM. It said §7.2 was
 * unbuilt, and then that §3.3 was; §7.2 is the algorithm BOTH writers reach — §7.3 step 4 is "Return the
 * results of running set a cookie with url, name, value, null, domain, path, "strict", partitioned, and 0" — so
 * it landed with `delete`, and `set` followed one diff later once its argument position had a declared type.
 * A reader who reads either sentence as "nothing writes this store" would build §7.2 a second time.
 * THIS SENTENCE USED TO NAME THE BLOCKER AS A REGISTRABLE-DOMAIN-SUFFIX PREDICATE THAT IS PRIVATE TO ANOTHER
 * COMPONENT, AND IT IS REWRITTEN RATHER THAN DELETED BECAUSE THE RETIRED HALF IS THE ONE A READER ACTS ON.
 * §7.2's step 12.3 does need that predicate, the predicate was private, and it is now
 * `registrable_domain_suffix_or_equal` in core/url/registrable_domain.h — whose own header names this
 * standard's step BY NAME as a second consumer. The extraction landed in the .c's header and left THIS file
 * asserting the privacy in the present tense, which is the direction that costs most: a reader opens the
 * header first (the .c's second line sends them here), is told the predicate cannot be reached, and either
 * stops or writes the second copy both files say must never exist. A stale blocker is not a stale label, it is
 * a standing reason not to do the work. RETIREMENT: this record goes when a blocker named in this component is
 * named by something a diff trips over rather than by prose in two files that can disagree.
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
