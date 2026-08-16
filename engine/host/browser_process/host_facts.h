/* host_facts.h — THE SANDBOX BOUNDARY, STATED AS TYPES: what this process may be TOLD, what it may COMPUTE,
 * and what no WASM instance of any kind may ever be trusted to STATE.
 *
 * `browser_process.c` is the trusted instance and this is the contract its network operations are written
 * against. It exists because the two halves of `extension/lib/safe-fetch.js` move in OPPOSITE directions and
 * look identical on the page. The SOP/CORS check, CORB/ORB, the scheme filter and the private-network guard
 * are ALGORITHMS — logic over values — and CLAUDE.md §Architecture says an algorithm in the bridge is a
 * component that has not moved yet. The values those algorithms read are something else entirely: `chrome.*`
 * answers that NOTHING inside a WASM instance can ask for. SECURITY.md keys authorization on `sender.tab.url`
 * and fixes the credentialed-read principal at `MessageSender.origin` precisely because the browser process
 * stamps them and a renderer cannot forge one. So the JS keeps the syscalls, the algorithms come here, and the
 * facts are HANDED across — and the C below is written so a decision cannot be reached without them.
 *
 * THREE CATEGORIES, AND THE THIRD IS THE ONE THAT GETS MISSED. `host_fact_class` is the classification itself:
 * a switch with no `default:`, so a `HostFact` added without a class is a -Wswitch diagnostic and then a
 * CHECK_FAIL, rather than a row in a comment somebody forgot to write.
 *
 *   ALGORITHM               — logic over values. Belongs in this process. It reads facts; it states none.
 *   BROWSER_STATED          — a fact only `chrome.*` can answer. Passed in, asserted present, NEVER derived.
 *   REPORTED_NEVER_DECIDED  — a value the untrusted side may legitimately REPORT (a name, a URL, a method) and
 *                             which no zone may turn into AUTHORITY. The tell is that it looks exactly like
 *                             the fact it is not: a sandboxed frame's address parses to a tuple origin the
 *                             browser refused to give that document, so URL-deriving a principal hands a
 *                             page's own sandboxed iframe same-origin access to the embedder's bytes.
 *
 * THE PRINCIPAL IS PER REQUEST, NOT PER PROCESS, AND THAT IS NOT A DETAIL. `browser_process.c` states it: there
 * is exactly ONE of this instance, "because it is keyed on nothing". A principal adopted at construction would
 * therefore be exactly the shared global SECURITY.md §Network already refuses — "per-call principal … not a
 * shared global: two grinds run concurrently in one worker, so a global principal would let one page's origin
 * contaminate another's fetch." Hence two records: `HostZoneFacts` is what this process IS (adopted once, and
 * every field of it is true of the EXTENSION rather than of any document), and `HostRequestFacts` travels with
 * each request and is never remembered between two.
 *
 * `bp_perform`'s `requester` IS NOT A PRINCIPAL, AND THE DISTINCTION IS SECURITY.md'S OWN. It is the document
 * NAME of the renderer instance whose line the zone is relaying, stated by the zone because "identity may be
 * minted by the untrusted side because it is only a name, but ROUTING and the ORIGIN STAMPED ON A DELIVERED
 * MESSAGE are the trusted zone's alone." A name routes; an origin authorizes. `OPERATIONS[].renderer_may_ask`
 * answers "may this asker reach this operation at all", which is authorization over the OPERATION; a
 * `HostRequestFacts` answers "on whose behalf, against which origin", which is the input to the operation. Two
 * questions, and a network operation needs both — the first is already built, the second is this file.
 *
 * THE AUDIT THIS ENCODES — every security-relevant thing the trusted zone does today, with the site that
 * establishes it, enumerated as `HostFact` below so the classification is checkable rather than readable.
 *
 *   ALGORITHM
 *     scheme filter (http/https only)          extension/lib/safe-fetch.js:220
 *     private-network classification           extension/lib/safe-fetch.js:162 (applied :234 and post-redirect :256)
 *     CORB/ORB by load type                    extension/lib/safe-fetch.js:122 (applied :275)
 *     credentialed SOP + CORS                  extension/lib/safe-fetch.js:293
 *     method forced to GET                     extension/lib/safe-fetch.js:243
 *     MIME Sniffing §7                         browser_process/network/mime_sniff.c — ALREADY HERE, the worked example
 *
 *   BROWSER_STATED
 *     MessageSender.origin (opaque-unique)     extension/offscreen-brain.js:159   — the credentialed-read principal
 *     sender.documentId                        extension/offscreen-brain.js:758   — the only stable per-document identity
 *     sender.url (the document's own address)  extension/offscreen-brain.js:_browserFacts — the private-network principal
 *     sender.tab.url (top-level address)       extension/offscreen-brain.js:_browserFacts — SECURITY.md's authorization key
 *     sender.tab.id (browsing-context group)   extension/bridge.js:245            — the cluster key's group half
 *     sender.frameId                           extension/offscreen-brain.js:_browserFacts — top vs sub-frame, browser-set
 *     webNavigation frameType                  extension/lib/popup-handlers.js:38 — outermost vs fenced
 *     response headers / status / final URL    extension/lib/safe-fetch.js:206, :260
 *     the user's cookie jar                    extension/lib/safe-fetch.js:243    — attached by the browser, never readable
 *     chrome-extension://<id>                  extension/background.js:211
 *     host_permissions                         extension/manifest.json:12         — WHY safeFetch owns SOP at all
 *
 *   REPORTED_NEVER_DECIDED
 *     an origin parsed out of a URL            extension/lib/safe-fetch.js:126, extension/bridge.js:670
 *     a content script's own msg.url           SECURITY.md:29
 *     sender.id                                SECURITY.md:23, extension/background.js:205
 *     a frame's self-reported "isTop"          extension/offscreen-brain.js (the buffer records frameId, never isTop)
 *     the engine's minted child NAME           extension/bridge.js:825  (a name may be minted; its ORIGIN may not)
 *     the engine's event.origin                extension/bridge.js:901  (CHECK: only the trusted zone stamps one)
 *     the engine's XHR method/credentials      extension/bridge.js:639  (refused, never obeyed and never downgraded)
 */
