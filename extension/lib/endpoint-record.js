/* lib/endpoint-record.js — THE ENDPOINT RECORD: ONE description of "an address the bundle can call", shared
   by the single producer that mints one and by every surface that reads one back.

   WHY THIS FILE EXISTS. The record has exactly one producer — lib/merge.js holds the extension's only
   `endpoints.set` — and until this file that producer's field list lived NOWHERE it could be checked. It was
   hand-copied into seven prose comments (lib/send.js, lib/popup-handlers.js, lib/discovery-probe.js,
   lib/response-decode.js, popup.js, offscreen-brain.js) and into one hand-written DCHECK in
   lib/popup-discovery.js, and a list transcribed eight times is a list that disagrees with itself the first
   time the producer moves. Every one of those comments exists because a reader HAD ALREADY projected a name
   onto an endpoint that no producer writes — `apiKey`, `apiKeySource`, `origin`, `referer`, `contentType`,
   `lastSeen` — and each read `undefined` for the life of the feature it was part of, with a `||` beside it
   turning that undefined into a plausible answer. That is CLAUDE.md §Architecture's defaulted-field defect,
   and its shape here is specific: an endpoint is the unit `netdiff --unused` counts, so a field silently
   substituted at a consumer becomes a KEY, and two endpoints that key alike are one endpoint in the moat.

   THE RULE THIS FILE ENFORCES, and it is the FieldDef rule (lib/field-def.js) applied to the record one layer
   up: a producer states the facts it observed; every other name takes its DECLARED ABSENT VALUE, which is a
   POSITIVE STATEMENT and never a hole a `||` filled. `requiredHeaders: null` MEANS "the forced execution
   observed this call attaching no header", which endpoint.c states in those words from its own side — it
   writes the `headers` key only when it has one, "because an endpoint with no learned header must not claim
   an empty requirement, which reads as 'needs nothing' rather than 'nothing was observed'". `pathParams:
   null` MEANS "no templated hole has been filled with an example yet". `pageUrl: null` MEANS "no document
   address was known when this was learned". The consumer reads each as itself.

   WHAT HAS NO ABSENT VALUE, AND WHY THAT IS THE HALF THAT WAS BEING DEFAULTED. `url`, `method`, `host`,
   `path`, `service`, `source` and `firstSeen` are written on EVERY record by the one producer: `method` is
   asserted non-empty where the call site arrives, and `host`/`path` come from lib/callsite-url.js's
   `astCallSiteAddress`, whose own header says it "always answers; never null" for both the literal-origin and
   the shape-origin arm. So `ep.method || "POST"`, `e.method || "GET"` and `e.path || e.url` were not reading
   an absence the producer can have — they were substituting for a producer that had gone silent, and the
   substitute is indistinguishable from an answer. `e.path || e.url` was worse than indistinguishable: `path`
   is legitimately the EMPTY STRING for an address that is nothing but a shape (`{origin}` with no literal
   remainder), and lib/callsite-url.js states the invariant those consumers are built on — "`host + path`
   reconstructs the address in both cases" — so falling through to `url` on an empty path reconstructed
   `https://{origin}` + `{origin}`, an address with its origin written twice, keyed into the netdiff as a
   distinct endpoint. An empty path is a FACT and it survives this file intact.

   `undefined` IS NOT A VALUE HERE, for lib/field-def.js's reason: the record crosses chrome.runtime.
   sendMessage to the popup (lib/serialize.js passes it whole), and that serialization DROPS an `undefined`
   property — so a field whose absent value were `undefined` would arrive ABSENT on one side of the boundary
   and present on the other. Every absent value below is `null` or a fact.

   TRUST. The record's shape is decided by our own literal in the trusted zone, so a malformed one is OUR
   logic broken and a DCHECK is correct (CLAUDE.md §Offensive programming). Its VALUES come from the engine's
   @H emission, which is the trusted zone's own forced execution rather than a third party's document — so
   unlike lib/field-def.js there is no refusal layer here, and every check below is an assertion about us. */

/* THE RECORD. Every name a surface reads off an endpoint is here, with the value that MEANS "the producer
   observed nothing of this kind". A name absent from BOTH lists is a name no producer writes, which is the
   defect the seven comments above were each written to record after the fact. */
