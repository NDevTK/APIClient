// lib/merge.js — Engine-result -> doc-model merge. Takes the engine's @RESULT (fetch call-sites, computed
// values, security sinks, param constraints) and merges it into a tab's Value-Driven-Discovery doc model
// (methods, params, schemas, findings). Extracted from the offscreen-brain.js monolith (one problem per file);
// loaded before it, resolves learnFromAstCallSite (lib/learn.js) + grouping/schema at call-time.

/* ONE POOL OF A METHOD'S TEMPLATED-HOLE EXAMPLES, PROJECTED ONTO THE FLAT ENDPOINT RECORD. Called once per
   pool, with the METHOD PARAMETER'S OWN FIELD NAME, so the two projections cannot differ in anything but
   which pool they read — a second hand-written loop is how one of them comes to skip a hole the other keeps,
   and the difference would be invisible: both answers are well-formed lists of real values.
   `undefined`-or-array IS THE METHOD PARAMETER'S VOCABULARY (lib/learn.js writes each pool's key only where
   the pool is non-empty), and it is normalised HERE, at the one hop between that record and this one, rather
   than by the fold — which is `foldValuePools`' own stated boundary: a record's spelling of an empty pool
   belongs to the record.
   A HOLE WITH AN EMPTY POOL IS SKIPPED RATHER THAN CARRIED AS `[]`, because lib/send.js requires every entry
   to carry a value: an entry with none would surface a templated segment as if something had filled it, which
   is the claim `pathParams: null` exists to be able to deny. */
function _astPathParamPool(method, poolField) {
  DCHECK(poolField === "_astValidValues" || poolField === "_astForcedValues",
         "a path-param pool was projected from `" + poolField + "`, which is neither of lib/learn.js's two " +
         "pools — the offerable/forced split is spelled as those two field names (lib/endpoint-record.js's " +
         "`provenanceOffersExample` states why it is a name and not a grade), so a third one reads a field " +
         "the method record does not have and every hole would project as empty");
  if (!method || !method.parameters) return null;   // dynamic URL: no method, no path template
  var out = [];
  for (var pn in method.parameters) {
    var pd = method.parameters[pn];
    if (!pd || pd.location !== "path") continue;
    var vals = Array.isArray(pd[poolField]) ? pd[poolField] : [];
    if (!vals.length) continue;
    out.push({ name: pn, values: vals.slice(0, PATH_PARAM_EXAMPLE_CAP) });
  }
  return out.length ? out : null;
}

/* TWO PARAMETERS ARE GONE AND NEITHER WAS EVER READ. `tabId` was passed by every caller and used by no line
   in this file — the DocView it merges into carries its own `tabId`, so the argument was a second copy of a
   fact the first argument already states, and a caller passing the wrong one would have been believed by
   nothing. `isPartial` was worse: the incremental merge passed `true` and this function had no idea what a
   partial was, so the one caller that tried to label its record as a snapshot was talking to nobody. The
   label is real now and travels ON the record, as `_run`, where every consumer can read it. */
