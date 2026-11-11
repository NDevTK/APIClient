// lib/merge.js — merges the engine's @RESULT (fetch call sites, computed values, security sinks, param
// constraints) into a document's doc model (methods, params, schemas, findings), and a document's model into
// the cumulative moat. Loaded before offscreen-brain.js; resolves learnFromAstCallSite (lib/learn.js) and the
// grouping and schema helpers at call time.

/* One pool of a method's templated-hole examples, projected onto the flat endpoint record. `poolField` is the
   method parameter's own field name, so the two projections differ only in the pool they read. lib/learn.js
   writes a pool's key only where the pool is non-empty, and that spelling is normalised here. A hole with an
   empty pool is skipped, because lib/send.js requires every entry to carry a value. Returns null for no
   entries. */
function _astPathParamPool(method, poolField) {
  DCHECK(poolField === "_astValidValues" || poolField === "_astForcedValues",
         "a path-param pool was projected from `" + poolField + "`, which is neither of lib/learn.js's two " +
         "pools — the offerable/forced split is spelled as those two field names (lib/endpoint-record.js's " +
         "`provenanceOffersExample` states why it is a name and not a grade), so a third one reads a field " +
         "the method record does not have and every hole would project as empty");
  /* A site learnFromAstCallSite refused has no method, and a probed discovery-document method, which is a
     third party's bytes, may carry no `parameters`. Both project nothing. */
  if (!method || !method.parameters) return null;
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

/* The drive mark projected beside `pathParams`: per hole, the drive-marked values among the ones
   `_astPathParamPool` copied, so the mark stays a subset of the capped offerable list. */
function _astPathParamDrivenPool(method) {
  const offered = _astPathParamPool(method, "_astValidValues");
  if (offered === null) return null;
  const out = [];
  for (const pp of offered) {
    const marks = method.parameters[pp.name]._astDrivenValues;   // lib/learn.js omits the key when empty
    if (marks === undefined) continue;
    DCHECK(Array.isArray(marks), "a path parameter's `_astDrivenValues` is neither omitted nor a list");
    const vals = pp.values.filter((v) => marks.indexOf(v) >= 0);
    if (vals.length) out.push({ name: pp.name, values: vals });
  }
  return out.length ? out : null;
}

/* One method's templated-hole domains (exclusions, bounds, predicates, loose equalities), projected onto the
   flat endpoint record from the same method parameters `_astPathParamPool` reads. It is a separate walk
   because the skip rules differ: a hole with values and no domain belongs in the pool only, and a hole with a
   domain and no value belongs here only. Each claim is copied only where it survived: lib/learn.js spells a
   disproved exclusion, predicate or loose equality as the empty array and a disproved interval as `null`, and
   this record spells both by omitting the key, so the tests are presence and non-emptiness, not truthiness. */
function _astPathParamDomains(method) {
  if (!method || !method.parameters) return null;
  var out = [];
  for (var pn in method.parameters) {
    var pd = method.parameters[pn];
    if (!pd || pd.location !== "path") continue;
    var e = { name: pn };
    if (Array.isArray(pd._excludedValues) && pd._excludedValues.length) e.excludes = pd._excludedValues.slice();
    if (pd._bounds !== null && pd._bounds !== undefined && Object.keys(pd._bounds).length)
      e.bounds = Object.assign({}, pd._bounds);
    if (Array.isArray(pd._predicates) && pd._predicates.length) e.predicates = pd._predicates.slice();
    if (Array.isArray(pd._looselyEquals) && pd._looselyEquals.length) e.looselyEquals = pd._looselyEquals.slice();
    if (Object.keys(e).length === 1) continue;   /* the name alone is not a claim — see the record's own check */
    out.push(e);
  }
  return out.length ? out : null;
}

/* Merges each analysis in `results` into the document view `tab`: registers the @H call sites as methods and
   flat endpoint records, and files the @S findings. */
function mergeASTResultsIntoVDD(tab, results) {
  for (var r = 0; r < results.length; r++) {
    var analysis = results[r];

    /* A record with no engine document (a crashed instance, a page with nothing to run) states that the run
       observed nothing: bridge.js writes `fetchCallSites`, `securitySinks` and `_park` only where an @RESULT
       document arrived. There is nothing to merge. */
    if (!analysisHasDocument(analysis)) continue;

    /* `tab.probeResults` is written by the systems that probe (lib/req2proto.js through
       lib/discovery-probe.js, and lib/response-decode.js). The engine issues no request, so its result
       carries none to relay. */

    /* Every engine-authored name on `analysis` comes from solver/result.c's one composition; the rest are
       host-side fields bridge.js's `linesToAnalysis` writes. Parameter values reach the model per call site,
       through learnFromAstCallSite and `callSite.params[i].validValues`; nothing is matched to a parameter
       by name across the bundle. */

    // Registers each observed fetch call site as a method on its service, through learnFromAstCallSite.
    var newEndpoints = 0;
    for (var fc = 0; fc < analysis.fetchCallSites.length; fc++) {
      var callSite = analysis.fetchCallSites[fc];
      /* The call-site contract, asserted where it arrives. A record states `method`, `url`, `provenance` and a
         `params` array (possibly empty) unconditionally; `headers` is written only where the run observed
         one. A param states `name`, `location`, `validValues` and `valueClass` unconditionally, plus one key
         per kind of gate that held on every observed path to the request (an equality's false arm, an
         ordering's interval, a method call, a loose equality), each omitted where that kind proved nothing.
         `valueClass` says whether those omissions mean "proved nothing" or "the value named no hole to look
         a gate up by". The authority is the producer: solver/endpoint.h sketches the record, and
         lib/learn.js's `AST_PARAM_KEYS` crashes on a param key no reader there asks for. Nothing is
         defaulted: an absent optional key is a positive statement, so it is read with an `in` test and never
         with `||`. */
      DCHECK(callSite && typeof callSite === "object", "a fetchCallSites entry is not an object — endpoint.c emits one JSON object per deduped endpoint");
      DCHECK(typeof callSite.method === "string" && callSite.method, "a fetchCallSites entry carries no method — endpoint_record takes it as a required argument, so an absent one is the @H serializer broken");
      DCHECK(Array.isArray(callSite.params), "a fetchCallSites entry carries no params array — endpoint.c writes \"params\":[…] for every endpoint (empty when the request carried no templated path segment, no query and no readable body), so its absence is the whole parameter surface arriving as nothing");
      /* What the call site is evidence of. endpoint.c writes the word on every record, so there is no absence
         to interpret, and a default here would render a forced address at the grade of a computed one. */
      DCHECK(isCallSiteProvenance(callSite.provenance),
             "a fetchCallSites entry carries the provenance " + JSON.stringify(callSite.provenance) +
             ", which is none of " + CALLSITE_PROVENANCE.join("/") + " — endpoint.c writes one on every " +
             "record through solver/engine.h's one mapping, so this is that emission and lib/endpoint-record" +
             ".js's vocabulary having parted, and every learned address would be read at the wrong grade");
      /* …and whose act the sighting was, a boolean endpoint.c writes on every record beside the grade. */
      DCHECK(typeof callSite.driven === "boolean",
             "a fetchCallSites entry carries the drive mark " + JSON.stringify(callSite.driven) + " — " +
             "endpoint.c writes `driven` as a boolean on every record, so this is that emission and this " +
             "zone having parted");
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

      /* What the code determined about this address (lib/callsite-url.js, over endpoint.c's template grammar).
         A relative address resolves against the page's origin and not the script's host, so the base is the
         document's url, and the analysis sourceUrl only when the view has none. An address whose origin is a
         shape is registered like any other, filed under that shape and never under the script's host: a
         domain-annotated shape is a first-class output. */
      var _addr = astCallSiteAddress(callSite.url, (tab && tab.url) ? tab.url : analysis.sourceUrl);
      var interfaceName = _addr.originKnown ? extractInterfaceName(_addr.url) : _addr.host;

      var _learned = learnFromAstCallSite(tab, interfaceName, callSite, analysis.sourceUrl);
      DCHECK(_learned && typeof _learned === "object" && "entry" in _learned && "method" in _learned,
             "learnFromAstCallSite did not answer with its {entry, method} pair — both halves are read " +
             "below and a missing one silently files this endpoint under the wrong service or drops the " +
             "path-param examples it just learned");
      /* The pair is all-or-nothing: a refused site answers null for both, and an accepted one registers a
         method whatever its address's origin. A live entry with no method would drop every parameter the
         call site computed, since the only walk over `callSite.params` is in learnFromAstCallSite. */
      DCHECK((_learned.entry === null) === (_learned.method === null),
             "learnFromAstCallSite answered with half a pair (entry " + (_learned.entry ? "present" : "null") +
             ", method " + (_learned.method ? "present" : "null") + ") — a site it refuses answers null for " +
             "BOTH, and a site it accepts registers a method whatever its address's origin turned out to be, " +
             "so a live entry with no method is the shape-origin arm's early return having come back and " +
             "every parameter this call site computed is about to be dropped between the engine and the moat");
      // Refine interfaceName for endpoint registration if the call site
      // got promoted to a prefix bucket via observed-prefix clustering.
      if (_learned.entry && _learned.entry.doc && _learned.entry.doc.name) {
        interfaceName = _learned.entry.doc.name;
      }

      // Register endpoint for popup display — separate concern from
      // method registration (endpoint list shows "what fetches exist
      // on this page," method list shows "what API endpoints we know").
      /* One key shape for both kinds of address: method + host + path, where `host` is the origin as far as
         the code determined it, so a shape-origin record dedups like a literal one. lib/endpoint-record.js
         mints the key; it is not spelled here. */
      var epKey = endpointKeyFromParts(callSite.method, _addr.host, _addr.path,
                                       "lib/merge.js registering an @H call-site endpoint");
      // Dedup by structural identity. One endpoint driven with positional args ({arg0}) and with declared
      // names ({id}) yields two keys, so placeholder path segments collapse to a structural key and, on a
      // collision, the declared-name record is kept over the positional one. Only placeholder segments
      // normalize. The stored key stays the raw path, so probe and replay of the surviving record are
      // unaffected.
      if (!tab._epNorm) tab._epNorm = new Map();
      /* The structural key is the same name over a hole-normalized path, minted by the same function: its
         values in `tab._epNorm` are endpoint keys and are compared against them. */
      var _structKey = endpointKeyFromParts(callSite.method, _addr.host,
                                            _addr.path.replace(/\{[^}]*\}/g, "{}"),
                                            "lib/merge.js building the structural dedup key");
      var _posRe = /\{arg\d+\}/;
      var _priorKey = tab._epNorm.get(_structKey);
      /* The record the upgrade branch deletes, held so its observations fold forward: the positional and the
         declared record are one endpoint driven two ways. `null` means this call site upgraded nothing. */
      var _upgradedFrom = null;
      if (_priorKey && _priorKey !== epKey && tab.endpoints.has(_priorKey)) {
        if (_posRe.test(_priorKey) && !_posRe.test(epKey)) {
          _upgradedFrom = tab.endpoints.get(_priorKey);
          tab.endpoints.delete(_priorKey); tab._epNorm.set(_structKey, epKey);   // prior positional, new declared -> upgrade (add epKey below)
        } else {
          epKey = _priorKey;   // keep prior (declared/equal); has() below is true -> skip add, no dup
        }
      } else {
        tab._epNorm.set(_structKey, epKey);
      }
      /* The record's key, carried onto the learned method: the Send panel selects by (service, methodId), so
         it reaches the flat record through the method or not at all. The key is carried and not recomposed
         from the method's `origin` and `path`, which do not hold the key's hostname and leading-slash path.
         First writer wins, because a method is created by the first call site to reach its name and `m.path`
         is that call site's. Where several records share one method name, the rest are unreachable from the
         panel. */
      if (_learned.method && _learned.method._endpointKey === undefined) {
        _learned.method._endpointKey = epKey;
      }
      /* The record is built for every sighting and the map decides what happens to it. The engine emits one
         @H row per (method, path, provenance, param-set) (engine/host/solver/endpoint.c's `same_identity`)
         and this key is method + host + path, so one address called at two grades, or with and without a
         query, arrives as several rows; each later row folds into the held record by
         `_foldEndpointRecordInto`. `_astPathParamPool` reads the method lib/learn.js has already merged this
         sighting into, so the fold carries the longer pools forward. lib/endpoint-record.js is the only
         description of the record: its constructor rejects a name the record does not carry. */
      var _rec = makeEndpointRecord({
            /* The address the parser resolved. lib/callsite-url.js masks a hole's name for the `new URL` parse,
               which would otherwise truncate or percent-encode it, so its `url.href` is not the address; its
               `href` is. */
            url: _addr.originKnown ? _addr.href : callSite.url,
            method: callSite.method,
            host: _addr.host,
            path: _addr.path,
            service: interfaceName,
            source: _addr.originKnown ? "ast_analysis" : "ast_shape_origin",
            /* `null` is this record's one spelling of "no document address was known". An `undefined` would be
               dropped by chrome.runtime.sendMessage and reach the popup as a missing key. */
            pageUrl: tab.url === undefined ? null : tab.url,
            /* The headers the bundle attached, each a literal or opaque. endpoint.c writes the `headers` key only
               where one was learned, so a missing key is "nothing was observed" and becomes null, and a
               present one is asserted to be the non-empty record it can only be. */
            requiredHeaders: callSite.headers === undefined ? null : astHeaderRecord(callSite.headers),
            /* The request body, in whichever kind the run observed; the key the engine chose says which.
               solver/endpoint.c writes `bodyBase64` only for bytes the request sends and `bodyShape` only for
               the display spelling of a body built from unknown external input, never both. An omitted key
               is "no body of this kind was observed" and becomes null. `bodyMime` is carried into whichever
               field is set: it is always present with `bodyBase64` and may be absent with a shape, where
               Fetch §5.2's string arm supplies a type the page never chose. These keys are recorded where
               the engine could name no field of the body; named fields reach the doc model through
               lib/learn.js. */
            bodySent: callSite.bodyBase64 === undefined ? null
                    : { mime: callSite.bodyMime, base64: callSite.bodyBase64 },
            bodyShape: callSite.bodyShape === undefined ? null
                     : { mime: callSite.bodyMime === undefined ? null : callSite.bodyMime,
                         shape: callSite.bodyShape },
            /* `bodyExampleBase64` is what a payload the page wrote byte by byte currently holds, at the byte
               ranges the `body[off:end]` params of this call site name, so it arrives beside `params` and
               not instead of them. Its `bodyMime` may be absent: §5.2 BodyInit unions gives a BufferSource
               no Content-Type. */
            bodyExample: callSite.bodyExampleBase64 === undefined ? null
                       : { mime: callSite.bodyMime === undefined ? null : callSite.bodyMime,
                           base64: callSite.bodyExampleBase64 },
            /* The path-param examples, read from the method lib/learn.js just registered. It has two producers
               of a `location:"path"` parameter: endpoint.c's `{hole}` examples, and the templated
               reconcile's concrete segment of a live request that matched the template, which never reaches
               the call-site record. The flat record must be complete because it is what `netdiff --unused`
               counts, what crosses to the popup, and what lib/send.js falls back to when the resolved schema
               does not declare the hole. The forced pool is its own field: lib/send.js takes a `pathParams`
               entry's `values[0]` as the prefilled example, so a hole filled only on a forced arm must not
               appear there; lib/popup-form.js renders `pathParamsForced` on its own row. */
            pathParams: _astPathParamPool(_learned.method, "_astValidValues"),
            pathParamsForced: _astPathParamPool(_learned.method, "_astForcedValues"),
            /* …and the drive mark over the offerable pool, kept to the values that pool projected. */
            pathParamsDriven: _astPathParamDrivenPool(_learned.method),
            /* What the gates proved each of those holes must satisfy. A hole carrying its values and not its
               domain reads as one nothing tested. It is one field for both pools because a domain is a fact
               about the hole and a grade is a fact about a value (lib/endpoint-record.js). */
            pathParamDomains: _astPathParamDomains(_learned.method),
            firstSeen: Date.now(),
      }, "lib/merge.js registering an @H fetch call site");
      /* The upgraded record's observations fold into the fresh record before the map sees either, so this
         call site's record is complete before the map decides whether it is the first. */
      if (_upgradedFrom !== null) _foldEndpointRecordInto(_rec, _upgradedFrom, epKey);
      if (!tab.endpoints.has(epKey)) {
        tab.endpoints.set(epKey, _rec);
        newEndpoints++;
      } else {
        /* A second @H row for one key is a second sighting of one address. `newEndpoints` is not raised,
           since it counts addresses this document added, but what the row observed is folded. */
        _foldEndpointRecordInto(tab.endpoints.get(epKey), _rec, epKey);
      }
      /* No catch here: a call site this zone cannot build is a broken engine↔JS contract, and it crashes. */
    }
    if (analysis.fetchCallSites.length) {
      console.debug("[AST:merge] Fetch sites: %d call sites processed, %d endpoints registered",
        analysis.fetchCallSites.length, newEndpoints);
    }


    /* The security merge. The replace-then-push below is idempotent for a repeated merge of one analysis, and
       the finding record carries only what solve.c emits. */
    var secSinks = analysis.securitySinks;
    DCHECK(Array.isArray(secSinks),
           "an analysis reached the security merge without its securitySinks array — bridge.js builds it on " +
           "every result document, so an absent one is that relay broken and this page would merge as clean");
    if (secSinks.length) {
      if (!tab._securityFindings) tab._securityFindings = [];
      // Replace any prior entry for this source: partial results each carry the accumulated securitySinks
      // for the same sourceUrl, so appending would pile up snapshots.
      for (var _sfx = tab._securityFindings.length - 1; _sfx >= 0; _sfx--)
        if (tab._securityFindings[_sfx].sourceUrl === analysis.sourceUrl) tab._securityFindings.splice(_sfx, 1);
      /* `null` means no document address was known when these sinks were observed. `getDoc` initialises a
         view's `url` to the empty string until CONTENT_SEED fills it, and the empty string is not an
         address: offscreen-brain.js's live-verify and lib/store-record.js both read `pageUrl` as the
         document to re-visit. */
      tab._securityFindings.push({
        sourceUrl: analysis.sourceUrl,
        pageUrl: tab.url ? tab.url : null,
        securitySinks: secSinks,
      });
      console.debug("[AST:merge] Security findings for %s: %d sinks", analysis.sourceUrl, secSinks.length);
    }
  }
}