const ENDPOINT_ABSENT = Object.freeze({
  pageUrl: null,          // the document that learned this endpoint; null = no address was known.
  requiredHeaders: null,  // {name: {value|opaque}} the bundle was observed attaching; null = NONE OBSERVED,
                          // which is a different claim from `{}` ("this endpoint needs no header") and must
                          // stay distinguishable from it — endpoint.c omits its key for exactly that reason.
  pathParams: null,       // [{name, values}] examples for the address's templated holes; null = no hole has
                          // been filled yet. `[]` never reaches here: lib/merge.js writes null for it.
});

/* NO ABSENT VALUE. An endpoint with no address, no verb, no origin, no path, no service, no provenance or no
   first sighting is not an endpoint — a producer that cannot state one has nothing to register, so these are
   stated and never defaulted. `path` may be the empty STRING (a shape-origin address with no literal
   remainder); that is a stated fact, not an absence, and it is why this list checks TYPE rather than truth. */
const _ENDPOINT_STATED = ["url", "method", "host", "path", "service", "source", "firstSeen"];

/* THE PROVENANCE DOMAIN — which of lib/callsite-url.js's two arms produced the address. Asserted as a
   membership rather than as "a string" because a third spelling would read as a provenance the surfaces
   below silently render as neither. */
const _ENDPOINT_SOURCES = ["ast_analysis", "ast_shape_origin"];

/* WHAT A LEARNED ADDRESS IS EVIDENCE OF — a DIFFERENT question from `source` above, which says which ARM of
   lib/callsite-url.js resolved the address. This says whether a real client's request ever produced it.
   CLAUDE.md §A-REQUEST-CARRIES-THE-PROVENANCE names the three and the engine emits one on every @H record
   (engine/host/solver/endpoint.h's `provenance`); this is the SAME vocabulary in the same order, and the order
   is the whole content — engine/host/solver/pending.h numbers them OBSERVED < DERIVED < FORCED and states that
   the numbering IS the join's, so INDEX IN THIS ARRAY IS RANK and "most observed" is the lower index.
     observed — a real load of the document makes exactly this request.
     derived  — the page's own code computed it from real inputs. No session sent it; it is still a fact about
                the app, and it is the surface forced execution exists to find.
     forced   — a value in it exists only because a gate was forced. A reply to it is evidence about what a
                server says to a request no client makes, and §@H forbids it ever being reported as the other
                two: "a 401 body parses as JSON and yields fields that exist nowhere, an error envelope becomes
                a config, and the fabrication then PROPAGATES".
   IT IS DECLARED HERE BECAUSE TWO ZONES READ IT AND NEITHER MAY SPELL IT FOR ITSELF: this file is loaded by
   ast-worker.html (before lib/learn.js and lib/merge.js, which fold it onto the method record) and by
   popup.html (before lib/popup-send.js, which renders it). A second copy of the three words is two
   vocabularies free to drift, and the drift is silent — a comparison against a word the producer stopped
   emitting is simply never true, and a `forced` endpoint then renders as one the bundle can reach. */
const CALLSITE_PROVENANCE = Object.freeze(["observed", "derived", "forced"]);

function isCallSiteProvenance(p) { return CALLSITE_PROVENANCE.indexOf(p) >= 0; }

/* THE FOLD, WHERE TWO SIGHTINGS OF ONE ADDRESS MEET — the MOST OBSERVED of them, which is the rule the
   engine's own pending-line join uses and for the same argument: the two sightings are one ADDRESS, so if
   either was reached without standing on a contradicted arm then a real client's code computes it and the
   record must say so. Labelling that pair `forced` would state "no client makes this request" of a request
   one does.
   THE ENGINE DOES *NOT* FOLD, AND THAT IS NOT A DISAGREEMENT — engine/host/solver/endpoint.h explains it: the
   grade is part of the @H record's IDENTITY there, so a forced sighting's VALUES can never merge into a
   derived record's. Two @H rows arrive here, and what folds is this record's one-line CLAIM ABOUT THE
   ADDRESS. The values behind it are still merged by the method record below, which is the residual named at
   lib/learn.js's fold. */
