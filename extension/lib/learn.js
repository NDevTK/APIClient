// lib/learn.js — VDD passive learning: build the API method model from AST fetch call-sites (engine results),
// from observed requests (URL/query/header/body params + path templating + per-field stats), and from observed
// responses (JSON/JSPB/gRPC/batchexecute decoded into schemas). Extracted from the offscreen-brain.js monolith
// (one problem per file); loaded before it, resolves its callers (generateSchemaFromJson from lib/schema.js,
// extractInterfaceName/calculateMethodMetadata from lib/grouping.js, protobuf/discovery libs) at call-time.
// The Passive Learning feature, just relocated out of the brain.

/* EVERY CATCH IN THIS FILE OPENS WITH `RETHROW_FATAL(e)`, AND THE REASON IS STATED ONCE HERE RATHER THAN
   TWENTY-ONE TIMES.
   Each of those catches has a REAL JOB and keeps it: "these bytes are not valid gRPC-Web / JSPB / JSON /
   SSE / NDJSON / multipart" is a DATUM about a server's response, and the right answer to it is to skip this
   decode and let the next captured response from the same URL try again. That is why they are `console.debug`
   and not throws, and none of that changes.
   WHAT WAS ALSO BEING CAUGHT IS THE PROBLEM. Every one of these `try` bodies calls into the code that turns a
   response into the model — `extractKeysFromText`, `generateSchemaFromJson`/`FromPbTree`, `mergeSchemaInto`,
   `indexResponseValues`, `learnEndpointParams` — and on this side an assertion is a THROW (extension/check.js:
   check.h aborts the process, so a C `if (err) goto fail` cannot swallow a DCHECK, while here every legitimate
   `catch` is a place one silently becomes a plausible answer). So a broken contract anywhere in the learning
   path was arriving here as one debug line reading "grpc-web frame decode failed" and the endpoint's schema
   simply never appeared — indistinguishable from a server that sent a malformed frame, on the path CLAUDE.md
   calls "the POINT". The failure would not even be rare: it is one line per response, over every response the
   extension ever sees, and the only symptom is a moat that learned less than it should have.
   `RETHROW_FATAL` is not a second assertion mechanism — it is what keeps the ONE mechanism from being locally
   disabled — so it goes FIRST in the body, before the datum is interpreted at all. It also means every catch
   here binds `e`: a `catch (_)` cannot re-raise what it declined to name. */

/* THE ONE HEADER VOCABULARY, IN THE ONE PLACE THAT SPEAKS IT.
   endpoint.c emits an endpoint's headers as a flat name -> STRING record and says what a value means:
   "a concrete one is the literal the code computed, and an unknown one is its SHAPE (`{state}.token`),
   which is what marks it as a runtime value the reviewer must supply." The doc model and the Send panel
   speak a different vocabulary for the same fact — {kind:"literal"|"opaque", value} — because the popup
   renders a literal read-only and an opaque as an input to paste a runtime token into, and because a
   literal must SUPERSEDE an opaque on merge rather than first-write-wins.

   It is a function because there are two consumers and there were two translations: this file translated,
   and lib/merge.js put the ENGINE's raw record on the flat endpoint entry untranslated. Both feed
   popup-form.js's one reader, which tests `hv.kind === "literal"` — so every endpoint-sourced header
   failed that test, rendered as "dynamic — paste runtime value", and was DROPPED by the auto-attach loop
   that skips `hv.kind !== "literal"`. A replay of an endpoint whose Authorization the engine had actually
   computed went out without it. One translation, at the boundary, for both.

   A `{hole}` in the value is the shape marker (concolic.c's own rendering), so it is what decides the kind. */
/* THE TWO DOMAIN-MERGE RULES, AT MODULE SCOPE BECAUSE THEY HAVE TWO CALLERS AND HAD ONE.
 *
 * `_excludedValues` and `_bounds` are the DOMAIN half of §@H's shape — what this endpoint's own gates PROVED
 * the value is not, and the interval its ordering gates proved it lies in. Both merge by the same law and it
 * is the OPPOSITE of the law for values: a value is knowledge that only accumulates, so it unions, while a
 * constraint is a CLAIM ABOUT THE ENDPOINT and belongs on the record only where EVERY observed path obeyed
 * it — so exclusions INTERSECT and an interval widens to the HULL.
 *
 * WHY THEY MOVED. The law was stated and applied inside `learnFromAstCallSite` only, which merges ONE call
 * site into ONE document. `lib/merge.js`'s `_mergeParamInto` is the OTHER merge — one document's parameter
 * into the cumulative moat's — and it is called exactly when both sides already exist, i.e. when a second
 * document has reached the same endpoint's same parameter. It carried `_astValidValues` (correctly, by
 * union) and `_exampleValue`, and it carried NEITHER of these two, so the later document's domain
 * observation was discarded and the moat kept the first document's claim untouched.
 * THAT ERRS TOWARD OVERSTATING, WHICH IS THE DIRECTION THAT MAKES IT A WRONG REPORT RATHER THAN A THIN ONE:
 * a second page that reached the request WITHOUT the constraint is a path that disproves it, and the moat
 * went on rendering `≠ admin` for a parameter one observed path never tested. §@H says a shape carrying the
 * wrong half is "a WRONG report, not a partial one", and a reviewer reads a surviving exclusion as a fact
 * the run established.
 * Hoisting rather than re-deriving is the point: two spellings of "intersect the claims" are two rules free
 * to disagree about one parameter, and the hull below is subtle enough (the inclusive side wins a tie,
 * because it is the WEAKER claim) that a second copy would get it wrong quietly.
 *
 * EACH CALLER STATES WHAT IT HAS, and the two callers genuinely hold different things. A CALL SITE is always
 * an observation — the engine either emitted `excludes` or proved nothing, and both are facts about that run
 * — so it passes the empty array for "no constraint" and lets the intersection erase. A MERGED PARAMETER may
 * never have been observed at all (a form scan or a live request can create one), so `lib/merge.js` tests
 * for the field's PRESENCE and passes nothing when it is absent. Collapsing those two would let a parameter
 * that no engine run ever touched erase every domain the engine emits. */

/* EVERY KEY THIS FILE READS OFF AN @H PARAM, NAMED ONCE SO THE ONE THAT ARRIVES UNREAD CAN CRASH.
 *
 * The producer is `endpoint_json_array` in engine/host/solver/endpoint.c, and solver/endpoint.h carries the
 * record's own sketch; this is the CONSUMER's half of that one contract, and it exists because the two halves
 * drift in an asymmetry no other assert in this file can see. A key the engine STOPS writing breaks a reader
 * loudly — the four `in` tests below each take their no-key arm and the intersection erases a claim. A key the
 * engine STARTS writing breaks nothing whatever: no reader asks for it, so a fact the forced execution proved
 * about a parameter is dropped between the engine and the popup, and the parameter renders with the bytes of
 * one nothing ever tested. §@H names that failure and its direction — "a shape carrying provenance alone
 * renders an UNCONSTRAINED parameter and a range-gated one with identical bytes, so its silence about the
 * gate is read as the positive statement 'anything goes'".
 *
 * THREE ARE UNCONDITIONAL AND FOUR ARE WRITTEN ONLY WHERE A GATE HELD, and that split is the contract rather
 * than a property of this list: `name`, `location` and `validValues` are on every param, while `excludes`,
 * `bounds`, `predicates` and `looselyEquals` are OMITTED where no constraint of that kind survived every
 * observed path — so each of those four absences is a POSITIVE statement and is read with an `in` test and
 * never with a `||`. This list is the set of names, not a claim about which of them arrived.
 *
 * IT IS A SET AND DELIBERATELY NOT A COUNT. A tally is a second copy of a fact that moves, and it rots in
 * silence: this project has already carried a header naming "the eight cost counters" and "all thirteen
 * fields" where both numbers were wrong before anything was added to them, and lib/merge.js's restatement of
 * THIS record spelled a param as `{name, location, validValues[], excludes[], bounds{}}` — written before
 * `predicates` and `looselyEquals` were added to the emission, and never revisited when they were, so it went
 * on naming the fields a reader "may read" while two of them were missing from it. A set is checkable by
 * grepping this file for each name; a number is checkable by nothing.
 *
 * WIDENING IT IS HALF A DIFF AND NEVER A DIFF. The crash below says so at the site, because the tempting
 * repair — add the name, make the abort stop — converts a loud unread key into the silent dropped one this
 * whole mechanism exists to prevent. */
const AST_PARAM_KEYS = Object.freeze([
  "name", "location", "validValues",              // written on every param
  "excludes", "bounds", "predicates", "looselyEquals",  // written only where that gate held on every path
]);

/* Exclusions: intersect, because only a token EVERY observed path proved the value is not belongs to the
   endpoint. `observed` is the array this sighting proved (empty = this sighting proved nothing). A target
   that has never carried the field takes the sighting whole — it has no claim to intersect against. */
function intersectExcludedValues(target, observed) {
  DCHECK(Array.isArray(observed),
         "an exclusion merge was handed something that is not an array — a sighting either proved a set of " +
         "tokens or proved none, and `none` is the EMPTY array rather than a missing argument, because the " +
         "empty set is what erases a claim an earlier path had made");
  if (!Array.isArray(target._excludedValues)) { target._excludedValues = observed.map(String); return; }
  const seen = observed.map(String);
  target._excludedValues = target._excludedValues.filter((v) => seen.indexOf(String(v)) >= 0);
}

/* ONE SIDE OF AN INTERVAL, READ OUT OF THE ENGINE'S OWN VOCABULARY. endpoint.c emits JSON Schema Validation
   2020-12 §6.2's keywords directly — §6.2.4 "minimum" / §6.2.5 "exclusiveMinimum" below, §6.2.2 "maximum" /
   §6.2.3 "exclusiveMaximum" above — so nothing between the engine and the OpenAPI export renames them, and
   there is one spelling of a bound in the whole pipeline instead of three that have to agree.
   `null` IS THE ANSWER FOR AN UNBOUNDED SIDE and it is a positive one: no ordering gate over this hole
   claimed anything in that direction on every observed path. */
function _boundSide(b, low) {
  const inc = low ? "minimum" : "maximum", exc = low ? "exclusiveMinimum" : "exclusiveMaximum";
  if (inc in b) return { v: b[inc], incl: true };
  if (exc in b) return { v: b[exc], incl: false };
  return null;
}
function _boundSideWrite(out, side, low) {
  if (!side) return;
  out[side.incl ? (low ? "minimum" : "maximum") : (low ? "exclusiveMinimum" : "exclusiveMaximum")] = side.v;
}

/* Bounds: widen to the hull, which is the exclusion rule spelled for an ORDERED domain and not a second rule.
   Two paths reaching one request with `x >= 5` and `x >= 10` leave `x >= 5`; with `x >= 5` and `x <= 3` they
   leave nothing, because neither claim survives the other path. A sighting with no bound on a side ERASES
   that side for the same reason a sighting with no exclusions erases the exclusions.
   `observed` is this sighting's bounds object, or NULL for "this sighting claimed no interval".
   THE THREE ABSENCES ARE KEPT APART and they are three different facts. `_bounds` absent on the target = it
   has never had an engine observation, so the first one is taken whole; `_bounds` null = a claim that WAS
   made and has since been disproved by another path, and nothing re-establishes it; a side missing inside a
   `_bounds` object = unbounded in that direction. Collapsing any two of them would let a form scan or a live
   request that created the parameter first erase every domain the engine emits. */
function widenBoundsInto(target, observed) {
  DCHECK(observed === null || (observed && typeof observed === "object" && !Array.isArray(observed)),
         "a bounds merge was handed something that is neither an interval nor null — `null` is how a " +
         "sighting says it claimed no interval, and it is a value rather than an omission because it is " +
         "what disproves a claim an earlier path had made");
  if (!("_bounds" in target)) { target._bounds = observed ? Object.assign({}, observed) : null; return; }
  if (target._bounds === null) return;   // already disproved by an earlier path — nothing re-establishes it
  if (!observed) { target._bounds = null; return; }
  const hull = (a, b, low) => {
    if (!a || !b) return null;           // one path unbounded on this side disproves the other's claim
    if (a.v === b.v) return a.incl ? a : b;   // tie: the INCLUSIVE side is the weaker claim, so it survives
    return (low ? (a.v < b.v) : (a.v > b.v)) ? a : b;
  };
  const out = {};
  _boundSideWrite(out, hull(_boundSide(target._bounds, true), _boundSide(observed, true), true), true);
  _boundSideWrite(out, hull(_boundSide(target._bounds, false), _boundSide(observed, false), false), false);
  target._bounds = Object.keys(out).length ? out : null;
}

/* WHICH TWO CALL PREDICATES ARE ONE — method, arm and EVERY argument, which is the identity the engine's own
   dedup and its own intersection both use (solver/concolic.h `concolic_pred_same`). It is a composed key and
   not a `JSON.stringify` of the record, because the field ORDER of an object literal would then decide
   equality: two producers writing the same three facts in two orders would compose two keys, and the
   intersection would drop a claim every observed path obeyed. The length prefix is the engine's own encoding
   rule for the same reason it uses one — no argument's contents can spell an argument boundary, so a
   `["a","b"]` and an `["a\0b"]` can never collide. */
function _predKey(p) {
  const parts = [String(p.method), p.holds ? "1" : "0"];
  for (const a of p.arguments) parts.push(String(a).length + ":" + String(a));
  return parts.map((s) => s.length + ":" + s).join("");
}

/* Call predicates: INTERSECT, which is `intersectExcludedValues`' rule and not a second one — a predicate
   belongs on the record only where EVERY observed path obeyed it, so a sighting that reached the request
   without testing it DISPROVES the claim. There is no hull and no widening because the domain is unordered:
   `startsWith("/api")` and `startsWith("/admin")` do not weaken to a common predicate, and inventing one that
   covered both would mean deciding what the method MEANS, which is the recogniser CLAUDE.md §RUN-DON'T-MATCH
   forbids arriving inside a merge rule.
   `observed` is the array this sighting proved (empty = this sighting proved nothing). A target that has never
   carried the field takes the sighting whole — it has no claim to intersect against. */