function mergeASTResultsIntoVDD(tab, results) {
  for (var r = 0; r < results.length; r++) {
    var analysis = results[r];

    /* A RECORD WITH NO ENGINE DOCUMENT IS NOT A MERGEABLE OBSERVATION, AND ITS ABSENCE IS WHAT SAYS SO.
       bridge.js writes `fetchCallSites`/`securitySinks`/`_park` exactly where an @RESULT document arrived and
       was asserted field for field; a crashed instance and a page with nothing to run carry none of them, and
       that absence is a POSITIVE statement ("this run observed nothing") rather than a hole to fill. Read as
       one here: the two passes below would otherwise walk arrays that are not there — and before the record
       became present-or-absent they walked bridge.js's fabricated empties instead, which is the same merge
       with the fabrication one file upstream. The eviction sweep at the bottom still runs, because a run that
       died is still a run that ENDED and this document's transient RAM view is still reclaimable. */
    if (!analysisHasDocument(analysis)) continue;

    /* NO probeResults RELAY OFF THE ENGINE RESULT. `tab.probeResults` is written by the two systems that
       actually probe — lib/req2proto.js through lib/discovery-probe.js and lib/response-decode.js — and the
       engine has no such record to relay: it never issues a request, so it never receives a rejection to read
       a schema out of. The relay that stood here (and the DCHECK on the seam in bridge.js) asserted a field
       the engine emitted, which made an engine that had learned nothing indistinguishable from a broken
       bridge. */

    /* WHAT AN ANALYSIS DOCUMENT ACTUALLY CONTAINS, AND WHY THREE MERGE PASSES ARE GONE. solver/result.c
       composes the engine's one @RESULT document in ONE snprintf and it has TWELVE fields: fetchCallSites,
       securitySinks, pageErrors, the eight cost counters, and _park. Every OTHER name on the object this
       function receives is a host-side constant written by bridge.js's `linesToAnalysis` — its own comment
       calls them "sibling fields the brain reads unconditionally, present + empty so it never throws".

       So `protoFieldMaps`, `protoEnums` and `sourceMapTypes` were three merge passes reading `[]` on every
       run this project has ever done, and `domEndpoints` was a fourth. They are the defect class in its
       purest form: not a wrong value but a plausible one — a `.length` of zero is what a page with no proto
       schema and no source map ALSO looks like, so 170 lines of matching machinery reported "0 matched, 0
       unmatched" forever and nothing could tell that apart from working. Deleted rather than DCHECK'd,
       because a capability's absence belongs in the engine that does not have it, not in a consumer standing
       ready for output that has no producer: field-number/enum/TypeScript enrichment and a DOM-attribute
       endpoint scan are things `engine/host/solver/endpoint.c` would have to emit before anything here can
       merge them, and when it does, the merge is written against what it emits.

       Their one live consequence went with them: the discovery-doc lookup that opened this loop existed ONLY
       to give those passes a `doc.schemas` to walk. Endpoint registration below never used it.

       (Bundle-wide `valueConstraints` was already NOT merged, and for a second reason worth keeping: matching
       constraints to params BY NAME is a heuristic — a switch on a variable named `q` anywhere in a bundle
       attached its cases to every method's `q`. Per-call-site values reach the model through
       learnFromAstCallSite ← `callSite.params[i].validValues`, which is structurally tied to the one site.) */

    // Register AST-observed fetch call sites as methods on their services.
    // Uses learnFromAstCallSite (direct fact-based registration), NOT the
    // old "synthesize fake URL/body, launder through learnFromRequest"
    // path — that conflated AST observations with real server traffic.
    var newEndpoints = 0;
    for (var fc = 0; fc < analysis.fetchCallSites.length; fc++) {
      var callSite = analysis.fetchCallSites[fc];
      /* THE CALL-SITE CONTRACT, ASSERTED WHERE IT ARRIVES. endpoint.c's `endpoint_json_array` writes exactly
         five keys per record — method, url, provenance,
         params[{name,location,validValues[],excludes[] and bounds{} when observed}],
         and headers ONLY when it observed
         one — so those are the fields this loop and learnFromAstCallSite may read, and each one is guaranteed
         rather than defaulted. `params` is always present (possibly empty), which is why the `|| []` that
         stood at each use is gone: an absent array here would mean the engine's serializer changed shape, and
         that must crash rather than register every endpoint of the run as parameterless.
         `excludes` and `bounds` are the OPTIONAL keys on a param and each absence is a POSITIVE statement —
         no equality gate over that hole took its false arm, and no ordering gate's claim survived every
         observed path to this request — so both are read with an `in` test and never with a `||`, which
         would make "unconstrained" and "the engine stopped emitting the field" the same datum. */
      DCHECK(callSite && typeof callSite === "object", "a fetchCallSites entry is not an object — endpoint.c emits one JSON object per deduped endpoint");
      DCHECK(typeof callSite.method === "string" && callSite.method, "a fetchCallSites entry carries no method — endpoint_record takes it as a required argument, so an absent one is the @H serializer broken");
      DCHECK(Array.isArray(callSite.params), "a fetchCallSites entry carries no params array — endpoint.c writes \"params\":[…] for every endpoint (empty when the request carried no templated path segment, no query and no readable body), so its absence is the whole parameter surface arriving as nothing");
      /* AND WHAT THIS CALL SITE IS EVIDENCE OF, CHECKED IN BOTH DIRECTIONS OF THE SKEW: that the engine wrote
         a word, and that the word is one this zone knows. It is NOT an optional key like `excludes` and
         `bounds` — endpoint.c writes it on EVERY record, because the three words are exhaustive over the ways
         the engine can come to know an address and there is therefore no absence for a reader to interpret.
         WHAT A DEFAULT HERE WOULD COST is the whole point of the field: `callSite.provenance || "derived"`
         renders an address that exists only because a gate was forced with bytes identical to one the app's
         own code computes, and the reviewer reads the fabrication as a measurement — CLAUDE.md §@H's "one
         invented field is the example that shapes the next endpoint", arriving as silence rather than as a
         value. So it crashes here instead. */
      DCHECK(isCallSiteProvenance(callSite.provenance),
             "a fetchCallSites entry carries the provenance " + JSON.stringify(callSite.provenance) +
             ", which is none of " + CALLSITE_PROVENANCE.join("/") + " — endpoint.c writes one on every " +
             "record through solver/engine.h's one mapping, so this is that emission and lib/endpoint-record" +
             ".js's vocabulary having parted, and every learned address would be read at the wrong grade");
      // Structural @T candidate (url:null — unreached site, value
      // unresolved): surfaced via focusedView/structuralCandidates, not
      // a learnable endpoint. Skip before new URL(null) fabricates a
      // "/null" path.
      if (callSite.url == null || callSite.url === "") continue;
      // Skip data:/blob:/about: URLs — those are inline content, not
      // API endpoints. Registering them as services produces empty-
      // host records with garbled paths (the URL parser reads the
      // scheme as origin="null" and path starts mid-string).
      if (/^(data|blob|about|javascript):/i.test(callSite.url)) continue;

      /* WHAT THE CODE DETERMINED ABOUT THIS ADDRESS — lib/callsite-url.js, over endpoint.c's own template
         grammar rather than over a regex guessing at three spellings of "dynamic". A relative address resolves
         against the PAGE's origin at runtime, NOT the script's host: `fetch('/svc/shreddit/graphql')` in a
         bundle served from www.redditstatic.com hits www.reddit.com, so the base is the tab's page url and
         only falls back to the analysis sourceUrl when this merge has no page url at all.
         AN UNDETERMINED ORIGIN IS A POSITIVE STATEMENT AND IS REGISTERED LIKE ANY OTHER ENDPOINT. What stood
         here read `csUrl.pathname` off the null the "dynamic" branch had just assigned, and the TypeError went
         to the console.debug below: EVERY call site whose address begins with a shape was dropped between the
         engine and the moat, which on a real corpus is most of them. §@H makes a domain-annotated shape a
         first-class output — the engine must never INVENT a value it only knows the shape of — so a shape
         standing where the origin would be is exactly the record this tool exists to emit. It is filed under
         that shape, never under the script's host, which is a different address the code never computed. */
      var _addr = astCallSiteAddress(callSite.url, (tab && tab.url) ? tab.url : analysis.sourceUrl);
      var interfaceName = _addr.originKnown ? extractInterfaceName(_addr.url) : _addr.host;

      var _learned = learnFromAstCallSite(tab, interfaceName, callSite, analysis.sourceUrl);
      DCHECK(_learned && typeof _learned === "object" && "entry" in _learned && "method" in _learned,
             "learnFromAstCallSite did not answer with its {entry, method} pair — both halves are read " +
             "below and a missing one silently files this endpoint under the wrong service or drops the " +
             "path-param examples it just learned");
      // Refine interfaceName for endpoint registration if the call site
      // got promoted to a prefix bucket via observed-prefix clustering.
      if (_learned.entry && _learned.entry.doc && _learned.entry.doc.name) {
        interfaceName = _learned.entry.doc.name;
      }

      // Register endpoint for popup display — separate concern from
      // method registration (endpoint list shows "what fetches exist
      // on this page," method list shows "what API endpoints we know").
      /* ONE KEY SHAPE FOR BOTH KINDS OF ADDRESS: method + host + path, where `host` is the origin as far as
         the code determined it. The "dynamic" arm keyed on the bundle id and the CALL-SITE INDEX `fc`, which
         is not an identity at all — it renumbers whenever the engine's emission order moves, so the same
         endpoint re-registered under a new key on every run and two runs never deduped. A shape is a stable
         name, so the shape-origin records dedup exactly like the literal ones.
         THE SHAPE IS NO LONGER SPELLED HERE. It was spelled here, again at the structural dedup key below,
         once more in offscreen-brain.js's per-delivery sweep, and a fourth time — WRONG, without the prefix —
         by a live-response consumer whose lookup therefore could never match. lib/endpoint-record.js mints
         it now: the file that
         owns the record's one description owns its one NAME, and a fifth spelling has nowhere to be written. */
      var epKey = endpointKeyFromParts(callSite.method, _addr.host, _addr.path,
                                       "lib/merge.js registering an @H call-site endpoint");
      // DEDUP by STRUCTURAL identity: the SAME endpoint driven with opaque-POSITIONAL args ({arg0}, from
      // __hostDrive's JS-side drive) and with NAMED args ({id}, from the grind's declared-name drive) yields
      // TWO keys for ONE endpoint (verified: spa_gated 5 raw / 4 distinct). Collapse {..} path-param segments
      // to a structural key; on collision keep the DECLARED-name record over the positional one. Only
      // {placeholder} segments normalize, so genuinely-distinct endpoints (differing elsewhere) never merge.
      // The stored KEY stays the raw path, so probe/replay of the surviving record are unaffected.
      if (!tab._epNorm) tab._epNorm = new Map();
      /* THE STRUCTURAL KEY IS THE SAME NAME OVER A HOLE-NORMALIZED PATH, so it is minted by the same function.
         It names a different map (`tab._epNorm`), but it is the same NAME SPACE — its values are endpoint
         keys and it is compared against them — so a shape that drifted in one and not the other would make
         this dedup silently stop collapsing, which reads as two genuinely distinct endpoints. */
      var _structKey = endpointKeyFromParts(callSite.method, _addr.host,
                                            _addr.path.replace(/\{[^}]*\}/g, "{}"),
                                            "lib/merge.js building the structural dedup key");
      var _posRe = /\{arg\d+\}/;
      var _priorKey = tab._epNorm.get(_structKey);
      if (_priorKey && _priorKey !== epKey && tab.endpoints.has(_priorKey)) {
        if (_posRe.test(_priorKey) && !_posRe.test(epKey)) {
          tab.endpoints.delete(_priorKey); tab._epNorm.set(_structKey, epKey);   // prior positional, new declared -> upgrade (add epKey below)
        } else {
          epKey = _priorKey;   // keep prior (declared/equal); has() below is true -> skip add, no dup
        }
      } else {
        tab._epNorm.set(_structKey, epKey);
      }
      if (!tab.endpoints.has(epKey)) {
        /* THROUGH lib/endpoint-record.js, WHICH IS NOW THE ONLY DESCRIPTION OF THIS RECORD. This literal used
           to BE the description, and it could only be consulted by reading it — which is why the field list
           had been hand-copied into seven comments and one DCHECK elsewhere, and why five names no producer
           writes had been projected onto it by readers with nothing to catch them. The constructor rejects a
           name this record does not carry, so the next such projection crashes at the producer instead of
           reading `undefined` for the life of the feature. */
        tab.endpoints.set(epKey, makeEndpointRecord({
            // new URL().href percent-encodes shape holes ({} -> %7B%7D); decode so the endpoint URL keeps
            // the canonical `{}` param placeholder the dedup/UI recognize (path is already decoded).
            url: _addr.originKnown ? _decHoles(_addr.url.href) : callSite.url,
            method: callSite.method,
            host: _addr.host,
            path: _addr.path,
            service: interfaceName,
            source: _addr.originKnown ? "ast_analysis" : "ast_shape_origin",
            /* `null` IS THIS RECORD'S ONE SPELLING OF "no document address was known", so the translation
               happens HERE, at the producer, rather than at each surface. What stood here tested a local
               alias of `tab` that is unconditionally truthy at this point, so its `: null` arm was
               unreachable and a doc view with no address wrote `undefined` — which chrome.runtime.
               sendMessage then DROPS, delivering the popup a record whose `pageUrl` key does not exist. */
            pageUrl: tab.url === undefined ? null : tab.url,
            /* AST-captured required headers (the SET the bundle attached at the host edge, per-header
               literal/opaque) — transport metadata the Send panel shows so the endpoint is actually usable.
               ABSENCE IS THE POSITIVE STATEMENT, and endpoint.c states it in those words: it writes the
               `headers` key ONLY when `e->nh`, because "an endpoint with no learned header must not claim an
               empty requirement, which reads as 'needs nothing' rather than 'nothing was observed'". So a
               missing key is "nothing was observed" and becomes null here, and a PRESENT one is asserted to
               be the non-empty record it can only be. */
            requiredHeaders: callSite.headers === undefined ? null : astHeaderRecord(callSite.headers),
            /* THE PATH-PARAM EXAMPLES, FROM THE RECORD THAT ACTUALLY HOLDS THEM — BOTH POOLS, because the
               flat record is the one that is READ where the method record is not. It reads the METHOD
               lib/learn.js just registered, which has TWO producers of a `location:"path"` parameter:
               endpoint.c emits one per `{hole}` the forced execution interpolated into the address, carrying
               the segment its concolic example computed, and the templated reconcile below folds in the
               concrete segment of a live request that matched the template — the "orgId=acme-42" case.
               (Reading `callSite.params` for `p.location === "path"` directly would work now that the
               producer states it, and is still the wrong source: the values a live request taught belong to
               the method and never reach the call-site record.)
               THE SENTENCE THAT STOOD HERE SAID THE METHOD SCHEMA IS "EVICTED AFTER REVIEW", AND IT IS A
               SCAR. The review-completion reclaim it named is deleted (offscreen-brain.js records why: this
               zone holds no document bytes any more), the DocData that dies with the session is not what
               lib/send.js reads, and the moat's own `discoveryDocs` are serialized WITH their doc — so the
               flat record and the method schema have the same lifetime today. The flat record is still the
               one this field must be complete on, and for a reason that does not depend on a lifetime: it is
               what `netdiff --unused` counts, what crosses to the popup, and what lib/send.js falls back to
               whenever the resolved schema does not DECLARE the hole (a partial path match, an address
               unioned here from a document whose method lives in another bucket). On every one of those the
               flat record is the whole statement, and half a statement about a shape is §@H's wrong report.
               WHICH IS WHY THE FORCED POOL IS ITS OWN FIELD AND NOT A MEMBER OF THIS ONE. `pathParams` is
               read by lib/send.js, which unions these into the Send form's value list AND takes `values[0]`
               as the prefilled `_exampleValue`, so a hole filled only on a forced arm must not appear here —
               it would arrive as the address the reviewer sends, which is the exact failure the pool split
               exists to stop. It appears in `pathParamsForced` instead, where lib/popup-form.js renders it on
               its own row as the real observation it is. */
            pathParams: _astPathParamPool(_learned.method, "_astValidValues"),
            pathParamsForced: _astPathParamPool(_learned.method, "_astForcedValues"),
            /* (No request body on THIS record. The body surface lands in the doc model: endpoint.c reads the
               request's own payload and lib/learn.js files its fields as `doc.schemas[…Request]` with
               `m.request.$ref` pointing at them, which is what lib/send.js's `requestBody` and the OpenAPI
               export already resolve. A flat copy here would be the eviction argument above applied to the
               body, and it needs a reader in lib/send.js first — that file projects only the ten fields this
               `endpoints.set` writes, deliberately.) */
            firstSeen: Date.now(),
        }, "lib/merge.js registering an @H fetch call site"));
        newEndpoints++;
      }
      /* THE CATCH THAT STOOD HERE IS DELETED, AND IT IS WHY THIS DEFECT WAS INVISIBLE FOR A WHOLE CORPUS.
         Its stated job was that one malformed call site must not cost the other N-1 their registration, and
         RETHROW_FATAL already carried every DCHECK straight through it — so the ONLY throws it still swallowed
         were the unanticipated ones, which is exactly the class that cost the moat every endpoint on 30 real
         sites: a TypeError off a null the branch above had assigned, printed to console.debug, and the record
         gone with nothing anywhere reporting it as gone. §Offensive programming has no "handled as merely
         malformed" state — a call site is either a record this zone can build or an engine↔JS contract that
         must crash where it breaks. There is no third answer to keep a catch for. */
    }
    if (analysis.fetchCallSites.length) {
      console.debug("[AST:merge] Fetch sites: %d call sites processed, %d endpoints registered",
        analysis.fetchCallSites.length, newEndpoints);
    }

    /* THE DOM-ENDPOINT REGISTRATION IS GONE BECAUSE `analysis.domEndpoints` HAS NO PRODUCER. It read
       `analysis.domEndpoints || []` and registered each entry as a `dom_<source>` endpoint keyed by the
       resolved href — 35 lines with their own try/catch and their own comment about vendor-page regressions
       being "visible instead of disappearing into an empty endpoint list", over an array bridge.js writes as
       the constant `[]`. There is no `domEndpoints` and no `dom_` anything in engine/host: the href/src/
       action/data-* scan the comment describes is a capability the engine does not have, and the `|| []` is
       what made its absence look like a page with no such markup.

       (The place to build it is the engine's Lexbor tree, where the attribute values are. This used to point
       at two driver assertions as "the record that the scan was intended" — the drivers are deleted, and a
       pointer to a file that is not there is worse than none, so the intent is stated here instead of being
       filed against something a reader would have to find first.) */

    /* THE ONE SECURITY MERGE. `!analysis._securityMerged` guarded this, and the flag's other writer was
       lib/analyze.js pre-empting this block to do its own per-script split — a split over `sink.location`,
       which solve.c does not emit, so it filed every finding under the page URL and produced exactly what
       this block produces. That file is deleted and so is the flag: the replace-then-push below is already
       idempotent for a repeated merge of one analysis, which is all the flag ever bought.

       `dangerousPatterns` IS GONE FROM BOTH SIDES OF THIS SEAM. It was read off every analysis and carried onto
       every finding record, while bridge.js wrote it as a host-side constant `[]` — the engine has no such
       surface and no renderer has ever read the field. A `.length` of 0 on a finding was therefore
       indistinguishable from "no dangerous patterns found on this page": a claim about the page made out of a
       constant, and §RUN, DON'T MATCH forbids the pattern list it claimed to be. The record now carries only
       what solve.c emits. */
    var secSinks = analysis.securitySinks;
    DCHECK(Array.isArray(secSinks),
           "an analysis reached the security merge without its securitySinks array — bridge.js builds it on " +
           "every result document, so an absent one is that relay broken and this page would merge as clean");
    if (secSinks.length) {
      if (!tab._securityFindings) tab._securityFindings = [];
      // REPLACE any prior entry for this source, don't append — the deep grind
      // streams partials each carrying the GROWING accumulated securitySinks for
      // the same sourceUrl, so appending would pile up snapshots (mergeToGlobal
      // set()s by sourceUrl so globalStore stays correct, but the tab array would
      // leak). Keep the latest (most complete) per source.
      for (var _sfx = tab._securityFindings.length - 1; _sfx >= 0; _sfx--)
        if (tab._securityFindings[_sfx].sourceUrl === analysis.sourceUrl) tab._securityFindings.splice(_sfx, 1);
      /* `pageUrl: _mfMeta ? _mfMeta.url : null` STOOD HERE OVER A `var _mfMeta = tab || null` FOUR LINES UP,
         and the ternary was dead twice over: `tab` is dereferenced on the line above it, so the guard could
         not fire — and what it could not stop is the case it looks like it is for. `getDoc` initialises a
         DocView's `url` to the EMPTY STRING and CONTENT_SEED fills it in, so a finding merged before that
         seed carried `pageUrl: ""`, which is not an address and is not this record's stated absence either:
         it is a third spelling that offscreen-brain.js's live-verify reads as an address it has (`entry &&
         entry.pageUrl` is false for it, so it silently probes nothing) and that lib/store-record.js names as
         the RECIPE — a finding whose document cannot be re-visited is the only copy of itself. The record
         states one of its two declared spellings, and `null` MEANS "no document address was known when these
         sinks were observed". */
      tab._securityFindings.push({
        sourceUrl: analysis.sourceUrl,
        pageUrl: tab.url ? tab.url : null,
        securitySinks: secSinks,
      });
      console.debug("[AST:merge] Security findings for %s: %d sinks", analysis.sourceUrl, secSinks.length);
    }
  }
  /* NO EVICTION SWEEP AFTER THIS MERGE. It was scheduled here to drop a reviewed document's "transient RAM
     view", which was the page source this zone used to hold — and this zone holds no document bytes any more:
     a seed is an address, the custom browser loads the document itself, and those bytes live in and die with
     that instance. What remained after the page source was identity, which a merge does not make stale. */
}

