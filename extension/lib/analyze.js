// lib/analyze.js — AST/engine-worker analysis orchestration: hash + cache combined scripts, drive the wasm
// worker (astDispatch) over the combined bundle, replay cached results, drain the review queue, recover source
// maps, resolve path-param names, and mark security-finding changes. Extracted from the offscreen-brain.js
// monolith (one problem per file); loaded before it, resolves mergeASTResultsIntoVDD + astDispatch at
// call-time. This is the JS BRIDGE driving the engine -- the analysis itself is in the wasm.

function _hashScriptCode(code) {
  var h = 0;
  for (var i = 0; i < Math.min(code.length, 500); i++) {
    h = ((h << 5) - h + code.charCodeAt(i)) | 0;
  }
  return "inline:" + h;
}

// Full-content SHA-256 hash for AST cache keys (async, SubtleCrypto)
async function _hashScriptSHA256(code) {
  var buf = new TextEncoder().encode(code);
  var hash = await crypto.subtle.digest("SHA-256", buf);
  return Array.from(new Uint8Array(hash)).map(function(b) {
    return b.toString(16).padStart(2, "0");
  }).join("");
}

function _findScriptForLine(line, scriptOffsets) {
  for (var i = scriptOffsets.length - 1; i >= 0; i--) {
    if (line >= scriptOffsets[i].lineStart) return scriptOffsets[i];
  }
  // EMPTY scriptOffsets (the engine sources inline scripts now, so a page with
  // only inline <script> has no per-script offset map) must NOT return undefined
  // — callers do sInfo.url and would throw, taking down the WHOLE findings/endpoint
  // result (every DOM-XSS + inline-script endpoint silently lost). A null url is
  // the correct inline attribution: page URL, original line numbers.
  return scriptOffsets[0] || { url: null, lineStart: 1 };
}

// Resolve an AST endpoint's path-param names (minified, in URL order) to the
// page's REAL declared names via its chunk's source map. No minified pattern-
// matching and no value guessing: it maps the fetch call-site position through
// the map to the original position, reads that line from the map's own
// `sourcesContent`, and takes the URL template's path interpolation identifiers
// in order — e.g. `await fetch(\`/${owner}/${repo}/…?source=${source}\`)` →
// ["owner","repo"]. Returns the names array, or null if anything's missing.
function _resolvePathParamNames(callSite, scriptOffsets, traceMapsByUrl) {
  // Resolve a SHOWN finding's minified path params (e/a) to their declared names
  // (owner/repo) by running the source-map LIBRARY (@jridgewell/trace-mapping)
  // on the finding's own call-site location — the position the engine already
  // emits via its normal stack trace (NO engine instrumentation, NO bundle
  // transform). originalPositionFor() + sourceContentFor() hand back the
  // ORIGINAL fetch line; we read its template literal's path interpolations.
  try {
    if (!callSite || !callSite.loc || !scriptOffsets || !scriptOffsets.length || !traceMapsByUrl) return null;
    var sc = _findScriptForLine(callSite.loc.line, scriptOffsets);
    if (!sc || !sc.url) return null;
    var tm = traceMapsByUrl[sc.url];
    if (!tm) return null;
    var genLine = callSite.loc.line - sc.lineStart + 1;            // 1-based line within the chunk
    var col0 = (callSite.loc.column != null ? callSite.loc.column : (callSite.loc.col || 1)) - 1;
    if (col0 < 0) col0 = 0;
    var op = traceMapping.originalPositionFor(tm, { line: genLine, column: col0 });
    if (!op || op.source == null || op.line == null) return null;
    var content = traceMapping.sourceContentFor(tm, op.source);
    if (!content) return null;
    var lines = content.split("\n");
    var BT = String.fromCharCode(96);   // backtick
    // The fetch's original line(s) hold the URL template literal; scan a small
    // window (beautified calls can wrap) for the first backtick template, then
    // read its PATH interpolations (before '?'), last identifier of each ${...}.
    var win = (lines[op.line - 1] || "") + " " + (lines[op.line] || "") + " " + (lines[op.line - 2] || "");
    var bt = win.indexOf(BT);
    if (bt < 0) return null;
    var bt2 = win.indexOf(BT, bt + 1);
    var tmpl = bt2 > bt ? win.slice(bt + 1, bt2) : win.slice(bt + 1);
    var qm = tmpl.indexOf("?");
    var pathPart = qm >= 0 ? tmpl.slice(0, qm) : tmpl;
    var names = [], re = /\$\{\s*(?:[A-Za-z_$][\w$]*\s*\.\s*)*([A-Za-z_$][\w$]*)\s*\}/g, mm;
    while ((mm = re.exec(pathPart))) names.push(mm[1]);
    return names.length ? names : null;
  } catch (e) { return null; }
}

