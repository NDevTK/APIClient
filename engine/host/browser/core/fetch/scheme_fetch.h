/* FETCH §4.3 Scheme fetch — the switch on a request's current URL's scheme. See scheme_fetch.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FETCH_SCHEME_FETCH_H
#define ENGINE_HOST_BROWSER_CORE_FETCH_SCHEME_FETCH_H
#include <stdbool.h>

#include "quickjs.h"
#include "core/fetch/fetch.h"

/* §4.3's THREE OUTCOMES, named as the algorithm names them rather than as a bool. Two of the three are
   answered INSIDE this agent and one is not, and a caller that read this as "local or not" would have no way
   to tell §4.3's network error apart from its 200 — which for `data:%zz` is the difference between a rejected
   `fetch()` and a Response carrying garbage. */
typedef enum {
    /* §4.3 hands this one to HTTP fetch: "Return the result of running HTTP fetch given fetchParams", which is
       the row Fetch §2.1 URL's "HTTP(S) scheme" names. Only this outcome may reach the host's network. */
    SCHEME_FETCH_NETWORK = 0,
    /* §4.3 built the whole response here — `out_reply` is the reply record (core/fetch/fetch.h). */
    SCHEME_FETCH_RESPONSE = 1,
    /* §4.3's network error: a `data:` URL §6 refuses, a `blob:` URL the store does not name, an `about:` URL
       whose path is not `blank`, a `file:` URL, and every scheme §4.3's switch does not list. */
    SCHEME_FETCH_NETWORK_ERROR = 2,
} SchemeFetchOutcome;

/* RUN §4.3 OVER `req`. The URL is PARSED HERE and by nobody else: an entry that computes the classification's
   own input computes a second answer to the question this component exists to answer once, and the two then
   drift exactly as `fetch()`'s and XMLHttpRequest's copies of this switch did — one grew a `blob` arm and the
   other did not, so `fetch(blobUrl)` was answered locally and `xhr.open("GET", blobUrl)` went to the network.
   `blob_entry` is §5.4's CAPTURED blob URL entry — a `Request` resolves its `blob:` URL when it is built, so a
   page that revoked the URL afterwards still fetches. JS_UNDEFINED is the positive statement "this entry
   captured none", and §4.3 then reads the entry off the store as the URL's own. Every caller but `fetch()`
   passes JS_UNDEFINED, because no other standard has a Request object to have captured with.
   `*out_reply` is written ONLY for SCHEME_FETCH_RESPONSE, and is owned by the caller. */
SchemeFetchOutcome scheme_fetch(JSContext *ctx, const FetchRequest *req, JSValueConst blob_entry,
                                JSValue *out_reply);

/* THE SAME ALGORITHM, PLUS THE DELIVERY, for a caller that owns a `deliver` closure — the shape core/fetch's
   host seam already has, so a component that answered §4.3 locally settles through the SAME steps a host reply
   settles through and there is no second delivery to keep in step. Returns true when this agent answered
   (deliver has already run, with the reply record or with JS_NULL for §4.3's network error, which is what
   core/fetch/fetch.h documents a network error as), false when §4.3 hands the request to HTTP fetch — which
   the caller then owes to the host. */
bool scheme_fetch_answer(JSContext *ctx, JSValueConst deliver, const FetchRequest *req, JSValueConst blob_entry);

/* §4.3's SWITCH ASKED AS A QUESTION: does §4.3 hand a request for this URL to HTTP fetch, or does it answer
 * inside this agent?
 *
 * IT IS THE PREDICATE AND NOT THE ASSERT, and that split is the whole point of it. What this answers is needed
 * at exactly one kind of place — the moment an address is handed to a party that can fetch NOTHING but an
 * HTTP(S) scheme (Fetch §2.1 "URL") — and a URL §4.3 answers locally reaching such a party is a park that
 * never ran §4.3, whose flow is then owed an answer that can only ever be a refusal. A shared function that
 * ASSERTED that would stamp its own file and line for every caller, so the crash would name a remedy and no
 * site; the assert therefore lives at the consumer that is handing the address over, and this answers the one
 * question that consumer cannot answer for itself.
 * IT IS THE SAME COMPONENT'S OWN SWITCH AND NOT A RE-DERIVATION: an arm added to `scheme_fetch` changes this
 * answer at the same instant, which is why "who answers these bytes" stays one fact with one owner.
 * A URL THAT NAMES NO SCHEME AT ALL answers TRUE — it is not §4.3's business. A concolic's DISPLAY SHAPE parks
 * as an address (core/fetch/fetch.c's projection) and it is not a URL, so the parser refuses it and this
 * passes it through to whatever the consumer does with an address it cannot fetch. */
bool scheme_fetch_is_network(JSContext *ctx, const char *url);

#endif
