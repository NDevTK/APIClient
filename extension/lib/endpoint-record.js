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
   null` MEANS "no templated hole has been filled with an OFFERABLE example yet" and `pathParamsForced: null`
   MEANS "no hole was filled on a forced arm either" — see the pair's own paragraph below for why one field
   could not state both. `pageUrl: null` MEANS "no document address was known when this was learned". The
   consumer reads each as itself.

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
  pathParams: null,       // [{name, values}] OFFERABLE examples for the address's templated holes; null = no
                          // hole has been filled yet. `[]` never reaches here: lib/merge.js writes null for it.
  pathParamsForced: null, // [{name, values}] the same holes at the one grade that must never be offered —
                          // values EVERY sighting of which stood on a forced arm. null = the run forced
                          // nothing into a hole. `[]` never reaches here, for the reason above.
});

/* WHY THE HOLES TAKE TWO FIELDS AND NOT ONE, AND WHY THE SECOND IS NOT A DUPLICATE OF THE FIRST.
   `pathParams` is this record's only statement about what the run learned for a templated segment, and its
   declared absence is a POSITIVE one: "no hole has been filled". For a hole every sighting of which stood on
   a forced arm that statement is FALSE — the run filled it, from a real re-execution, and this record had no
   way to say so, so it said the opposite. The two consumers acted on the opposite: lib/merge.js's moat union
   carried nothing forward for that hole across documents or sessions, and lib/send.js's path-param literal
   stated `_astForcedValues: null`, which lib/popup-form.js renders as "every value learned for this field was
   computed on a path that stood on no forced arm" — the exact inverse of what was observed, in the surface a
   reviewer reads. CLAUDE.md §@H calls that a WRONG report rather than a thin one, and it is: a shape that
   drops one of its two facts is read as the positive claim that the fact is absent.

   THE FIX IS A SECOND FIELD RATHER THAN A GRADE ON EACH ENTRY, which is the same choice `provenanceOffersExample`
   settles one record down and for the identical reason: a consumer of `pathParams` must not be ABLE to be
   handed a forced value, and a `{value, grade}` entry makes that a rule each of them has to remember. It is
   also why the pair cannot collapse into one entry carrying two lists — lib/send.js requires a `pathParams`
   entry to carry at least one value, precisely so a hole nothing filled cannot surface as if it had, and a
   forced-ONLY hole has no offerable value to put there. So the hole appears in the second list and in
   neither of the first's shapes.

   A NAME MAY APPEAR IN BOTH LISTS AND A VALUE MAY NOT. One `{orgId}` legitimately has an offerable example
   from one path and a forced one from another; the same STRING in both is the per-value fold not having
   happened, which `checkEndpointRecord` asserts against below and `foldValuePools` is the one place that
   performs. */

/* HOW MANY EXAMPLES A HOLE CARRIES ON THE FLAT RECORD — one constant, because the two pools truncating at
   different lengths would make "this pool ran out" and "this pool has no more" different questions with the
   same appearance. It is a cap over what this record COPIES per hole, never over work: the values themselves
   are the method parameter's, and dropping the tail of a copy truncates no path the solver would have taken.
   It was the literal `20`, written twice in lib/merge.js. */
const PATH_PARAM_EXAMPLE_CAP = 20;

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
   ADDRESS. The VALUES behind it fold too, one level down and by this same rule — see
   `provenanceOffersExample` below for where the two facts part company. */
function mostObservedProvenance(a, b) {
  DCHECK(isCallSiteProvenance(a) && isCallSiteProvenance(b),
         "a call-site provenance fold was handed `" + a + "` and `" + b + "` — the engine emits one of " +
         CALLSITE_PROVENANCE.join("/") + " on every @H record, so a word outside that set is the engine's " +
         "serializer and this vocabulary having parted, and the fold would answer with whichever argument " +
         "happened to be first");
  return CALLSITE_PROVENANCE.indexOf(a) <= CALLSITE_PROVENANCE.indexOf(b) ? a : b;
}

/* WHETHER A VALUE LEARNED AT THIS GRADE MAY BE OFFERED AS AN EXAMPLE OF WHAT THIS APP COMPUTES — the ONE
   place that line is drawn, and the reason lib/learn.js keeps a parameter's learned values in TWO POOLS
   rather than in one list carrying a grade beside each entry.

   THE CHOICE IS SETTLED BY WHICH DESIGN MAKES THE WRONG ANSWER IMPOSSIBLE RATHER THAN DISCOURAGED, which is
   the argument engine/host/solver/endpoint.h already made one layer up when it put the grade in the @H
   record's IDENTITY: a folded grade does not, because "a FORCED sighting merging into a record whose grade
   folds to `observed` publishes, under the strongest claim this surface can make, a value that exists only
   because a gate was forced". A list of `{value, grade}` entries has exactly that failure one level down —
   every reader of `_astValidValues` (the example picker's ast-constraint tier, the Send form's prefill and
   its datalist, the value chips, the cross-document parameter merge, the two record projections, the
   harness's netdiff value census) would have to REMEMBER to consult the grade, nothing can assert that it
   did, and the one that forgot renders a fabricated example with nothing to say so. Two pools delete the
   remembering: `_astValidValues` holds what this predicate admits, so a consumer that reads it CANNOT be
   handed a forced value, and `_astForcedValues` has exactly the readers that asked for it by name.

   THREE WORDS AND TWO POOLS IS NOT A COLLAPSE OF THE VOCABULARY. A pool is named by the QUESTION a consumer
   asks, and every consumer of a learned value asks ONE question: may this be offered as an example of what
   this app computes? `observed` and `derived` answer it identically — CLAUDE.md §A-REQUEST-CARRIES-THE-
   PROVENANCE says of the second that "the real code computed it from real inputs …, so it is a fact about
   the app even where no session sent it", which is the surface forced execution exists to find — and
   `forced` answers it no. A third pool would be a distinction no consumer can act on and would put every one
   of them back to unioning two of three, which is the remembered rule this shape exists to delete. The
   three-word grade is not lost by that: it stays on the METHOD, where `_astProvenance` carries it and
   lib/popup-send.js renders it as its own tag, because THERE the difference between "a real load makes this
   request" and "the app's own code composes it" is a fact about the ADDRESS and has a reader. */
function provenanceOffersExample(p) {
  DCHECK(isCallSiteProvenance(p),
         "a learned value was graded `" + p + "`, which is none of " + CALLSITE_PROVENANCE.join("/") +
         " — every producer of a value states the grade of the sighting that computed it, so a word outside " +
         "that set is that producer and this vocabulary having parted, and this predicate would answer " +
         "`offerable` for a value nothing graded");
  return p !== "forced";
}

/* THE PER-VALUE FOLD ITSELF — most-observed, performed as a MOVE BETWEEN POOLS, spelled ONCE.
   `mostObservedProvenance` folds the one claim a record makes about an ADDRESS. This is the same law over a
   VALUE, and it is separate because the engine deliberately does not merge its two @H rows for one address:
   the grade is part of the record's identity there, so the two rows' values meet on this side, in one bag,
   and something has to decide which pool each lands in.
   THE LAW: a value some path computed WITHOUT forcing anything is a value the app computes, whatever another
   path had to force to reach it — so a promotion is one-way, out of the forced pool and into the offerable
   one, never back. Membership IS the folded grade.
   IT IS ONE FUNCTION BECAUSE IT IS ONE LAW AT FOUR MERGES, and a fold written per merge is a fold free to
   disagree per merge. The four: lib/learn.js folding one sighting's values onto a method parameter,
   lib/merge.js folding one document's parameter into the moat's, lib/merge.js folding two documents' flat
   endpoint records, and lib/send.js folding the flat record's holes onto a resolved schema. The last two are
   NEW seams, and the moat is exactly where a fourth private copy would have been invisible from either page.
   IT RETURNS RATHER THAN MUTATES, because the three record shapes spell an EMPTY pool differently — a method
   parameter omits the key, a FieldDef writes `null`, this record writes `null` — and that spelling belongs to
   the record, not to the fold. Each caller writes its own absence; none of them decides the membership. */
function foldValuePools(valid, forced, inValid, inForced) {
  DCHECK(Array.isArray(valid) && Array.isArray(forced) && Array.isArray(inValid) && Array.isArray(inForced),
         "the value-pool fold was handed something that is not a pool — every caller normalises its record's " +
         "own spelling of an empty pool (an omitted key, `null`, an absent field) to `[]` BEFORE this point, " +
         "because deciding what an absence means is the record's job and not the fold's, and a fold that " +
         "guessed would answer for a producer that had gone silent");
  const v = valid.slice(), f = forced.slice();
  for (const x of inValid) {
    const at = f.indexOf(x);
    if (at >= 0) f.splice(at, 1);          // THE FOLD: most-observed wins, and it wins by MOVING.
    if (v.indexOf(x) < 0) v.push(x);
  }
  /* AND THE OTHER DIRECTION IS NOT SYMMETRIC. An incoming FORCED value joins the forced pool only where no
     pool already holds it: if the offerable pool has it, some path computed it unforced and the fold has
     already been decided in that direction. Appending it anyway is how a value comes to sit in both, which
     renders the same bytes twice under two contradictory claims. */
  for (const x of inForced) if (v.indexOf(x) < 0 && f.indexOf(x) < 0) f.push(x);
  return { valid: v, forced: f };
}

/* WHAT THE ENGINE'S PATH TO A LEARNED METHOD WAS WORTH — one fact, asked in ONE place, because TWO zones ask
   it. lib/popup-send.js asks it to tag the row and to count the methods a bucket reached only by forcing;
   lib/learn.js asks it of the CONCRETE record it is about to dissolve into a templated one, because that
   record's path IS its identity, so the fold over its sightings is exactly the fold over the sightings of
   the path segment it is handing over. Two spellings of `if (!m._astInferred) return null` are two rules
   free to disagree about what an ungraded record means, and this one is loaded by both zones already.

   `null` IS A POSITIVE STATEMENT AND NOT A HOLE: a method with no bundle sighting (a probed discovery
   document's, or one only the wire produced) was never graded by the engine, so there is no grade to state,
   which is exactly what `_astInferred` being false means. The caller reads that absence as itself.

   THE DCHECK IS THE HALF THAT MATTERS. `_astProvenance` is written by lib/learn.js on the same line as
   `_astInferred` — the two are one write — so a record carrying the first and not the second is that pairing
   broken, or a store an older build wrote being read by this one. Both must crash rather than render,
   because what a missing grade renders as is nothing, and nothing reads as "not forced". */
function methodProvenance(m, where) {
  DCHECK(!!m && typeof m === "object" && !Array.isArray(m),
         "a method provenance was asked of something that is not a method record (" + where + ") — the " +
         "learned-method map holds one per (verb, path) and every reader walks that map, so anything else " +
         "is that walk having left it");
  if (!m._astInferred) return null;
  DCHECK(isCallSiteProvenance(m._astProvenance),
         "a bundle-inferred method carries the provenance " + JSON.stringify(m._astProvenance) + ", which is " +
         "none of " + CALLSITE_PROVENANCE.join("/") + " (" + where + ") — lib/learn.js writes it beside " +
         "`_astInferred` on every call site it registers, so this record has one half of that write and not " +
         "the other and the row would render with no grade, which a reviewer reads as `not forced`");
  return m._astProvenance;
}

/* THE RECORD'S TEMPLATED HOLES AS PER-NAME PAIRS — the ONE read of the two lists together, because a hole is
   ONE question and its two pools are one answer. Every consumer that does anything but render needs both
   sides of it: the moat's cross-document fold, and the Send panel's attach onto a resolved schema. Written
   twice it would be two walks free to disagree about which list a name is looked up in first, and about what
   an absent list contributes — and the second is the interesting one, because it is where a `||` would go.
   `null` CONTRIBUTES AN EMPTY POOL, AND `undefined` IS NOT A SPELLING OF IT. Turning the record's stated
   absence into `[]` HERE — once, at the record's own reader — is what lets `foldValuePools` refuse anything
   that is not a pool. It is `null` and nothing else because `checkEndpointRecord` has already refused every
   other form, including the missing key an older store writes: a walk that also accepted `undefined` would
   be usable on a record nobody had checked, and would answer for it. */
function endpointHolePairs(ep, where) {
  const out = new Map();
  const take = (field, side) => {
    const list = ep[field];
    DCHECK(list === null || Array.isArray(list),
           "an endpoint's `" + field + "` is neither a list nor its stated absence (" + where + ") — this " +
           "is the one walk of both hole lists, so every caller hands it a record `checkEndpointRecord` has " +
           "passed, and a third form here means it did not");
    if (list === null) return;
    for (const pp of list) {
      DCHECK(!!pp && typeof pp.name === "string" && Array.isArray(pp.values) && pp.values.length,
             "an endpoint's `" + field + "` entry is not {name, values[]} with at least one value (" + where +
             ") — lib/merge.js builds both lists from the method parameters whose location is \"path\" and " +
             "SKIPS any hole nothing filled at that grade, so an empty one would surface a templated segment " +
             "as if a value had been learned for it");
      let cur = out.get(pp.name);
      if (!cur) { cur = { valid: [], forced: [] }; out.set(pp.name, cur); }
      /* CONCAT AND NOT A FOLD: within ONE record the two lists are already disjoint — `checkEndpointRecord`
         asserts exactly that — so this side composes and the caller's fold is the only place that decides. */
      cur[side] = cur[side].concat(pp.values);
    }
  };
  take("pathParams", "valid");
  take("pathParamsForced", "forced");
  return out;
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
  DCHECK(ep.pathParamsForced === null || Array.isArray(ep.pathParamsForced),
         "an endpoint record's `pathParamsForced` is neither a list of examples nor a stated absence (" +
         where + ") — `null` MEANS the run forced nothing into a templated hole, and a record that cannot " +
         "say that has to say \"nothing was learned\" instead, which is the inverse of what was observed. " +
         "`undefined` here is the IndexedDB door and not the producer: a store written before this field " +
         "existed carries no such key, and it must crash rather than be read as the absence, because " +
         "\"stored by an older build\" and \"this run forced nothing\" are different facts and only one of " +
         "them is something the moat observed");
  _checkPathParamPools(ep, where);
}

/* THE TWO HOLE POOLS ARE DISJOINT PER (NAME, VALUE), AND THAT DISJOINTNESS *IS* THE FOLD — the same
   assertion lib/field-def.js makes over a FieldDef's two pools, at the one record that outlives the method
   schema those pools were folded on. A value in both is not a duplicate to tidy: it is `foldValuePools`
   having been bypassed by a producer that appended where it had to promote, and the Send panel would offer
   the value as one the app computes AND label it as one no client sends. */
function _checkPathParamPools(ep, where) {
  if (ep.pathParams === null || ep.pathParamsForced === null) return;
  const offerable = new Map();
  for (const pp of ep.pathParams) offerable.set(pp.name, pp.values);
  for (const pp of ep.pathParamsForced) {
    /* A NAME IN ONE LIST AND NOT THE OTHER IS THE ORDINARY CASE and says nothing — a hole reached at one
       grade only. `has` and not a truthiness test, because a Map miss is the question being asked here. */
    if (!offerable.has(pp.name)) continue;
    const other = offerable.get(pp.name);
    DCHECK(!pp.values.some((v) => other.indexOf(v) >= 0),
           "an endpoint record carries the same learned value for `" + pp.name + "` in both the offerable " +
           "and the forced hole pool (" + where + ") — membership of one list or the other IS this record's " +
           "spelling of the per-value grade fold, so a value in both means a producer unioned the pools " +
           "independently instead of folding them, which is the defect the split exists to remove in the one " +
           "place it is invisible from either page");
  }
}