// Compare new security findings against globalStore to mark as new/existing/fixed
function _markSecurityFindingChanges(scriptUrl, findings) {
  var prev = globalStore.securityFindings.get(scriptUrl);
  if (prev) {
    var prevSigs = new Set();
    var ps = prev.securitySinks || [];
    for (var i = 0; i < ps.length; i++) {
      prevSigs.add(ps[i].sink + ":" + (ps[i].sourceType || "") + ":" + (ps[i].location ? ps[i].location.line : ""));
    }
    var pp = prev.dangerousPatterns || [];
    for (var i = 0; i < pp.length; i++) {
      prevSigs.add(pp[i].pattern + ":" + (pp[i].location ? pp[i].location.line : ""));
    }
    for (var i = 0; i < findings.sinks.length; i++) {
      var s = findings.sinks[i];
      var sig = s.sink + ":" + (s.sourceType || "") + ":" + (s.location ? s.location.line : "");
      findings.sinks[i]._changeType = prevSigs.has(sig) ? "existing" : "new";
      prevSigs.delete(sig);
    }
    for (var i = 0; i < findings.patterns.length; i++) {
      var p = findings.patterns[i];
      var sig = p.pattern + ":" + (p.location ? p.location.line : "");
      findings.patterns[i]._changeType = prevSigs.has(sig) ? "existing" : "new";
      prevSigs.delete(sig);
    }
    findings._fixedCount = prevSigs.size;
  } else {
    for (var i = 0; i < findings.sinks.length; i++) findings.sinks[i]._changeType = "new";
    for (var i = 0; i < findings.patterns.length; i++) findings.patterns[i]._changeType = "new";
  }
}

// Replay a cached AST analysis result — mirrors the post-analysis flow in
// _analyzeCombinedScripts() but skips the offscreen worker entirely.
function _replayCachedAST(tabId, tab, cached, sourceMapScripts, buf) {
  // Clear previous AST-derived endpoints only. _astResults and
  // _securityFindings are swapped in atomically below (see the same
  // rationale in _analyzeCombinedScripts above): consumers should never
  // see an empty-but-transient state.
  var keysToDelete = [];
  tab.endpoints.forEach(function(val, key) {
    if (key.startsWith("AST ") || key.startsWith("AST DYN ")) {
      keysToDelete.push(key);
    }
  });
  for (var di = 0; di < keysToDelete.length; di++) {
    tab.endpoints.delete(keysToDelete[di]);
  }

  var analysis = JSON.parse(JSON.stringify(cached.result)); // deep copy
  var scriptOffsets = cached.scriptOffsets || [];
  var tabUrl = cached.tabUrl || "";

  // Override tabUrl with current tab URL if available
  var meta = tab;   // tab-level url folded onto the per-tab state
  if (meta && meta.url) tabUrl = meta.url;
  else if (buf && buf.pageUrl) tabUrl = buf.pageUrl;

  var hasFindings = analysis.protoEnums.length || analysis.protoFieldMaps.length ||
    analysis.fetchCallSites.length || analysis.sourceMapUrl ||
    (analysis.securitySinks && analysis.securitySinks.length) ||
    (analysis.dangerousPatterns && analysis.dangerousPatterns.length);

  if (!hasFindings && sourceMapScripts.length === 0) return;

  if (hasFindings) {
    analysis._securityMerged = true;

    // Build the new security-findings list in a LOCAL array first, then
    // swap it into tab._securityFindings atomically once fully populated.
    // Same rationale as tab._astResults: no transient empty window.
    var newSecurityFindings = [];
    var secSinks = analysis.securitySinks || [];
    var dangerousPats = analysis.dangerousPatterns || [];
    if (secSinks.length || dangerousPats.length) {
      var byScript = {};
      for (var _fsi = 0; _fsi < secSinks.length; _fsi++) {
        var sink = secSinks[_fsi];
        var sLine = sink.location ? sink.location.line : 0;
        var sInfo = _findScriptForLine(sLine, scriptOffsets);
        var sKey = sInfo.url || tabUrl;
        if (!byScript[sKey]) byScript[sKey] = { sinks: [], patterns: [] };
        var adjustedSink = Object.assign({}, sink);
        if (sInfo.url && sink.location) {
          adjustedSink.location = Object.assign({}, sink.location, {
            line: sink.location.line - sInfo.lineStart + 1
          });
        }
        byScript[sKey].sinks.push(adjustedSink);
      }
      for (var _fpi = 0; _fpi < dangerousPats.length; _fpi++) {
        var pat = dangerousPats[_fpi];
        var pLine = pat.location ? pat.location.line : 0;
        var pInfo = _findScriptForLine(pLine, scriptOffsets);
        var pKey = pInfo.url || tabUrl;
        if (!byScript[pKey]) byScript[pKey] = { sinks: [], patterns: [] };
        var adjustedPat = Object.assign({}, pat);
        if (pInfo.url && pat.location) {
          adjustedPat.location = Object.assign({}, pat.location, {
            line: pat.location.line - pInfo.lineStart + 1
          });
        }
        byScript[pKey].patterns.push(adjustedPat);
      }
      for (var sUrl in byScript) {
        _markSecurityFindingChanges(sUrl, byScript[sUrl]);
        newSecurityFindings.push({
          sourceUrl: sUrl,
          pageUrl: tabUrl,
          securitySinks: byScript[sUrl].sinks,
          dangerousPatterns: byScript[sUrl].patterns,
          _fixedCount: byScript[sUrl]._fixedCount || 0,
        });
      }
    }
    // Atomic swap for both state slots — a concurrent reader sees either
    // the previous (valid) analysis or this one, never an empty interim.
    tab._astResults = [analysis];
    tab._securityFindings = newSecurityFindings;
    mergeASTResultsIntoVDD(tab, [analysis], tabId);

    mergeToGlobal(tab);
    notifyPopup(tabId);
  }

  // Fetch source maps (not cached — they're fetched separately and may change)
  for (var smi = 0; smi < sourceMapScripts.length; smi++) {
    _fetchSourceMapForScript(tabId, tab, analysis, sourceMapScripts[smi].scriptUrl, sourceMapScripts[smi].smUrl);
  }
  // Lazy chunks are loaded by the ONE scheduler IN PLACE: the engine emits @CHUNK during forced
  // execution, runEngine fetches it (self.safeFetch) and qjs_provide evals it in the live instance,
  // surfacing its endpoints in the SAME run. No host-side re-fetch/re-analyze round (that was a second
  // scheduler — deleted). A chunk form the engine doesn't yet discover in-place is an engine gap to close.
}