// ── Cross-document aggregation: the brain merges each document's results into the one global moat
//    (SECURITY.md) ──
/* The per-parameter union, one document's parameter into the moat's. The caller copies whole a param the
   existing record lacks; a param both have keeps the existing declaration (type, location and description are
   functions of the same URL and verb) and merges the observations. */
function _mergeParamInto(ep, np) {
  /* The value pools fold by `foldValuePools` (most-observed, lib/endpoint-record.js): a value the other
     document computed without forcing is promoted out of our forced pool, and one it could only force is
     added only where no document computed it otherwise. A method parameter spells an empty pool as an omitted
     key, which is the only part spelled here. */
  const _folded = foldValuePools(
    Array.isArray(ep._astValidValues) ? ep._astValidValues : [],
    Array.isArray(ep._astForcedValues) ? ep._astForcedValues : [],
    Array.isArray(np._astValidValues) ? np._astValidValues : [],
    Array.isArray(np._astForcedValues) ? np._astForcedValues : [],
    Array.isArray(ep._astDrivenValues) ? ep._astDrivenValues : [],
    Array.isArray(np._astDrivenValues) ? np._astDrivenValues : []);
  if (_folded.valid.length) ep._astValidValues = _folded.valid;
  /* An emptied forced or driven pool is deleted, not left as `[]`. */
  if (_folded.forced.length) ep._astForcedValues = _folded.forced;
  else if (Array.isArray(ep._astForcedValues)) delete ep._astForcedValues;
  if (_folded.driven.length) ep._astDrivenValues = _folded.driven;
  else if (Array.isArray(ep._astDrivenValues)) delete ep._astDrivenValues;
  /* An example is a computed value and its absence is a statement: a param known only to satisfy a range gate
     has a shape and no example. An existing example is never overwritten, and one filled from the other
     document brings its source label with it. */
  if (ep._exampleValue === undefined && np._exampleValue !== undefined) {
    ep._exampleValue = np._exampleValue;
    ep._exampleValueSource = np._exampleValueSource;
    ep._exampleConfidence = np._exampleConfidence;
  }
  if (np._astInferred) ep._astInferred = true;
  /* The domains merge by the opposite law: a constraint is a claim about the endpoint and stays only where
     every observed path obeyed it, so exclusions intersect and an interval widens to the hull (lib/learn.js's
     rules). A document that reached the request without the constraint disproves it. Presence is tested, not
     truthiness, because a parameter no engine run touched (a form scan or a live request can create one) must
     contribute nothing while an observation that proved nothing must erase: `_excludedValues` spells "proved
     nothing" as the empty array, so being an array is the test; `_bounds` spells it as `null`, so `in` is the
     test and `null` is passed through. */
  if (Array.isArray(np._excludedValues)) intersectExcludedValues(ep, np._excludedValues);
  if ("_bounds" in np) widenBoundsInto(ep, np._bounds);
  /* The call predicates merge by the same law and spell "proved nothing" as the empty array. */
  if (Array.isArray(np._predicates)) intersectPredicates(ep, np._predicates);
  /* So do the loose equalities. */
  if (Array.isArray(np._looselyEquals)) intersectLooselyEquals(ep, np._looselyEquals);
  /* `_astValueClass` says which of two things the four silences mean: solver/endpoint.c reads all four
     domains through the param's hole key, so a value that named no hole had them skipped. It unions where
     they intersect (solver/endpoint.h's rule): "unknown" is sticky and "concrete" fills only an unstated
     slot, because a second document's literal cannot take back a hole the first one had. An absent key is a
     third state, a record learned by an engine that predates the key, and it contributes nothing. */
  if (np._astValueClass === "unknown") ep._astValueClass = "unknown";
  else if (np._astValueClass === "concrete" && ep._astValueClass !== "unknown") ep._astValueClass = "concrete";
}