// ─── Message Handling ────────────────────────────────────────────────────────

// ─── Form Metadata Processing ─────────────────────────────────────────────

// ── Cross-tab / global aggregation (host-level per SECURITY.md: the brain aggregates results from the
//    per-page engines into the ONE global moat) ──
/* THE PER-PARAMETER UNION. Only `_astValidValues` needed naming when this merge did nothing else; now that
   the whole record travels, the two example-bearing fields do too. A param the existing record does not have
   is copied whole; a param both records have keeps the existing DECLARATION (type/location/description are a
   function of the same URL and verb) and unions the OBSERVATIONS, because those are what differ per page. */
function _mergeParamInto(ep, np) {
  /* THE UNION IS PER POOL, AND ACROSS DOCUMENTS THE POOLS STILL FOLD. lib/learn.js splits a parameter's
     learned values by whether the sighting that computed each one stood on a forced arm, and this is the
     OTHER merge — one document's parameter into the cumulative moat's — so the same law applies to it or the
     split holds within a page and dissolves between two. The law is most-observed (lib/endpoint-record.js),
     spelled here exactly as it is spelled there: a value the other document computed WITHOUT forcing anything
     is promoted out of our forced pool, and one it could only force is added to ours only where no document
     has computed it otherwise. Unioning the two pools independently would have re-created the defect the
     split exists to remove, in the one place it is invisible from either page — the moat, which is what
     `netdiff --unused` counts and what a reviewer reads without the per-document schema in front of it.
     THE LAW IS NOT SPELLED HERE ANY MORE, AND THAT IS THE CHANGE. It was written out at this merge, again at
     lib/learn.js's per-sighting merge, and a THIRD copy was about to be written at the flat endpoint records'
     merge below — three private spellings of one rule, each correct on the day it was written and each free
     to drift. `foldValuePools` is the one performance of it; what stays here is the one thing that IS this
     record's own — how a method parameter spells an empty pool, which is an omitted key. */
  const _folded = foldValuePools(
    Array.isArray(ep._astValidValues) ? ep._astValidValues : [],
    Array.isArray(ep._astForcedValues) ? ep._astForcedValues : [],
    Array.isArray(np._astValidValues) ? np._astValidValues : [],
    Array.isArray(np._astForcedValues) ? np._astForcedValues : []);
  if (_folded.valid.length) ep._astValidValues = _folded.valid;
  /* AN EMPTIED FORCED POOL IS DELETED RATHER THAN LEFT AS `[]` — the last promotion out of it is exactly
     where the key would otherwise survive as an empty list, which is a third statement no reader has. */
  if (_folded.forced.length) ep._astForcedValues = _folded.forced;
  else if (Array.isArray(ep._astForcedValues)) delete ep._astForcedValues;
  /* THE EXAMPLE IS A COMPUTED VALUE AND ITS ABSENCE IS A STATEMENT (§RUN, DON'T MATCH: a param known only to
     satisfy a range gate has a SHAPE and no example). So an existing example is never overwritten and an
     absent one is filled from the other page — and the SOURCE label travels with it, because a value whose
     provenance came from a different record is a value nobody can judge. */
  if (ep._exampleValue === undefined && np._exampleValue !== undefined) {
    ep._exampleValue = np._exampleValue;
    ep._exampleValueSource = np._exampleValueSource;
    ep._exampleConfidence = np._exampleConfidence;
  }
  if (np._astInferred) ep._astInferred = true;
  /* THE DOMAIN HALF, WHICH THIS MERGE DID NOT CARRY AT ALL. `_excludedValues` and `_bounds` are what the
     endpoint's own gates PROVED about this parameter, and lib/learn.js states their law where it applies it
     to one call site: exclusions INTERSECT and an interval widens to the HULL, because a constraint is a
     claim about the ENDPOINT and belongs on the record only where EVERY observed path obeyed it. This
     function is the OTHER merge — one document's parameter into the cumulative moat's — and it is called
     exactly when both sides already exist, which is to say when a SECOND document has reached the same
     endpoint's same parameter. It carried `_astValidValues` by union (right: a value is knowledge that
     accumulates) and neither of these, so the second document's domain observation was discarded and the
     moat kept the first document's claim standing.
     THE DIRECTION OF THAT ERROR IS WHY IT IS A WRONG REPORT RATHER THAN A THIN ONE. A page that reached the
     request WITHOUT the constraint is a path that DISPROVES it, so what survived was `≠ admin` on a
     parameter one observed path never tested — and §@H is explicit that a shape carrying the wrong half is
     "a WRONG report, not a partial one", because a reviewer reads a surviving exclusion as a fact the run
     established rather than as a claim nobody re-checked.
     PRESENCE, NOT TRUTHINESS, IS WHAT IS TESTED — and the two fields are tested differently because their
     vocabularies differ. A merged parameter may never have had an engine observation at all (a form scan or
     a live request can create one), and that is a different fact from an observation that proved nothing:
     the first must contribute NOTHING, and the second must erase. `_excludedValues` spells "proved nothing"
     as the empty array, so its presence as an array is the whole test; `_bounds` spells it as `null`, so
     `in` is the test and `null` is passed through to do the erasing. Collapsing either pair would let a
     parameter no engine run ever touched wipe every domain the engine emits. */
  if (Array.isArray(np._excludedValues)) intersectExcludedValues(ep, np._excludedValues);
  if ("_bounds" in np) widenBoundsInto(ep, np._bounds);
  /* …AND THE CALL PREDICATES, WHICH ARE THE THIRD FACT OF THE SAME KIND AND MERGE BY THE SAME LAW. They spell
     "proved nothing" the way `_excludedValues` does — as the empty array — so presence as an array is the
     whole test here too, and a parameter no engine run ever touched contributes nothing rather than wiping
     what the engine emitted. Leaving them out would repeat, one fact later, exactly the error the paragraph
     above records: a second document that reached the request without testing `startsWith("/api")` is a path
     that disproves the claim, and the moat would go on rendering it. */
  if (Array.isArray(np._predicates)) intersectPredicates(ep, np._predicates);
}