function mostObservedProvenance(a, b) {
  DCHECK(isCallSiteProvenance(a) && isCallSiteProvenance(b),
         "a call-site provenance fold was handed `" + a + "` and `" + b + "` — the engine emits one of " +
         CALLSITE_PROVENANCE.join("/") + " on every @H record, so a word outside that set is the engine's " +
         "serializer and this vocabulary having parted, and the fold would answer with whichever argument " +
         "happened to be first");
  return CALLSITE_PROVENANCE.indexOf(a) <= CALLSITE_PROVENANCE.indexOf(b) ? a : b;
}

/* THE KEY — THE NAME THIS RECORD IS FILED UNDER — MINTED HERE AND SPELLED NOWHERE ELSE.

   THE RECORD HAD ONE DESCRIPTION AND ITS NAME HAD NONE, and the two halves fail the same way for the same
   reason. A key composed independently at two sites is a contract with nothing enforcing it, and its failure
   is SILENT BY CONSTRUCTION: a Map miss is a legitimate, ordinary outcome, so a key that can never match is
   indistinguishable from an address nothing learned. It has now happened TWICE, on this one map, in one
   file — a live-response consumer built `method + " " + host + path` while the producer mints the same three
   parts behind a prefix, so every `get()` missed and the `if (ep)` beside it read as the true and common
   statement "this live request hit an address the forced execution never derived". That is CLAUDE.md's
   defaulted-field defect performed on a KEY: a plausible negative, produced by a lookup that cannot succeed,
   with nothing anywhere to say so. The first instance was found and repaired; the second sat FURTHER DOWN
   THE SAME FILE, below the comment written to describe the first — which is the whole argument for minting
   over describing: a prose record of a defect does not stop the next copy, even in the file it is written in.

   So the shape is not written down for a reader to COPY — copying is how both instances arose. It is MINTED:
   one spelling of the prefix, one spelling of the separator, and `isEndpointKey` as the only thing permitted
   to ask whether a string is one of these. A future consumer that needs an endpoint by address calls this and
   cannot get it wrong, which is the whole of the repair; making the two existing spellings agree would have
   been the third copy.

   WHY EACH OF THE THREE PARTS IS IN THE NAME. `method`, because one address answering GET and POST is two
   endpoints. `host`, because a path-only key collapsed the same path across DIFFERENT origins into one
   record — and the cumulative moat spans sites, so that collapse LOSES the "many sites per session" surface
   rather than tidying it. `path`, because it is the remainder of the address. `host` + `path` IS the address:
   that is lib/callsite-url.js's own stated invariant ("`host + path` reconstructs the address in both
   cases"), and it is why an address whose ORIGIN IS A SHAPE keys exactly like a literal one — `host` is then
   the shape verbatim, and nothing here treats the two arms differently, so a shape-origin endpoint dedups
   across runs like any other.

   THIS FUNCTION DOES NOT FOLD CASE, and that is a rule rather than an omission. Both sides of the seam carry
   a method already normalized by Fetch §2.2.1 "Methods" — verbatim: "To normalize a method, if it is a
   byte-case-insensitive match for `DELETE`, `GET`, `HEAD`, `OPTIONS`, `POST`, or `PUT`, byte-uppercase it" —
   the engine half in browser/core/fetch/request.c and the network observer in extension/intercept.js. §2.2.1
   deliberately leaves everything else alone ("Using `patch` is highly likely to result in a `405 Method Not
   Allowed`"), so `patch` travels lowercase and a `toUpperCase()` here would be a THIRD rule disagreeing with
   both producers — which is the same defect as a second key spelling, one field further in. */