#ifndef ENGINE_HOST_BROWSER_PROCESS_HOST_FACTS_H
#define ENGINE_HOST_BROWSER_PROCESS_HOST_FACTS_H

#include <stdbool.h>
#include <stddef.h>

/* ── The classification ──────────────────────────────────────────────────────────────────────────────────── */

typedef enum {
    HOST_FACT_CLASS_ALGORITHM = 1,
    HOST_FACT_CLASS_BROWSER_STATED,
    HOST_FACT_CLASS_REPORTED_NEVER_DECIDED
} HostFactClass;

typedef enum {
    /* BROWSER_STATED, per request. Each of these four is a STRING field of HostRequestFacts and is answered by
       `host_request_fact_value`, which is what makes "every fact is passed in" mechanical rather than
       remembered — and they are FIRST and CONTIGUOUS because `host_request_facts_check` walks that run. */
    HOST_FACT_INITIATOR_PRINCIPAL = 1,
    HOST_FACT_INITIATOR_ADDRESS,
    HOST_FACT_TOP_LEVEL_ADDRESS,
    HOST_FACT_BROWSING_CONTEXT_GROUP,
    /* BROWSER_STATED, per request, and not strings. */
    HOST_FACT_FRAME_ID,
    HOST_FACT_HOST_PERMISSION,
    HOST_FACT_CREDENTIALED,
    /* BROWSER_STATED, per process — fields of HostZoneFacts, true of the extension and of no document. */
    HOST_FACT_EXTENSION_ORIGIN,
    HOST_FACT_OFFSCREEN_DOCUMENT_URL,
    /* BROWSER_STATED, per reply. They exist only AFTER the syscall, and the syscall is the JS side's, so they
       ride the reply record rather than either struct. Named here so the ledger is whole. */
    HOST_FACT_RESPONSE_HEADERS,
    HOST_FACT_RESPONSE_URL_LIST,
    HOST_FACT_COOKIE_JAR,
    /* ALGORITHM — the decisions this process makes. Each is declared as an entry at the bottom of this file. */
    HOST_FACT_SCHEME_FILTER,
    HOST_FACT_PRIVATE_NETWORK_CLASS,
    HOST_FACT_CORB_BY_LOAD_TYPE,
    HOST_FACT_CORS_CREDENTIALED,
    HOST_FACT_METHOD_GET_ONLY,
    HOST_FACT_MIME_SNIFF,
    /* REPORTED_NEVER_DECIDED — values that cross and must never become authority. */
    HOST_FACT_URL_DERIVED_ORIGIN,
    HOST_FACT_CONTENT_SCRIPT_STATED_URL,
    HOST_FACT_SENDER_ID,
    HOST_FACT_SELF_REPORTED_TOP,
    HOST_FACT_ENGINE_MINTED_DOCUMENT_NAME,
    HOST_FACT_ENGINE_STATED_ORIGIN,
    HOST_FACT_ENGINE_STATED_METHOD
} HostFact;