/* THE PER-METHOD STATS UNION — `lib/stats.js`'s `mergeParamStats`, WHICH HAD NO CALLER.
   Forty lines that add two observation counts, union two value histograms under the same 50-value cap, take
   the min/max of two numeric ranges and add two format-hint tallies: written for exactly this merge and never
   wired to it, so every cumulative-moat method carried whichever page's traffic reached the global store
   first. A helper with no caller is the same broken contract as a reader with no writer — the code reads as
   if the moat accumulates, and it did not.

   AND IT IS SUMMED OVER CONTRIBUTORS, NOT ADDED ON ARRIVAL, BECAUSE THIS MERGE IS NOT CALLED ONCE.
   `mergeToGlobal(tab)` runs on every analysis, every probe outcome and every captured response, and each run
   presents the DOCUMENT'S OWN CUMULATIVE `_stats` again — so `global += tab` counts the same requests once
   per merge. MEASURED, in the change that first wrote it that way: two probe pages that had made ONE request
   each reported `requestCount: 8`. An inflating counter is a fabricated measurement, which is the same defect
   as the dropped field this function exists to fix, pointing the other way.
   So the global keeps WHO CONTRIBUTED WHAT — `_statsByDoc`, keyed by the contributing document — and derives
   `_stats` from it. Re-merging a document REPLACES that document's entry, so the operation is idempotent no
   matter how many times it runs, and two different documents genuinely add. `correlations` is DERIVED
   (detectCorrelations over the summed distribution) rather than picked from one contributor: a correlation is
   a claim about a set of observations, and carrying one document's answer over another's data is a statement
   about a distribution that was never observed. */
