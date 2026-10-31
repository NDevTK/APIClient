/* RFC 6265bis-22 "Cookies: HTTP State Management Mechanism" — §5.7 "Storage Model" and §5.8.3 "Retrieval
 * Algorithm", as the ONE store an AGENT has. Blink calls this file `core/loader/cookie_jar`, so this one is called that; see cookie_jar.c.
 *
 * WHY IT IS THE AGENT'S AND NOT A REALM'S. It was a realm's, held in a per-context slot beside the interface
 * prototypes, and that is a category error about what a cookie is:
 *
 *   - §5.7 opens "The user agent stores the following fields about each cookie", and §5.8.3 returns a
 *     cookie-string "from a given cookie store" for each retrieval — one store, many retrieval URIs. A realm is
 *     not a party to either sentence.
 *   - HTML §3.1.4 states the consequence in its own words: "The cookie attribute's getter and setter
 *     synchronously access SHARED STATE. Since there is no locking mechanism, other browsing contexts in a
 *     multiprocess user agent can modify cookies while scripts are running." A per-realm store is a store no
 *     other browsing context can modify, which is the one property that paragraph says it does not have. The
 *     same section says the sharing is deliberate and is why paths are not a security feature: "Since the
 *     cookie attribute is accessible ACROSS FRAMES, the path restrictions on cookies are only a tool to help
 *     manage which cookies are sent to which parts of the site."
 *   - CLAUDE.md's SECURITY.md section says what an instance is, and the words are that document's rather than
 *     a standard's: an instance is an ORIGIN-KEYED AGENT CLUSTER — `(browsing-context group, origin)` —
 *     because that is the spec's heap boundary, and across one the COW delta synchronises. Two same-origin documents
 *     in one browsing-context group are ONE agent, and a cookie is a fact about an ORIGIN's host, not about a
 *     document. So `frame.contentDocument.cookie = "s=1"` followed by `document.cookie` in the parent read two
 *     jars where a browser has one, and a bundle that stores a session token from an iframe and reads it from
 *     the top saw nothing — with no assert to say so, because each realm's answer was internally consistent.
 *
 * WHAT THE STORE IS KEYED BY, WHICH IS MORE THAN THE ORIGIN. §5.7 step 23 names the identity of a stored
 * cookie: "If the cookie store contains a cookie with the same name, domain, host-only-flag, and path as the
 * newly-created cookie" — one entry per such tuple, an invariant the standard notes. That tuple is this store's
 * key. They are NOT the origin: an origin is a (scheme,
 * host, port) tuple, and a cookie ignores the port entirely, admits a parent DOMAIN through §5.1.3's
 * domain-match, and carries a PATH the origin has no notion of. So "one jar per agent" is exact for the STORE
 * and would be wrong for the READ, and §5.8.3 is the read: it filters by domain-match, by path-match and by the
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

/* §5.6's parse and §5.7 "receive a cookie" from `uri` for a "non-HTTP" API — the whole of what HTML §3.1.4's
   setter means by "act as it would when receiving a set-cookie-string for the document's URL via a non-HTTP
   API". `uri` is the request-uri: its host is §5.1.2's canonicalized request-host, its path is what §5.1.4's
   default-path is computed from, and its origin is what §5.7 step 13 measures the Secure attribute against. A set-cookie-string the
   standard says to ignore leaves the store untouched. `set_cookie` is the page's value, a String or an unknown
   (borrowed): its bytes are its example where one is held, and the stored value is the JS string cut from it. */
void cookie_jar_receive(JSContext *ctx, const UrlRecord *uri, JSValueConst set_cookie);

/* §5.6's parse and §5.7 "receive a cookie" from `uri` for an HTTP API — a response's Set-Cookie field, which Fetch §4.7
   "HTTP-network fetch" stores when its request included credentials. The same parse and store as
   cookie_jar_receive, except that an HttpOnly cookie is kept, flagged, and hidden from every read this store
   serves (all of them "non-HTTP"). `uri` is the URL that response answered. */