HostFactClass host_fact_class(HostFact f);

/* ── The principal ───────────────────────────────────────────────────────────────────────────────────────── */

/* THE STAMP, WRITTEN ONLY BY `host_principal_adopt`. C cannot make a struct field private, so the TYPE stops
   the ACCIDENT — a `const char *` out of a URL parse does not compile where a HostPrincipal is wanted — and
   this word stops the value from arriving through a brace initialiser that a reviewer would read as ordinary
   struct setup. It is checked with CHECK and not DCHECK because it IS the authorization boundary: continuing
   past it in a shipped build is worse than aborting. */
#define HOST_PRINCIPAL_STATED 0x50524e43u   /* 'PRNC' */

typedef struct {
    const char *serialized;    /* the browser's MessageSender.origin, or this zone's opaque token for one */
    const char *document_id;   /* sender.documentId — which document the browser said this of */
    unsigned    stated;        /* HOST_PRINCIPAL_STATED */
} HostPrincipal;

/* THE ONLY CONSTRUCTOR, AND BOTH ARGUMENTS ARE BROWSER-STATED. `document_id` is not decoration: the browser
   answers an origin per DOCUMENT and a (tab, frame) pair is reused across navigations at a different origin,
   so a principal that cannot name its document is one whose provenance cannot be audited. */
HostPrincipal host_principal_adopt(const char *browser_stated_origin, const char *document_id);

/* THERE IS NO DERIVATION, AND THIS IS THE SPELLING THE NEXT READER WILL REACH FOR. The TYPE is the mechanism;
   this macro closes the one NAME that would otherwise be written to bridge the gap, and closes it at compile
   time. `safe-fetch.js:126` states the reason at its own site: "pageOrigin is the AUTHORITATIVE browser origin
   of the loading document, passed in; NEVER url-parsed (a sandboxed frame's url would fabricate a tuple origin
   it lacks)". Note what it does NOT do: a differently-named derivation is stopped by the type, not by this. */
#define host_principal_from_url(...) \
    HOST_FACTS__A_PRINCIPAL_IS_NEVER_DERIVED_FROM_A_URL__see_host_principal_adopt

/* A SERIALIZED ORIGIN IS A TUPLE ORIGIN ONLY IF IT HAS AN AUTHORITY — the same one test `safe-fetch.js`'s
   `_isRealOrigin` makes, and for the same reason: HTML §7.5 serializes an opaque origin as "null", and the
   trusted zone additionally mints "null:<uuid>" per document so two opaque documents never compare equal.
   Neither spelling has a scheme before "://", and a real origin is the only form that does. */
bool host_origin_is_tuple(const char *serialized);

bool host_principal_is_opaque(HostPrincipal p);

/* SAME-ORIGIN IS ASYMMETRIC BY CONSTRUCTION, AND THE ASYMMETRY IS THE RULE. The PRINCIPAL side is a fact the
   browser stated and can never be derived; the RESOURCE side is an origin legitimately parsed out of the
   target URL, because a network resource's origin IS its URL's origin — safe-fetch.js:126 draws exactly that
   line, in exactly those words, and it is right. Giving the two sides different types is what stops a URL from
   drifting into the principal position. An OPAQUE principal is same-origin with NOTHING, not even another
   opaque one: SECURITY.md's "the `"null"==="null"` collision is closed", which is CLAUDE.md §7.2.5.1's rule
   for the WindowProxy filter because it is the same concept. */
bool host_principal_same_origin(HostPrincipal p, const char *resource_origin_from_url);

/* ── What this process IS, adopted once ──────────────────────────────────────────────────────────────────── */

#define HOST_ZONE_STATED 0x5a4f4e45u   /* 'ZONE' */

typedef struct {
    const char *extension_origin;        /* chrome-extension://<id> — background.js:211's EXT_ORIGIN */
    const char *offscreen_document_url;  /* the one document allowed to drive the privileged chrome.* relay */
    unsigned    stated;
} HostZoneFacts;

HostZoneFacts host_zone_facts_adopt(const char *extension_origin, const char *offscreen_document_url);