/* A method's statistics summed over its contributor records, `_statsByDoc`, keyed by contributing document.
   lib/learn.js raises only the requesting document's record and re-derives `_stats` from the map, and the moat
   merge copies contributor records rather than adding totals, so re-merging a document is idempotent and two
   documents add. The sum shares no child with a contributor (lib/stats.js `mergeParamStats`), because the
   contributors go on being raised under it. `correlations` is derived over the summed distribution. */
function _sumStats(byDoc) {
  const out = { requestCount: 0, params: {}, bodyFields: {},
                body: { present: 0, decoded: 0, undecoded: 0, undecodedTypes: {}, uncounted: 0 } };
  for (const dk in byDoc) {
    const s = byDoc[dk];
    DCHECK(s && typeof s.requestCount === "number" && s.params && s.bodyFields,
           "a contributed _stats record is not the {requestCount, params, bodyFields} shape lib/learn.js " +
           "creates — the moat's per-method totals are summed straight off these, so a malformed one would " +
           "report an endpoint's traffic as a number nobody observed (document=" + dk + ")");
    out.requestCount += s.requestCount;
    for (const pn in s.params) out.params[pn] = mergeParamStats(out.params[pn], s.params[pn]);
    for (const fn in s.bodyFields) out.bodyFields[fn] = mergeParamStats(out.bodyFields[fn], s.bodyFields[fn]);
    /* This function rebuilds `out` from a fixed literal, so a name lib/learn.js adds to `_stats` is dropped
       at every global merge unless it is summed here. A contributor with no `body` census (a store written
       before the census existed) is counted as uncounted, not as zero. */
    if (s.body) {
      out.body.present += s.body.present;
      out.body.decoded += s.body.decoded;
      out.body.undecoded += s.body.undecoded;
      for (const t in s.body.undecodedTypes) {
        if (!out.body.undecodedTypes[t]) out.body.undecodedTypes[t] = 0;
        out.body.undecodedTypes[t] += s.body.undecodedTypes[t];
      }
    } else {
      out.body.uncounted++;
    }
  }
  DCHECK(out.body.present === out.body.decoded + out.body.undecoded,
         "a summed request-body census does not sum — each contributor's three counts are raised together " +
         "at one site in lib/learn.js and added here in one arm, so a disagreement means a contributor was " +
         "summed from a record whose census had already been written by something else");
  out.correlations = detectCorrelations(out);
  return out;
}