void cookie_jar_receive_http(JSContext *ctx, const UrlRecord *uri, const char *set_cookie, size_t len);

/* cookie_jar_receive_http over each [URL, Set-Cookie field] pair of `list`, the trusted zone's record of the
   fields a fetch's responses (its redirect hops included) carried. The zone routes each pair to the jar of the
   agent its URL's origin keys, so every URL is this agent's origin; that is asserted. `list` is BORROWED. */
void cookie_jar_receive_http_pairs(JSContext *ctx, JSValueConst list);

/* §5.7's cookie-attribute-list as §5.7 reads it: the last value of each attribute-name its steps ask for, in
   parsed shape. `domain` and `path` are BORROWED and must outlive the store call, and each is at most 1024
   octets (asserted). Zeroed, it is the empty list a Set-Cookie carrying only a name and a value produces. */
typedef struct {
    bool        max_age_seen;   /* step 6's "an attribute with an attribute-name of Max-Age" */
    bool        have_expiry;    /* step 6: either Max-Age or Expires is in the list */
    long long   expiry;         /* the expiry-time, in SECONDS — the unit cookie_jar.c compares against time() */
    const char *domain; size_t domain_len;   /* step 7's domain-attribute, leading U+002E dropped; empty is host-only */
    bool        have_path;      /* a Path attribute is in the list, which step 21's `__Host-` test asks */
    bool        path_is_default;/* its value is §5.6.4's default-path; `path` is then NULL */
    const char *path;   size_t path_len;     /* the Path value; read when have_path && !path_is_default */
    bool        secure;         /* the Secure attribute */
    bool        http_only;      /* the HttpOnly attribute — step 15 refuses it for a non-HTTP API */
} CookieJarAttributes;

/* §5.7 "receive a cookie" over an ALREADY-PARSED cookie-attribute-list, for a "non-HTTP" API — the half
   `cookie_jar_receive` reaches after §5.6 has read a set-cookie-string, and the half a SECOND standard reaches
   with no string in its hand at all.
 *
 * THE SPLIT IS THE OTHER STANDARD'S OWN WORDS, exactly as cj_collect's is for the read side. Cookie Store API
 * §7.2 "Set a cookie" builds `attributes` as a list and then says, in its last step but one: "Perform the steps
 * defined in Cookies § Storage Model for when the user agent "receives a cookie" with url as request-uri,
 * encodedName as cookie-name, encodedValue as cookie-value, and attributes as cookie-attribute-list." Those
 * four are this signature. §5.6 is named nowhere in it, because that algorithm's input is a header field and
 * §7.2's input is a dictionary.
 *
 * AND ROUTING §7.2 THROUGH `cookie_jar_receive` INSTEAD IS NOT A LONGER ROAD TO THE SAME PLACE, WHICH IS WHY
 * THIS ENTRY EXISTS RATHER THAN A SERIALIZER. §7.2 refuses U+003B (;) in the NAME and the VALUE and in nothing
 * else — its own Note says the restriction "should also apply to expires, domain, path, and sameSite as well"
 * is still an open question — so a `path` member is free to carry one. Spell that attribute-list as a
 * set-cookie-string and §5.6's unparsed-attributes parse splits it there, so `path: "/;Domain=example"` arrives
 * as a Domain attribute the page never wrote and §7.2 step 12.3 never judged. That step is the whole of what
 * bounds a cookie to its registrable domain, so the round trip does not lose an attribute, it MANUFACTURES one
 * past the check the algorithm exists to make.
 *
 * `name` AND `value` ARE THE ENCODED BYTE SEQUENCES §7.2 step 24 hands over, and this entry parses NOTHING out
 * of them: a U+003D (=) inside `value` is part of the value, where §5.6 would have to have found it after the
 * first one. Every caller's refusals are its own algorithm's; what this entry asserts is only what it computed.
 *
 * RETIREMENT: this record goes when no caller in this tree spells a cookie-attribute-list as a string for
 * another to re-read — the injection is then unreachable by construction rather than argued against here. */