function _sumStats(byDoc) {
  const out = { requestCount: 0, params: {}, bodyFields: {} };
  for (const dk in byDoc) {
    const s = byDoc[dk];
    DCHECK(s && typeof s.requestCount === "number" && s.params && s.bodyFields,
           "a contributed _stats record is not the {requestCount, params, bodyFields} shape lib/learn.js " +
           "creates — the moat's per-method totals are summed straight off these, so a malformed one would " +
           "report an endpoint's traffic as a number nobody observed (document=" + dk + ")");
    out.requestCount += s.requestCount;
    for (const pn in s.params) out.params[pn] = mergeParamStats(out.params[pn], s.params[pn]);
    for (const fn in s.bodyFields) out.bodyFields[fn] = mergeParamStats(out.bodyFields[fn], s.bodyFields[fn]);
  }
  out.correlations = detectCorrelations(out);
  return out;
}

/* A METHOD RECORD IS THE UNION OF WHAT TWO PRODUCERS LEARNED ABOUT ONE ENDPOINT, AND THIS MERGE COPIED ONE
   NESTED ARRAY OF ONE SUB-OBJECT.
   The two producers are the BUNDLE (lib/learn.js `learnFromAstCallSite`, off the engine's @H surface) and the
   WIRE (`learnFromRequest`, off a real response), and which of them reaches `globalStore` first is decided by
   page timing. Everything the second one knew that the first did not was dropped here: `_astInferred`,
   `_astSourceScript`, `_stats`, `response`, `request`, `contentTypes`, `requiredHeaders`, `_responseKind`,
   `_chains`. MEASURED in Chrome on the one-fetch probe page: the tab's own record carried
   `_astInferred:true` and the global record — the one the popup renders for every page you are not standing
   on — did not, so `_methodOrigin` tagged the endpoint the forced execution had just found in the bundle as
   "fired only (no bundle origin)". The cumulative moat is where "what the bundle CAN do but didn't" is read,
   and it was answering the opposite.
   THE IDENTITY FIELDS ARE KEPT, THE LEARNED ONES ARE FILLED, AND THE TWO THAT ARE NOT FIRST-WRITER-WINS SAY
   SO: `_astInferred` is MONOTONE (a call site found in the bundle by either page is a call site in the
   bundle), and `_stats` is SUMMED PER CONTRIBUTING DOCUMENT (see `_sumStats`). Everything else the newer record carries and the older lacks is copied
   as-is — a real discovery document's methods carry server-authored fields (scopes, parameterOrder, flatPath)
   that this file must not have to enumerate, so the rule is "fill what is missing", never an allowlist that
   would silently drop what it had not heard of. */
