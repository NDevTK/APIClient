// lib/merge.js — Engine-result -> doc-model merge. Takes the engine's @RESULT (fetch call-sites, computed
// values, security sinks, param constraints) and merges it into a tab's Value-Driven-Discovery doc model
// (methods, params, schemas, findings). Extracted from the offscreen-brain.js monolith (one problem per file);
// loaded before it, resolves learnFromAstCallSite (lib/learn.js) + grouping/schema at call-time.

function mergeASTResultsIntoVDD(tab, results, tabId, isPartial) {
  for (var r = 0; r < results.length; r++) {
    var analysis = results[r];
    var sourceHost = "";
    try { sourceHost = new URL(analysis.sourceUrl).hostname; } catch (_) {}

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
         four keys per record — method, url, params[{name,location,validValues[]}], and headers ONLY when it observed
         one — so those are the fields this loop and learnFromAstCallSite may read, and each one is guaranteed
         rather than defaulted. `params` is always present (possibly empty), which is why the `|| []` that
         stood at each use is gone: an absent array here would mean the engine's serializer changed shape, and
         that must crash rather than register every endpoint of the run as parameterless. */
      DCHECK(callSite && typeof callSite === "object", "a fetchCallSites entry is not an object — endpoint.c emits one JSON object per deduped endpoint");
      DCHECK(typeof callSite.method === "string" && callSite.method, "a fetchCallSites entry carries no method — endpoint_record takes it as a required argument, so an absent one is the @H serializer broken");
      DCHECK(Array.isArray(callSite.params), "a fetchCallSites entry carries no params array — endpoint.c writes \"params\":[…] for every endpoint (empty when the request carried no templated path segment, no query and no readable body), so its absence is the whole parameter surface arriving as nothing");
      try {
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

        var isDynamic = /^\$\{|^\(dynamic\)|^\{[a-zA-Z]/.test(callSite.url);
        var csUrl = null;
        var interfaceName = null;

        if (isDynamic) {
          if (!sourceHost) continue;
          interfaceName = sourceHost;
        } else if (/^https?:\/\//i.test(callSite.url)) {
          csUrl = new URL(callSite.url);
          interfaceName = extractInterfaceName(csUrl);
        } else {
          // A relative fetch URL resolves against the PAGE's origin at
          // runtime, NOT the script's host. Using analysis.sourceUrl as
          // the base misattributes cross-origin-hosted scripts: e.g.
          // `fetch('/svc/shreddit/graphql')` in a script served from
          // www.redditstatic.com actually hits www.reddit.com (the
          // page origin). Prefer the tab's page URL when available.
          var _csMeta = tab;
          var _csBaseForRel = (_csMeta && _csMeta.url) ? _csMeta.url : analysis.sourceUrl;
          csUrl = new URL(callSite.url, _csBaseForRel);
          interfaceName = extractInterfaceName(csUrl);
        }

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
        var bundleId = analysis.sourceUrl ? analysis.sourceUrl.replace(/^https?:\/\//, "").slice(-60) : "";
        // __feUrlShape renders an opaque path segment as {id}; the worker's
        // ep() emits that, but new URL() (csUrl) re-encodes the braces to
        // %7B/%7D. Decode so the learned endpoint path keeps the OpenAPI
        // template ({id}) instead of %7Bid%7D — used for BOTH the dedup key
        // and the stored path so they stay consistent.
        var _csPath = _decHoles(csUrl.pathname);
        /* THE DYNAMIC KEY NO LONGER NAMES A FUNCTION, because nothing has ever named one. This term read
           `callSite.enclosingFunction || "anon"`, and endpoint.c emits no such field — every dynamic endpoint
           this project has keyed since the engine took over @H has been keyed on the literal "anon", so the
           term distinguished nothing while reading like the one thing that would tell two dynamic sites in
           one bundle apart. The index `fc` is what actually separates them. (Keys already carrying " anon "
           in a persisted store do not match the new shape and re-register once, which is correct: they are
           records of an identity that was never real.) */
        var epKey = isDynamic
          ? "AST DYN " + bundleId + " " + callSite.method + " " + fc
          : "AST " + callSite.method + " " + csUrl.hostname + _csPath;   // include HOST: an endpoint is method+host+path. Path-only collapsed same-path endpoints across DIFFERENT hosts (and across sites in the cumulative store) → lost the moat's "many sites per session" surface. Mirrors the network key (method + hostname + pathname).
        // DEDUP by STRUCTURAL identity: the SAME endpoint driven with opaque-POSITIONAL args ({arg0}, from
        // __hostDrive's JS-side drive) and with NAMED args ({id}, from the grind's declared-name drive) yields
        // TWO keys for ONE endpoint (verified: spa_gated 5 raw / 4 distinct). Collapse {..} path-param segments
        // to a structural key; on collision keep the DECLARED-name record over the positional one. Only
        // {placeholder} segments normalize, so genuinely-distinct endpoints (differing elsewhere) never merge.
        // The stored KEY stays the raw path, so probe/replay of the surviving record are unaffected.
        if (!isDynamic) {
          if (!tab._epNorm) tab._epNorm = new Map();
          var _structKey = "AST " + callSite.method + " " + csUrl.hostname + _csPath.replace(/\{[^}]*\}/g, "{}");
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
        }
        if (!tab.endpoints.has(epKey)) {
          var _epMeta = tab;
          tab.endpoints.set(epKey, {
            // new URL().href percent-encodes shape holes ({} -> %7B%7D); decode so the endpoint URL keeps
            // the canonical `{}` param placeholder the dedup/UI recognize (path is already decoded via _csPath).
            url: isDynamic ? callSite.url : _decHoles(csUrl.href),
            method: callSite.method,
            host: isDynamic ? sourceHost : csUrl.hostname,
            path: isDynamic ? callSite.url : _csPath,
            service: interfaceName,
            source: isDynamic ? "ast_dynamic" : "ast_analysis",
            pageUrl: _epMeta ? _epMeta.url : null,
            /* AST-captured required headers (the SET the bundle attached at the host edge, per-header
               literal/opaque) — transport metadata the Send panel shows so the endpoint is actually usable.
               ABSENCE IS THE POSITIVE STATEMENT, and endpoint.c states it in those words: it writes the
               `headers` key ONLY when `e->nh`, because "an endpoint with no learned header must not claim an
               empty requirement, which reads as 'needs nothing' rather than 'nothing was observed'". So a
               missing key is "nothing was observed" and becomes null here, and a PRESENT one is asserted to
               be the non-empty record it can only be. */
            requiredHeaders: callSite.headers === undefined ? null : astHeaderRecord(callSite.headers),
            /* THE PATH-PARAM EXAMPLES, FROM THE RECORD THAT ACTUALLY HOLDS THEM. The rich per-doc method
               schema is EVICTED after review, so without a copy on the flat endpoint the cumulative moat
               loses the real learned values — the whole point of the tool. It reads the METHOD lib/learn.js
               just registered, which now has TWO producers of a `location:"path"` parameter: endpoint.c
               emits one per `{hole}` the forced execution interpolated into the address, carrying the
               segment its concolic example computed, and the templated reconcile below folds in the concrete
               segment of a live request that matched the template — the "orgId=acme-42" case. (Reading
               `callSite.params` for `p.location === "path"` directly would work now that the producer states
               it, and is still the wrong source: the values a live request taught belong to the method and
               never reach the call-site record.) */
            pathParams: (function () {
              if (!_learned.method || !_learned.method.parameters) return null;   // dynamic URL: no method, no path template
              var out = [];
              for (var _pn in _learned.method.parameters) {
                var _pd = _learned.method.parameters[_pn];
                if (!_pd || _pd.location !== "path") continue;
                var _vals = Array.isArray(_pd._astValidValues) ? _pd._astValidValues : [];
                if (!_vals.length) continue;   // a templated hole nothing has filled yet is not an example
                out.push({ name: _pn, values: _vals.slice(0, 20) });
              }
              return out.length ? out : null;
            })(),
            /* (No request body on THIS record. The body surface lands in the doc model: endpoint.c reads the
               request's own payload and lib/learn.js files its fields as `doc.schemas[…Request]` with
               `m.request.$ref` pointing at them, which is what lib/send.js's `requestBody` and the OpenAPI
               export already resolve. A flat copy here would be the eviction argument above applied to the
               body, and it needs a reader in lib/send.js first — that file projects only the ten fields this
               `endpoints.set` writes, deliberately.) */
            firstSeen: Date.now(),
          });
          newEndpoints++;
        }
      } catch (mergeErr) {
        /* AN INVARIANT ABORT TRAVELS ON THROUGH THIS CATCH. It has a real job — one malformed call site must
           not cost the other N-1 their registration — but everything it wraps calls into lib/learn.js,
           lib/grouping.js and lib/schema.js, every one of which DCHECKs its own contract, and a DCHECK is a
           THROW on this side (extension/check.js). Without this line the loudest assertion in the zone
           degraded to a console.debug and the run continued as if that endpoint were merely malformed. */
        RETHROW_FATAL(mergeErr);
        console.debug("[AST:merge] Error processing fetch site %d (%s %s): %s", fc, callSite.method, callSite.url, mergeErr.message || mergeErr);
      }
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

       (testing/test-spec.js asserts `result.domEndpoints` is populated, in two tests. They are assertions
       about the same absent capability and they are not this file's to change; they are the record that the
       scan was intended, and the place to build it is the engine's Lexbor tree, where the attribute values
       are.) */

    /* THE ONE SECURITY MERGE. `!analysis._securityMerged` guarded this, and the flag's other writer was
       lib/analyze.js pre-empting this block to do its own per-script split — a split over `sink.location`,
       which solve.c does not emit, so it filed every finding under the page URL and produced exactly what
       this block produces. That file is deleted and so is the flag: the replace-then-push below is already
       idempotent for a repeated merge of one analysis, which is all the flag ever bought.
       `dangerousPatterns` is NOT a defaulted engine field — bridge.js emits it as a host-side constant `[]`
       (the engine has no such surface), so it is read here as the empty statement it is. */
    var secSinks = analysis.securitySinks;
    var dangerousPats = analysis.dangerousPatterns;
    DCHECK(Array.isArray(secSinks) && Array.isArray(dangerousPats),
           "an analysis reached the security merge without its finding arrays — bridge.js builds both on " +
           "every result document, so an absent one is that relay broken and this page would merge as clean");
    if (secSinks.length || dangerousPats.length) {
      if (!tab._securityFindings) tab._securityFindings = [];
      var _mfMeta = tab || null;
      // REPLACE any prior entry for this source, don't append — the deep grind
      // streams partials each carrying the GROWING accumulated securitySinks for
      // the same sourceUrl, so appending would pile up snapshots (mergeToGlobal
      // set()s by sourceUrl so globalStore stays correct, but the tab array would
      // leak). Keep the latest (most complete) per source.
      for (var _sfx = tab._securityFindings.length - 1; _sfx >= 0; _sfx--)
        if (tab._securityFindings[_sfx].sourceUrl === analysis.sourceUrl) tab._securityFindings.splice(_sfx, 1);
      tab._securityFindings.push({
        sourceUrl: analysis.sourceUrl,
        pageUrl: _mfMeta ? _mfMeta.url : null,
        securitySinks: secSinks,
        dangerousPatterns: dangerousPats,
      });
      console.debug("[AST:merge] Security findings for %s: %d sinks, %d dangerous patterns",
        analysis.sourceUrl, secSinks.length, dangerousPats.length);
    }
  }
  // Schedule the eviction sweep after this merge: the doc's forced-exec run has produced
  // results (globalStore updated, residue parked to IDB), so once it is no longer in-flight
  // the sweep drops its transient RAM view. Debounced to one pending timer.
  _scheduleEvictSweep();
}

// ─── Message Handling ────────────────────────────────────────────────────────

// ─── Form Metadata Processing ─────────────────────────────────────────────

// ── Cross-tab / global aggregation (host-level per SECURITY.md: the brain aggregates results from the
//    per-page engines into the ONE global moat) ──
function _mergeDocInto(existingDoc, newDoc) {
  if (!existingDoc || !existingDoc.resources) return newDoc || existingDoc || null;
  if (!newDoc || !newDoc.resources) return existingDoc;
  for (const bk in newDoc.resources) {
    const nb = newDoc.resources[bk];
    if (!nb || !nb.methods) continue;
    let eb = existingDoc.resources[bk];
    if (!eb) { existingDoc.resources[bk] = nb; continue; }
    if (!eb.methods) eb.methods = {};
    for (const mk in nb.methods) {
      const nm = nb.methods[mk], em = eb.methods[mk];
      if (!em) { eb.methods[mk] = nm; continue; }   // distinct endpoint from another page -> keep both
      if (nm.parameters) {                          // same method key: union each param's example values
        em.parameters = em.parameters || {};
        for (const pn in nm.parameters) {
          const np = nm.parameters[pn], ep = em.parameters[pn];
          if (!ep) { em.parameters[pn] = np; continue; }
          const ev = Array.isArray(ep._astValidValues) ? ep._astValidValues : [];
          const nv = Array.isArray(np._astValidValues) ? np._astValidValues : [];
          for (const x of nv) if (ev.indexOf(x) < 0) ev.push(x);
          if (ev.length) ep._astValidValues = ev;
        }
      }
    }
  }
  if (newDoc.schemas) { existingDoc.schemas = existingDoc.schemas || {}; for (const sk in newDoc.schemas) if (!existingDoc.schemas[sk]) existingDoc.schemas[sk] = newDoc.schemas[sk]; }
  return existingDoc;
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
        origin: v.origin,
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
    if (ge && (ge.pathParams || v.pathParams)) {
      var _mp = new Map();
      for (var _s of [ge.pathParams, v.pathParams]) {
        // Absent on a side that has no filled path hole: this file writes null for that, and a record
        // stored before the field existed carries nothing at all. Both mean "no examples from here".
        if (!_s) continue;
        for (var _pp of _s) {
          var _cur = _mp.get(_pp.name) || new Set();
          for (var _val of _pp.values) _cur.add(_val);
          _mp.set(_pp.name, _cur);
        }
      }
      v.pathParams = Array.from(_mp, function (e) { return { name: e[0], values: Array.from(e[1]).slice(0, 20) }; });
    }
    globalStore.endpoints.set(k, v);
  }
  for (const [k, v] of tab.discoveryDocs) {
    if (v.status === "found") {
      // Merge pageUrls and frameOrigins Sets with existing global entry
      var _existingGDoc = globalStore.discoveryDocs.get(k);
      var _mergedPageUrls = new Set(_existingGDoc?.pageUrls || []);
      if (v.pageUrls) for (var _pu of v.pageUrls) _mergedPageUrls.add(_pu);
      var _mergedFrameOrigins = new Set(_existingGDoc?.frameOrigins || []);
      if (v.frameOrigins) for (var _fo of v.frameOrigins) _mergedFrameOrigins.add(_fo);
      globalStore.discoveryDocs.set(k, {
        status: v.status,
        url: v.url,
        method: v.method,
        apiKey: v.apiKey,
        fetchedAt: v.fetchedAt,
        doc: _mergeDocInto(_existingGDoc && _existingGDoc.doc, v.doc) || null,   // UNION, not replace: keep every page's methods
        grouping: v.grouping || null,
        pageUrls: _mergedPageUrls,
        frameOrigins: _mergedFrameOrigins,
        isVirtual: !!v.isVirtual,
      });
    } else if (!globalStore.discoveryDocs.has(k)) {
      globalStore.discoveryDocs.set(k, { status: v.status });
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
      } catch (_) { /* non-URL source (e.g. "unknown_N") — no base path to match */ }
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

    for (var sf = 0; sf < tab._securityFindings.length; sf++) {
      var finding = tab._securityFindings[sf];
      globalStore.securityFindings.set(finding.sourceUrl || ("unknown_" + sf), finding);
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