void cookie_jar_store(JSContext *ctx, const UrlRecord *uri, const char *name, size_t name_len,
                      const char *value, size_t value_len, const CookieJarAttributes *attrs);

/* §5.1.4's DEFAULT-PATH of `uri` — the path a cookie that states none is given, and what a SECOND STANDARD
   reaches through Fetch. OWNED; the caller frees.
 *
 * WHY IT IS DECLARED AND NOT PRIVATE, WHICH IS THE SAME ARGUMENT core/url/registrable_domain.h MAKES. Cookie
 * Store API §7.2 "Set a cookie" step 15 is "If path is the empty string, then set path to the serialized cookie
 * default path of url", and `serialized cookie default path` is FETCH §3.1.3 "Cookie infrastructure"'s: "Let
 * cloneURL be a clone of url. Set cloneURL's path to the cookie default path of cloneURL's path. Return the URL
 * path serialization of cloneURL." That inner `cookie default path` is this algorithm, so two standards share
 * one definition and this tree holds one implementation of it.
 *
 * AND ITS CALLER CANNOT LEAVE THE PATH ABSENT AND LET THE STORE FILL IT IN, which is the shape that makes this
 * an export rather than a convenience. §5.7 step 11 already computes this for an attribute-list that states no
 * Path — which is what `have_path` false means — so a caller with an EMPTY path could route there and never
 * name the value. §7.2 cannot: its steps 16, 17 and 19 run AFTER step 15 and they READ the substituted path,
 * refusing one that does not begin with U+002F, refusing a `__Host-` name whose path is not exactly U+002F, and
 * refusing one whose UTF-8 encoding exceeds the maximum attribute value size. A caller that deferred the
 * substitution would be running three of its own algorithm's refusals against a path it does not have.
 *
 * IT IS AT SHORTEST "/" AND NEVER EMPTY, which is what cj_path_match's own assert relies on. */
char *cookie_jar_default_path(const UrlRecord *uri);

/* §5.8.3's cookie-string for `uri` for a "non-HTTP" retrieval — the cookies step 3 admits, sorted by step 4
   and serialized by step 6 (`name=value`, or the bare value for an empty name, joined by "; ").
   Returns an OWNED JS string, an unknown where a value was set from one (concolic_string_concat). */
JSValue cookie_jar_cookie_string(JSContext *ctx, const UrlRecord *uri);

/* §5.8.3's cookie-list for `uri` — steps 1-4 alone, which is the half Cookie Store API §7.1 "Query cookies"
   step 1 asks for in those words: the cookie-string "itself is ignored, but the intermediate cookie-list is
   used in subsequent steps". Same filtering and same order as the string above, because it is the same walk.

   THE SHAPE IS A JS ARRAY OF TWO-ELEMENT ARRAYS, « name, value », in §5.8.3 step 4's order. It is a JS value for
   the reason the store itself is one (see above): it crosses no C lifetime, and a flow that parks between
   building it and reading it parks with it. It carries the two fields §7.1's "create a CookieListItem" reads
   and NOT the other fields §5.7 stores, because that algorithm's step 3 returns exactly «[ "name" → name,
   "value" → value ]» — the wider CookieListItem of earlier drafts is gone, and that standard's own Note says
   so: "One implementation is known to expose information beyond _name_ and _value_." A jar that handed back
   `domain` or `expires` here would be offering a caller a field no member of that API may return.
   OWNED — the caller frees. */
JSValue cookie_jar_cookie_list(JSContext *ctx, const UrlRecord *uri);

/* ---- COOKIE STORE API §2.2 "Cookie store"'s TRIGGER, AND §7.4 "Process changes"' CHANGE SET ---------------- */