function intersectPredicates(target, observed) {
  DCHECK(Array.isArray(observed),
         "a call-predicate merge was handed something that is not an array — a sighting either proved a set " +
         "of predicates or proved none, and `none` is the EMPTY array rather than a missing argument, because " +
         "the empty set is what erases a claim an earlier path had made");
  if (!Array.isArray(target._predicates)) { target._predicates = observed.slice(); return; }
  const seen = observed.map(_predKey);
  target._predicates = target._predicates.filter((p) => seen.indexOf(_predKey(p)) >= 0);
}

/* WHICH TWO LOOSE EQUALITIES ARE ONE — the operand AND ITS TYPE, which is the identity the engine's own dedup
   and its own intersection both use (solver/concolic.h `concolic_looseeq_same`). The type is half the key and
   not a label on it: ECMAScript §7.1.19 ToString ( arg ) flattens `undefined`, `null`, `0` and `false` onto
   text that is also a legal String operand, so a key over the value alone would merge `== undefined` with
   `== "undefined"` — two gates whose §7.2.13 IsLooselyEqual ( x, y ) holding sets have nothing in common —
   into one claim no run ever made. Composed and length-prefixed for `_predKey`'s reason. */
function _leqKey(q) {
  return [String(q.type), String(q.value)].map((s) => s.length + ":" + s).join("");
}

/* Loose equalities: INTERSECT, which is `intersectExcludedValues`' rule and not a second one — a gate belongs
   on the record only where EVERY observed path obeyed it, so a sighting that reached the request without
   holding it DISPROVES the claim. There is no hull and no widening because the domain is unordered: `== 0`
   and `== ""` do not weaken to a common claim, and inventing one that covered both would mean computing the
   union of two of §7.2.13's holding sets, whose Object arm runs the page's own ToPrimitive — deciding what
   `==` MEANS, in a merge rule, which is the recogniser CLAUDE.md §RUN-DON'T-MATCH forbids.
   `observed` is the array this sighting proved (empty = this sighting proved nothing). A target that has never
   carried the field takes the sighting whole — it has no claim to intersect against. */
function intersectLooselyEquals(target, observed) {
  DCHECK(Array.isArray(observed),
         "a loose-equality merge was handed something that is not an array — a sighting either proved a set " +
         "of loose equalities or proved none, and `none` is the EMPTY array rather than a missing argument, " +
         "because the empty set is what erases a claim an earlier path had made");
  if (!Array.isArray(target._looselyEquals)) { target._looselyEquals = observed.slice(); return; }
  const seen = observed.map(_leqKey);
  target._looselyEquals = target._looselyEquals.filter((q) => seen.indexOf(_leqKey(q)) >= 0);
}

function astHeaderRecord(headers) {
  DCHECK(headers && typeof headers === "object" && !Array.isArray(headers) && Object.keys(headers).length,
         "astHeaderRecord was handed something that is not a non-empty header record — endpoint.c omits the " +
         "`headers` key entirely when it observed none, so the caller must read that absence as 'nothing was " +
         "observed' and never call here with an empty or absent one");
  const out = {};
  for (const hk in headers) {
    const hv = headers[hk];
    DCHECK(typeof hv === "string",
           "an @H header value is not a string — endpoint.c writes name -> string, so a structured value here " +
           "is that serializer having changed shape under a reader that would classify it as an opaque header");
    out[hk] = { kind: /\{[^}]*\}/.test(hv) ? "opaque" : "literal", value: hv };
  }
  return out;
}

/* WHAT `learnFromAstCallSite` HANDS BACK, AS TWO POSITIVE STATEMENTS RATHER THAN A NULLABLE ONE.
   `entry` is the service (discovery doc) the call site was filed under, and `method` is the method record
   the call site's values were merged into — the object holding this endpoint's parameters, each with its
   `location` and its `_astValidValues`. `method: null` is a STATEMENT: the URL was dynamic, so a service
   exists but no method could be registered against it; `entry: null` says the site is not a learnable
   endpoint at all (an unreached @T candidate, or inline content).

   It returns the method because the caller PERSISTS its path-param examples onto the flat endpoint record,
   and the caller must not have to find the method again: re-deriving it from (verb, path, origin) would be
   this file's naming rules restated in lib/merge.js, and the first time they disagreed the caller would
   quietly persist nothing — which is the failure this whole pass exists to remove. */
const _NOT_LEARNABLE = Object.freeze({ entry: null, method: null });