/* ── What one request IS, carried with every decision ────────────────────────────────────────────────────── */

typedef struct {
    HostPrincipal principal;             /* HOST_FACT_INITIATOR_PRINCIPAL    — MessageSender.origin */
    const char *initiator_address;       /* HOST_FACT_INITIATOR_ADDRESS      — sender.url, this document's own */
    const char *top_level_address;       /* HOST_FACT_TOP_LEVEL_ADDRESS      — sender.tab.url */
    const char *browsing_context_group;  /* HOST_FACT_BROWSING_CONTEXT_GROUP — sender.tab.id, stringified */
    int         frame_id;                /* HOST_FACT_FRAME_ID               — sender.frameId; 0 IS the top */
    bool        host_permission;         /* HOST_FACT_HOST_PERMISSION        — the browser applied NO SOP here */
    bool        credentialed;            /* HOST_FACT_CREDENTIALED           — the TRUSTED zone's decision */
} HostRequestFacts;

/* THE FACT A REQUEST ANSWERS, BY NAME. A new BROWSER_STATED enumerator with no field here fails the same
   -Wswitch diagnostic the classifier does, and then a CHECK_FAIL. NULL is the honest answer for a fact that
   belongs to another record (the process's, or the reply's) — never for one this record owes. */
const char *host_request_fact_value(const HostRequestFacts *req, HostFact f);

/* EVERY BROWSER-STATED STRING THIS RECORD OWES, CHECKED WHERE IT ARRIVES rather than where some later decision
   happens to read it. CLAUDE.md: "a bad state caught late is a bug you built on; caught where born it is a
   fix." It walks the classification instead of restating it, so a fact added above is checked without this
   function being edited — the failure mode of a hand-written list is precisely that it stops naming
   everything. */
void host_request_facts_check(const HostRequestFacts *req);

/* ── The decisions, declared so they cannot be reached without the facts ─────────────────────────────────── */

/* DECLARED AND NOT DEFINED, WHICH IS THE STATE AND IS ALSO THE FORCING FUNCTION. `safe-fetch.js` still makes
   every one of these decisions, so there is nothing here to define yet; a bodyless declaration cannot be
   called without a LINK error naming the symbol, and it has no runtime behaviour to be wrong in the meantime.
   A stub returning `false` would be a policy DENY that silently ships as a fallback, which is the shape
   CLAUDE.md refuses; a stub that DFAILs would be a dev-only assertion about what is a compile-time fact, which
   is the shape `build.mjs` just deleted from `mime_sniff_compute`.
   Each takes the request record BY POINTER as its FIRST argument, and that is the whole enforcement: a policy
   check that needs `MessageSender.origin` or `sender.tab.url` does not compile without being handed one. The
   RESOURCE side of each is derived from the target URL and its parameter name says so. */

/* safe-fetch.js:220 — http(s) only, so a crafted URL cannot read a local or extension resource. */
bool host_policy_allows_scheme(const HostRequestFacts *req, const char *target_scheme);

/* safe-fetch.js:234/:256 — a PRIVATE target is refused unless the INITIATOR is itself private. Called on the
   initial target and again on the post-redirect final host, because the first call cannot see a 30x. */
bool host_policy_allows_private_target(const HostRequestFacts *req, const char *target_host_from_url);

/* safe-fetch.js:275 — a body that will RUN as code must be JS-typed when cross-origin. `resource_origin_from_url`
   is the origin of the URL the response ACTUALLY came from (Fetch §2.2.6's LAST URL-list item), not the one
   that was requested: a same-origin request that redirects cross-origin is a cross-origin response, and
   safe-fetch.js passes the requested `parsed.href` here today. */
bool host_policy_allows_as_script(const HostRequestFacts *req, const char *resource_origin_from_url,
                                  const char *content_type_value, bool no_sniff,
                                  const unsigned char *body, size_t body_n);

/* safe-fetch.js:293 — the credentialed read. Same-origin to the principal is readable; otherwise the server
   must have granted this EXACT origin a credentialed read. The browser does not apply its same-origin policy
   to a host-permission fetch, which is the only reason this exists — `req->host_permission` is that fact,
   passed rather than assumed. */
bool host_policy_allows_credentialed_read(const HostRequestFacts *req, const char *resource_origin_from_url,
                                          const char *acao, const char *acac);

#endif /* ENGINE_HOST_BROWSER_PROCESS_HOST_FACTS_H */