/* §7.4 "Process changes"' `A cookie change` is "a cookie and a type (either changed or deleted)". The type is
   that section's own four-bullet classification, and every bullet is a fact about what THIS STORE just did:
     - "A cookie which is removed due to an insertion of another cookie with the same name, domain, and path is
       ignored" — RFC 6265bis-22 §5.7 "Storage Model" step 23.4's removal, which in this store is one property write
       landing on a key that already existed. Nothing is recorded for the cookie it replaced.
     - "A newly-created cookie which is not immediately evicted is considered changed."
     - "A newly-created cookie which is immediately evicted is considered deleted" — a stored cookie whose
       expiry has already passed, which is the `expires=Thu, 01 Jan 1970` every page writes to delete one.
     - "A cookie which is otherwise evicted or removed is considered deleted." */
typedef enum {
    COOKIE_CHANGE_CHANGED = 0,
    COOKIE_CHANGE_DELETED = 1,
} CookieChangeType;

/* §2.2 "Cookie store"'s TRIGGER: "When any of the following conditions occur for a cookie store, perform the
 * steps to process cookie changes." Its three conditions are conditions on THE STORE, which is the whole reason
 * the producer is here rather than at whichever member wrote:
 *   - "A newly-created cookie is inserted into the cookie store" — reached by HTML §3.1.4 "Resource metadata
 *     management"'s `document.cookie` setter, by a network Set-Cookie, and by Cookie Store API §7.2 "Set a
 *     cookie" alike. A record placed at any ONE of those would answer for that writer and miss every other
 *     writer of the one store §2.2 names.
 *   - "A user agent evicts expired cookies from the cookie store" — §5.7's "The user agent MUST evict all
 *     expired cookies from the cookie store if, at any time, an expired cookie exists in the cookie store",
 *     which this store performs at the two moments an expired cookie is known to exist.
 *   - "A user agent removes excess cookies from the cookie store" — §5.7's "At any time, the user agent MAY
 *     \"remove excess cookies\" from the cookie store if the number of cookies sharing a domain field exceeds
 *     some implementation-defined upper bound (such as 50 cookies)". A MAY, and this user agent does not: there
 *     is no per-domain and no store-wide count here, so the condition cannot occur and there is no site for it.
 *     That is a complete implementation of a MAY rather than a gap — the same answer core/storage/storage.c
 *     gives HTML §12.2.1 "The Storage interface"'s "implementation-defined manner" reorder.
 *
 * §7.4 IS THE OTHER STANDARD'S AND IS NOT PERFORMED HERE, WHICH IS WHY THIS IS A HOOK AND NOT A CALL. Its step
 * 1 fires a `CookieChangeEvent` at a Window's `CookieStore`, and this file is RFC 6265bis-22's store: it knows nothing
 * about either interface and must not learn, or the store becomes a component of one API that views it while
 * `document.cookie` and the network write the same bytes through the same steps.
 * ONE CLAIMANT, AND NULL IS THE RELEASE — core/events/event_target.h's own seams state why the two are one call.
 * `run` is asked with the changes already recorded and has queued whatever it queues by the time it returns;
 * this file CLEARS the set afterwards, because the set is what the CONDITION produced and a set that outlived
 * its condition would be re-offered to every Window at the next one. */
typedef void (*CookieJarProcessChanges)(JSContext *ctx);
void cookie_jar_set_process_changes(CookieJarProcessChanges run);

/* §7.4's `The observable changes for url`: the changes "which meet the requirements in step 1 of Cookies §
 * Retrieval Algorithm" (RFC 6265bis-22 §5.8.3 step 3 since that draft renumbered), applied by the predicate the
 * §5.8.3 walk uses. Expiry is not among those requirements, and must not be tested here: a deleted-by-expiry
 * change filtered out of its own notification would leave §2.2's second condition processing an empty set.
 * The http-only requirement is applied, since every observer of this store is a "non-HTTP" API.
 * Returns a JS array of « name, value, type »: the two strings §7.1's `create a CookieListItem` reads and the
 * `CookieChangeType` choosing the list. OWNED — the caller frees. `uri` is asserted to be this instance's
 * principal: every Window §7.4 step 1 walks here shares one origin. */
JSValue cookie_jar_observable_changes(JSContext *ctx, const UrlRecord *uri);

#endif