function _mergeMethodInto(em, nm, docKey) {
  for (const k in nm) {
    if (k === "parameters" || k === "_stats" || k === "_astInferred") continue;   // the three with real rules
    if (em[k] === undefined || em[k] === null) em[k] = nm[k];
  }
  if (nm._astInferred) em._astInferred = true;
  if (nm.parameters) {
    if (!em.parameters) em.parameters = {};
    for (const pn in nm.parameters) {
      if (!em.parameters[pn]) { em.parameters[pn] = nm.parameters[pn]; continue; }
      _mergeParamInto(em.parameters[pn], nm.parameters[pn]);
    }
  }
  if (nm._statsByDoc) {
    if (!em._statsByDoc) em._statsByDoc = {};
    for (const dk in nm._statsByDoc) em._statsByDoc[dk] = nm._statsByDoc[dk];
    em._stats = _sumStats(em._statsByDoc);
  }
}

/* WHO OBSERVED THIS TRAFFIC, STAMPED BEFORE ANY ADOPTION CAN LOSE IT. Three paths adopt a record whole — a
   service the moat has never seen (the doc), a bucket it has never seen, a method it has never seen — and each
   would file that document's requests under nobody, so the next document's merge would report only its own.
   Stamping the incoming doc first makes all three carry their contributor, and makes `_mergeMethodInto` a
   union of two contributor MAPS rather than of a total and a document. Idempotent: a record already carrying
   its map (the global doc is aliased to the first tab doc that produced it) is left alone, and the entry
   ALIASES the DocView's live `_stats`, so a later request through that document is already counted. */
function _seedStatsContributors(res, docKey) {
  for (const bk in res) {
    const b = res[bk];
    if (!b) continue;
    if (b.methods) {
      for (const mk in b.methods) {
        const m = b.methods[mk];
        if (!m._stats || m._statsByDoc) continue;
        DCHECK(typeof docKey === "string" && docKey,
               "a method carrying _stats reached the global merge with no contributing document key — " +
               "mergeToGlobal passes the DocView's own documentId, and without it the same document's " +
               "traffic would be added again on every merge instead of replacing its own contribution");
        m._statsByDoc = { [docKey]: m._stats };
      }
    }
    if (b.resources) _seedStatsContributors(b.resources, docKey);
  }
}

function _mergeDocInto(existingDoc, newDoc, docKey) {
  if (newDoc && newDoc.resources) _seedStatsContributors(newDoc.resources, docKey);
  if (!existingDoc || !existingDoc.resources) return newDoc || existingDoc || null;
  if (!newDoc || !newDoc.resources) return existingDoc;
  _mergeResourcesInto(existingDoc.resources, newDoc.resources, docKey);
  if (newDoc.schemas) { existingDoc.schemas = existingDoc.schemas || {}; for (const sk in newDoc.schemas) if (!existingDoc.schemas[sk]) existingDoc.schemas[sk] = newDoc.schemas[sk]; }
  return existingDoc;
}

/* AND IT RECURSES, BECAUSE A DISCOVERY DOCUMENT'S RESOURCES NEST. `getDocMethods` (popup.js) walks
   `r.resources` at every level, so a nested bucket's methods ARE rendered; this merge read only
   `resources[bk].methods` and stopped, so for any service whose bucket already existed globally every nested
   resource of every later page was read past in silence. The whole-bucket copy one line up hid it: the first
   page's nested methods arrived (the bucket was absent, so it was taken entire) and no page's ever did
   again. */
function _mergeResourcesInto(eres, nres, docKey) {
  for (const bk in nres) {
    const nb = nres[bk];
    if (!nb) continue;
    const eb = eres[bk];
    if (!eb) { eres[bk] = nb; continue; }
    if (nb.methods) {
      if (!eb.methods) eb.methods = {};
      for (const mk in nb.methods) {
        const nm = nb.methods[mk];
        /* THE SAME NAME FOR TWO VERBS IS TWO ENDPOINTS, and merging them would OR one's bundle-origin onto
           the other's. `learnFromAstCallSite`/`learnFromRequest` resolve this INSIDE one document by moving
           the incumbent to `<verb>_<name>`; across documents neither page ever saw the other's, so both wrote
           the bare name and this loop was the first place the collision existed. It is resolved the same way,
           and the id — which the Send panel keys on — is re-derived rather than left naming the bare key. */
        let key = mk, em = eb.methods[key];
        if (em && em.httpMethod !== nm.httpMethod) {
          key = String(nm.httpMethod).toLowerCase() + "_" + mk;
          if (typeof nm.id === "string" && nm.id.lastIndexOf(".") >= 0)
            nm.id = nm.id.slice(0, nm.id.lastIndexOf(".") + 1) + key;
          em = eb.methods[key];
        }
        if (!em) { eb.methods[key] = nm; continue; }   // distinct endpoint from another page -> keep both
        _mergeMethodInto(em, nm, docKey);
      }
    }
    if (nb.resources) {
      if (!eb.resources) eb.resources = {};
      _mergeResourcesInto(eb.resources, nb.resources, docKey);
    }
  }
}
/* AN EMPTY PAIR, NAMED ONCE: what a record that has no such hole contributes to the fold below. */
const _NO_HOLE_POOLS = Object.freeze({ valid: Object.freeze([]), forced: Object.freeze([]) });
/* THE FLAT RECORDS' OWN HOLE MERGE — two sightings of ONE address, met at the moat, folded by the ONE law.
   THIS IS THE THIRD MERGE OF THE SAME PAIR AND IT USED TO BE A PLAIN UNION OF ONE POOL. That was sound only
   while the record HELD one pool: the union ran over `pathParams`, the forced pool did not exist on this
   record, and a hole the run could only force reached the moat as a hole nothing had filled. Carrying the
   second field without carrying the FOLD would have been the worse half of that — `_mergeParamInto` states
   why in its own words, one record down: "Unioning the two pools independently would have re-created the
   defect the split exists to remove, in the one place it is invisible from either page". A value one document
   computed unforced and another could only force would then sit in both lists, and the Send panel would offer
   it as one the app computes AND label it as one no client sends.
   NEITHER SIDE'S ABSENCE IS INTERPRETED HERE: `endpointHolePairs` is the one walk that reads what a record
   says about its holes, and it refuses everything but `null` and a list — so a record an older store wrote
   short of a field crashes at the door it came through, rather than contributing a silence that reads here
   exactly like "this document forced nothing".
   THE RESULT IS RE-CHECKED, because this function WRITES a record the constructor did not build: it is the
   one place the two lists are composed from two sources, so it is the one place their disjointness can be
   broken, and `checkEndpointRecord` asserts it where it can still name this merge. */