/* THE PREFIX, WHICH IS A PROVENANCE TAG WELDED INTO A LOOKUP NAME.
   It once discriminated: a second producer minted `AST DYN ` for shape-origin addresses, and the prefix said
   which of the two you were holding. That producer is gone — one mint remains, so EVERY key in these maps
   carries this, and the one predicate that reads it selects everything. Provenance also already lives where
   CLAUDE.md puts it, as a FIELD the record states: `source` is "ast_analysis" or "ast_shape_origin".

   NAMED RESIDUAL — the prefix is not removed here, and this is what that leaves.
     WHAT IS NOT COVERED: the key still carries a provenance tag no consumer distinguishes on, so
       `isEndpointKey` is a filter that is true of every entry and `offscreen-brain.js`'s per-delivery sweep
       is a `clear()` written as a predicate.
     WHAT THE NEXT DIFF BUILDS: dropping the prefix from the mint, which CANNOT be done alone — these keys are
       PERSISTED. lib/persistence.js serializes globalStore.endpoints BY KEY and restores it, so a prefix-free
       mint would make every entry a previous session stored unreachable by a freshly minted name, and
       lib/merge.js's `globalStore.endpoints.get(k)` would miss for all of them. That is this same defect one
       layer out — across sessions instead of across files — so the diff is the mint plus a store migration
       that rekeys what IndexedDB already holds, verified against a store written before it.
     HOW ITS ABSENCE WOULD SHOW: it does not show as breakage, which is why it is a residual and not a bug —
       it shows as this constant existing at all, and as a sweep predicate that has no false case. */
const _ENDPOINT_KEY_PREFIX = "AST ";

/* Mint the name for one endpoint address. `where` names the caller, because an assertion that cannot say
   WHICH producer went silent sends the reader to read all of them. */
function endpointKeyFromParts(method, host, path, where) {
  /* A SPACE IS THE SEPARATOR, so a method holding one makes the name ambiguous — `AST A B h/p` parses as two
     different (method, host) splits and the two would key alike. This is ours to assert rather than a page's
     input to refuse: a method is an RFC 9110 §5.6.2 "Tokens" token, and Fetch §5.4 "Request class" throws a
     TypeError on a method that is not one, so no request that ever reached a server carried a space in it. */
  DCHECK(typeof method === "string" && method !== "" && method.indexOf(" ") < 0,
         "an endpoint key was minted from a method that is not a single token (" +
         JSON.stringify(method) + ", " + where + ") — the space between method and host is this name's one " +
         "separator, so a method carrying one produces a key two different addresses can both spell");
  /* AN EMPTY HOST WOULD MINT A NAME WITH NO ORIGIN IN IT, which is precisely the path-only key that collapsed
     the same path across different sites into one record. lib/callsite-url.js answers a non-empty host on
     both arms — the hostname for a literal origin, the shape verbatim for an undetermined one. */
  DCHECK(typeof host === "string" && host !== "",
         "an endpoint key was minted with no origin (" + where + ") — lib/callsite-url.js's " +
         "`astCallSiteAddress` always answers a host, literal or shape, so an empty one is that parser " +
         "broken and this address would key alike with every same-path address on every other site");
  /* `path` MAY BE THE EMPTY STRING and that is a fact, not an absence: a shape-origin address can be nothing
     but its shape. So this checks TYPE, exactly as the record's own `path` check does. */
  DCHECK(typeof path === "string",
         "an endpoint key was minted from a path that is not a string (" + where + ") — an address with no " +
         "path is written \"\", which is a stated fact about a shape-origin address rather than a gap");
  return _ENDPOINT_KEY_PREFIX + method + " " + host + path;
}

/* IS THIS STRING ONE OF THESE NAMES — the only permitted reader of the prefix. Spelled out at a call site it
   is the prefix's fourth independent copy, and a copy of a name is what this whole section exists to end. */
function isEndpointKey(key) {
  return typeof key === "string" && key.startsWith(_ENDPOINT_KEY_PREFIX);
}

/* THE ONE ORIGIN. Every endpoint record in the extension is built here. `where` names the producer, because
   an assertion that cannot say WHICH producer went silent sends the reader to read all of them. */