function learnFromAstCallSite(docData, interfaceName, callSite, scriptUrl) {
  // Takes the DocData object directly (not documentId) so a TRANSIENT view
  // (_emptyDocView, documentId=null) can carry a globalStore-only merge for a
  // GONE document without getDoc(null) creating a phantom state.docs entry.
  const tab = docData;

  // Structural @T candidates carry url:null (a host-edge site in
  // unreached code whose value never resolved). They are surfaced as
  // structural candidates / focusedView review items, never as a
  // learnable endpoint — resolving null through new URL() fabricates a
  // bogus "/null" path (origin + String(null)). Skip cleanly.
  if (callSite.url == null || callSite.url === "") return _NOT_LEARNABLE;

  // Resolve URL. Dynamic / unresolvable → register service-level only,
  // no synthetic method entry (would confuse the reviewer with made-up
  // paths like `dynamic_0`).
  //
  // Inline-content schemes (data:/blob:/about:/javascript:) aren't API
  // endpoints — they're content embedded in the bundle. Skip them so
  // the service list doesn't accumulate empty-host records with
  // garbled paths.
  if (/^(data|blob|about|javascript):/i.test(callSite.url)) return _NOT_LEARNABLE;
  // Relative URLs resolve against the DOCUMENT's own url at runtime, NOT the
  // script's host. Cross-origin-hosted scripts (e.g. Reddit serves its
  // shreddit bundle from www.redditstatic.com but it fetches against
  // www.reddit.com when the bundle executes on a reddit.com page) would
  // otherwise be misattributed to the script's host. Using THIS document's url
  // (not the tab's top-page url) keeps an iframe's relative fetches on its own
  // origin. Fall back to scriptUrl only when the document url isn't available.
  /* WHICH URL COMPONENTS THE CODE DETERMINED — lib/callsite-url.js, the one parser of endpoint.c's address
     template. The regex that stood here was this file's copy of lib/merge.js's, and the two had to agree by
     hand about what "dynamic" meant. A parse failure is no longer swallowed into _NOT_LEARNABLE: an address
     whose origin is fully literal and still does not parse is a request Fetch §5.4 says the page's own
     fetch() would have thrown a TypeError on, so it crashes where it is born rather than removing the call
     site from the surface with nothing to say it went. */
  const _addr = astCallSiteAddress(callSite.url, (tab && tab.url) ? tab.url : scriptUrl);
  const csUrl = _addr.originKnown ? _addr.url : null;

  // Classification at AST-time is an OPEN question — we don't have a
  // response body to magic-byte-sniff, and request shape alone (GET
  // with no query / body) can mean either "static asset fetch" or
  // "plain API endpoint." We register the method regardless and defer
  // real API-vs-asset classification to the moment real traffic flows
  // (handleResponseBody stamps _responseKind="asset" via magic bytes —
  // classifyResponseAsset in lib/discovery.js).


  // Get-or-create docEntry — same prologue as learnFromRequest. WHICH RULE NAMED THIS BUCKET is stated on the
  // entry (lib/discovery-entry.js): a literal origin is classifyInterface's "origin", and a call site whose
  // address is a SHAPE has no origin to classify, so the shape itself is what named it.
  const grouping = csUrl
    ? classifyInterface(csUrl)
    : { rule: "ast-dynamic", matched: "origin is a shape: " + _addr.host };
  let docEntry = tab.discoveryDocs.get(interfaceName);
  if (!docEntry || !docEntry.doc) {
    docEntry = {
      status: "found",
      isVirtual: true,
      grouping: makeGroupingRecord(grouping.rule, grouping.matched, csUrl ? csUrl.href : callSite.url,
                                   "lib/learn.js minting a virtual entry for an AST call site, service " +
                                   JSON.stringify(interfaceName)),
      doc: {
        kind: "discovery#restDescription",
        name: interfaceName,
        title: `${interfaceName} (Learned)`,
        /* THE ORIGIN, AND NOTHING FABRICATED WHERE THERE ISN'T ONE. `"https://" + interfaceName + "/"` stood
           on both lines and wrote a scheme the code never determined onto a bucket whose whole defining fact
           is that its origin is unknown — a plausible absolute URL that no fetch of this page resolves to.
           A shape origin's root IS the shape (URL §4.7: the origin is scheme+host+port, and here none of the
           three was computed); lib/discovery.js already asks URL.canParse before deriving a basePath from it. */
        rootUrl: csUrl ? csUrl.origin + "/" : _addr.host,
        baseUrl: csUrl ? csUrl.origin + "/" : _addr.host,
        resources: { learned: { methods: {} } },
        schemas: {},
      },
    };
    tab.discoveryDocs.set(interfaceName, docEntry);
  }
  const doc = docEntry.doc;
  if (!doc.resources.learned) doc.resources.learned = { methods: {} };

  if (!csUrl) return { entry: docEntry, method: null };   // dynamic URL: the service exists, no method to register against

  // Method name + collision handling — mirrors learnFromRequest.
  const { methodName: baseMethodName } = calculateMethodMetadata(csUrl, interfaceName);
  const qualifiedName = callSite.method.toLowerCase() + "_" + baseMethodName;
  // Verb-matched probed lookup (see learnFromRequest for the same rule):
  // a probed POST entry must not absorb GET traffic.
  const probedBase = doc.resources.probed?.methods?.[baseMethodName];
  const probedMethod = (probedBase && probedBase.httpMethod === callSite.method) ? probedBase : null;

  let methodName;
  const existingBase = doc.resources.learned.methods[baseMethodName];
  const existingQualified = doc.resources.learned.methods[qualifiedName];
  if (existingQualified) {
    methodName = qualifiedName;
  } else if (existingBase && existingBase.httpMethod !== callSite.method) {
    const existQualName = existingBase.httpMethod.toLowerCase() + "_" + baseMethodName;
    if (!doc.resources.learned.methods[existQualName]) {
      existingBase.id = `${interfaceName.replace(/\//g, ".")}.${existQualName}`;
      doc.resources.learned.methods[existQualName] = existingBase;
    }
    delete doc.resources.learned.methods[baseMethodName];
    methodName = qualifiedName;
  } else {
    methodName = baseMethodName;
  }

  const methodId = `${interfaceName.replace(/\//g, ".")}.${methodName}`;
  if (!doc.resources.learned.methods[methodName] && !probedMethod) {
    doc.resources.learned.methods[methodName] = {
      id: methodId,
      path: _decHoles(csUrl.pathname.substring(1)),
      httpMethod: callSite.method,
      parameters: {},
      request: null,
      origin: csUrl.origin,
    };
  }
  const m = probedMethod || doc.resources.learned.methods[methodName];
  DCHECK(m && typeof m === "object",
         "learnFromAstCallSite reached its param merge with no method — the block above either found the " +
         "verb-matched probed entry or created the learned one, so an absent method here means that " +
         "get-or-create stopped covering a case and every value this call site computed has nowhere to land");
  if (!m.origin) m.origin = csUrl.origin;
  /* THE BUNDLE-ORIGIN FACT IS STAMPED BY THE FUNCTION THAT LEARNED IT, NOT BY THE CREATE THAT HAPPENED FIRST.
     `_astInferred` was written inside the object literal above, so it existed only when the AST path was the
     first producer to name this method. It is READ as "the engine found this call site in the shipped
     bundle" — lib/popup-send.js `_methodOrigin` turns it into the UNUSED / unused+fired / fired-only tag,
     which is the tool's whole claim — and the wire producer (learnFromRequest, and the batch/chunk
     registrations below it) creates the same record with no such field. So an endpoint whose live request
     landed before the analysis merged was reported to the user as "fired only (no bundle origin)": the exact
     inverse of what the forced execution had just proved, arrived at by which producer ran first.
     MEASURED in Chrome on the one-fetch probe page (127.0.0.1:8890): the method carried `_astValidValues`
     from this very call and no `_astInferred`, and the Send panel tagged it `[fired only]`.
     Reaching this line IS the fact, for the record and for the script that carried it, so both are written
     here unconditionally. `_astSourceScript` keeps the FIRST script that reached it — one method reached from
     two chunks is one endpoint, and overwriting would make the attribution the last writer's. */
  m._astInferred = true;
  if (!m._astSourceScript) m._astSourceScript = scriptUrl || null;

  /* AND WHAT THE BUNDLE ORIGIN IS EVIDENCE OF — `_astInferred` says the engine found this address, and this
     says whether a real client's code computes it. They are written together because the second is worthless
     without the first and misleading without the second: a method the engine reached only by FORCING a gate
     carries `_astInferred` exactly like one the app's own code composes, so with the grade silent both wear
     lib/popup-send.js's `[UNUSED]` tag — which is the tool's headline claim about what the bundle CAN do —
     and one of them is a claim about a request no client makes.
     MOST OBSERVED ACROSS SIGHTINGS (lib/endpoint-record.js's fold): this method record is keyed by verb and
     path, so the engine's two @H rows for one address (which it deliberately does NOT merge, because the
     grade is part of the record's identity there) meet here, and the record's ONE claim about the address is
     the strongest that any path to it supports.
     THIS FOLD IS THE RECORD'S CLAIM AND NOT EACH VALUE'S, AND THE VALUES HAVE THEIR OWN — `_mergeAstValues`
     below folds the grade of every sighting that computed a VALUE by this same most-observed rule and spells
     the result as which of the parameter's two pools the value sits in, so a value the engine computed on a
     FORCED arm can no longer sit beside a derived one under a record this line folded to `derived`. The two
     folds are separate because their subjects are: one address can be reached at two grades, and so can one
     value, and neither answer follows from the other.
     THE FIRST SIGHTING FOLDS AGAINST ITSELF rather than being assigned past the fold, so the vocabulary is
     asserted on EVERY path through this line and not only on the second call site to reach a method. A record
     restored from a store an older build wrote carries no `_astProvenance` at all, and taking the new word
     for it is the right answer to that: an old record makes no claim, so there is nothing to fold with. */
  {
    const _prev = m._astProvenance === undefined ? callSite.provenance : m._astProvenance;
    m._astProvenance = mostObservedProvenance(_prev, callSite.provenance);
  }

  /* `_astCallSites` IS GONE, AND SO IS THE SOURCE LOCATION IT WAS MADE OF. It recorded one entry per call
     site as {script, line, column, enclosingFunction}, keyed and deduped on `line:column`, and its comment
     said the reviewer clicks through to the JS location. endpoint.c emits `method`, `url`, `params` and
     (when observed) `headers`; there is no `loc` and no `enclosingFunction` anywhere in engine/host, so
     every entry ever pushed was {script, null, null, null} and the dedup key was the constant
     "<script>:null:null" — one entry per method, carrying the script this file already stores as
     `_astSourceScript`. Nothing read the array: grep finds no consumer in the extension, the popup or the
     harness. A write-only record of a location nobody produced is not provenance, so it goes rather than
     being defaulted into looking like one. If click-through is wanted, the engine has to carry the site's
     position on the endpoint record first. */

  /* THE TYPE THE RUN OBSERVED, WHICH IS THE THIRD THING A DOMAIN STATES AND THE ONE THAT DECIDES WHETHER THE
     OTHER TWO SURVIVE THE REPORT.
     What stood here read `validValues[0]` and branched on its `typeof` for "number" and "boolean". Every
     value on that array is a JSON STRING by construction — endpoint.c writes each one through
     `json_buf_str`, and `_mergeAstValues` below re-stringifies whatever it is handed — so neither branch
     could ever be taken and this helper answered "string" for EVERY parameter the forced execution ever
     learned. That is the read-with-no-writer defect with the writer on the other side of the ABI: two
     branches describing a producer that does not exist, and nothing to say so, because "string" is also the
     right answer for most parameters.
     IT IS A WRONG REPORT AND NOT A THIN ONE, WHICH IS WHY IT IS FIXED HERE RATHER THAN DELETED. A parameter
     carrying `bounds` is one the page compared against a NUMBER: concolic_rel_hook records a bound only for
     a finite Number operand, and ECMAScript §7.2.12 IsLessThan step 3 takes the string comparison only when
     BOTH sides are Strings, so a Number on the concrete side puts the comparison on the numeric path
     whatever the unknown turns out to be. Typed "string", that observation is then ERASED at the last hop:
     JSON Schema Validation 2020-12 §6.2.5 "exclusiveMinimum" asserts "If the instance is a number, then the
     instance is valid only if …", so `{"type":"string","exclusiveMinimum":5}` — which is exactly what
     lib/openapi-export.js emitted — asserts NOTHING, and every validator silently drops the only domain the
     run proved.
     THE POPUP IS NOT WHERE THIS SHOWED, WHICH IS WHY IT SURVIVED. lib/popup-form.js renders `_bounds` as its
     own badge and as the input's placeholder whatever the type says, so on screen the interval was visible
     the whole time and only the box it sat beside was the wrong kind. The export is the surface where a
     domain either asserts or does not, and it is the surface nobody reads by eye.
     IT INVENTS NOTHING. `bounds` present is a fact this run observed, exactly as a pin is; the line §@H
     draws is whether a VALUE was determined, and no value is determined by naming the type of the
     comparison the page performed. Absence of `bounds` still answers "string", which is this record's
     stated absence and not a default — a parameter nothing ordered has no type observation to report. */
  const _paramType = (p) => {
    DCHECK(p.validValues.every((v) => typeof v === "string"),
           "an @H param's validValues holds something that is not a string — endpoint.c writes every entry " +
           "through json_buf_str, so a non-string here is that producer having grown a second vocabulary " +
           "and this helper answering the type of a value it cannot have been handed");
    return p.bounds ? "number" : "string";
  };

  /* MERGE THE VALUES ONE SIGHTING COMPUTED ONTO A TARGET (param or schema prop), AT THE GRADE THAT SIGHTING
     WAS REACHED AT — the third argument is REQUIRED and every caller states it, because a value's grade is a
     fact about the value and there is no default for it that is not a fabrication.

     THIS IS THE FOLD THE RECORD-LEVEL ONE NAMED AS ITS RESIDUAL. `m._astProvenance` above folds the METHOD's
     one claim about the address; this folds each VALUE, and it has to be a separate fold because the method
     record is keyed by verb and path, so the engine's two @H rows for one address — which it deliberately
     does NOT merge, the grade being part of the record's identity there — meet here with their values in one
     bag. Before this, a value the engine computed on a FORCED arm sat in `_astValidValues` beside one from a
     derived arm, under a record folded to `derived`, and the Send panel prefilled it: an example a server
     will reject, offered under a method the tool says the app's own code computes. That is CLAUDE.md §@H's
     fabrication with a server behind it, and its distinguishing property is not that it is useless but that
     it is PLAUSIBLE.

     THE FOLD IS THE SAME RULE AS THE RECORD'S — MOST OBSERVED — AND IT IS PERFORMED AS A MOVE BETWEEN POOLS.
     A value one path computed without forcing anything is a value the app computes, whatever some other path
     had to force to reach it, so a promotion is one-way: out of `_astForcedValues` and into
     `_astValidValues`, never back. lib/endpoint-record.js's `provenanceOffersExample` is the one place the
     line between the pools is drawn and states why it is a FIELD NAME rather than a grade every reader must
     remember to consult.

     A VALUE ENTERS ONE POOL ONLY, so the pools stay disjoint and lib/field-def.js asserts that they are.
     Membership IS the folded grade; a value in both would render the same bytes twice under two
     contradictory claims. */
  // Promotes to `enum` when distinct count >= 2 (matches prior behavior).
  // Re-picks the example value when new AST values land — without this,
  // a form-scan-created param whose initial pickExampleValue ran BEFORE
  // any AST values were added would stay frozen at "type-default" even
  // though tier-3 (ast-constraint) is now satisfied.
  const _mergeAstValues = (target, validValues, prov) => {
    /* `provenanceOffersExample` ASSERTS THE VOCABULARY, so there is no second check of it here: a grade
       outside the three words crashes inside it, at the one place the words are declared. */
    const _offerable = provenanceOffersExample(prov);
    let merged = false;
    if (Array.isArray(validValues) && validValues.length) {
      /* `undefined`-or-array IS THIS RECORD'S VOCABULARY AND NOT A DEFAULT. A method parameter is a raw
         object rather than a FieldDef (lib/field-def.js's `null` spelling is that record's), and this
         producer writes each pool's key only where the pool is NON-EMPTY — which is the vocabulary
         `_astValidValues` already speaks here and which lib/send.js's two literals normalise to `null` on the
         way out. A second spelling would be two rules. */
      /* THE LAW IS NOT SPELLED HERE ANY MORE — `foldValuePools` (lib/endpoint-record.js) performs it, and
         this was one of THREE private spellings of it, with a fourth about to be written at the flat endpoint
         records' merge. What stays here is what belongs to THIS record and to this producer: the vocabulary
         above, and the stringification below. */
      /* THE INCOMING VALUES ARE COERCED BEFORE THE FOLD AND NOT INSIDE IT. A pool's identity is the STRING —
         two sightings computing the same bytes are two paths to ONE value, which is why the fold compares by
         it — and the engine's @H param record carries numbers as well as strings, so the coercion is this
         producer's own and happens where its values arrive. A fold that coerced would be deciding a record's
         vocabulary for it, which is exactly what it refuses to do for the empty pool. */
      const _in = validValues.map(String);
      const _before = Array.isArray(target._astValidValues) ? target._astValidValues.length : 0;
      const _f = foldValuePools(
        Array.isArray(target._astValidValues) ? target._astValidValues : [],
        Array.isArray(target._astForcedValues) ? target._astForcedValues : [],
        _offerable ? _in : [], _offerable ? [] : _in);
      const valid = _f.valid;
      if (valid.length > _before) merged = true;
      if (valid.length) target._astValidValues = valid;
      /* AN EMPTIED FORCED POOL IS DELETED RATHER THAN LEFT AS `[]`, because the last promotion out of it is
         exactly the case where the key would otherwise survive as an empty list — and an empty list here is
         a THIRD statement beside "no forced value was observed" and "these were", which no reader has. */
      if (_f.forced.length) target._astForcedValues = _f.forced;
      else if (Array.isArray(target._astForcedValues)) delete target._astForcedValues;
      // (No `customEnum` test: nothing writes that flag — see _applyStatsToField. An existing `enum` still
      //  wins, which is the real condition, since a declared one is a fact about the API description.)
      /* AND THE MEMBERSHIP IS PROMOTED OUT OF THE OFFERABLE POOL ALONE. An `enum` is a CLAIM about what this
         API accepts, rendered as a `<select>` the reviewer picks from and exported into the OpenAPI document
         as a validation keyword — so a member that exists only because a gate was forced is the same
         fabrication one container further out, where it also escapes the two-pool split entirely. */
      if (valid.length >= 2 && !target.enum) {
        target.enum = valid.slice();
        target._detectedEnum = true;
      }
    }
    /* NO `_astDefault`. It was written from `p.defaultValue` — a field the engine's param record does not
       have — and read by nothing but the line that wrote it, so the pair was a self-consistent record of
       an observation that never occurred. */
    if (merged) {
      const ex = pickExampleValue(target, null);
      if (ex) {
        target._exampleValue = ex.value;
        target._exampleValueSource = ex.source;
        if (ex.confidence != null) target._exampleConfidence = ex.confidence;
      } else {
        // No real value was traceable — leave the field without an
        // example so callers can surface the gap rather than acting on
        // a synthesised type-default that nothing in the bundle
        // produced.
        delete target._exampleValue;
        delete target._exampleValueSource;
        delete target._exampleConfidence;
      }
    }
  };

  /* THE OTHER FACT THE SHAPE STATES. `validValues` says WHO must supply the parameter and what the code
     computed for it; `excludes` says WHAT the value must look like — the tokens this endpoint's own equality
     gates PROVED it is not, on every path the engine observed reaching the request. Neither substitutes for
     the other, and the failure is asymmetric: a param carrying provenance alone renders identically whether
     the run proved something about it or never tested it at all, and that silence is read as "anything goes".
     THE MERGE IS AN INTERSECTION, WHICH IS THE OPPOSITE OF `_mergeAstValues`' UNION, and the two rules are
     opposite because the facts are. A value is knowledge that only accumulates; a constraint is a CLAIM about
     the endpoint, and a later observation that did not obey it is a path that disproves the claim.
     THE MERGE ITSELF IS `intersectExcludedValues` at module scope, because lib/merge.js's cross-document
     merge must obey the identical law and two spellings of it are two rules free to disagree. What is
     stated here is only what this caller knows: a call site is always an observation, so its no-key
     arm passes the empty set rather than declining to speak. */
  const _mergeExcludes = (target, p) => {
    /* A CALL SITE IS ALWAYS AN OBSERVATION, which is why the no-key arm passes the EMPTY array rather than
       returning: endpoint.c omits `excludes` exactly where no equality gate's claim survived every observed
       path to this request, so its absence is the positive statement "this run proved nothing here" and the
       intersection is what turns that into the erasure of an earlier path's claim. */
    if (!("excludes" in p)) { intersectExcludedValues(target, []); return; }
    DCHECK(Array.isArray(p.excludes) && p.excludes.length > 0,
           "an @H param carries an `excludes` that is not a non-empty array — endpoint.c omits the key " +
           "entirely where no constraint held on every observed path, so an empty or non-array one here is " +
           "the engine stating a domain it does not have");
    intersectExcludedValues(target, p.excludes);
  };

  /* THE ORDERING GATE'S HALF OF THE SAME SHAPE. The RULE — widen to the hull, because a claim belongs
     on the record only where every observed path obeyed it — is `widenBoundsInto` at module scope,
     beside the exclusion rule it is the ordered-domain spelling of. What stays here is what is true of
     THIS caller: the four DCHECKs are on the ENGINE's vocabulary, so they belong at the boundary the
     engine's record crosses and nowhere else. */
  const _mergeBounds = (target, p) => {
    /* NULL IS HOW THIS CALLER SAYS "THIS RUN CLAIMED NO INTERVAL", and it is a value rather than an
       omission for the reason the empty array above is one: endpoint.c omits `bounds` exactly where no
       ordering gate's claim survived every observed path, so the absence disproves an earlier path's
       claim instead of leaving it standing. */
    if (!("bounds" in p)) { widenBoundsInto(target, null); return; }
    DCHECK(p.bounds && typeof p.bounds === "object" && !Array.isArray(p.bounds),
           "an @H param carries a `bounds` that is not an object — endpoint.c omits the key entirely where " +
           "no ordering gate's claim survived every observed path, so anything else here is the engine " +
           "stating a domain it does not have");
    DCHECK(["minimum", "exclusiveMinimum", "maximum", "exclusiveMaximum"]
             .filter((k) => k in p.bounds).length > 0,
           "an @H param carries an empty `bounds` — the object is written only as a side is written, so one " +
           "with neither side is an interval that was allocated and never narrowed");
    DCHECK(!("minimum" in p.bounds && "exclusiveMinimum" in p.bounds) &&
           !("maximum" in p.bounds && "exclusiveMaximum" in p.bounds),
           "an @H param states a bound as inclusive AND exclusive on one side — the two are alternative " +
           "spellings of one side and a consumer reading both would apply the looser one silently");
    DCHECK(Object.keys(p.bounds).every((k) => typeof p.bounds[k] === "number" && isFinite(p.bounds[k])),
           "an @H param's bound is not a finite number — JSON Schema Validation 2020-12 §6.2 requires a " +
           "number for all four keywords, and concolic_rel_hook records a bound only for a finite Number " +
           "operand, so a string or an Infinity here is the two ends disagreeing about what a bound is");
    widenBoundsInto(target, p.bounds);
    /* AND THE TYPE OBSERVATION THE SAME BOUND CARRIES, WHICH IS WHY IT IS WRITTEN HERE AND NOT ONLY AT THE
       MINT. A parameter's record is created by its FIRST sighting and merged by every later one, so a run
       that reached the request with no ordering gate and then reached it again through one would leave the
       type at the mint's "string" and the second sighting's whole interval vacuous — the erasure this rule
       exists to stop, arriving one sighting late instead of at creation.
       IT IS PROMOTED FROM THIS SIGHTING'S OWN `p.bounds`, NEVER FROM THE MERGED `target._bounds`, and the
       two are different facts. The merged interval is the claim EVERY observed path obeyed, so a later path
       that ordered nothing ERASES it (widenBoundsInto above); "the page compares this parameter as a
       number" is not disproved by a path that never compared it at all, so the type accumulates the way
       `_astValidValues` does and is never taken back.
       ONLY OVER A TYPE THIS FILE ITSELF INFERRED. A parameter a Google Discovery document DECLARED carries
       its server's own statement about the type, which is not ours to overrule from a client-side run. */
    if (target._astInferred && target.type === "string") target.type = _paramType(p);
  };

  /* THE THIRD OF THE THREE WAYS A GATE NARROWS A DOMAIN, and the one CLAUDE.md §@H names in its own headline
     example (`{startsWith:/api}`). An equality determines a VALUE and its negation, an ordering an INTERVAL,
     and a METHOD CALL neither — so `if (!path.startsWith("/api")) return;` had nothing to arrive through, and
     a parameter a prefix check gated rendered with the same bytes as one nothing had ever tested. The RULE —
     intersect, because a claim belongs on the record only where every observed path obeyed it — is
     `intersectPredicates` at module scope, beside the exclusion rule it is the unordered spelling of. What
     stays here is what is true of THIS caller: the DCHECKs are on the ENGINE's vocabulary (solver/endpoint.h
     states the record), so they belong at the boundary the engine's record crosses and nowhere else.
     NOTHING HERE RE-IMPLEMENTS A METHOD. `p.method` is a name the page wrote and this file never asks what it
     means; it is carried, intersected and rendered as the transcript it is. A consumer that turned
     `startsWith("/api")` into a prefix test would be the recogniser §RUN-DON'T-MATCH forbids, moved one hop
     downstream of the engine that refused to build it. */
  const _mergePredicates = (target, p) => {
    /* A CALL SITE IS ALWAYS AN OBSERVATION, so the no-key arm passes the EMPTY array rather than returning —
       endpoint.c omits `predicates` exactly where no call predicate survived every observed path to this
       request, so its absence is the positive statement "this run proved nothing here" and the intersection
       is what turns that into the erasure of an earlier path's claim. */
    if (!("predicates" in p)) { intersectPredicates(target, []); return; }
    DCHECK(Array.isArray(p.predicates) && p.predicates.length > 0,
           "an @H param carries a `predicates` that is not a non-empty array — endpoint.c omits the key " +
           "entirely where no call predicate held on every observed path, so an empty or non-array one here " +
           "is the engine stating a domain it does not have");
    DCHECK(p.predicates.every((q) => q && typeof q === "object" && !Array.isArray(q) &&
                                    typeof q.method === "string" && q.method.length > 0 &&
                                    Array.isArray(q.arguments) &&
                                    q.arguments.every((a) => typeof a === "string") &&
                                    typeof q.holds === "boolean"),
           "an @H call predicate is not {method:<non-empty string>, arguments:[<string>...], holds:<boolean>} " +
           "— endpoint.c writes the method through json_buf_str, every argument through json_buf_str, and " +
           "`holds` as a bare JSON true/false, so anything else is that producer having changed shape under a " +
           "reader that would carry the wrong half of the observation. `arguments` may be EMPTY (a page can " +
           "branch on `x.trim()`), which is why its length is not asserted and its element type is");
    intersectPredicates(target, p.predicates);
  };

  /* …AND THE LOOSE EQUALITIES THAT HELD, which is the FOURTH way a gate narrows a domain and the one that
     reached this file as silence. A `===` that held determined the value and it is in `validValues`; a `==`
     that held determined none — ECMAScript §7.2.13 IsLooselyEqual ( x, y ) coerces — so the engine records the
     PREDICATE instead, and without carrying it a param whose only gate was `x == 0` renders exactly like one
     nothing ever tested while the sibling path's `excludes` carries the same gate's other arm.
     NOTHING HERE RE-IMPLEMENTS `==`. The value and its type are carried, intersected and rendered as the
     transcript they are; §7.2.13's holding set is never computed, which would mean running the page's own
     ToPrimitive for its step 12 arm in a consumer where no page code is running.
     A CALL SITE IS ALWAYS AN OBSERVATION, so the no-key arm passes the EMPTY array rather than returning —
     endpoint.c omits `looselyEquals` exactly where no loose equality held on every observed path to this
     request, so its absence is the positive statement "this run proved nothing here" and the intersection is
     what turns that into the erasure of an earlier path's claim. */
  const _mergeLooselyEquals = (target, p) => {
    if (!("looselyEquals" in p)) { intersectLooselyEquals(target, []); return; }
    DCHECK(Array.isArray(p.looselyEquals) && p.looselyEquals.length > 0,
           "an @H param carries a `looselyEquals` that is not a non-empty array — endpoint.c omits the key " +
           "entirely where no loose equality held on every observed path, so an empty or non-array one here " +
           "is the engine stating a domain it does not have");
    DCHECK(p.looselyEquals.every((q) => q && typeof q === "object" && !Array.isArray(q) &&
                                   typeof q.value === "string" &&
                                   (q.type === "string" || q.type === "number" || q.type === "boolean" ||
                                    q.type === "null" || q.type === "undefined" || q.type === "bigint")),
           "an @H loose equality is not {value:<string>, type:one of string/number/boolean/null/undefined/" +
           "bigint} — endpoint.c writes the operand through json_buf_str and the type through " +
           "concolic_lit_report_name, whose switch is exhaustive over ConcolicLit and aborts on the kindless " +
           "one, so anything else is that producer having changed shape under a reader that would carry the " +
           "wrong half. The value may legitimately be the EMPTY string (`x == \"\"` is a gate a bundle " +
           "writes), which is why its length is not asserted and its type is");
    intersectLooselyEquals(target, p.looselyEquals);
  };

  /* WHERE EACH VALUE LANDED IS THE PRODUCER'S STATEMENT, NEVER THIS FILE'S DEFAULT. endpoint.c writes
     `location` on every param — "path" for a `{hole}` the code interpolated into the address (its example
     value aligned out of the concolic's concrete URL), "query" for the display URL's query string, "body"
     for the fields of the request payload it parsed in the payload's own format. So the three branches are
     a dispatch over a field that is always present, and a spelling outside the three CRASHES here rather
     than silently taking the query arm — which is what `(p.location || "query")` did to all three of them.
     A path param is `required` because it IS the address: the request cannot be replayed without it. */
  const _bodyParams = [];
  for (const p of callSite.params) {
    DCHECK(p && typeof p.name === "string" && Array.isArray(p.validValues) &&
           (p.location === "path" || p.location === "query" || p.location === "body"),
           "an @H param is not {name, location, validValues[]} with a location of path/query/body — " +
           "endpoint.c writes those three keys on EVERY param (the four domain keys below are written only " +
           "where a gate held), and a param that arrives without them takes every learned example value " +
           "out of this method");
    /* AND THAT NO FIFTH DOMAIN ARRIVED THAT THIS FILE READS NOWHERE — the check the four `in` tests above
       cannot make, because each of them asks about a key it already knows. A consumer never defaults a
       producer's field, and an unread key is that defect with the default supplied by the language: the
       record carries the fact, every reader here is silent about it, and the popup renders the parameter
       exactly as it renders one nothing ever tested. §@H calls that a WRONG report and not a thin one, and
       it is the one direction of drift no assert in this file could see — a key REMOVED breaks a reader
       loudly, a key ADDED breaks nothing at all.
       IT IS A DCHECK AND NOT A REFUSAL because of who authored the bytes (§THE-DISCRIMINATOR-IS-WHOSE-BYTES-
       STATE-THE-VALUE): an @H param is this zone's own forced execution talking to itself — lib/endpoint-
       record.js states the same trust for the record around it — so an unknown key is OUR two halves having
       parted, which is precisely what a DCHECK asserts. A third party's document gets a refusal; this does
       not, and must not, because a silent skip is the whole defect.
       IT ASSERTS THE KEY SET AND NEVER A COUNT. A tally is a second copy of a fact that moves, and this file
       and lib/merge.js have each already carried one that was wrong before anything was added to it. What is
       written here is the set this file READS, which is checkable by grepping this function for each name —
       so the list cannot drift from the readers without the drift being the thing that fires. */
    for (const _k of Object.keys(p)) {
      DCHECK(AST_PARAM_KEYS.indexOf(_k) >= 0,
             "an @H param carries the key `" + _k + "`, which nothing in lib/learn.js reads — endpoint.c's " +
             "`endpoint_json_array` and this file are the two halves of one record, so a name on one and " +
             "not the other is a fact the engine proved about this parameter and the popup will render as " +
             "absent. Add the reader (a domain intersects, like `excludes`; a value unions, like " +
             "`validValues`) and name it in AST_PARAM_KEYS — never widen the list alone, which turns the " +
             "crash into the silent drop it exists to prevent");
    }
    if (p.location === "body") { _bodyParams.push(p); continue; }
    if (!m.parameters[p.name]) {
      m.parameters[p.name] = {
        type: _paramType(p),
        location: p.location,
        description: p.location === "path"
          ? "Learned from a forced-execution call site (path template)"
          : "Learned from a forced-execution call site",
        _astInferred: true,
      };
      if (p.location === "path") m.parameters[p.name].required = true;
    }
    /* THE GRADE IS THE SIGHTING'S, NOT THE RECORD'S. `m._astProvenance` above is already the FOLD of every
       sighting that reached this method, so reading it here would grade this call site's values by a claim
       some OTHER call site supports — which is the merge this pool split exists to stop, performed one line
       after it. `callSite.provenance` is what THIS row was reached at, asserted at lib/merge.js's boundary
       before it arrives. */
    _mergeAstValues(m.parameters[p.name], p.validValues, callSite.provenance);
    _mergeExcludes(m.parameters[p.name], p);
    _mergeBounds(m.parameters[p.name], p);
    _mergePredicates(m.parameters[p.name], p);
    _mergeLooselyEquals(m.parameters[p.name], p);
  }

  // Reverse cross-doc reconcile: if THIS method is templated ({hole} path
  // segments), fold any existing CONCRETE same-host live records that match
  // the template into it, then drop the duplicate. Closes the dominant
  // first-load split where live requests (en-us/...) created concrete records
  // BEFORE the deep grind learned the template ({userLocale}/...) — the
  // forward `_matchTemplatedMethodAcrossHost` only catches the other order.
  // Each concrete segment aligned with a {hole} becomes that path-param's
  // example value (goal #2); the concrete method's query params / response /
  // request / stats fold in where the template lacks them. Precise match:
  // same segment count, literal segments equal, same HTTP method, matched
  // method's origin host == this host, and the dup must be concrete at >=1
  // hole (else it IS this template, not a distinct concrete record) — so
  // distinct endpoints are never merged.
  if (typeof m.path === "string" && m.path.indexOf("{") >= 0) {
    const _tSegs = m.path.split("/").filter(Boolean);
    const _hostname = csUrl.hostname;
    for (const [, _de] of tab.discoveryDocs) {
      if (!_de || !_de.doc || !_de.doc.resources) continue;
      for (const _bucket of Object.values(_de.doc.resources)) {
        if (!_bucket || !_bucket.methods) continue;
        for (const _key of Object.keys(_bucket.methods)) {
          const _cm = _bucket.methods[_key];
          if (!_cm || _cm === m || _cm.httpMethod !== m.httpMethod || typeof _cm.path !== "string") continue;
          const _cSegs = _cm.path.split("/").filter(Boolean);
          if (_cSegs.length !== _tSegs.length) continue;
          const _oh = (_cm.origin && URL.canParse(_cm.origin)) ? new URL(_cm.origin).hostname : null;
          if (_oh !== _hostname) continue;
          let _ok = true, _concreteAtHole = false;
          for (let _i = 0; _i < _tSegs.length; _i++) {
            const _isHole = _tSegs[_i].charAt(0) === "{" && _tSegs[_i].slice(-1) === "}";
            const _cHole = _cSegs[_i].charAt(0) === "{" && _cSegs[_i].slice(-1) === "}";
            if (_isHole) { if (!_cHole) _concreteAtHole = true; continue; }
            if (_tSegs[_i] !== _cSegs[_i]) { _ok = false; break; }
          }
          if (!_ok || !_concreteAtHole) continue;
          /* WHAT THE CONCRETE RECORD'S PATH SEGMENTS ARE WORTH — asked ONCE, of the record being dissolved,
             before any of its segments becomes a path-param example on the template.
             THE RECORD'S OWN GRADE IS EXACTLY THE SEGMENT'S. A learned method is keyed by (verb, path), so
             its path IS its identity and every sighting folded into it carried these very segments — which
             makes `methodProvenance`'s fold over those sightings the fold over this segment, and not an
             approximation of it. That identity is why this is the one place a record-level grade may be read
             for a value.
             A RECORD THE ENGINE NEVER GRADED IS NOT THEREFORE OBSERVED, and splitting that absence is the
             half this line did not used to have at all. `null` from `methodProvenance` says only "no bundle
             sighting"; the wire is what makes a concrete address an observation, so a record with request
             stats states `observed` — a real client sent exactly this — and one WITHOUT them is a probed
             DISCOVERY DOCUMENT's declaration, whose concrete segment is a third party's example and not a
             value this run computed at any grade. §@H forbids rendering that as either, so its holes are not
             filled from here; the rest of the reconcile (parameters, response, request, stats) is unchanged,
             because none of those claims that the RUN determined a value. */
          const _cmProv = methodProvenance(_cm, "lib/learn.js templated-path reconcile");
          const _cmFired = !!(_cm._stats && _cm._stats.requestCount);
          if (_cmProv !== null || _cmFired) {
            const _segProv = _cmProv === null ? "observed" : _cmProv;
            for (let _i = 0; _i < _tSegs.length; _i++) {
              if (!(_tSegs[_i].charAt(0) === "{" && _tSegs[_i].slice(-1) === "}")) continue;
              const _val = _cSegs[_i];
              if (_val.charAt(0) === "{") continue;
              const _hole = _tSegs[_i].slice(1, -1);
              if (!_hole) continue;   // generic {} hole -> the ENGINE's shape/concrete collapse owns its path-param
                                      // example (arg{i}); creating m.parameters[""] here made a duplicate empty-name @path param.
              if (!m.parameters[_hole]) m.parameters[_hole] = { type: "string", location: "path", required: true, description: "Learned (concrete value from live traffic)" };
              _mergeAstValues(m.parameters[_hole], [_val], _segProv);
            }
          }
          if (_cm.parameters) for (const _pn in _cm.parameters) { if (!m.parameters[_pn]) m.parameters[_pn] = _cm.parameters[_pn]; }
          if (_cm.response && !m.response) m.response = _cm.response;
          if (_cm.request && !m.request) m.request = _cm.request;
          if (_cm._stats && !m._stats) m._stats = _cm._stats;
          delete _bucket.methods[_key];
        }
      }
    }
  }

  // Record content-type when the forced execution captured one and the method hasn't seen a real
  // request-time content type yet. Real traffic overrides.
  if (callSite.headers !== undefined) {
    const _rh = astHeaderRecord(callSite.headers);
    const ctEntry = _rh["content-type"] || _rh["Content-Type"];
    if (ctEntry && (!m.contentTypes || m.contentTypes.length === 0)) {
      m.contentTypes = [ctEntry.value];
    }
    // Store the full set per-endpoint as transport metadata (NOT body params),
    // so the Send panel can show "this endpoint needs header X". A literal
    // supersedes an earlier opaque for the same header; real traffic refines.
    if (!m.requiredHeaders) m.requiredHeaders = {};
    for (const hk in _rh) {
      const prev = m.requiredHeaders[hk];
      if (!prev || (prev.kind === "opaque" && _rh[hk].kind === "literal")) m.requiredHeaders[hk] = _rh[hk];
    }
  }

  /* THE REQUEST BODY'S FIELDS, AS THE METHOD'S REQUEST SCHEMA — what lib/send.js resolves through
     `m.request.$ref` and what the OpenAPI export writes as `requestBody`. The producer is endpoint.c reading
     the bytes the page composed in the body's own content-type; each field carries the literal the code
     computed, or its `{shape}` where it did not.

     A BINARY body (protobuf / gRPC-Web) reaches no field here: endpoint.c reads JSON and form-urlencoded, so
     a protobuf payload records nothing rather than a guess. The decode belongs beside those bytes when they
     arrive, and lib/protobuf.js is still here to read them with. */
  if (_bodyParams.length) {
    const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Request`;
    DCHECK(doc.schemas && typeof doc.schemas === "object",
           "a doc reached the request-body schema with no `schemas` map — every docEntry this file creates " +
           "carries one and an imported description has one, so an absent map means the body fields have " +
           "nowhere to land and lib/send.js would resolve `m.request.$ref` against nothing");
    if (!doc.schemas[schemaName]) {
      doc.schemas[schemaName] = { id: schemaName, type: "object", properties: {}, _astInferred: true };
    }
    const schema = doc.schemas[schemaName];
    if (!schema.properties) schema.properties = {};
    for (const bp of _bodyParams) {
      if (!schema.properties[bp.name]) {
        schema.properties[bp.name] = { type: _paramType(bp), _astInferred: true };
      }
      /* AT THIS SIGHTING'S GRADE, for the reason the query params carry it: a body field the page POSTs is
         learned on the same path the address was, so it is worth exactly what that path is worth. */
      _mergeAstValues(schema.properties[bp.name], bp.validValues, callSite.provenance);
      /* A BODY FIELD'S DOMAIN IS THE SAME FACT AS A QUERY PARAM'S. endpoint.c reads the request body in the
         body's own format and mints a param per field, so a gate over a value the page then POSTs is observed
         exactly as one over a value it appends to the query is. Leaving it out here would make the report's
         silence mean two different things in two halves of the same record. */
      _mergeExcludes(schema.properties[bp.name], bp);
      /* …and the ordering gate's, for the same reason: a body field the page POSTs is gated by the same
         predicates a query param is, and a projection that carried one fact and not the other would make the
         report's silence mean two different things in two halves of one record. */
      _mergeBounds(schema.properties[bp.name], bp);
      /* …and the call gate's, for the third time and the same reason. A body field the page POSTs is gated by
         `startsWith` exactly as a query param is, and a projection carrying two of the three facts would make
         the report's silence mean two different things in two halves of one record. */
      _mergePredicates(schema.properties[bp.name], bp);
      /* …and the loose equality's, for the fourth time and the same reason. A body field the page POSTs is
         gated by `== 0` exactly as a query param is, and a projection carrying three of the four facts would
         make the report's silence mean two different things in two halves of one record. */
      _mergeLooselyEquals(schema.properties[bp.name], bp);
    }
    if (!m.request) m.request = { $ref: schemaName };
  }

  // Apply example-value picker so the Send form has prefills even
  // before any real traffic hits — pickExampleValue's `ast-constraint`
  // tier uses the _astValidValues we just attached. applyStatsToMethod
  // also walks any body-schema props we created with _astInferred:true.
  applyStatsToMethod(m, doc);

  return { entry: docEntry, method: m };
}

// Find an existing method whose TEMPLATED path matches this concrete request
// path, so network traffic merges into the QuickJS-learned (or earlier
// network-templated) endpoint instead of forking a new per-value method. This
// is what lets QuickJS supply the editable URL structure (/{e}/{a}/…) while the
// network supplies the real example values for those path params: a match routes
// the request to that method, and the path-param value capture below records the
// concrete segment (NDevTK, APIClient) into its stats. Match = same verb, same
// segment count, every non-{…} segment equal, at least one {…} segment. No
// scoring — a literal segment-by-segment structural match.
function _matchTemplatedMethod(learnedMethods, httpMethod, pathname) {
  const segs = pathname.split("/").filter(Boolean);
  if (!segs.length) return null;
  for (const key in learnedMethods) {
    const mm = learnedMethods[key];
    if (!mm || mm.httpMethod !== httpMethod || !mm.path) continue;
    const tps = mm.path.split("/").filter(Boolean);
    if (tps.length !== segs.length) continue;
    let hasTemplate = false, ok = true;
    for (let i = 0; i < tps.length; i++) {
      if (tps[i].startsWith("{") && tps[i].endsWith("}")) { hasTemplate = true; continue; }
      if (tps[i] !== segs[i]) { ok = false; break; }
    }
    if (ok && hasTemplate) return key;
  }
  return null;
}

// Cross-doc template reconcile. Forced-exec may learn an endpoint as a
// TEMPLATED method (e.g. {userLocale}/content-nav/site-header.json) under
// one service grouping, while a concrete live request (en-us/content-nav/
// site-header.json) lands in a DIFFERENT same-host bucket (a shape origin
// and a literal one are named by different rules) — leaving the same
// logical endpoint split into a
// templated [ast] record and a concrete [live] one (observed live on
// learn.microsoft.com: /{userLocale}/content-nav vs /en-us/content-nav).
// `_matchTemplatedMethod` only searches one doc, so it can't bridge the
// split. This searches EVERY same-host doc and returns the doc name whose
// templated method matches this concrete path+method, so learnFromRequest
// can redirect the request into that doc — the concrete segment then lands
// as the param's example value (goal #2) instead of duplicating the
// endpoint. Safe: precise segment match (same count; each segment equal or
// a {hole}), same HTTP method only, and the matched method's origin host
// must equal this host — so distinct services aren't mis-merged.
function _matchTemplatedMethodAcrossHost(tab, hostname, httpMethod, pathname) {
  if (!tab || !tab.discoveryDocs) return null;
  for (const [docName, docEntry] of tab.discoveryDocs) {
    if (!docEntry || !docEntry.doc || !docEntry.doc.resources) continue;
    for (const bucket of Object.values(docEntry.doc.resources)) {
      if (!bucket || !bucket.methods) continue;
      const mk = _matchTemplatedMethod(bucket.methods, httpMethod, pathname);
      if (!mk) continue;
      const mm = bucket.methods[mk];
      const oh = (mm && mm.origin && URL.canParse(mm.origin)) ? new URL(mm.origin).hostname : null;
      if (oh === hostname) return docName;
    }
  }
  return null;
}

function learnFromRequest(documentId, interfaceName, entry, headers) {
  const tab = _docForLearning(documentId);
  const url = new URL(entry.url);
  const method = entry.method;

  // Record WHICH grouping rule fired when this service was first
  // created. Grouping decisions must be traceable to the rule that
  // produced them so reviewers can judge (lib/discovery-entry.js).
  const grouping = classifyInterface(url);
  // Cross-doc template reconcile: if forced-exec already learned this endpoint
  // as a templated method under a DIFFERENT same-host service grouping, route
  // this concrete request into THAT doc so the same logical endpoint isn't
  // split into a templated [ast] record + a concrete [live] one. The
  // subsequent _matchTemplatedMethod (below) then finds the template within
  // the redirected doc and merges, recording the concrete segment as the
  // param example. Only redirects on a precise same-host templated match.
  const _crossHostDoc = _matchTemplatedMethodAcrossHost(tab, url.hostname, method, url.pathname);
  if (_crossHostDoc && _crossHostDoc !== interfaceName) {
    interfaceName = _crossHostDoc;
  }
  // Stamp the resolved name onto the entry so callers (handleResponseBody)
  // can use it for downstream lookups instead of the `service` variable they
  // came in with — the cross-doc template reconcile above may have routed this
  // request into a different bucket entirely.
  entry.interfaceName = interfaceName;
  let docEntry = tab.discoveryDocs.get(interfaceName);
  if (!docEntry || !docEntry.doc) {
    docEntry = {
      status: "found",
      isVirtual: true,
      grouping: makeGroupingRecord(grouping.rule, grouping.matched, entry.url,
                                   "lib/learn.js minting a virtual entry for a live request, service " +
                                   JSON.stringify(interfaceName)),
      doc: {
        kind: "discovery#restDescription",
        name: interfaceName,
        title: `${interfaceName} (Learned)`,
        rootUrl: url.origin + "/",
        baseUrl: url.origin + "/",
        resources: {
          learned: { methods: {} },
        },
        schemas: {},
      },
    };
    tab.discoveryDocs.set(interfaceName, docEntry);
  }

  const doc = docEntry.doc;
  if (!doc.resources.learned) doc.resources.learned = { methods: {} };

  // GraphQL: method name = operationName (or first root field). Every op on
  // a /graphql endpoint is its own method entry. Detect by parsing the
  // request body — we don't rely on URL containing "graphql" alone.
  let _nameHint = null;
  if (entry.rawBodyB64) {
    try {
      const _bodyText = new TextDecoder().decode(base64ToUint8(entry.rawBodyB64));
      const _gql = parseGraphQLRequest(_bodyText);
      if (_gql && _gql.operations && _gql.operations.length > 0) {
        // Batched: name from first op; other ops registered separately would
        // need splitting at the call site — left as one method for now.
        _nameHint = deriveGraphQLMethodName(_gql.operations[0]);
      }
    } catch (e) {
      RETHROW_FATAL(e);
      /* GraphQL operation-name detection failed (body wasn't base64 /
         wasn't text / wasn't valid GraphQL syntax). The endpoint still
         registers under its fallback methodName; only the named
         disambiguation between `gql_GetUser` and `gql_GetRepo` on the
         same /graphql URL is missed. Surface so a GraphQL parser
         regression on a real bundle is visible. */
      console.debug("[brain] GraphQL name-hint derive failed:", e && e.message || e, "url=" + entry.url);
    }
  }

  const { methodName: baseMethodName } = calculateMethodMetadata(url, interfaceName, _nameHint);
  const qualifiedName = method.toLowerCase() + "_" + baseMethodName;

  // If this method was already probed with richer schema, update it there
  // instead — but ONLY when the probed entry's HTTP verb matches. A POST
  // probe entry must not absorb GET traffic (or vice versa); they're
  // distinct methods that happen to share a path. Without the verb match,
  // real GET stats land on the POST schema, corrupting the probed entry.
  const probedBase = doc.resources.probed?.methods?.[baseMethodName];
  const probedMethod = (probedBase && probedBase.httpMethod === method) ? probedBase : null;

  // Resolve method name — disambiguate when different HTTP methods hit the same path
  let methodName;
  // A concrete request that matches an existing TEMPLATED method (QuickJS gave
  // /{e}/{a}/… or earlier traffic templatized it) merges into that method, so
  // its real path-segment values become examples for the editable params.
  const _tplMatch = _matchTemplatedMethod(doc.resources.learned.methods, method, url.pathname);
  const existingBase = doc.resources.learned.methods[baseMethodName];
  const existingQualified = doc.resources.learned.methods[qualifiedName];

  if (_tplMatch) {
    methodName = _tplMatch;
  } else if (existingQualified) {
    // Already disambiguated from a prior collision — use qualified name
    methodName = qualifiedName;
  } else if (existingBase && existingBase.httpMethod !== method) {
    // Collision: different HTTP method to same path — rename existing, qualify new
    const existQualName = existingBase.httpMethod.toLowerCase() + "_" + baseMethodName;
    if (!doc.resources.learned.methods[existQualName]) {
      existingBase.id = `${interfaceName.replace(/\//g, ".")}.${existQualName}`;
      doc.resources.learned.methods[existQualName] = existingBase;
    }
    delete doc.resources.learned.methods[baseMethodName];
    methodName = qualifiedName;
  } else {
    // No collision — use base name
    methodName = baseMethodName;
  }

  const methodId = `${interfaceName.replace(/\//g, ".")}.${methodName}`;
  entry.methodId = methodId;

  if (!doc.resources.learned.methods[methodName] && !probedMethod) {
    doc.resources.learned.methods[methodName] = {
      id: methodId,
      path: _decHoles(url.pathname.substring(1)),
      httpMethod: method,
      parameters: {},
      request: null,
      origin: url.origin,
    };
  }

  const m = probedMethod || doc.resources.learned.methods[methodName];
  if (m && !m.origin) m.origin = url.origin;

  // Learn query parameters from URL
  if (!url.pathname.includes("batchexecute")) {
    url.searchParams.forEach((value, name) => {
      if (name === "key" || name === "api_key") return;
      // $httpHeaders is a gRPC-Web transport mechanism (CRLF-separated headers in URL),
      // not an API parameter. Putting it in a form input strips \r\n and corrupts the URL.
      if (name === "$httpHeaders") return;
      // $ct is a multipart batch Content-Type transport param, not an API parameter.
      if (name === "$ct") return;
      if (!m.parameters[name]) {
        m.parameters[name] = {
          type: isNaN(value) ? "string" : "number",
          location: "query",
          description: "Learned from request",
        };
      }
    });

    // Learn path parameters by comparing URL to stored template AND
    // by detecting ID-like segments on first observation.
    const segments = url.pathname.split("/").filter(Boolean);
    const templateParts = (m.path || "").split("/").filter(Boolean);
    if (templateParts.length === segments.length) {
      let changed = false;
      for (let i = 0; i < segments.length; i++) {
        if (templateParts[i].startsWith("{")) continue; // Already templated
        if (templateParts[i] !== segments[i]) {
          // Segment differs from template — definitely a parameter
          const paramName = `path_${templateParts[i] || "param" + i}`;
          templateParts[i] = `{${paramName}}`;
          if (!m.parameters[paramName]) {
            m.parameters[paramName] = {
              type: "string",
              location: "path",
              description: "Inferred path parameter",
            };
          }
          changed = true;
        }
        // (The `looksLikeDynamicSegment` regex-GUESS branch was deleted: a path segment becomes a {param}
        //  ONLY when it is OBSERVED to vary across requests (data-driven, above), never because a regex thinks
        //  it "looks like an ID". RUN, DON'T MATCH — the engine already marks genuinely-dynamic segments as
        //  {shape} holes from data-flow; a regex guess on a concrete segment merges distinct real endpoints.)
      }
      if (changed) m.path = templateParts.join("/");
    }
  }

  // Record the observed Content-Type on the method for replay fidelity
  if (headers["content-type"]) {
    const ct = headers["content-type"].split(";")[0].trim();
    if (!m.contentTypes) m.contentTypes = [];
    if (!m.contentTypes.includes(ct)) m.contentTypes.unshift(ct);
  }

  // Learn request body if present
  if (entry.rawBodyB64) {
    const bytes = base64ToUint8(entry.rawBodyB64);
    const text = new TextDecoder().decode(bytes);
    const isBatch = url.pathname.includes("batchexecute");

    if (isBatch) {
      const calls = parseBatchExecuteRequest(text);
      if (calls) {
        for (const call of calls) {
          const callMethodId = `${interfaceName.replace(/\//g, ".")}.${call.rpcId}`;
          if (!doc.resources.learned.methods[call.rpcId]) {
            doc.resources.learned.methods[call.rpcId] = {
              id: callMethodId,
              path: _decHoles(url.pathname.substring(1)),
              httpMethod: "POST",
              parameters: {},
              request: null,
            };
          }
          const callM = doc.resources.learned.methods[call.rpcId];
          const schemaName = `${call.rpcId}Request`;
          callM.request = { $ref: schemaName };
          const newSchema = generateSchemaFromJson(
            call.data,
            schemaName,
            doc.schemas,
            true,
          );
          mergeSchemaInto(doc, schemaName, newSchema);
        }
      }
    } else if (isMultipartBatch(headers["content-type"])) {
      // Multipart batch: each part is an individual HTTP sub-request with its own body
      const parts = parseMultipartBatchRequest(text, headers["content-type"]);
      if (parts) {
        for (const part of parts) {
          // Derive method name from part path
          const pathSegs = part.path.split("?")[0].split("/").filter(Boolean)
            .filter((s) => s.length <= 32);   // concrete segments as-is; the engine {shape}s dynamic ones
          const partMethodName = part.method.toLowerCase() + "_" +
            (pathSegs.join("_") || "batch_part");
          const partMethodId = `${interfaceName.replace(/\//g, ".")}.${partMethodName}`;
          if (!doc.resources.learned.methods[partMethodName]) {
            doc.resources.learned.methods[partMethodName] = {
              id: partMethodId,
              path: part.path,
              httpMethod: part.method,
              parameters: {},
              request: null,
              _batchPart: true,
            };
          }
          const partM = doc.resources.learned.methods[partMethodName];
          if (part.body) {
            try {
              const json = JSON.parse(part.body);
              const schemaName = `${partMethodName.replace(/[^a-zA-Z0-9]/g, "")}Request`;
              partM.request = { $ref: schemaName };
              const newSchema = generateSchemaFromJson(json, schemaName, doc.schemas);
              mergeSchemaInto(doc, schemaName, newSchema);
            } catch (e) {
              RETHROW_FATAL(e);
              /* Multipart-batch sub-request JSON parse failed for one
                 part — other parts still process. Surface so a malformed
                 sub-request body on an otherwise-valid batch is visible. */
              console.debug("[brain] multipart-batch request-part JSON parse failed:", e && e.message || e, "part=" + partMethodName, "url=" + entry.url);
            }
          }
        }
      }
    } else if (
      headers["content-type"]?.includes("grpc-web") ||
      headers["content-type"]?.includes("grpc+proto")
    ) {
      // gRPC-Web request body: 5-byte frame header + protobuf payload
      try {
        const parsed = parseGrpcWebFrames(bytes);
        if (parsed) {
          for (const frame of parsed.frames) {
            if (frame.type !== "data") continue;
            const tree = pbDecodeTree(frame.data, 8);
            if (tree && tree.length > 0) {
              const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Request`;
              m.request = { $ref: schemaName };
              const newSchema = generateSchemaFromPbTree(tree, schemaName, doc.schemas);
              mergeSchemaInto(doc, schemaName, newSchema);
            }
          }
        }
      } catch (e) {
        RETHROW_FATAL(e);
        console.debug("[brain] grpc-web request-body decode failed:", e && e.message || e, "url=" + entry.url);
      }
    } else if (headers["content-type"]?.includes("json+protobuf")) {
      // JSPB body — positional array encoding
      try {
        const json = JSON.parse(text);
        if (Array.isArray(json)) {
          const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Request`;
          m.request = { $ref: schemaName };
          const newSchema = generateSchemaFromJson(json, schemaName, doc.schemas, true);
          mergeSchemaInto(doc, schemaName, newSchema);
        }
      } catch (e) {
        RETHROW_FATAL(e);
        console.debug("[brain] JSPB request-body parse failed:", e && e.message || e, "url=" + entry.url);
      }
    } else if (
      headers["content-type"]?.includes("x-protobuf") ||
      headers["content-type"]?.includes("application/protobuf")
    ) {
      // Binary protobuf body
      try {
        const tree = pbDecodeTree(bytes, 8);
        if (tree && tree.length > 0) {
          const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Request`;
          m.request = { $ref: schemaName };
          const newSchema = generateSchemaFromPbTree(tree, schemaName, doc.schemas);
          mergeSchemaInto(doc, schemaName, newSchema);
        }
      } catch (e) {
        RETHROW_FATAL(e);
        console.debug("[brain] x-protobuf request-body decode failed:", e && e.message || e, "url=" + entry.url);
      }
    } else if (headers["content-type"]?.includes("json")) {
      try {
        const json = JSON.parse(text);
        const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Request`;
        m.request = { $ref: schemaName };
        const newSchema = generateSchemaFromJson(json, schemaName, doc.schemas);
        mergeSchemaInto(doc, schemaName, newSchema);
      } catch (e) {
        RETHROW_FATAL(e);
        console.debug("[brain] JSON request-body parse failed:", e && e.message || e, "url=" + entry.url);
      }
    } else if (headers["content-type"]?.includes("x-www-form-urlencoded")) {
      // Form-urlencoded with f.req JSPB (non-batchexecute, e.g. browserinfo)
      try {
        const params = new URLSearchParams(text);
        const fReq = params.get("f.req");
        if (fReq) {
          const json = JSON.parse(fReq);
          if (Array.isArray(json)) {
            const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Request`;
            m.request = { $ref: schemaName };
            const newSchema = generateSchemaFromJson(json, schemaName, doc.schemas, true);
            mergeSchemaInto(doc, schemaName, newSchema);
          }
        }
      } catch (e) {
        RETHROW_FATAL(e);
        console.debug("[brain] form-urlencoded f.req parse failed:", e && e.message || e, "url=" + entry.url);
      }
    } else if (text && /^[\s﻿\x00-\x1f]*[{\[]/.test(text)) {
      // Structural JSON detection for request bodies whose content-type
      // is text/plain, missing, or anything other than the explicit
      // tagged shapes above. Many analytics + telemetry endpoints
      // (reddit /svc/shreddit/events, GitHub error reporters, snowplow
      // collectors) POST JSON with `Content-Type: text/plain` to dodge
      // CORS preflight. Body STRUCTURE is authoritative — same rule
      // already applied to responses at line ~2183. Without this,
      // observed traffic produces 1000+ orphan body-field stats with
      // 0 declared schema fields (verified on reddit.com events: 35
      // requests captured, 1141 orphan fields, 0 attached).
      try {
        const json = JSON.parse(text);
        if (json !== null && typeof json === "object") {
          const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Request`;
          m.request = { $ref: schemaName };
          const newSchema = generateSchemaFromJson(json, schemaName, doc.schemas);
          mergeSchemaInto(doc, schemaName, newSchema);
        }
      } catch (e) {
        RETHROW_FATAL(e);
        /* Structural JSON sniff parse failure — text LOOKED like JSON
           (starts with `{`/`[` after whitespace) but JSON.parse rejected
           it. Most often: a partial body / malformed JSON / a JSON
           prefix followed by binary. Surface so the schema-not-learned
           symptom is traceable to the parse failure. */
        console.debug("[brain] structural-JSON request-body parse failed:", e && e.message || e, "url=" + entry.url);
      }
    }
  }

  // ─── Statistics collection ───────────────────────────────────────────────
  // Only real network traffic reaches here — AST fetch call sites go
  // through learnFromAstCallSite directly (no synthetic entries).
  if (!m._stats) m._stats = { requestCount: 0, params: {}, bodyFields: {} };
  m._stats.requestCount++;

  // Track query param values
  if (!url.pathname.includes("batchexecute")) {
    url.searchParams.forEach((value, name) => {
      if (name === "key" || name === "api_key") return;
      if (name === "$httpHeaders" || name === "$ct") return;
      if (!m._stats.params[name]) m._stats.params[name] = createParamStats();
      updateParamStats(m._stats.params[name], value);
    });
  }

  // Track path param values. The path-param detection block above
  // updates m.path to a templated form like `/posts/{path_param1}` and
  // adds the parameter to m.parameters, but the runtime value of the
  // segment was not being recorded as an observation — leaving the
  // example resolver with only a type-default empty string. Walk the
  // current request's segments alongside the (now-templated) m.path and
  // record each {name} segment's value into the same stats bucket as
  // query params, so pickExampleValue picks the observed-top value.
  {
    const reqSegs = url.pathname.split("/").filter(Boolean);
    const tmplSegs = (m.path || "").split("/").filter(Boolean);
    if (tmplSegs.length === reqSegs.length) {
      for (let i = 0; i < tmplSegs.length; i++) {
        const t = tmplSegs[i];
        if (t.startsWith("{") && t.endsWith("}")) {
          const paramName = t.slice(1, -1);
          if (!paramName) continue;
          if (!m._stats.params[paramName]) m._stats.params[paramName] = createParamStats();
          updateParamStats(m._stats.params[paramName], reqSegs[i]);
        }
      }
    }
  }

  // Track body field values (JSON bodies only)
  if (entry.isJson && entry.decodedBody && typeof entry.decodedBody === "object") {
    const flat = flattenObjectValues(entry.decodedBody);
    for (const [fieldPath, value] of Object.entries(flat)) {
      if (typeof value === "string" || typeof value === "number") {
        if (!m._stats.bodyFields[fieldPath]) m._stats.bodyFields[fieldPath] = createParamStats();
        updateParamStats(m._stats.bodyFields[fieldPath], String(value));
      }
    }
  }

  // Apply stats-derived metadata back to parameters AND body-field schemas
  applyStatsToMethod(m, doc);

  // ─── Chain detection ────────────────────────────────────────────────────
  if (tab._valueIndex) {
    const requestParams = {};
    url.searchParams.forEach((v, k) => { requestParams[k] = v; });
    // Extract body values for chain matching: use JSON body if available
    let chainBody = {};
    if (entry.isJson && entry.decodedBody) {
      chainBody = entry.decodedBody;
    } else if (entry.rawBodyB64) {
      try {
        const _cbText = new TextDecoder().decode(base64ToUint8(entry.rawBodyB64));
        chainBody = JSON.parse(_cbText);
      } catch (e) {
        RETHROW_FATAL(e);
        /* Chain-tracking body parse failed — request body isn't JSON
           (binary protobuf / form-encoded / etc.). chainBody stays
           empty so this request doesn't contribute body-value chain
           links; URL params still flow through findChainLinks above.
           Surface so a chain-detection gap on binary-body endpoints
           is visible. */
        console.debug("[brain] chain-body parse failed:", e && e.message || e, "url=" + entry.url);
      }
    }
    const bodyValues = flattenObjectValues(chainBody);
    const links = findChainLinks(tab._valueIndex, requestParams, bodyValues, methodId);
    if (links.length) {
      m._chains = mergeChainLinks(m._chains, links);
      // Update outgoing chains on source methods
      for (var li = 0; li < links.length; li++) {
        var srcMethod = findMethodInDoc(doc, links[li].sourceMethodId);
        if (srcMethod) {
          if (!srcMethod._chains) srcMethod._chains = { incoming: [], outgoing: [] };
          var outLink = {
            targetMethodId: methodId,
            paramName: links[li].paramName,
            sourceFieldPath: links[li].sourceFieldPath,
            lastSeen: links[li].lastSeen,
          };
          var outDupe = false;
          for (var oi = 0; oi < srcMethod._chains.outgoing.length; oi++) {
            var o = srcMethod._chains.outgoing[oi];
            if (o.targetMethodId === methodId && o.paramName === links[li].paramName && o.sourceFieldPath === links[li].sourceFieldPath) {
              o.observedCount = (o.observedCount || 1) + 1;
              o.lastSeen = links[li].lastSeen;
              outDupe = true;
              break;
            }
          }
          if (!outDupe) {
            outLink.observedCount = 1;
            srcMethod._chains.outgoing.push(outLink);
          }
        }
      }
    }
  }
}

function _applyStatsToField(field, fieldStats, requestCount) {
  if (!field || !fieldStats) return;

  /* TWO USER-OVERRIDE GUARDS ARE GONE, BECAUSE NOTHING SETS EITHER FLAG. `customRequired` and `customEnum`
     read as the pair of `customName` — the flag lib/popup-handlers.js writes when the reviewer RENAMES a
     parameter, which every merge path then honours so an automated re-derivation never clobbers a manual
     edit. But the popup sets `customName` and only `customName`: grep finds no writer for the other two
     anywhere in the extension, the harness or the engine, so both guards were permanently open and the
     automated answer has always won. Deleted rather than left standing, because a guard against a state no
     producer can create is not protection — it is a claim that manual enum/required overrides exist and are
     respected. If they should, the popup gains the same write it already makes for a rename, and the guard
     comes back at that point with a producer behind it. */

  // Required detection
  const reqAnalysis = analyzeRequired(fieldStats, requestCount);
  field.required = reqAnalysis.required;
  field._requiredConfidence = reqAnalysis.confidence;

  // Enum detection
  const enumAnalysis = analyzeEnum(fieldStats);
  if (enumAnalysis.isEnum) {
    field.enum = enumAnalysis.values;
    field._detectedEnum = true;
  }

  // Default detection
  const defaultAnalysis = analyzeDefault(fieldStats);
  if (defaultAnalysis.hasDefault) {
    field._defaultValue = defaultAnalysis.value;
    field._defaultConfidence = defaultAnalysis.confidence;
  }

  // Type narrowing from format hints
  const narrowedFormat = analyzeFormat(fieldStats);
  if (narrowedFormat && field.type === "string") {
    field.format = narrowedFormat;
  }

  // Numeric range
  const range = analyzeRange(fieldStats);
  if (range) field._range = range;
}

// Aggregate stats across all array-index variants matching the schema's
// path pattern: schema uses `info[].source`, stats are keyed
// `info[0].source`, `info[1].source`, etc (per flattenObjectValues).
// Walking the schema with `[]` placeholders, we collect the per-index
// stats keys that match the pattern and merge their counters into a
// single virtual stats bucket so pickExampleValue + dominance reporting
// see the full population, not a single index slice.
function _aggregateStatsForSchemaPath(bodyFieldStats, schemaPath) {
  // Build a regex from the schema path: "info[].source" → /^info\[\d+\]\.source$/
  // First swap the `[]` placeholders for a sentinel that won't be touched
  // by regex escaping; escape the rest; then swap the sentinel back to
  // the index pattern. No regex from user input — schemaPath is built
  // by the walker from controlled property names + literal `[]` markers.
  const SENTINEL = "\0ARR\0";
  const escaped = schemaPath
    .split("[]").join(SENTINEL)
    .replace(/[.*+?^${}()|[\]\\]/g, "\\$&")
    .split(SENTINEL).join("\\[\\d+\\]");
  const re = new RegExp("^" + escaped + "$");
  let merged = null;
  for (const key of Object.keys(bodyFieldStats)) {
    if (!re.test(key)) continue;
    const fs = bodyFieldStats[key];
    if (!fs) continue;
    if (!merged) {
      merged = createParamStats();
    }
    // Sum scalar counters
    merged.observed = (merged.observed || 0) + (fs.observed || 0);
    merged.empty = (merged.empty || 0) + (fs.empty || 0);
    merged.absent = (merged.absent || 0) + (fs.absent || 0);
    // Merge value frequency map
    if (fs.values) {
      if (!merged.values) merged.values = {};
      for (const v in fs.values) {
        merged.values[v] = (merged.values[v] || 0) + fs.values[v];
      }
    }
    // Track type observations
    if (fs.types) {
      if (!merged.types) merged.types = {};
      for (const t in fs.types) {
        merged.types[t] = (merged.types[t] || 0) + fs.types[t];
      }
    }
  }
  return merged;
}

// Walk a request schema's property tree, resolving $refs, and attach
// body-field stats at each dot-path that matches a stats.bodyFields
// entry. Stops when the schema ref has already been visited to avoid
// cycles on self-referential message types.
function _applyBodyFieldStats(m, doc, bodyFieldStats, requestCount) {
  if (!m.request || !m.request.$ref || !doc || !doc.schemas) return;
  function walk(sch, prefix, visited) {
    if (!sch || !sch.properties) return;
    for (const [k, def] of Object.entries(sch.properties)) {
      const dotPath = prefix ? prefix + "." + k : k;
      // Direct match: stats keyed exactly by dotPath (no array indices).
      // Aggregated match: stats keyed by index variants of an array-path
      // (info[0].x, info[1].x, ...) — merge into one virtual bucket.
      const fs = bodyFieldStats[dotPath] ||
        (dotPath.indexOf("[]") >= 0 ? _aggregateStatsForSchemaPath(bodyFieldStats, dotPath) : null);
      if (fs) {
        _applyStatsToField(def, fs, requestCount);
        // Example value + provenance on the field def so the form
        // renderer can prefill without a second pass.
        const ex = pickExampleValue(def, fs);
        if (ex) {
          def._exampleValue = ex.value;
          def._exampleValueSource = ex.source;
          if (ex.confidence != null) def._exampleConfidence = ex.confidence;
        } else {
          delete def._exampleValue;
          delete def._exampleValueSource;
          delete def._exampleConfidence;
        }
      } else {
        const ex = pickExampleValue(def, null);
        if (ex) {
          def._exampleValue = ex.value;
          def._exampleValueSource = ex.source;
        } else {
          delete def._exampleValue;
          delete def._exampleValueSource;
        }
      }
      // Recurse into sub-schemas. Track visited refs per-walk to prevent
      // infinite loops on recursive schemas (e.g. tree-shaped messages).
      if (def.$ref && !visited.has(def.$ref)) {
        visited.add(def.$ref);
        walk(doc.schemas[def.$ref], dotPath, visited);
        visited.delete(def.$ref);
      } else if (def.type === "array" && def.items && def.items.$ref && !visited.has(def.items.$ref)) {
        visited.add(def.items.$ref);
        walk(doc.schemas[def.items.$ref], dotPath + "[]", visited);
        visited.delete(def.items.$ref);
      } else if (def.type === "array" && def.items && def.items.properties) {
        // Inline array items (no $ref) — recurse into items.properties
        // directly. Without this, observed paths like `info[0].source`
        // never reach the schema's array-item field declarations and
        // every nested field stays orphaned. Verified on reddit
        // /svc/shreddit/events: 1141 orphan paths under info[0].* .
        walk(def.items, dotPath + "[]", visited);
      } else if (def.properties) {
        walk(def, dotPath, visited);
      }
    }
  }
  const rootSchema = doc.schemas[m.request.$ref];
  if (!rootSchema) return;
  walk(rootSchema, "", new Set([m.request.$ref]));
}

function applyStatsToMethod(m, doc) {
  // Runs for:
  //   - Real-traffic methods (m._stats populated by learnFromRequest).
  //   - AST-only methods (m._stats may be empty; m.parameters populated
  //     by learnFromAstCallSite with _astValidValues / _astInferred).
  // pickExampleValue naturally handles both: if paramStats is null/empty
  // and the field has _astValidValues, the `ast-constraint` tier wins;
  // otherwise it falls through to enum/format/type-default.
  const stats = m._stats || { requestCount: 0, params: {}, bodyFields: {} };

  // Observed-stats pass: fires analyzer metadata (required/enum/default/
  // format/range) for params we have counts on.
  for (const [name, paramStats] of Object.entries(stats.params || {})) {
    if (!m.parameters[name]) continue;
    _applyStatsToField(m.parameters[name], paramStats, stats.requestCount);
  }

  // Example value pass: cover EVERY declared parameter, real-observed
  // or AST-only. Previously AST-only params got no _exampleValue
  // because we iterated stats.params (which didn't include them) and
  // the Send form showed empty inputs forever until real traffic hit.
  for (const [name, param] of Object.entries(m.parameters || {})) {
    const paramStats = (stats.params || {})[name] || null;
    const ex = pickExampleValue(param, paramStats);
    if (ex) {
      param._exampleValue = ex.value;
      param._exampleValueSource = ex.source;
      if (ex.confidence != null) param._exampleConfidence = ex.confidence;
      else delete param._exampleConfidence;
    } else {
      delete param._exampleValue;
      delete param._exampleValueSource;
      delete param._exampleConfidence;
    }
  }

  // Body fields: the schema tree lives in doc.schemas — walk it and
  // attach stats + example values by dot-path. Without this, popup
  // rendering knew which FIELDS existed but not what values to prefill.
  if (doc && stats.bodyFields) {
    _applyBodyFieldStats(m, doc, stats.bodyFields, stats.requestCount);
  }

  // Correlations
  stats.correlations = detectCorrelations(stats);
}

function findMethodInDoc(doc, methodId) {
  if (!doc || !doc.resources) return null;
  for (const rKey of Object.keys(doc.resources)) {
    var methods = doc.resources[rKey]?.methods;
    if (methods) {
      for (var mKey in methods) {
        if (methods[mKey].id === methodId) return methods[mKey];
      }
    }
  }
  return null;
}

function learnFromResponse(documentId, interfaceName, entry) {
  if (!entry.responseBody) return;

  const tab = _docForLearning(documentId);
  const url = new URL(entry.url);
  const { methodName } = calculateMethodMetadata(url, interfaceName);
  // Check tab-level first, then fall back to globalStore (survives SW restarts)
  let docEntry = tab.discoveryDocs.get(interfaceName);
  if (!docEntry?.doc) {
    const globalEntry = globalStore.discoveryDocs.get(interfaceName);
    if (globalEntry?.doc) {
      docEntry = globalEntry;
      // Also set on tab so subsequent lookups are fast
      tab.discoveryDocs.set(interfaceName, docEntry);
    }
  }
  if (!docEntry || !docEntry.doc) return;
  const doc = docEntry.doc;
  // Find method — try base name first, then HTTP-qualified name (from disambiguation)
  const qualifiedName = entry.method ? entry.method.toLowerCase() + "_" + methodName : null;
  const learned = doc.resources.learned?.methods;
  const m = learned
    ? (learned[methodName] || (qualifiedName ? learned[qualifiedName] : null))
    : null;
  // Also check probed methods
  const proM = doc.resources.probed
    ? doc.resources.probed.methods[methodName]
    : null;
  const targetM = m || proM;
  if (!targetM) return;

  // Decode base64 to text for JSON/Batch parsing
  let textBody = entry.responseBody;
  if (entry.responseBase64) {
    try {
      const bytes = base64ToUint8(entry.responseBody);
      textBody = new TextDecoder().decode(bytes);
    } catch (e) {
      RETHROW_FATAL(e);
      textBody = null;
    }
  }
  if (!textBody) return;

  /* Magic-byte mimeType fallback. Servers commonly omit/genericize the
     Content-Type on binary responses (application/octet-stream, or no
     header at all); the existing isGrpcWeb/isSSE/isNDJSON dispatch
     below is string-on-mimeType, so an unmarked gRPC-Web frame or
     protobuf body falls through to the generic JSON path and the
     schema/example-value extraction never runs. Per CLAUDE.md #7
     (magic-byte sniff + content-type, never URL-suffix), peek at the
     leading bytes and synthesize a mimeType when the server didn't
     supply one. Real wire-format signatures (gRPC frame, protobuf
     varint tag, gzip/zlib magic) come straight from the spec.  */
  /* `_pushGlobalLog` asserts `mimeType` is a string on every record with a response half, so this is the
     SERVER'S ANSWER and the empty string is the server having stated no type — which is exactly the case the
     sniff below exists for. A `|| ""` here said the same thing about a record that had simply not written
     one, and those are different facts: one is a server to sniff around, the other is a producer to fix. */
  let mimeType = entry.mimeType;
  if (!mimeType || /^application\/octet-stream(?:$|;)/i.test(mimeType)) {
    let _sniffBytes = null;
    if (entry.responseBase64) {
      try { _sniffBytes = base64ToUint8(entry.responseBody); }
      catch (e) { RETHROW_FATAL(e); console.warn("[brain] mime-sniff base64 decode failed:", e && e.message || e, entry.url); }
    } else if (typeof entry.responseBody === "string") {
      try { _sniffBytes = new TextEncoder().encode(entry.responseBody); }
      catch (e) { RETHROW_FATAL(e); console.warn("[brain] mime-sniff encode failed:", e && e.message || e, entry.url); }
    }
    if (_sniffBytes && _sniffBytes.length >= 1) {
      const b0 = _sniffBytes[0];
      const n = _sniffBytes.length;
      // gRPC-Web frame: flag byte (0x00 uncompressed / 0x01 compressed)
      // + 4-byte BE payload length matching the remaining bytes
      if (n >= 5 && (b0 === 0x00 || b0 === 0x01)) {
        const declared = (_sniffBytes[1] << 24) | (_sniffBytes[2] << 16) | (_sniffBytes[3] << 8) | _sniffBytes[4];
        if (declared === n - 5) mimeType = "application/grpc-web+proto";
      }
      // gzip magic 1f 8b — the brain doesn't decompress here, but tagging
      // the mimeType means downstream sees a real content-encoding rather
      // than treating gzip bytes as text and corrupting the schema.
      if (!mimeType && n >= 2 && b0 === 0x1f && _sniffBytes[1] === 0x8b) mimeType = "application/gzip";
      // zlib magic 78 da | 78 9c | 78 01
      if (!mimeType && n >= 2 && b0 === 0x78 && (_sniffBytes[1] === 0xda || _sniffBytes[1] === 0x9c || _sniffBytes[1] === 0x01)) mimeType = "application/zlib";
      // Protobuf varint tag — first byte's low 3 bits ∈ {0,1,2,5}
      // (valid wire types) and field number > 0. Apply only when the
      // body decidedly isn't JSON (first byte not whitespace / { / [ /
      // " / digit / true|false|null start), since JSON is the
      // overwhelming default.
      if (!mimeType && (b0 !== 0x7b && b0 !== 0x5b && b0 !== 0x22 &&
                        !(b0 >= 0x30 && b0 <= 0x39) &&
                        b0 !== 0x74 && b0 !== 0x66 && b0 !== 0x6e &&
                        b0 !== 0x20 && b0 !== 0x09 && b0 !== 0x0a && b0 !== 0x0d)) {
        const wireType = b0 & 0x07;
        const fieldNum = (b0 & 0x78) >> 3;
        if ((wireType === 0 || wireType === 1 || wireType === 2 || wireType === 5) && fieldNum > 0) {
          mimeType = "application/x-protobuf";
        }
      }
    }
  }
  if (isAsyncChunkedResponse(textBody)) {
    const chunks = parseAsyncChunkedResponse(textBody);
    if (chunks) {
      if (!doc.resources.learned) doc.resources.learned = { methods: {} };
      // Use endpoint path as the method key (e.g. "hpba" from /async/hpba)
      const asyncPath = url.pathname.split("/").filter(Boolean).pop() || methodName;
      for (let i = 0; i < chunks.length; i++) {
        const chunk = chunks[i];
        if (chunk.type !== "jspb" || !Array.isArray(chunk.data)) continue;

        const chunkKey = `${asyncPath}_chunk${i}`;
        let callM =
          doc.resources.learned.methods[chunkKey] ||
          doc.resources.probed?.methods[chunkKey];
        if (!callM) {
          doc.resources.learned.methods[chunkKey] = {
            id: `${interfaceName.replace(/\//g, ".")}.${chunkKey}`,
            path: _decHoles(url.pathname.substring(1)),
            /* THE VERB IS THE RECORD'S, AND THE PROOF THAT IT ALWAYS WAS IS THAT THE TWO DEFAULTS DISAGREED.
               This read carried `|| "GET"` and its twin in the multipart-part builder below carried
               `|| "POST"` — one field, one producer, two different substitutes, which cannot both be the
               right answer for an absent value and is what a dead default looks like from the outside.
               `_pushGlobalLog` asserts `method` is a non-empty string on every record it files, so the
               discovery method now records the verb the request was actually made with. */
            httpMethod: entry.method,
            parameters: {},
            request: null,
            response: null,
          };
          callM = doc.resources.learned.methods[chunkKey];
        }

        const schemaName = `${chunkKey}Response`;
        callM.response = { $ref: schemaName };
        const newSchema = generateSchemaFromJson(
          chunk.data,
          schemaName,
          doc.schemas,
          true,
        );
        mergeSchemaInto(doc, schemaName, newSchema);
      }
    }
  } else if (isBatchExecuteResponse(textBody)) {
    const results = parseBatchExecuteResponse(textBody);
    if (results) {
      if (!doc.resources.learned) doc.resources.learned = { methods: {} };
      for (const res of results) {
        let callM =
          doc.resources.learned.methods[res.rpcId] ||
          doc.resources.probed?.methods[res.rpcId];
        // Create method entry if response arrived before request was learned
        if (!callM) {
          doc.resources.learned.methods[res.rpcId] = {
            id: `${interfaceName.replace(/\//g, ".")}.${res.rpcId}`,
            path: _decHoles(url.pathname.substring(1)),
            httpMethod: "POST",
            parameters: {},
            request: null,
            response: null,
          };
          callM = doc.resources.learned.methods[res.rpcId];
        }

        const schemaName = `${res.rpcId}Response`;
        callM.response = { $ref: schemaName };
        const newSchema = generateSchemaFromJson(
          res.data,
          schemaName,
          doc.schemas,
          true,
        );
        mergeSchemaInto(doc, schemaName, newSchema);
      }
    }
  } else if (isGrpcWeb(mimeType)) {
    // gRPC-Web: unwrap frames, decode protobuf payload
    try {
      let bytes;
      if (isGrpcWebText(mimeType)) {
        // grpc-web-text uses base64 encoding
        bytes = base64ToUint8(
          entry.responseBase64 ? entry.responseBody : btoa(entry.responseBody),
        );
      } else {
        bytes = entry.responseBase64
          ? base64ToUint8(entry.responseBody)
          : new TextEncoder().encode(entry.responseBody);
      }
      const parsed = parseGrpcWebFrames(bytes);
      if (parsed) {
        for (const frame of parsed.frames) {
          if (frame.type !== "data") continue;
          const tree = pbDecodeTree(frame.data, 8, (val) => {
            if (typeof val === "string") {
              extractKeysFromText(documentId, val, entry.url, "response_grpc");
            }
          });
          const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Response`;
          targetM.response = { $ref: schemaName };
          const newSchema = generateSchemaFromPbTree(
            tree,
            schemaName,
            doc.schemas,
          );
          mergeSchemaInto(doc, schemaName, newSchema);
        }
      }
    } catch (e) {
      RETHROW_FATAL(e);
      /* gRPC-Web frame decode failed — the schema for this endpoint
         won't be learned from THIS response, the next captured response
         from the same URL may decode correctly. Surface so a real
         malformed-frame symptom (server-side bug or wrong protocol
         classification) is visible instead of disappearing into an
         empty schema. */
      console.debug("[brain] grpc-web frame decode failed:", e && e.message || e, "url=" + entry.url);
    }
  } else if (isSSE(mimeType)) {
    // Server-Sent Events: learn schema from JSON data payloads
    try {
      const events = parseSSE(textBody);
      if (events) {
        for (const evt of events) {
          if (typeof evt.data === "object" && evt.data !== null) {
            const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Event`;
            targetM.response = { $ref: schemaName };
            const newSchema = generateSchemaFromJson(
              evt.data,
              schemaName,
              doc.schemas,
            );
            mergeSchemaInto(doc, schemaName, newSchema);
            break; // Schema from first JSON event is representative
          }
        }
      }
    } catch (e) {
      RETHROW_FATAL(e);
      /* SSE parse failed — malformed event stream (server sent
         data without `data: ` prefix, missing terminator, etc.).
         Surface so the schema-learning skip is observable. */
      console.debug("[brain] SSE parse failed:", e && e.message || e, "url=" + entry.url);
    }
  } else if (isNDJSON(mimeType)) {
    // NDJSON: learn schema from first object
    try {
      const objects = parseNDJSON(textBody);
      if (objects && objects.length > 0) {
        const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Response`;
        targetM.response = { $ref: schemaName };
        const newSchema = generateSchemaFromJson(
          objects[0],
          schemaName,
          doc.schemas,
        );
        mergeSchemaInto(doc, schemaName, newSchema);
      }
    } catch (e) {
      RETHROW_FATAL(e);
      console.debug("[brain] NDJSON parse failed:", e && e.message || e, "url=" + entry.url);
    }
  } else if (isMultipartBatch(mimeType)) {
    // Multipart batch: learn schema from each part's body
    try {
      const parts = parseMultipartBatch(textBody, mimeType);
      if (parts) {
        for (let i = 0; i < parts.length; i++) {
          const part = parts[i];
          if (!part.body) continue;
          try {
            const json = JSON.parse(part.body);
            const partKey = `${methodName}_part${i}`;
            if (!doc.resources.learned) doc.resources.learned = { methods: {} };
            let partM = doc.resources.learned.methods[partKey];
            if (!partM) {
              doc.resources.learned.methods[partKey] = {
                id: `${interfaceName.replace(/\//g, ".")}.${partKey}`,
                path: _decHoles(url.pathname.substring(1)),
                httpMethod: entry.method,
                parameters: {},
                request: null,
                response: null,
              };
              partM = doc.resources.learned.methods[partKey];
            }
            const schemaName = `${partKey}Response`;
            partM.response = { $ref: schemaName };
            const newSchema = generateSchemaFromJson(
              json,
              schemaName,
              doc.schemas,
            );
            mergeSchemaInto(doc, schemaName, newSchema);
          } catch (e) {
            RETHROW_FATAL(e);
            /* One part's JSON parse failed — rest of the batch still
               processes. Surface so a malformed part on an otherwise-
               valid batch response is visible. */
            console.debug("[brain] multipart part JSON parse failed:", e && e.message || e, "partIdx=" + i, "url=" + entry.url);
          }
        }
      }
    } catch (e) {
      RETHROW_FATAL(e);
      console.debug("[brain] multipart batch parse failed:", e && e.message || e, "url=" + entry.url);
    }
  /* THE `text/x-component` BRANCH IS GONE TO engine/host/solver/reply_decode.c. It parsed a React Flight
     stream here and did two things with it: registered every client reference's chunk as an endpoint, and
     merged each json row's shape into a synthesized response schema. The FIRST is the @H surface, and the
     engine now learns it at engine_provide — the one point every reply crosses once — from the reply's
     COMPUTED MIME type rather than from `looksLikeRSC`, a two-line regex over the body that fired whenever the
     Content-Type was empty and the payload looked like a JSON object keyed by digits (§RUN, DON'T MATCH). The
     SECOND is this file's own moat aggregation, and it STAYS: it reads bodies intercept.js captured off the
     LIVE page, which the engine never fetched and holds no reply record for, so it is a different input and
     not a duplicated algorithm. (This cited "jsaudit step 4" — a position in a derived queue, in a gate that
     is now deleted. A number nothing can be checked against is the stale-`DFAIL` shape exactly, which is why
     the rule was that nothing outside that file could cite one; three comments cited one anyway.) */
  } else if (isGraphQLUrl(url.href) && mimeType.includes("json")) {
    // GraphQL response: extract data/errors structure
    try {
      const gqlResp = parseGraphQLResponse(textBody);
      if (gqlResp) {
        for (const r of gqlResp.results) {
          if (r.data) {
            const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Response`;
            targetM.response = { $ref: schemaName };
            const newSchema = generateSchemaFromJson(
              r.data,
              schemaName,
              doc.schemas,
            );
            mergeSchemaInto(doc, schemaName, newSchema);
          }
        }
      }
    } catch (e) {
      RETHROW_FATAL(e);
      console.debug("[brain] GraphQL response parse failed:", e && e.message || e, "url=" + entry.url);
    }
  } else if (mimeType.includes("json") || mimeType.includes("javascript") ||
             /^[\s﻿\x00-\x1f]*[{\[]/.test(textBody)) {
    // JSON or JSONP (callback-wrapped JSON returned as text/javascript) OR
    // body whose first non-whitespace byte is `{` or `[`. Many APIs return
    // JSON under text/plain or no content-type (analytics endpoints,
    // Cloudflare-fronted services, etc.); gating on mimetype alone misses
    // them. Body STRUCTURE is authoritative — if it parses as a JSON
    // object/array, learn the schema; otherwise the catch silently drops.
    try {
      var _lrText = textBody;
      if (!mimeType.includes("json") && (mimeType.includes("javascript") || mimeType === "")) {
        var _lrJsonp = stripJsonp(textBody);
        if (_lrJsonp) _lrText = _lrJsonp;
      }
      // Strip Google XSSI prefix if present. Many Google (and now GitLab
      // snowplow, others) endpoints prepend `)]}'\n` to prevent <script>
      // JSON hijacking. JSON.parse would fail without this.
      if (_lrText.startsWith(")]}'")) _lrText = _lrText.replace(/^\)\]\}'[\r\n]*/, "");
      // Plain "ok" / "OK" confirmations aren't JSON — avoid learning a
      // schema from them.
      if (/^(ok|OK|true|false|null)\s*$/.test(_lrText)) throw new Error("non-object response");
      const json = JSON.parse(_lrText);
      // Only learn from object/array roots — bare strings/numbers/bool
      // don't carry a useful schema and would clutter the doc.
      if (json === null || (typeof json !== "object")) throw new Error("non-object root");
      const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Response`;
      targetM.response = { $ref: schemaName };
      const newSchema = generateSchemaFromJson(json, schemaName, doc.schemas);
      mergeSchemaInto(doc, schemaName, newSchema);
    } catch (e) {
      RETHROW_FATAL(e);
      /* JSON/JSONP response parse failed — common when the body is
         truncated, has a JSONP callback we couldn't strip, isn't valid
         JSON, or is just `ok`/`true`/etc. (the explicit throw above).
         Surface so a schema-not-learned symptom traces to the parse
         step rather than disappearing. */
      console.debug("[brain] JSON/JSONP response parse failed:", e && e.message || e, "url=" + entry.url);
    }
  } else if (
    mimeType.includes("protobuf") ||
    entry.contentType.includes("protobuf") ||
    mimeType.includes("octet-stream") ||
    entry.contentType.includes("octet-stream")
  ) {
    // Decode response protobuf heuristically
    try {
      const bytes = entry.responseBase64
        ? base64ToUint8(entry.responseBody)
        : new TextEncoder().encode(entry.responseBody);
      const tree = pbDecodeTree(bytes, 8, (val) => {
        if (typeof val === "string") {
          extractKeysFromText(documentId, val, entry.url, "response_protobuf");
        }
      });
      const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Response`;
      targetM.response = { $ref: schemaName };
      const newSchema = generateSchemaFromPbTree(tree, schemaName, doc.schemas);
      mergeSchemaInto(doc, schemaName, newSchema);
    } catch (e) {
      RETHROW_FATAL(e);
      /* Protobuf response decode failed — body bytes didn't decode as
         valid wire format (might be a mis-classified text body, a
         compressed payload the brain didn't decompress, or a truncated
         response). Surface so the schema-not-learned symptom is
         traceable. */
      console.debug("[brain] protobuf response decode failed:", e && e.message || e, "url=" + entry.url);
    }
  }

  // ─── Chain value indexing ─────────────────────────────────────────────────
  // Index response values so subsequent requests can detect chains
  if (tab._valueIndex && textBody) {
    const methodId = targetM.id || `${interfaceName.replace(/\//g, ".")}.${methodName}`;
    try {
      var _ciText = stripJsonp(textBody) || textBody;
      const parsed = JSON.parse(_ciText);
      indexResponseValues(tab._valueIndex, parsed, methodId);
    } catch (e) {
      RETHROW_FATAL(e);
      // Not JSON/JSONP — index the raw text body if it looks like a useful value
      if (textBody.length >= 4 && textBody.length <= 500) {
        indexResponseValues(tab._valueIndex, textBody, methodId);
      }
    }
  }
}

// (Schema inference -- generateSchemaFromPbTree/Json, inferJsonType, inferRepeatedItemType,
//  mergeSchemaInto -- extracted to lib/schema.js, loaded first. One problem per file.)
// (The page-context fetch binders -- makePageGetFn/makePageFetchFn -- live in offscreen-brain.js beside the
//  relay entries they wrap. A docblock for them survived here after the move, describing a function this file
//  no longer has; prose that outlives its mechanism reads as a gap where there is none.)