function _foldEndpointHolesInto(into, from, key) {
  /* BOTH RECORDS READ AS PER-NAME PAIRS THROUGH THE ONE WALK (lib/endpoint-record.js), so this merge is a
     fold of two RECORDS and not of four lists. */
  const _a = endpointHolePairs(from, "lib/merge.js moat-folding the stored record for " + JSON.stringify(key));
  const _b = endpointHolePairs(into, "lib/merge.js moat-folding this document's record for " + JSON.stringify(key));
  const _folded = new Map();
  /* A HOLE ONLY ONE SIDE HAS STILL GOES THROUGH THE FOLD, against an empty pair — one path, so there is no
     shortcut in which a single-contributor hole skips the promotion the two-contributor one gets. A Map miss
     here is not a field absence to interpret: the name came out of the UNION of the two key sets, so a miss
     is the positive statement "the other record has no such hole", which is what an empty pair says. */
  for (const name of new Set([..._a.keys(), ..._b.keys()])) {
    const x = _a.has(name) ? _a.get(name) : _NO_HOLE_POOLS;
    const y = _b.has(name) ? _b.get(name) : _NO_HOLE_POOLS;
    _folded.set(name, foldValuePools(x.valid, x.forced, y.valid, y.forced));
  }
  const _project = (side) => {
    const out = [];
    for (const [name, p] of _folded)
      if (p[side].length) out.push({ name: name, values: p[side].slice(0, PATH_PARAM_EXAMPLE_CAP) });
    return out.length ? out : null;
  };
  into.pathParams = _project("valid");
  into.pathParamsForced = _project("forced");
  checkEndpointRecord(into, "lib/merge.js folding two documents' path-hole examples at the moat, key " +
                            JSON.stringify(key));
}