function makeEndpointRecord(parts, where) {
  DCHECK(!!parts && typeof parts === "object" && !Array.isArray(parts),
         "an endpoint record was constructed from something that is not a record of stated facts (" + where +
         ") — the one producer passes an object literal naming what the forced execution observed, so " +
         "anything else is that producer broken and the moat would carry an address nothing described");

  /* A NAME THIS RECORD DOES NOT CARRY IS A PRODUCER TALKING TO NOBODY — the read-with-no-writer defect seen
     from the writing side. It is the reason this is a constructor rather than an object literal: the five
     names lib/send.js's comment records (apiKey, apiKeySource, origin, referer, contentType) were projected
     by a READER, and nothing existed that could have said so at the time they were written. */
  for (const k of Object.keys(parts)) {
    DCHECK(Object.prototype.hasOwnProperty.call(ENDPOINT_ABSENT, k) || _ENDPOINT_STATED.indexOf(k) >= 0,
           "an endpoint producer stated `" + k + "`, which this record does not carry (" + where + ") — " +
           "either the name is a typo for one it does carry (in which case the fact it states is dropped and " +
           "every surface reads that field's declared absence instead) or the record must learn the name; a " +
           "third option, writing it anyway, is a producer emitting into a reader that does not exist");
  }

  const ep = {};
  for (const k of Object.keys(ENDPOINT_ABSENT)) {
    ep[k] = Object.prototype.hasOwnProperty.call(parts, k) ? parts[k] : ENDPOINT_ABSENT[k];
  }
  for (const k of _ENDPOINT_STATED) {
    DCHECK(Object.prototype.hasOwnProperty.call(parts, k),
           "an endpoint producer did not state `" + k + "` (" + where + ") — an endpoint with no address, " +
           "verb, origin, path, service, provenance or first sighting is not one, so these seven have no " +
           "absent value to fall back on and this record would be keyed on whichever substitute a consumer " +
           "happened to carry");
    ep[k] = parts[k];
  }
  checkEndpointRecord(ep, where);
  return ep;
}

/* THE BOUNDARY CHECK. Called wherever a record ARRIVES from somewhere that is not this constructor — across
   chrome.runtime.sendMessage into the popup, and out of the IndexedDB store a previous session wrote. Those
   are the two places a record can be short of a name without any producer in this session having gone wrong,
   and they are exactly the two places where a `||` would otherwise have decided what the missing name meant. */
function checkEndpointRecord(ep, where) {
  DCHECK(!!ep && typeof ep === "object" && !Array.isArray(ep),
         "an endpoint record is not a record (" + where + ") — lib/merge.js is the extension's only " +
         "`endpoints.set` and every value in that map is one, so anything else is that map having been " +
         "written by something other than the producer");
  DCHECK(typeof ep.url === "string" && ep.url !== "" && typeof ep.method === "string" && ep.method !== "" &&
         typeof ep.host === "string" && typeof ep.path === "string" && typeof ep.service === "string",
         "an endpoint record is missing part of its address (" + where + ") — lib/merge.js writes url/method/" +
         "host/path/service on every record from lib/callsite-url.js's `astCallSiteAddress`, which always " +
         "answers, so a gap here is that producer broken or a store written by a build whose record shape " +
         "differed; either way the surface reading it would substitute a verb or an origin of its own and " +
         "key a second endpoint onto the same address. (`path` may legitimately be \"\": a shape-origin " +
         "address with no literal remainder. `host` + `path` is the address — never `url` as a fallback.)");
  DCHECK(_ENDPOINT_SOURCES.indexOf(ep.source) >= 0,
         "an endpoint record's `source` is `" + ep.source + "`, which is neither of lib/callsite-url.js's two " +
         "arms (" + where + ") — it states whether the origin was LITERAL or a SHAPE, which is the " +
         "difference between an address that can be fetched and one that can only be reported");
  DCHECK(typeof ep.firstSeen === "number",
         "an endpoint record carries no first sighting (" + where + ") — it is the moat's only statement of " +
         "when this address entered it");
  DCHECK(ep.pageUrl === null || typeof ep.pageUrl === "string",
         "an endpoint record's `pageUrl` is neither an address nor a stated absence (" + where + ") — `null` " +
         "is this record's one spelling of \"no document address was known\", and a second spelling is a " +
         "consumer having to guess which of them it is looking at");
  DCHECK(ep.requiredHeaders === null ||
         (typeof ep.requiredHeaders === "object" && !Array.isArray(ep.requiredHeaders)),
         "an endpoint record's `requiredHeaders` is neither a header record nor a stated absence (" + where +
         ") — `null` MEANS \"nothing was observed\" and `{}` would mean \"this endpoint requires no header\", " +
         "and the Send panel renders those two as the same empty list only if something upstream collapsed them");
  DCHECK(ep.pathParams === null || Array.isArray(ep.pathParams),
         "an endpoint record's `pathParams` is neither a list of examples nor a stated absence (" + where +
         ") — null MEANS no templated hole has been filled, which is what §@H forbids being rendered as a " +
         "value the code computed");
}