// Deterministic in-flight signal for the diagnostic / e2e harness:
// _analyzeCombinedScripts sets this for the tab on entry, clears on
// exit (success OR error). Lets a test poll "wait until !running"
// instead of guessing a wall-clock budget — the wait scales with
// real worker execution.
const _analysisInflight = new Set();

// Review queue. New pages (and their JS) are QUEUED, then a single drainer
// reviews ONE page at a time. Combined with the worker throttling itself
// (it yields the core between every schedule/deep batch), this means many
// open tabs never stack analyses onto the CPU — the reviewer runs cool in
// the background and never pins a core. Time is free; a maxed core is not.
var _reviewQueue = [];
var _reviewDraining = false;
function _analyzeCombinedScripts(docKey) {
  var buf = _scriptBuffers.get(docKey);
  if (!buf || !buf.pageHtml) return;                              // engine-sourced: gate on the document HTML, not pre-buffered scripts
  if (_reviewQueue.indexOf(docKey) < 0) _reviewQueue.push(docKey);   // dedupe within queue; a re-queue after run re-reviews (combined-cache makes an unchanged doc a fast hit)
  _drainReviewQueue();
}
async function _drainReviewQueue() {
  if (_reviewDraining) return;
  _reviewDraining = true;
  try {
    while (_reviewQueue.length) {
      /* Recency-priority pick instead of FIFO shift — the tab the user is LOOKING
         AT (most recently activated: lastActivatedTs, stamped at CONTENT_HTML) gets
         analyzed next; docs without a timestamp default to 0 and trail. ORDER only,
         never COVERAGE (every queued doc is still analyzed eventually). Inlined here —
         the shared lib/priority.js WFQ mirror was DELETED (the C engine owns the real
         WFQ: flow_weight/wfq_yield). Splicing IS the pick; the comparison is pure. */
      var docKey = null;
      if (_reviewQueue.length) {
        var _bestI = 0, _bestTs = ((_scriptBuffers.get(_reviewQueue[0]) || {}).lastActivatedTs) | 0;
        for (var _k = 1; _k < _reviewQueue.length; _k++) {
          var _ts = ((_scriptBuffers.get(_reviewQueue[_k]) || {}).lastActivatedTs) | 0;
          if (_ts > _bestTs) { _bestI = _k; _bestTs = _ts; }
        }
        docKey = _reviewQueue[_bestI];
        _reviewQueue.splice(_bestI, 1);
      }
      if (docKey == null) break;
      var buf = _scriptBuffers.get(docKey);
      if (!buf || !buf.pageHtml) continue;
      var tabId = buf.tabId;
      /* Same-tab guard: a re-queue from a late-arriving script for a tab
         whose analysis is STILL IN FLIGHT (round-1 BFS or chunk-merged
         round-2 still grinding) must not spawn a CONCURRENT second call —
         two wasm instances on the same 4.4MB+ bundle would compete for
         memory (wasm `memory.grow` is monotonic per-instance) and one
         would trap "memory access out of bounds" mid-eval. The previous
         absence of this guard produced 7 concurrent round-2 retries on
         github, each duplicating the 17MB compiled-bytecode footprint.
         Different tabId CAN still overlap (see below). The re-queue isn't
         dropped; the late scripts are folded into the current buf and the
         next drain iteration after the in-flight one finishes picks them
         up via the cache-miss path. */
      if (_analysisInflight.has(docKey)) continue;   // per-DOCUMENT in-flight guard (distinct documents of a tab may overlap)
      _analysisInflight.add(docKey);
      /* Fire-and-forget — do NOT await. astDispatch ENQUEUES this document
         into the host WFQ pool (bridge.js): a bounded set of live wasm
         instances the pool interleaves by value-of-information (each engine
         exposes qjs_top_weight; it advances the highest one a slice, then
         re-ranks) — so this is TWO levels of ONE WFQ: flows within a doc
         (C engine, g_reg) and engines across docs (the host pool). This
         recency pick only orders the doc's ENTRY into the pool; the pool
         then arbitrates by weight and admission-gates creation to the RAM
         cap. The cold tail (parked recipes) is advanced separately by
         _driveGlobalFrontierBurst -> driveFrontier. The per-doc cache +
         _dataEpoch guard keep results attribution-correct; errors surface
         via analysis.resolverErrors / _astError, never a bare catch. */
      _analyzeCombinedScriptsInner(tabId, buf)
        .catch(function (e) { console.debug("[AST:queue] doc review error: %s", e && e.message); })
        .finally(function () { _analysisInflight.delete(docKey); });
    }
  } finally { _reviewDraining = false; }
}
async function _analyzeCombinedScriptsInner(tabId, buf) {
  var tab = getDoc(buf.docKey);
  var _ep = _dataEpoch;   // a Clear during the worker round-trip invalidates this run
  // Concatenate in DOM/execution order, not fetch-arrival order — a
  // later chunk that reads state an earlier chunk set up (GitHub's app
  // chunk reading client-env loaded by environment-*.js) throws
  // "requested before it was loaded" if combined out of order. Stable
  // in-place sort so every downstream reader (scriptOffsets, the
  // fallback combine paths, _findScriptForLine) sees the same order.
  buf.scripts.sort(function (a, b) { return (a.order == null ? 1e9 : a.order) - (b.order == null ? 1e9 : b.order); });
  // Split executable scripts from server-rendered data islands. Islands
  // are NOT concatenated into the executable bundle (they're JSON, not
  // code) — they're rebuilt into the worker's virtual DOM so the bundle
  // bootstraps from them (GitHub #client-env) and runs correctly.
  // Lexbor inside the engine worker parses CONTENT_HTML and produces
  // the real spec DOM the bundle reads — including the page's data
  // islands (inline <script type="application/json">). No need to
  // ship them as a separate domIslands array.
  var scripts = buf.scripts;
  // DIAGNOSTIC: record each analysis round's script set so a dropped external
  // script (the cross-origin CDN case) is visible — which round ran, how many
  // scripts, and whether the CDN bundle was present.
  if (!self._analyzeDiag) self._analyzeDiag = [];
  self._analyzeDiag.push({ n: scripts.length, pending: buf.pending, loadFired: !!buf.loadFired,
    hasCDN: scripts.some(function (s) { return /sentry-cdn|browser\.sentry/.test(s.url || ""); }),
    urls: scripts.map(function (s) { return (s.url || "inline").split("/").pop().slice(0, 24); }).slice(0, 8) });
  if (self._analyzeDiag.length > 12) self._analyzeDiag.shift();
  // Real <script src> URLs (in execution order) — built into the
  // virtual DOM so webpack's auto-publicPath (document.currentScript
  // .src / getElementsByTagName("script")) finds a real script URL
  // instead of throwing "Automatic publicPath is not supported". Just
  // URLs (tiny); bodies are already in the combined code.
  var scriptUrls = [];
  for (var _sui = 0; _sui < scripts.length; _sui++) if (scripts[_sui].url) scriptUrls.push(scripts[_sui].url);
  var totalChars = 0;
  for (var i = 0; i < scripts.length; i++) totalChars += scripts[i].code.length;

  console.debug("[AST:combined] Analyzing %d scripts (%d total chars) for tab=%d",
    scripts.length, totalChars, tabId);

  // Extract source map URLs from individual scripts before concatenation
  var sourceMapScripts = []; // [{url, smUrl}]
  for (var si = 0; si < scripts.length; si++) {
    var smUrl = extractSourceMapUrl(scripts[si].code);
    if (smUrl) {
      sourceMapScripts.push({ scriptUrl: scripts[si].url, smUrl: smUrl });
    }
  }

  // ─── AST Cache Check ───────────────────────────────────────────────
  // Hash each script individually, then combine hashes into a cache key.
  // If the exact same set of scripts was analyzed before AND the
  // analyzer fingerprint (the SHA of the analyzer worker source) is
  // unchanged, replay the cached result without touching the offscreen
  // worker. Analyzer fingerprint is baked into cacheKey, so a stale
  // entry simply does not match — no manual version bumps needed.
  var scriptHashes = [];
  try {
    // The combination submitted IS the cache identity: hash the pageHtml (round 1's
    // scripts are engine-sourced, so buf.scripts is empty) + each downloaded chunk
    // (round 2). Content/combination keying — NOT url: urls change and pages reuse
    // scripts (jquery), so the same combination at a new url must HIT (dedup) and
    // distinct content at the same url (about:blank) must MISS (no leak). The
    // principal ORIGIN (buf.origin, MessageSender-derived, NEVER url) disambiguates
    // identical content under different principals (their credentialed reads +
    // relative resolution differ); opaque principals are unique so they never share.
    scriptHashes.push("doc:" + (buf.origin || "") + ":" + (await _hashScriptSHA256(buf.pageHtml || "")));
    for (var hi = 0; hi < scripts.length; hi++) {
      scriptHashes.push(await _hashScriptSHA256(scripts[hi].code));
    }
  } catch (_) {
    // SubtleCrypto unavailable — proceed without cache
    scriptHashes = [];
  }

  var cacheKey = null;
  var analyzerFp = await getAnalyzerFingerprint();
  // scriptHashes = [the leading "doc:" pageHtml-identity entry] + one per chunk,
  // so a successful hash run is exactly scripts.length + 1 (SubtleCrypto failure
  // resets it to [], which won't match — no cache, fail safe). The "+ 1" is the
  // post-migration fix: round-1 scripts are engine-sourced (buf.scripts empty), so
  // the document identity lives in that leading pageHtml entry, not a per-script one.
  if (analyzerFp && scriptHashes.length === scripts.length + 1) {
    // Cache key = (analyzer fingerprint) + (script content hashes). Any change to the analyzer
    // worker files OR the analyzed scripts flips the key, so stale entries simply don't match.
    // There is ONE analysis pass now (the engine drives BFS + orphan-residue + in-place chunks in
    // the ONE scheduler); no round-mode suffix.
    cacheKey = analyzerFp + "|" + scriptHashes.join("+");
    var cached = globalStore.scriptCache.get(cacheKey);
    if (cached) {
      console.debug("[AST:cache] Cache HIT for tab=%d (%d scripts, key=%s…)",
        tabId, scripts.length, cacheKey.slice(0, 16));
      cached.timestamp = Date.now();      // LRU touch
      _replayCachedAST(tabId, tab, cached, sourceMapScripts, buf);
      return;
    }
    console.debug("[AST:cache] Cache MISS for tab=%d (%d scripts, key=%s…)",
      tabId, scripts.length, cacheKey.slice(0, 16));
  }

  // DO NOT reset tab._astResults / tab._securityFindings here. Clearing
  // them at the start of analysis creates a visible "empty" window for
  // consumers (popup, harness, test suites) that poll during the async
  // sendToOffscreen() await below. Instead, we build the new results into
  // local variables and swap them into the tab atomically AFTER the
  // offscreen worker returns successfully. Endpoints are AST-derived too
  // but the popup tolerates staleness there — safe to clear them up-front
  // to avoid double-registration when a late script triggers re-analysis.
  var keysToDelete = [];
  tab.endpoints.forEach(function(val, key) {
    if (key.startsWith("AST ") || key.startsWith("AST DYN ")) {
      keysToDelete.push(key);
    }
  });
  for (var di = 0; di < keysToDelete.length; di++) {
    tab.endpoints.delete(keysToDelete[di]);
  }

  // Concatenate all scripts with semicolons (safe delimiter for script mode)
  // Track line offsets for per-script finding attribution
  var combined = "";
  var scriptOffsets = []; // [{url, lineStart}]
  var nlCount = 0;
  for (var ci = 0; ci < scripts.length; ci++) {
    if (ci > 0) { combined += ";\n"; nlCount++; }
    scriptOffsets.push({ url: scripts[ci].url, lineStart: nlCount + 1 });
    var code = scripts[ci].code;
    for (var ch = 0; ch < code.length; ch++) {
      if (code.charCodeAt(ch) === 10) nlCount++;
    }
    combined += code;
  }

  // Source URL for the combined analysis. SECURITY: this becomes the analysis
  // PRINCIPAL — safeFetch's origin-relative SSRF origin (self.__sfPageOrigin in
  // the worker) AND window.location. It MUST derive only from the browser-
  // provided sender.url (captured into buf.url on CONTENT_HTML), NEVER a
  // content-script-supplied value (msg.url ->
  // scripts[0].url) — else a hostile page could claim a localhost origin to
  // defeat the SSRF guard. No untrusted fallback: unknown origin leaves tabUrl ""
  // -> safeFetch's safe default (block private) + a placeholder window.location.
  // Per-DOCUMENT principal: buf.url is THIS document's own browser-provided url
  // (set from sender on CONTENT_HTML), not the tab's — a sub-frame analyses as its
  // own origin, never the embedder's.
  var tabUrl = buf.url || buf.pageUrl || "";

  // Analyze combined in offscreen document (non-blocking)
  var analysis;
  var response;
  try {
    response = await sendToOffscreen({
      type: "AST_ANALYZE", code: combined, sourceUrl: tabUrl, documentId: buf.docKey, origin: buf.origin, forceScript: true,
      scriptUrls: scriptUrls,
      // Per-chunk line offsets + each chunk's sourceMappingURL so the OFFSCREEN
      // worker (long-lived, owns IndexedDB) can fetch maps and resolve minified
      // path-param names (e→owner) itself — the SW is evicted mid-grind and must
      // not fetch maps. sourceMapScripts = [{scriptUrl, smUrl}] (smUrl is the
      // bundle's real pragma: relative filename OR full address).
      scriptOffsets: scriptOffsets,
      sourceMapScripts: sourceMapScripts,
      pageHtml: getDoc(buf.docKey)._pageHtml || null,
      responseHeaders: getDoc(buf.docKey)._responseHeaders || {},   // real CSP/Content-Type -> engine (header-CSP is the PRIMARY policy; meta-CSP is secondary)
      // Participate in the GLOBAL cross-session frontier: this engine's residue parks to IDB under RAM
      // pressure (resource-driven, host-side) and rehydrates by value order later. With headroom the page
      // runs to completion in one visit — nothing is lost to a clock; there is NO dispatch/step quantum.
      persist: true,
    });
  } catch (e) {
    console.debug("[AST:combined] sendToOffscreen failed for tab=%d: %s", tabId, e.message || e);
    getDoc(buf.docKey)._astError = "sendToOffscreen threw: " + (e.message || String(e));
    return;
  }
  if (!response || !response.success) {
    // The Clear button terminated the worker mid-analysis. Abort cleanly — do
    // NOT fall back to per-script re-analysis, which would re-flood the freshly
    // respawned worker right after a Clear and repopulate the just-wiped store.
    if (response && response.error === "cleared") {
      console.debug("[AST:combined] tab=%d aborted — worker cleared", tabId);
      return;
    }
    console.debug("[AST:combined] analyzeJSBundle failed for tab=%d: %s", tabId,
      response ? response.error : "no response");
    if (response && response.stack) console.debug(response.stack);
    getDoc(buf.docKey)._astError = "offscreen unsuccessful: " + (response ? (response.error + " | " + (response.stack || "")) : "no response");
    // SURFACE the combined-analysis failure — do NOT fall back to per-script
    // analysis. A page is reviewed as the COMBINATION of all its scripts; analysing
    // them in isolation loses the cross-script interprocedural visibility (webpack
    // chunk exports, shared globals) the whole design depends on, and emitting that
    // degraded result would MASK the real failure. _astError (above) + the worker's
    // @WHY/@E are the signal to root-cause; the resumable frontier retries via replay recipes.
    return;
  }
  getDoc(buf.docKey)._astError = null;
  // The bin/Clear reset fired while this analysis was in the worker. Its result
  // predates the wipe, so merging it (or downloading its chunks / spawning the
  // deep round) would repopulate the just-cleared store. Abandon the whole tail.
  if (_ep !== _dataEpoch) {
    console.debug("[AST:combined] tab=%d result discarded — store reset mid-analysis", tabId);
    return;
  }
  analysis = response.result;
  // Carry the combined→per-script line map onto the analysis so the VDD merge
  // can resolve a path param's minified name to its real source-map name.
  analysis.scriptOffsets = scriptOffsets;

  // NOTE: lazy-chunk consumption is no longer a separate host-side re-review
  // step. The C engine now fetches every discovered chunk IN-RUN: it surfaces
  // chunk URLs (analysis.chunkUrls) and bridge.runEngine's step loop fetches
  // each and feeds it back via qjs_provide, so the chunk's code (classic, or a
  // linked ESM module graph) is analyzed inside the same instance as it arrives.
  // There is no combined-whole re-analysis and no per-schedule re-instantiation
  // to gate on.

  if (analysis._timings) {
    // Surface per-script AST latency on the analysis result so the
    // harness (`scripts` command) can display it — useful when a
    // user-facing stall needs root-causing without leaning on the
    // background console. _analysisTimings is a stable name distinct
    // from the internal _timings the worker fills in and strips.
    tab._lastAstTimings = analysis._timings;
    analysis._analysisTimings = analysis._timings;
    delete analysis._timings;
  }

  // ─── Cache the analysis result ──────────────────────────────────────
  // Cache key already encodes the analyzer fingerprint + script hashes;
  // no separate version field — a stale fingerprint just won't match.
  //
  // BUT: don't cache a DEGENERATE result — a run that produced zero learned
  // facts AND surfaced a resolverError is a host-model gap (e.g. the wasm
  // aborted mid-bundle, or a host-edge stub is missing).
  // Caching it under the script-hash key would block re-analysis even after
  // the engine bug is fixed: the next navigation hashes the same scripts,
  // hits the cache, replays the empty result. The analyzer-fingerprint
  // covers the JS worker source but NOT the embedded wasm — a wasm rebuild
  // does not bump it, so the cache stays wedged until the user explicitly
  // hits the bin/Clear button. Skipping the cache write on a degenerate
  // result means a fresh navigation actually re-runs against the fixed
  // engine. A run with at least one fact or no resolverError is preserved
  // (the structural-learning rule — a real "no endpoints on this page" is
  // legitimate; a resolverError-bearing zero is not).
  var _hasFacts = ((analysis.fetchCallSites && analysis.fetchCallSites.length) ||
                   (analysis.securitySinks && analysis.securitySinks.length) ||
                   (analysis.protoEnums && analysis.protoEnums.length) ||
                   (analysis.protoFieldMaps && analysis.protoFieldMaps.length) ||
                   (analysis.domEndpoints && analysis.domEndpoints.length) ||
                   (analysis.chunkUrls && analysis.chunkUrls.length));
  var _hasResolverErr = analysis.resolverErrors && analysis.resolverErrors.length > 0;
  if (cacheKey && !(_hasResolverErr && !_hasFacts)) {
    globalStore.scriptCache.set(cacheKey, {
      result: JSON.parse(JSON.stringify(analysis)), // deep copy to avoid aliasing
      scriptOffsets: scriptOffsets,
      tabUrl: tabUrl,
      timestamp: Date.now(),
    });
    scheduleSave();
  } else if (cacheKey) {
    console.debug("[AST:cache] SKIPPING write for tab=%d (degenerate result: %d resolverErrors, no learned facts) — next navigation will retry", tabId, analysis.resolverErrors.length);
  }

  if (analysis.resolverErrors && analysis.resolverErrors.length > 0) {
    // Surface to the popup diagnostic view, not console-only. A reached-but-
    // opaque host call (fully-opaque URL/method) or a host-model gap (@E
    // bundle throw) is a P1 the reviewer must SEE and act on — per CLAUDE.md
    // "@WHY/diagnostics SHOULD be exposed in the popup's diagnostic view".
    // Deduped by message (the distinct-message set is the natural bound — no
    // cap); diagnostic buffer, not analysis state, so it drops nothing learned.
    if (!Array.isArray(tab._resolverErrors)) tab._resolverErrors = [];
    var _seenRe = new Set(tab._resolverErrors.map(function (r) { return r.message; }));
    // The fromReply reply-example (a fully-opaque URL that IS a reply field ->
    // fetch the source, extract the field) is ENGINE-SIDE now: g_reply_table +
    // @REPLYWANT/qjs_provide inject the concrete reply so r.json() returns the
    // real value in-flow. That logic must NOT live in this bridge (engine is the
    // browser; the offscreen only relays safeFetch/IDB/chrome). So resolverErrors
    // here is purely a DIAGNOSTIC surface (@E crashes) for the popup.
    for (var _rei = 0; _rei < analysis.resolverErrors.length; _rei++) {
      var _re = analysis.resolverErrors[_rei];
      console.debug("[AST:resolver] %s: %s", _re.context, _re.message);
      if (_re.stack) console.debug(_re.stack);
      if (!_seenRe.has(_re.message)) {
        _seenRe.add(_re.message);
        tab._resolverErrors.push({ context: _re.context, message: _re.message, snippet: _re.snippet || null });
      }
    }
  }

  var hasFindings = analysis.protoEnums.length || analysis.protoFieldMaps.length ||
    analysis.fetchCallSites.length || analysis.sourceMapUrl ||
    (analysis.securitySinks && analysis.securitySinks.length) ||
    (analysis.dangerousPatterns && analysis.dangerousPatterns.length);
  if (!hasFindings && sourceMapScripts.length === 0) {
    console.debug("[AST:combined] No findings for tab=%d", tabId);
    // The deep orphan-residue drive already ran IN the ONE scheduler (seed_orphans is continuous,
    // chunks eval'd in place) — nothing more to dispatch here.
    return;
  }

  if (hasFindings) {
    console.debug("[AST:combined] Findings for tab=%d: %d protoEnums, %d fieldMaps, %d fetchSites, %d secSinks, %d dangerousPatterns",
      tabId, analysis.protoEnums.length, analysis.protoFieldMaps.length, analysis.fetchCallSites.length,
      (analysis.securitySinks ? analysis.securitySinks.length : 0),
      (analysis.dangerousPatterns ? analysis.dangerousPatterns.length : 0));

    // Pre-empt mergeASTResultsIntoVDD's security merge — we split findings per-script below
    analysis._securityMerged = true;

    // Build security findings locally, then swap into tab._* slots atomically.
    // Matches the visibility-preserving pattern in _replayCachedAST above:
    // consumers never see an empty-but-populating state.
    var newSecurityFindings = [];
    var secSinks = analysis.securitySinks || [];
    var dangerousPats = analysis.dangerousPatterns || [];
    if (secSinks.length || dangerousPats.length) {
      // Shift every nested-location field by -(lineStart-1) so the hop/
      // candidate coords end up in SCRIPT-LOCAL space, matching the
      // primary sink location. Without this, taintPath.at.line and
      // sanitizerReport.candidates[i].loc.line stay in combined-bundle
      // space and sourcemap lookups silently return null.
      function _shiftFindingLines(finding, lineDelta) {
        if (!lineDelta) return finding;
        if (Array.isArray(finding.taintPath)) {
          finding.taintPath = finding.taintPath.map(function(h) {
            if (!h || !h.at || typeof h.at.line !== "number") return h;
            return Object.assign({}, h, { at: Object.assign({}, h.at, { line: h.at.line + lineDelta }) });
          });
        }
        if (finding.sanitizerReport && Array.isArray(finding.sanitizerReport.candidates)) {
          finding.sanitizerReport = Object.assign({}, finding.sanitizerReport, {
            candidates: finding.sanitizerReport.candidates.map(function(c) {
              if (!c || !c.loc || typeof c.loc.line !== "number") return c;
              return Object.assign({}, c, { loc: Object.assign({}, c.loc, { line: c.loc.line + lineDelta }) });
            }),
          });
        }
        return finding;
      }

      var byScript = {}; // scriptUrl → {sinks: [], patterns: []}
      for (var _fsi = 0; _fsi < secSinks.length; _fsi++) {
        var sink = secSinks[_fsi];
        var sLine = sink.location ? sink.location.line : 0;
        var sInfo = _findScriptForLine(sLine, scriptOffsets);
        // External scripts: attribute to script URL with adjusted line numbers
        // Inline scripts (url empty): attribute to page URL with original line numbers
        var sKey = sInfo.url || tabUrl;
        if (!byScript[sKey]) byScript[sKey] = { sinks: [], patterns: [] };
        var adjustedSink = Object.assign({}, sink);
        if (sInfo.url && sink.location) {
          var sDelta = -(sInfo.lineStart - 1);
          adjustedSink.location = Object.assign({}, sink.location, {
            line: sink.location.line + sDelta
          });
          _shiftFindingLines(adjustedSink, sDelta);
        }
        byScript[sKey].sinks.push(adjustedSink);
      }
      for (var _fpi = 0; _fpi < dangerousPats.length; _fpi++) {
        var pat = dangerousPats[_fpi];
        var pLine = pat.location ? pat.location.line : 0;
        var pInfo = _findScriptForLine(pLine, scriptOffsets);
        var pKey = pInfo.url || tabUrl;
        if (!byScript[pKey]) byScript[pKey] = { sinks: [], patterns: [] };
        var adjustedPat = Object.assign({}, pat);
        if (pInfo.url && pat.location) {
          var pDelta = -(pInfo.lineStart - 1);
          adjustedPat.location = Object.assign({}, pat.location, {
            line: pat.location.line + pDelta
          });
          _shiftFindingLines(adjustedPat, pDelta);
        }
        byScript[pKey].patterns.push(adjustedPat);
      }
      for (var sUrl in byScript) {
        // Mark findings as new/existing by comparing against globalStore
        _markSecurityFindingChanges(sUrl, byScript[sUrl]);
        newSecurityFindings.push({
          sourceUrl: sUrl,
          pageUrl: tabUrl,
          securitySinks: byScript[sUrl].sinks,
          dangerousPatterns: byScript[sUrl].patterns,
          _fixedCount: byScript[sUrl]._fixedCount || 0,
        });
      }
      console.debug("[AST:combined] Split security findings across %d scripts for tab=%d",
        Object.keys(byScript).length, tabId);
    }
    // Atomic swap — never show consumers an empty interim.
    tab._astResults = [analysis];
    tab._securityFindings = newSecurityFindings;
    mergeASTResultsIntoVDD(tab, [analysis], tabId);

    mergeToGlobal(tab);
    notifyPopup(tabId);
  }

  // Idle burst of the ONE host-level attention: advance other origins' parked frontiers by value
  // (non-blocking, serialized). This page's own residue (if it parked) is now in the global frontier.
  _driveGlobalFrontierBurst(4);

  // Fetch source maps — SCOPED to the chunks that actually hold a learned
  // fetch call site (so path-param names like e/a → owner/repo resolve),
  // not all ~661 shipped maps. The deep grind's endpoints (e.g. preheat) live
  // in lazy chunks, so this must consider the combined-bundle loc of every
  // fetchCallSite, mapped back to its chunk via scriptOffsets.
  var _needSM = new Set();
  for (var _fi = 0; _fi < analysis.fetchCallSites.length; _fi++) {
    var _fcl = analysis.fetchCallSites[_fi];
    if (_fcl && _fcl.loc && typeof _fcl.loc.line === "number") {
      var _fsc = _findScriptForLine(_fcl.loc.line, scriptOffsets);
      if (_fsc && _fsc.url) _needSM.add(_fsc.url);
    }
  }
  for (var smi = 0; smi < sourceMapScripts.length; smi++) {
    if (_needSM.size && !_needSM.has(sourceMapScripts[smi].scriptUrl)) continue;
    _fetchSourceMapForScript(tabId, tab, analysis, sourceMapScripts[smi].scriptUrl, sourceMapScripts[smi].smUrl);
  }

  // Lazy chunks were already fetched + eval'd IN PLACE by the ONE scheduler during this run
  // (@CHUNK → runEngine safeFetch → qjs_provide), so their endpoints are in `analysis` already.
  // No host-side re-fetch/re-analyze round.
}