function mergeToGlobal(tab) {
  // Central merge — EVERY analysis result (hot tab + cold frontier) funnels here, so guard the DocView contract
  // once: a malformed tab is a should-never-happen the callers must not construct, not a shape to defend against.
  DCHECK(tab && typeof tab === "object", "mergeToGlobal: tab (DocView) must be an object");
  DCHECK(tab.apiKeys && typeof tab.apiKeys[Symbol.iterator] === "function", "mergeToGlobal: tab.apiKeys must be an iterable Map");
  DCHECK(globalStore && globalStore.apiKeys, "mergeToGlobal: globalStore must be initialized before a merge");
  for (const [k, v] of tab.apiKeys) {
    /* THE KEY'S TYPE IS PART OF THE KEY, AND THIS LOOP USED TO DROP IT. lib/keys.js writes `name` — the
       KEY_PATTERNS entry that matched, "GitHub Token" / "JWT" / "Stripe Key" — and both branches below
       rebuilt the global entry field by field without it, so a key kept its type only while the tab that
       found it was the one being rendered. Every key in the cumulative moat (another tab, an earlier
       session) reached the popup with no type, where `info.name || "API Key"` turned that into a generic
       label indistinguishable from a key whose pattern really was the generic one. Asserted here rather
       than at the reader because THIS is the origin: the value in hand comes straight from lib/keys.js. */
    DCHECK(typeof v.name === "string" && v.name,
           "a tab's API-key entry carries no `name` — lib/keys.js sets it from the KEY_PATTERNS entry that " +
           "matched, so an absent one means a key was recorded by something that does not know what kind of " +
           "key it is, and the moat would show it as a nameless secret");
    /* THE TAB SIDE OF THE ENTRY, ASSERTED ONCE FOR BOTH ARMS BELOW — a fresh global entry is built out of
       these values directly and a merge unions them into an existing one, so a wrong shape here is either a
       key whose usage total resets to zero or a union that silently stops accumulating. */
    DCHECK(typeof v.requestCount === "number", "a tab's API-key entry carries no numeric requestCount — lib/keys.js initialises it to 0");
    for (const _f of ["services", "hosts", "endpoints", "pageUrls"]) {
      DCHECK(v[_f] instanceof Set,
             "a tab's API-key entry's `" + _f + "` is not a Set — lib/keys.js builds all four, and what this " +
             "key was seen against is the whole of what the moat knows about it");
    }
    const existing = globalStore.apiKeys.get(k);
    if (existing) {
      existing.lastSeen = v.lastSeen;
      // The pattern that matched is a function of the key text, so the tab's answer is authoritative and
      // an entry stored before this field travelled heals on the next sighting.
      existing.name = v.name;
      // Take the higher count — tab count is a running total, not a delta
      DCHECK(typeof existing.requestCount === "number",
             "a stored API-key entry's requestCount is not a number — this file and " +
             "_deserializeIntoGlobalStore are its only writers, and a `|| 0` here would silently reset a " +
             "key's usage total to zero");
      existing.requestCount = Math.max(existing.requestCount, v.requestCount);
      /* ALL FOUR COLLECTIONS ARE SETS ON BOTH SIDES — lib/keys.js builds them, the arm below builds them,
         and _deserializeIntoGlobalStore reconstructs them from the stored arrays. Each merge stood behind
         its own `instanceof Set` test whose false arm did NOTHING, so a record that had somehow lost a Set
         would have kept the key and silently stopped accumulating the services, hosts and endpoints it was
         seen against — the union quietly frozen, with a full-looking entry to show for it. */
      for (const _f of ["services", "hosts", "endpoints", "pageUrls"]) {
        DCHECK(existing[_f] instanceof Set,
               "a stored API-key entry's `" + _f + "` is not a Set — the union of what this key was seen " +
               "against is the moat's cross-session surface for it");
        for (const _s of v[_f]) existing[_f].add(_s);
      }
    } else {
      globalStore.apiKeys.set(k, {
        name: v.name,
        /* NO `origin` — see lib/keys.js, which minted it: a prefix of `referer`, computed from the same URL
           in the same breath, copied across this seam and into IndexedDB on every save, and read by no
           surface in either realm. A store written before this carries the name and nothing asks for it
           (lib/store-record.js's door reads records rather than minting them, so an extra name is not an
           error there). */
        referer: v.referer,
        source: v.source,
        firstSeen: v.firstSeen,
        lastSeen: v.lastSeen,
        requestCount: v.requestCount,
        services: new Set(v.services),
        hosts: new Set(v.hosts),
        endpoints: new Set(v.endpoints),
        pageUrls: new Set(v.pageUrls),
      });
    }
  }
  /* THREE BOOKKEEPING FIELDS ARE GONE FROM THIS LOOP AND THE UNION IS WHAT IS LEFT. `_isNew`,
     `_firstSeenGlobal` and `lastSeen` were written here and read NOWHERE — not by the popup, not by the
     serializer, not by the harness — and `lastSeen` was worse than unread: its only writer
     (lib/response-decode.js) built a key without the "AST " prefix every key in this map carries, so it
     could never match, and the line here then set the tab entry's lastSeen to Date.now() because the global
     entry had any, timing the MERGE rather than a sighting. What survives is the one thing this loop is
     for: the path-param union, so a later paramless re-emit (a re-visit before the concolic reply re-run
     landed) never DROPS values a prior emit learned — the moat is monotonic. */
  for (const [k, v] of tab.endpoints) {
    var ge = globalStore.endpoints.get(k);
    if (ge) _foldEndpointHolesInto(v, ge, k);
    globalStore.endpoints.set(k, v);
  }
  for (const [k, v] of tab.discoveryDocs) {
    /* A TAB ENTRY REACHES THE MOAT WHEN IT CARRIES A DOC, whatever the published fetch answered — the same
       split lib/serialize.js states. Keyed on `status === "found"`, a not_found record that had kept its
       learned methods would have taken the arm below and been written to the global store as a bare
       `{status}`, which is the drop this change exists to end, one layer up. */
    if (v.doc) {
      // Merge pageUrls and frameOrigins Sets with existing global entry
      var _existingGDoc = globalStore.discoveryDocs.get(k);
      var _mergedPageUrls = new Set(_existingGDoc?.pageUrls || []);
      if (v.pageUrls) for (var _pu of v.pageUrls) _mergedPageUrls.add(_pu);
      var _mergedFrameOrigins = new Set(_existingGDoc?.frameOrigins || []);
      if (v.frameOrigins) for (var _fo of v.frameOrigins) _mergedFrameOrigins.add(_fo);
      /* NO `method` ON THE WAY UP EITHER. No tab-side producer has ever written one (lib/learn.js,
         lib/grouping.js, lib/discovery-probe.js and lib/response-decode.js write none), so this copied
         `undefined` on every merge into a field no surface reads. */
      /* AND `grouping` IS READ, NOT ANSWERED FOR. `v.grouping || null` stood below and was the first of FIVE
         copies of that line — the moat merge, the IDB serialize, lib/serialize.js's two projections — none of
         which could tell a producer that stated no rule from a producer that stated nothing. Four of the five
         were a `||` on a fresh object literal, which is why the field gate saw one of them and the contract
         looked one fifth as broken as it was. Every producer now states it; this hop checks the statement it
         is about to copy, so the copier cannot be the place it goes missing. */
      checkDiscoveryGrouping(v, "lib/merge.js merging a tab's discovery doc into the moat, service " +
                                JSON.stringify(k));
      globalStore.discoveryDocs.set(k, {
        status: v.status,
        url: v.url,
        apiKey: v.apiKey,
        fetchedAt: v.fetchedAt,
        doc: _mergeDocInto(_existingGDoc && _existingGDoc.doc, v.doc, tab.documentId) || null,   // UNION, not replace: keep every page's methods
        grouping: v.grouping,
        pageUrls: _mergedPageUrls,
        frameOrigins: _mergedFrameOrigins,
        isVirtual: !!v.isVirtual,
      });
    } else if (!globalStore.discoveryDocs.has(k)) {
      // An outcome-only record: the published fetch's verdict and no method surface. No rule is recorded for
      // this bucket, stated rather than left absent — lib/persistence.js serializes THIS object to IndexedDB.
      globalStore.discoveryDocs.set(k, { status: v.status, grouping: null });
    }
  }
  for (const [k, v] of tab.probeResults) {
    globalStore.probeResults.set(k, v);
  }
  for (const [k, v] of tab.scopes) {
    globalStore.scopes.set(k, v);
  }
  if (tab._securityFindings) {
    // Evict prior findings whose URL has the same origin+pathname as a
    // script in this round but a different query/hash. Versioned asset
    // URLs (`index.js?v=14` vs `?v=15`) otherwise accumulate forever
    // because each version is keyed separately even though only the
    // latest bundle is live.
    var newBasePaths = new Set();
    for (var _spi = 0; _spi < tab._securityFindings.length; _spi++) {
      try {
        var _u = new URL(tab._securityFindings[_spi].sourceUrl);
        newBasePaths.add(_u.origin + _u.pathname);
      } catch (_) { /* a source address `new URL` cannot parse — no base path to match. It is NOT the
                       `"unknown_N"` this comment used to name: there is no such source (see the key below). */ }
    }
    var staleKeys = [];
    for (var _key of globalStore.securityFindings.keys()) {
      try {
        var _ku = new URL(_key);
        var _kbase = _ku.origin + _ku.pathname;
        if (newBasePaths.has(_kbase)) {
          // Same base path AND full URL changed → new version replaces old.
          var sameUrl = tab._securityFindings.some(function(_f) { return _f.sourceUrl === _key; });
          if (!sameUrl) staleKeys.push(_key);
        }
      } catch (_) { /* skip non-URL keys */ }
    }
    for (var _ski = 0; _ski < staleKeys.length; _ski++) {
      globalStore.securityFindings.delete(staleKeys[_ski]);
    }

    /* THE KEY IS THE SOURCE ADDRESS, NOT A NAME THIS LOOP MAKES UP FOR IT. `finding.sourceUrl || ("unknown_" +
       sf)` stood here, and bridge.js's own comment at the field it defaults says exactly what that cost when
       it could fire: a message that had gone silent "would have produced findings filed under a made-up name
       beside real ones, which is the FABRICATED half of the defaulted-field defect rather than the concealed
       half". It cannot fire — `linesToAnalysis` DCHECKs `msg.sourceUrl` as a non-empty string before it builds
       the analysis, and the finding is minted from that one field a few hundred lines up — so what the `||`
       did was stand between a broken relay and the abort that would have named it, while an ordinal that is
       meaningless across two merges of one tab (§AN-INDEX-NAMES-A-THING-ONLY-WHILE-THE-SET-IS-FIXED) went
       into the key of the cumulative store. Asserted at the record instead, through the one shape
       lib/store-record.js declares, which is the same statement the IndexedDB door asks of it a session later. */
    for (var sf = 0; sf < tab._securityFindings.length; sf++) {
      var finding = tab._securityFindings[sf];
      checkStoreRecord("securityFindings", finding.sourceUrl, finding,
                       "lib/merge.js filing this document's @S findings into the moat");
      globalStore.securityFindings.set(finding.sourceUrl, finding);
    }
  }
  scheduleSave();
}

// Incremented every time the store is wiped (the bin/Clear reset). An analysis
// or its async continuation (the worker round-trip, a source-map re-merge, a
// resume merge) captures the epoch when it starts and bails before writing to
// the store if the epoch has moved — so work already in flight when Clear ran
// can't repopulate the just-wiped store. Eviction-agnostic (unlike a buffer
// check), so it never suppresses a legitimate post-eviction resume merge.
var _dataEpoch = 0;