/* Merges one document's method record into the moat's. A method record is the union of what two producers
   learned about one endpoint, the bundle (lib/learn.js `learnFromAstCallSite`) and the wire
   (`learnFromRequest`), and page timing decides which reaches `globalStore` first. Identity fields are kept,
   and anything the newer record carries that the older lacks is copied as-is, never through an allowlist: a
   real discovery document's methods carry server-authored fields (scopes, parameterOrder, flatPath) this file
   does not enumerate. The exceptions: `_astInferred` is monotone, `_stats` is summed per contributing document
   (`_sumStats`), and parameters, witnesses and record facts merge by their own rules below. */
function _mergeMethodInto(em, nm) {
  DCHECK(!nm._stats || !!nm._statsByDoc,
         "a document's method reached the moat merge carrying summed statistics with no contributor records — " +
         "lib/learn.js files every request under its document and derives the sum, so this sum was written elsewhere");
  for (const k in nm) {
    if (k === "parameters" || k === "_stats" || k === "_statsByDoc" || k === "_astInferred" ||
        k === "_astWitnesses" || k === "_astFacts") continue;
    if (em[k] === undefined || em[k] === null) em[k] = nm[k];
  }
  if (nm._astInferred) em._astInferred = true;
  // The witness pool is a union across documents: each member is one sighting's statement and none is joined
  // to another.
  if (nm._astWitnesses !== undefined) {
    const where = "lib/merge.js moat-merging a method's witnesses";
    for (const p of checkWitnessPool(nm._astWitnesses, where)) {
      em._astWitnesses = witnessPoolWith(em._astWitnesses, p.provenance, p.acts, p.assumed, p.verification, where);
    }
  }
  // …and the record facts fold across documents by the rule they fold by across rows.
  if (nm._astFacts !== undefined) em._astFacts = recordFactsJoin(em._astFacts, nm._astFacts);
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

function _mergeDocInto(existingDoc, newDoc) {
  if (!existingDoc || !existingDoc.resources) return newDoc || existingDoc || null;
  if (!newDoc || !newDoc.resources) return existingDoc;
  _mergeResourcesInto(existingDoc.resources, newDoc.resources);
  if (newDoc.schemas) { existingDoc.schemas = existingDoc.schemas || {}; for (const sk in newDoc.schemas) if (!existingDoc.schemas[sk]) existingDoc.schemas[sk] = newDoc.schemas[sk]; }
  return existingDoc;
}

/* Merges resource buckets, recursing because a discovery document's resources nest and `getDocMethods`
   (popup.js) renders every level. Both levels are keyed by a name this zone did not choose (see
   lib/stranger-keyed.js): a bucket by a fetched discovery document or lib/openapi-import.js's lowercased tag,
   a method by lib/grouping.js's answer. The maps are restored from IndexedDB by `JSON.parse` and so carry
   `Object.prototype`, which is why every read and write goes through the stranger-key helpers. */
function _mergeResourcesInto(eres, nres) {
  for (const bk in nres) {
    const nb = nres[bk];
    if (!nb) continue;
    const eb = strangerKeyHeld(eres, bk) ? eres[bk] : null;
    if (!eb) { strangerKeySet(eres, bk, nb); continue; }
    if (nb.methods) {
      if (!eb.methods) eb.methods = strangerKeyedMap();
      for (const mk in nb.methods) {
        const nm = nb.methods[mk];
        /* The same name for two verbs is two endpoints. Within one document lib/learn.js resolves it by
           moving the incumbent to `<verb>_<name>`; across documents the collision first exists here, and
           the incoming method is filed under `<verb>_<name>` with the id the Send panel keys on re-derived. */
        let key = mk, em = strangerKeyHeld(eb.methods, key) ? eb.methods[key] : null;
        if (em && em.httpMethod !== nm.httpMethod) {
          key = String(nm.httpMethod).toLowerCase() + "_" + mk;
          if (typeof nm.id === "string" && nm.id.lastIndexOf(".") >= 0)
            nm.id = nm.id.slice(0, nm.id.lastIndexOf(".") + 1) + key;
          em = strangerKeyHeld(eb.methods, key) ? eb.methods[key] : null;
        }
        if (!em) { strangerKeySet(eb.methods, key, nm); continue; }   // distinct endpoint from another page -> keep both
        _mergeMethodInto(em, nm);
      }
    }
    if (nb.resources) {
      if (!eb.resources) eb.resources = strangerKeyedMap();
      _mergeResourcesInto(eb.resources, nb.resources);
    }
  }
}
/* The flat records' merge: two sightings of one address, folded by one law per field. It is called within one
   document (a second @H row for a key, an upgraded positional record) and at the moat, because the engine's
   identity is (method, path, provenance, param-set) (engine/host/solver/endpoint.h) and this zone's key is
   method + host + path. The hole pools fold by `foldValuePools`, the hole domains by lib/learn.js's
   intersection rules, and the required headers by `foldHeaderRecords`; the three body fields do not fold (the
   residual stated at `foldHeaderRecords` says what that leaves). Neither side's absence is interpreted:
   `endpointHolePairs` refuses everything but `null` and a list. The result is re-checked by `checkEndpointRecord`, since this function writes a
   record the constructor did not build. `into` survives and `from` is only read. */
function _foldEndpointRecordInto(into, from, key) {
  /* Both records are read as per-name pairs through the one walk (lib/endpoint-record.js). */
  const _a = endpointHolePairs(from, "lib/merge.js moat-folding the stored record for " + JSON.stringify(key));
  const _b = endpointHolePairs(into, "lib/merge.js moat-folding this document's record for " + JSON.stringify(key));
  const _folded = new Map();
  /* A hole only one side has still goes through the fold, against an empty pair, so it gets the same
     promotion. */
  for (const name of new Set([..._a.keys(), ..._b.keys()])) {
    const x = _a.has(name) ? _a.get(name) : _NO_HOLE_POOLS;
    const y = _b.has(name) ? _b.get(name) : _NO_HOLE_POOLS;
    _folded.set(name, foldValuePools(x.valid, x.forced, y.valid, y.forced, x.driven, y.driven));
  }
  /* The fold does not truncate: `PATH_PARAM_EXAMPLE_CAP` bounds what `_astPathParamPool` copies from a live
     method parameter, and here the operands are records with no method parameter behind them, so a slice
     would delete accumulated values (a promotion appends past a full pool). A moat record is re-derivable
     from its `pageUrl` (`endpointRecordRecipe`), so its residency is the store's value-ordered share.
     Named residual: not covered — `foldValuePools` decides membership with `indexOf`, so folding a hole's
     pools is O(n*m) in their lengths, which nothing here bounds. Next diff — membership over a Set inside
     `foldValuePools`, leaving its one-way promotion and append order untouched. Absence shows as — the
     moat-fold loop's wall time rising with the square of one address's learned-value count, on a document
     that re-visits one templated address many times. */
  const _project = (side) => {
    const out = [];
    for (const [name, p] of _folded)
      if (p[side].length) out.push({ name: name, values: p[side].slice() });
    return out.length ? out : null;
  };
  into.pathParams = _project("valid");
  into.pathParamsForced = _project("forced");
  into.pathParamsDriven = _project("driven");
  /* The domains of the same holes merge by the opposite law: a value accumulates, so the pools union; a
     domain is a claim about the address, so the four facts intersect (an interval to the hull), through the
     lib/learn.js rules `_mergeParamInto` calls. A hole only one side names is taken whole: a record that does
     not name the hole observed no path through that segment and disproves nothing, while one that names it
     without a claim reached the request without that gate. lib/endpoint-record.js spells an absent claim as
     `null` and lib/learn.js's rules spell "proved nothing" as the empty array (`null` for the interval); the
     conversion is done here. */
  const _da = endpointHoleDomains(from, "lib/merge.js moat-folding the stored record's hole domains for " +
                                        JSON.stringify(key));
  const _db = endpointHoleDomains(into, "lib/merge.js moat-folding this document's hole domains for " +
                                        JSON.stringify(key));
  const _domains = [];
  for (const name of new Set([..._da.keys(), ..._db.keys()])) {
    /* One side only: nothing to intersect against, so the claim stands as that record states it. */
    if (!_da.has(name) || !_db.has(name)) {
      const only = _da.has(name) ? _da.get(name) : _db.get(name);
      const e = { name: name };
      if (only.excludes !== null) e.excludes = only.excludes;
      if (only.bounds !== null) e.bounds = only.bounds;
      if (only.predicates !== null) e.predicates = only.predicates;
      if (only.looselyEquals !== null) e.looselyEquals = only.looselyEquals;
      _domains.push(e);
      continue;
    }
    const x = _da.get(name), y = _db.get(name);
    const t = {};
    const _list = (v) => (v === null ? [] : v);
    intersectExcludedValues(t, _list(x.excludes));
    intersectExcludedValues(t, _list(y.excludes));
    widenBoundsInto(t, x.bounds);
    widenBoundsInto(t, y.bounds);
    intersectPredicates(t, _list(x.predicates));
    intersectPredicates(t, _list(y.predicates));
    intersectLooselyEquals(t, _list(x.looselyEquals));
    intersectLooselyEquals(t, _list(y.looselyEquals));
    const e = { name: name };
    if (Array.isArray(t._excludedValues) && t._excludedValues.length) e.excludes = t._excludedValues;
    if (t._bounds !== null && t._bounds !== undefined && Object.keys(t._bounds).length) e.bounds = t._bounds;
    if (Array.isArray(t._predicates) && t._predicates.length) e.predicates = t._predicates;
    if (Array.isArray(t._looselyEquals) && t._looselyEquals.length) e.looselyEquals = t._looselyEquals;
    /* A hole whose every claim was disproved has no entry, the skip `_astPathParamDomains` makes at the
       mint. */
    if (Object.keys(e).length > 1) _domains.push(e);
  }
  into.pathParamDomains = _domains.length ? _domains : null;
  /* The headers: `into` is the prior side, so a header both sightings attached keeps its value unless the
     incoming one is a literal over an opaque. `foldHeaderRecords` refuses `{}` from either side. */
  into.requiredHeaders = foldHeaderRecords(into.requiredHeaders, from.requiredHeaders,
                                           "lib/merge.js folding two sightings' required headers, key " +
                                           JSON.stringify(key));
  checkEndpointRecord(into, "lib/merge.js folding two sightings of one address, key " +
                            JSON.stringify(key));
}

function mergeToGlobal(tab) {
  // Every analysis result funnels through here, so the DocView contract is asserted once.
  DCHECK(tab && typeof tab === "object", "mergeToGlobal: tab (DocView) must be an object");
  DCHECK(tab.apiKeys && typeof tab.apiKeys[Symbol.iterator] === "function", "mergeToGlobal: tab.apiKeys must be an iterable Map");
  DCHECK(globalStore && globalStore.apiKeys, "mergeToGlobal: globalStore must be initialized before a merge");
  for (const [k, v] of tab.apiKeys) {
    /* lib/keys.js writes `name`, the KEY_PATTERNS entry that matched, and the moat keeps it. It is asserted
       here, at the origin, because the popup would otherwise label the key generically. */
    DCHECK(typeof v.name === "string" && v.name,
           "a tab's API-key entry carries no `name` — lib/keys.js sets it from the KEY_PATTERNS entry that " +
           "matched, so an absent one means a key was recorded by something that does not know what kind of " +
           "key it is, and the moat would show it as a nameless secret");
    /* The tab side of the entry, asserted once for both arms below. */
    DCHECK(typeof v.requestCount === "number", "a tab's API-key entry carries no numeric requestCount — lib/keys.js initialises it to 0");
    for (const _f of ["services", "hosts", "endpoints", "pageUrls"]) {
      DCHECK(v[_f] instanceof Set,
             "a tab's API-key entry's `" + _f + "` is not a Set — lib/keys.js builds all four, and what this " +
             "key was seen against is the whole of what the moat knows about it");
    }
    DCHECK(Array.isArray(v.positions), "a tab's API-key entry carries no positions list — lib/keys.js mints one");
    const existing = globalStore.apiKeys.get(k);
    if (existing) {
      // Every position the key was seen sent at, in either entry.
      for (const p of v.positions)
        if (!existing.positions.some((q) => q.in === p.in && q.name === p.name)) existing.positions.push(p);
      existing.lastSeen = v.lastSeen;
      // The pattern that matched is a function of the key text, so the tab's answer replaces the stored one.
      existing.name = v.name;
      // Take the higher count — tab count is a running total, not a delta
      DCHECK(typeof existing.requestCount === "number",
             "a stored API-key entry's requestCount is not a number — this file and " +
             "_deserializeIntoGlobalStore are its only writers, and a `|| 0` here would silently reset a " +
             "key's usage total to zero");
      existing.requestCount = Math.max(existing.requestCount, v.requestCount);
      /* All four collections are Sets on both sides: lib/keys.js builds them, the arm below builds them, and
         _deserializeIntoGlobalStore reconstructs them from the stored arrays. */
      for (const _f of ["services", "hosts", "endpoints", "pageUrls"]) {
        DCHECK(existing[_f] instanceof Set,
               "a stored API-key entry's `" + _f + "` is not a Set — the union of what this key was seen " +
               "against is the moat's cross-session surface for it");
        for (const _s of v[_f]) existing[_f].add(_s);
      }
    } else {
      globalStore.apiKeys.set(k, {
        name: v.name,
        referer: v.referer,
        source: v.source,
        firstSeen: v.firstSeen,
        lastSeen: v.lastSeen,
        requestCount: v.requestCount,
        services: new Set(v.services),
        hosts: new Set(v.hosts),
        endpoints: new Set(v.endpoints),
        pageUrls: new Set(v.pageUrls),
        positions: v.positions.slice(),
      });
    }
  }
  /* The moat is monotonic: a stored record's path-param pools fold into this document's record, so a later
     paramless re-emit never drops values a prior emit learned. */
  for (const [k, v] of tab.endpoints) {
    var ge = globalStore.endpoints.get(k);
    if (ge) _foldEndpointRecordInto(v, ge, k);
    globalStore.endpoints.set(k, v);
  }
  for (const [k, v] of tab.discoveryDocs) {
    /* A tab entry reaches the moat when it carries a doc, whatever the published fetch answered (the split
       lib/serialize.js states), so a not_found record keeps its learned methods. */
    if (v.doc) {
      // Merge pageUrls and frameOrigins Sets with existing global entry
      var _existingGDoc = globalStore.discoveryDocs.get(k);
      var _mergedPageUrls = new Set(_existingGDoc?.pageUrls || []);
      if (v.pageUrls) for (var _pu of v.pageUrls) _mergedPageUrls.add(_pu);
      var _mergedFrameOrigins = new Set(_existingGDoc?.frameOrigins || []);
      if (v.frameOrigins) for (var _fo of v.frameOrigins) _mergedFrameOrigins.add(_fo);
      /* `grouping` is copied as the producer stated it and is checked first, never defaulted. */
      checkDiscoveryGrouping(v, "lib/merge.js merging a tab's discovery doc into the moat, service " +
                                JSON.stringify(k));
      globalStore.discoveryDocs.set(k, {
        status: v.status,
        url: v.url,
        apiKey: v.apiKey,
        fetchedAt: v.fetchedAt,
        doc: _mergeDocInto(_existingGDoc && _existingGDoc.doc, v.doc) || null,   // a union, not a replacement: every page's methods are kept
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
      } catch (_) { /* a source address `new URL` cannot parse has no base path to match */ }
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

    /* The key is the source address. `linesToAnalysis` asserts `msg.sourceUrl` is a non-empty string and the
       finding is minted from it, so nothing is defaulted here; the record is checked through the shape
       lib/store-record.js declares. */
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