function _fetchSourceMapForScript(tabId, tab, analysis, scriptUrl, smUrl) {
  var _ep = _dataEpoch;   // a Clear during the (async) source-map fetch invalidates this re-merge
  try {
    if (!/^https?:\/\//i.test(smUrl)) {
      smUrl = new URL(smUrl, new URL(scriptUrl)).href;
    }
  } catch (_) {
    console.debug("[AST:sourcemap] Failed to resolve URL: %s (base: %s)", smUrl, scriptUrl);
    return;
  }
  console.debug("[AST:sourcemap] Fetching: %s (from %s)", smUrl, scriptUrl);
  pageContextFetch(tabId, smUrl, { method: "GET" }, tab && tab.documentId)
    .then(async function(smResp) {
      if (!smResp.body || smResp.error) {
        console.debug("[AST:sourcemap] Fetch failed for %s: %s", smUrl, smResp.error || "empty body");
        return;
      }
      try {
        var smJson = JSON.parse(smResp.body);
        // Name resolution (e→owner) uses the standard library on the
        // engine-stamped hole position; stored per chunk URL (each lazy chunk
        // has its own map). The AST_PARSE_SOURCEMAP call below stays only for
        // proto-file/type extraction from the map's sources/sourcesContent.
        try { analysis.traceMapsByUrl = analysis.traceMapsByUrl || {}; analysis.traceMapsByUrl[scriptUrl] = new traceMapping.TraceMap(smJson); }
        catch (e) { console.debug("[AST:sourcemap] TraceMap failed for %s: %s", scriptUrl, e && e.message); }
        var smResp2 = await sendToOffscreen({ type: "AST_PARSE_SOURCEMAP", sourceMapJson: smJson });
        if (!smResp2 || !smResp2.success) {
          console.debug("[AST:sourcemap] parseSourceMap failed for %s: %s", smUrl, smResp2 ? smResp2.error : "no response");
          return;
        }
        var smData = smResp2.result;
        analysis.sourceMap = smData;
        // Per-script map store: a page loads many chunks, each with its OWN
        // map; `analysis.sourceMap` keeps only the last fetched, so param-name
        // resolution must look up the map for the SPECIFIC chunk an endpoint's
        // call site lives in (via scriptOffsets), not the last one.
        analysis.sourceMapsByUrl = analysis.sourceMapsByUrl || {};
        analysis.sourceMapsByUrl[scriptUrl] = smData;
        console.debug("[AST:sourcemap] Parsed: %d sources, %d names, %d proto files, %d API client files",
          smData.sources.length, smData.names.length, smData.protoFileNames.length, smData.apiClientFiles.length);
        if (smData.sourcesContent && smData.sourcesContent.length) {
          var typesResp = await sendToOffscreen({
            type: "AST_EXTRACT_TYPES",
            sourcesContent: smData.sourcesContent,
            sources: smData.sources
          });
          if (typesResp && typesResp.success) {
            analysis.sourceMapTypes = typesResp.result;
            if (analysis.sourceMapTypes.length) {
              console.debug("[AST:sourcemap] Extracted %d types", analysis.sourceMapTypes.length);
            }
          }
          // Per-file security analysis on sourcemap sourcesContent is
          // intentionally disabled. It was meant to catch sinks that the
          // main bundled analysis missed, but in practice every sink
          // surfaces in both coord spaces. The dedup key was
          // `(type, sink, line, column)` — bundled findings sit at
          // minified (line=2, col=big-number), source-mapped findings
          // sit at beautified (line=107, col=8), so the key never
          // matches and EVERY source-mapped sink duplicates one that
          // already exists. Duplicates then can't be opened in the
          // viewer (the source-mapped URL isn't HTTP-fetchable), so
          // they're pure reviewer noise. Re-enable only after a proper
          // cross-coord dedup (reverse-map the source-mapped location
          // through smData back to bundled coords, compare those).
        }
        if (_ep !== _dataEpoch) return;   // store was reset while this map was fetching — don't repopulate
        mergeASTResultsIntoVDD(tab, [analysis], tabId);
        mergeToGlobal(tab);
        notifyPopup(tabId);
      } catch (e) {
        console.debug("[AST:sourcemap] Parse error for %s: %s", smUrl, e.message);
      }
    }).catch(function(e) {
      console.debug("[AST:sourcemap] Network error for %s: %s", smUrl, e.message || e);
    });
}

// ─── AST Bundle Analysis ─────────────────────────────────────────────────────
