// lib/merge.js — Engine-result -> doc-model merge. Takes the engine's @RESULT (fetch call-sites, computed
// values, security sinks, param constraints) and merges it into a tab's Value-Driven-Discovery doc model
// (methods, params, schemas, findings). Extracted from the offscreen-brain.js monolith (one problem per file);
// loaded before it, resolves learnFromAstCallSite (lib/learn.js) + grouping/schema at call-time.

function mergeASTResultsIntoVDD(tab, results, tabId, isPartial) {
  for (var r = 0; r < results.length; r++) {
    var analysis = results[r];
    var sourceHost = "";
    try { sourceHost = new URL(analysis.sourceUrl).hostname; } catch (_) {}

    // Find matching discovery doc for this host (optional — endpoint registration works without it)
    var doc = null;
    var matchedSvc = null;
    tab.discoveryDocs.forEach(function(entry, svc) {
      if (entry.doc && svc.includes(sourceHost)) { doc = entry.doc; matchedSvc = svc; }
    });
    if (!doc) {
      globalStore.discoveryDocs.forEach(function(entry, svc) {
        if (entry.doc && svc.includes(sourceHost)) { doc = entry.doc; matchedSvc = svc; }
      });
    }

    /* NO probeResults RELAY OFF THE ENGINE RESULT. `tab.probeResults` is written by the two systems that
       actually probe — lib/req2proto.js through lib/discovery-probe.js and lib/response-decode.js — and the
       engine has no such record to relay: it never issues a request, so it never receives a rejection to read
       a schema out of. The relay that stood here (and the DCHECK on the seam in bridge.js) asserted a field
       the engine emitted, which made an engine that had learned nothing indistinguishable from a broken
       bridge. */

    // Proto field/enum merge — requires a matching doc
    if (doc) {
      console.debug("[AST:merge] Matched doc %s for host=%s", matchedSvc, sourceHost);

      // Merge proto field maps: match by field number to existing schema properties
      if (analysis.protoFieldMaps.length && doc.schemas) {
        var fieldMapMatches = 0;
        var fieldMapUnmatched = [];
        var matchedFieldNums = new Set();
        for (var schemaName in doc.schemas) {
          var schema = doc.schemas[schemaName];
          if (!schema.properties) continue;
          for (var propName in schema.properties) {
            var prop = schema.properties[propName];
            if (!prop["x-field-number"]) continue;
            for (var fm = 0; fm < analysis.protoFieldMaps.length; fm++) {
              var fieldMap = analysis.protoFieldMaps[fm];
              if (fieldMap.fieldNumber === prop["x-field-number"] && !prop.customName) {
                prop._astName = fieldMap.fieldName;
                prop._astAccessor = fieldMap.accessorName;
                fieldMapMatches++;
                matchedFieldNums.add(fieldMap.fieldNumber);
                console.debug("[AST:merge] Field #%d → %s.%s renamed to '%s'", fieldMap.fieldNumber, schemaName, propName, fieldMap.fieldName);
              }
            }
          }
        }
        for (var fmu = 0; fmu < analysis.protoFieldMaps.length; fmu++) {
          if (!matchedFieldNums.has(analysis.protoFieldMaps[fmu].fieldNumber)) {
            fieldMapUnmatched.push("#" + analysis.protoFieldMaps[fmu].fieldNumber + "=" + analysis.protoFieldMaps[fmu].fieldName);
          }
        }
        console.debug("[AST:merge] Field maps: %d matched, %d unmatched [%s]", fieldMapMatches, fieldMapUnmatched.length,
          fieldMapUnmatched.slice(0, 10).join(", ") + (fieldMapUnmatched.length > 10 ? ", ..." : ""));
      }

      // Merge proto enums: enrich existing enum-type fields
      if (analysis.protoEnums.length && doc.schemas) {
        var enumMatches = 0;
        for (var eName in doc.schemas) {
          var eSchema = doc.schemas[eName];
          if (!eSchema.properties) continue;
          for (var ePropName in eSchema.properties) {
            var eProp = eSchema.properties[ePropName];
            if (eProp.enum && !eProp.customEnum) {
              for (var pe = 0; pe < analysis.protoEnums.length; pe++) {
                var protoEnum = analysis.protoEnums[pe];
                if (!protoEnum.isReverseMap) {
                  var enumKeys = Object.keys(protoEnum.values);
                  if (enumKeys.length === eProp.enum.length) {
                    eProp._astEnum = protoEnum.values;
                    enumMatches++;
                    console.debug("[AST:merge] Enum matched: %s.%s ← {%s} (%d values)", eName, ePropName,
                      enumKeys.slice(0, 5).join(", ") + (enumKeys.length > 5 ? ", ..." : ""), enumKeys.length);
                    break;
                  }
                }
              }
            }
          }
        }
        if (analysis.protoEnums.length > enumMatches) {
          console.debug("[AST:merge] %d/%d proto enums unmatched (no schema field with same value count)", analysis.protoEnums.length - enumMatches, analysis.protoEnums.length);
        }
      }
      // Note: bundle-wide value constraints (analysis.valueConstraints) are
      // NOT merged into params by name. That was a heuristic — any switch/
      // case on a variable named `q` anywhere in the bundle would attach
      // its values to every method's `q` param, including unrelated
      // form-scan-derived ones. Real-world FP: stackoverflow's `/search` q
      // received `["&", "read", "write", 0]` from an unrelated module.
      // Per-call-site values flow through learnFromAstCallSite →
      // _mergeAstValues from `callSite.params[i].validValues`, which is
      // structurally tied to the specific fetch site.
      // Merge sourceMap TypeScript types: enrich VDD parameters with type info from original sources
      if (analysis.sourceMapTypes && analysis.sourceMapTypes.length) {
        var typeMatches = 0;
        var tsMethods = doc.resources && doc.resources.learned ? doc.resources.learned.methods || {} : {};
        for (var _tmName in tsMethods) {
          var _tmMethod = tsMethods[_tmName];
          if (!_tmMethod.parameters) continue;
          for (var _tpName in _tmMethod.parameters) {
            var _tpParam = _tmMethod.parameters[_tpName];
            if (_tpParam._tsType) continue; // already enriched
            for (var _sti = 0; _sti < analysis.sourceMapTypes.length; _sti++) {
              var _smType = analysis.sourceMapTypes[_sti];
              for (var _stf = 0; _stf < _smType.fields.length; _stf++) {
                if (_smType.fields[_stf].name === _tpName) {
                  _tpParam._tsType = _smType.fields[_stf].type;
                  _tpParam._tsInterface = _smType.name;
                  _tpParam._tsOptional = _smType.fields[_stf].optional || false;
                  if (!_tpParam.type) _tpParam.type = _smType.fields[_stf].type;
                  typeMatches++;
                  break;
                }
              }
              if (_tpParam._tsType) break;
            }
          }
        }
        // Note: proto-field-map enrichment from TypeScript .pb.ts interfaces
        // was removed. The previous heuristics — fuzzy field-count tolerance
        // (Math.abs(diff) <= 2) and source-filename pattern matching
        // (/\.pb\.|_pb\.|proto/i) — both violated CLAUDE.md (magic-number
        // cap + framework-specific naming). Proto field maps work by field
        // ID without TS-name enrichment; if field-name learning is needed
        // it must come from a structural signal (e.g. the .proto definition
        // file via sourcemap, or AST extraction of the message class).
        if (typeMatches > 0) {
          console.debug("[AST:merge] TypeScript type enrichment: %d matches", typeMatches);
        }
      }
    } else {
      console.debug("[AST:merge] No doc for host=%s — registering endpoints only (script: %s)", sourceHost, analysis.sourceUrl);
    }

    // Register AST-observed fetch call sites as methods on their services.
    // Uses learnFromAstCallSite (direct fact-based registration), NOT the
    // old "synthesize fake URL/body, launder through learnFromRequest"
    // path — that conflated AST observations with real server traffic.
    var newEndpoints = 0;
    for (var fc = 0; fc < analysis.fetchCallSites.length; fc++) {
      var callSite = analysis.fetchCallSites[fc];
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

        var _astDocEntry = learnFromAstCallSite(tab, interfaceName, callSite, analysis.sourceUrl);
        // Refine interfaceName for endpoint registration if the call site
        // got promoted to a prefix bucket via observed-prefix clustering.
        if (_astDocEntry && _astDocEntry.doc && _astDocEntry.doc.name) {
          interfaceName = _astDocEntry.doc.name;
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
        var epKey = isDynamic
          ? "AST DYN " + bundleId + " " + (callSite.enclosingFunction || "anon") + " " + callSite.method + " " + fc
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
            // AST-captured required headers (the SET the bundle attached at the
            // host edge, per-header literal/opaque) — transport metadata shown
            // in the Send panel so the endpoint is actually usable.
            requiredHeaders: (callSite.headers && Object.keys(callSite.headers).length) ? callSite.headers : null,
            // Concrete PATH-PARAM examples the engine computed (e.g. a reply field `orgId=acme-42` collapsed
            // into /api/org/{}/members). The rich per-doc method schema carries these, but it is EVICTED after
            // review, so without persisting them onto the flat endpoint the cumulative moat loses the real
            // learned values — the whole point of the tool. Carried here so they survive eviction.
            pathParams: (function () {
              var pp = (callSite.params || []).filter(function (p) { return (p.location === "path") && p.validValues && p.validValues.length; });
              return pp.length ? pp.map(function (p) { return { name: p.name, values: p.validValues.slice(0, 20) }; }) : null;
            })(),
            // (Request body: the @BODY params[location:body] feed the discovery method schema, which is the
            //  SINGLE source the Send panel (schema.requestBody) and OpenAPI export (convertDiscoveryToOpenApi)
            //  read. An endpoint-entry requestBody copy was DEAD — resolveEndpointSchema never surfaced it — so
            //  it is deleted, not duplicated here.)
            firstSeen: Date.now(),
          });
          newEndpoints++;
        }
      } catch (mergeErr) {
        console.debug("[AST:merge] Error processing fetch site %d (%s %s): %s", fc, callSite.method, callSite.url, mergeErr.message || mergeErr);
      }
    }
    if (analysis.fetchCallSites.length) {
      console.debug("[AST:merge] Fetch sites: %d call sites processed, %d endpoints registered",
        analysis.fetchCallSites.length, newEndpoints);
    }

    // DOM-derived endpoints (href/src/action/data-* values from page
    // markup). Per user directive: "what DOM gets sent in the first
    // place [is] useful for learning". Surfaced into tab.endpoints
    // alongside AST-derived ones, with source="dom_html_<kind>" for
    // origin tracking.
    var domEps = analysis.domEndpoints || [];
    for (var dei = 0; dei < domEps.length; dei++) {
      var domEp = domEps[dei];
      try {
        var deBase = (tab && tab.url) || analysis.sourceUrl;
        if (!deBase) continue;
        var deResolved = new URL(domEp.url, deBase);
        if (/^(data|blob|about|javascript):/i.test(deResolved.protocol)) continue;
        var deKey = "DOM " + (domEp.source || "html") + " " + deResolved.href;
        if (tab.endpoints.has(deKey)) continue;
        tab.endpoints.set(deKey, {
          url: _decHoles(deResolved.href),
          method: "?",
          host: deResolved.hostname,
          path: _decHoles(deResolved.pathname),
          service: extractInterfaceName(deResolved),
          source: "dom_" + (domEp.source || "html").replace(/-/g, "_"),
          pageUrl: deBase,
          firstSeen: Date.now(),
        });
      } catch (e) {
        /* DOM-endpoint registration failed for one entry — almost always
           a malformed `url` attribute (relative path the bundle didn't
           normalize, javascript: handler we didn't filter early enough,
           etc.). Other entries in the batch still register. Surface
           so a real DOM-extraction regression on a vendor page is
           visible instead of disappearing into an empty endpoint list. */
        console.debug("[brain] DOM endpoint registration failed:", e && e.message || e, "url=" + (domEp && domEp.url), "src=" + (domEp && domEp.source));
      }
    }

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
    const existing = globalStore.apiKeys.get(k);
    if (existing) {
      existing.lastSeen = v.lastSeen;
      // Take the higher count — tab count is a running total, not a delta
      existing.requestCount = Math.max(
        existing.requestCount || 0,
        v.requestCount || 0,
      );
      const mergeSet = (target, source) => {
        if (source instanceof Set)
          source.forEach((s) => (target instanceof Set ? target.add(s) : null));
        else if (Array.isArray(source))
          source.forEach((s) => (target instanceof Set ? target.add(s) : null));
      };
      if (existing.services instanceof Set)
        mergeSet(existing.services, v.services);
      if (existing.hosts instanceof Set) mergeSet(existing.hosts, v.hosts);
      if (existing.endpoints instanceof Set)
        mergeSet(existing.endpoints, v.endpoints);
      if (!existing.pageUrls) existing.pageUrls = new Set();
      if (existing.pageUrls instanceof Set)
        mergeSet(existing.pageUrls, v.pageUrls);
    } else {
      globalStore.apiKeys.set(k, {
        origin: v.origin,
        referer: v.referer,
        source: v.source,
        firstSeen: v.firstSeen,
        lastSeen: v.lastSeen,
        requestCount: v.requestCount || 0,
        services: new Set(v.services || []),
        hosts: new Set(v.hosts || []),
        endpoints: new Set(v.endpoints || []),
        pageUrls: new Set(v.pageUrls || []),
      });
    }
  }
  for (const [k, v] of tab.endpoints) {
    if (!globalStore.endpoints.has(k)) {
      v._isNew = true;
      v._firstSeenGlobal = Date.now();
    } else {
      var ge = globalStore.endpoints.get(k);
      if (ge.lastSeen) v.lastSeen = Date.now();
      if (ge._firstSeenGlobal) v._firstSeenGlobal = ge._firstSeenGlobal;
      v._isNew = false;
      // UNION path-param examples so a later paramless re-emit (e.g. a re-visit before the concolic reply
      // re-run landed) never DROPS values a prior emit learned — the moat is monotonic.
      if (ge.pathParams || v.pathParams) {
        var _mp = new Map();
        for (var _s of [ge.pathParams || [], v.pathParams || []]) for (var _pp of _s) {
          var _cur = _mp.get(_pp.name) || new Set();
          for (var _val of (_pp.values || [])) _cur.add(_val);
          _mp.set(_pp.name, _cur);
        }
        v.pathParams = Array.from(_mp, function (e) { return { name: e[0], values: Array.from(e[1]).slice(0, 20) }; });
      }
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
