// lib/analyze.js — the ONE handoff from a delivered document to the ONE frontier. It concatenates the
// chunks this document accumulated, dispatches AST_ANALYZE (bridge.js's host WFQ pool builds the engine and
// interleaves it with every other live document by value-of-information), then attributes the engine's
// findings back to the script they came from and merges them into the doc + the global moat.
//
// WHAT WAS DELETED HERE, AND WHY IT MAY NOT COME BACK IN ANY SPELLING:
//
//   * globalStore.scriptCache + _replayCachedAST — a content-hash SEEN-SET keyed on (analyzer fingerprint,
//     document origin, SHA-256 of the page HTML). On a hit it replayed a stored result document and
//     RETURNED, so astDispatch was never called and NO ENGINE EXISTED for that document: a page whose HTML
//     is byte-identical to a previous visit never resumed its parked flows, which made the ONE continuous
//     cross-session frontier unreachable for exactly the pages a user revisits most. §NO BOUNDS is
//     categorical — "Only EMITTED OUTPUT — never identity — proves a flow is done: shared state means
//     byte-identical args can still PROGRESS, so a 're-computes nothing' proof is a cap in disguise."
//     Byte-identical HTML is identity, and the flows it skipped are distinct work. A revisit is now SLOWER
//     by exactly the work the cache was refusing to do, which is the correct behaviour and not a regression.
//
//   * _reviewQueue / _reviewDraining / _analysisInflight — a per-document, recency-ordered
//     (lastActivatedTs) scheduler with its own in-flight registry, standing in FRONT of the ONE BFS/WFQ.
//     §scheduler: "Any per-page/per-session/per-tab scheduler, registry, run-counter, or reset is the
//     cardinal violation." Recency is not value: the level-1 order is the pool's (rank live engines by
//     qjs_top_weight, admit against the RAM working-set floor, rehydrate the cold tail into the same pool),
//     and it lives in bridge.js because no instance can rank the others.
//
// A document reaches the pool ONCE because the POOL is the register of which instance holds which document
// — SECURITY.md gives the offscreen that job and hostHolderOf already answers it — never because this file
// remembers having seen it. Nothing in this file keys work on identity any more.

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

/* THE ONE CALL. A document that has delivered its CONTENT_HTML is APPENDED to the ONE frontier here and
   nowhere else: astDispatch enqueues it into bridge.js's host pool, which admits it against the RAM
   working-set floor and interleaves its engine with every other live document (and with the cold recipes
   it rehydrates) by the one WFQ. There is no queue in front of that, no in-flight register, and no cache
   consulted first — the awaited promise resolves when THIS document's engine finalizes.

   NOT awaited by its caller, and NOT wrapped in a catch: an assertion in here (or anywhere down the
   dispatch) is an invariant abort, and an unhandled rejection is the honest shape of it. A `.catch` that
   printed one debug line is what let the bridge's own contract checks land in a log nothing reads. */
async function _analyzeCombinedScripts(docKey) {
  var buf = _scriptBuffers.get(docKey);
  /* THE BUFFER IS THIS DOCUMENT'S RECORD AND THE HANDLER JUST WROTE IT. `if (!buf || !buf.pageHtml) return`
     stood here and made three different broken states — no buffer, a buffer for another document, and a
     document whose HTML never arrived — indistinguishable from a page legitimately having nothing to
     analyse, which is the one outcome that produces no symptom at all. content.js THROWS on an empty body
     rather than shipping one, so an empty pageHtml at this point is our own delivery path having lost it. */
  DCHECK(!!buf, "a document was handed to the analysis with no script buffer — the CONTENT_HTML handler " +
                "creates the buffer immediately before this call, so its absence is that record being lost " +
                "between the two lines");
  DCHECK(buf.docKey === docKey, "a script buffer is filed under a documentId that is not its own — every " +
                                "principal this analysis runs under (the SSRF origin, window.location, the " +
                                "credentialed-read origin) is read off this buffer, so a mis-filed one " +
                                "analyses a document under another document's identity");
  DCHECK(!!buf.pageHtml, "a document reached the analysis with no page HTML — content.js refuses to ship an " +
                         "empty body (it throws), so this is the bundle that was fetched being lost on the " +
                         "way here, and analysing nothing would report the page as clean");
  var tabId = buf.tabId;
  var tab = getDoc(docKey);
  var _ep = _dataEpoch;   // a Clear during the engine round-trip invalidates this run
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
      type: "AST_ANALYZE", code: combined, sourceUrl: tabUrl, documentId: docKey, origin: buf.origin, forceScript: true,
      // THE AGENT CLUSTER THIS DOCUMENT BELONGS TO — SECURITY.md keys one WASM instance on
      // `(browsing-context group, origin)`, and BOTH halves have to be BROWSER-STATED because the untrusted
      // engine may state neither. `origin` above is the MessageSender principal (opaque-unique per document);
      // `groupId` is the tab, which is the browser-set fact closest to a browsing-context group — a tab is
      // exactly one top-level traversable and every navigable nested under it is in that traversable's group.
      // NOTE THE DISTINCTION FROM THE RULE THIS FILE OTHERWISE KEEPS: a tabId may never key a DOCUMENT (a tab
      // holds many, and a (tab,frame) pair is reused across navigations with a different origin). A GROUP is
      // not a document, and it is the only thing the tab id is used as here — the origin half is what keeps
      // two documents of one tab in two clusters when they are cross-origin. `frameId` distinguishes the
      // group's TOP document from a sub-frame, which the pool needs because a same-origin sub-frame is created
      // and run INSIDE its cluster's instance (§4.8.5's insertion steps) while a top document is not.
      groupId: buf.tabId, frameId: buf.frameId,
      // HTML §8.1.3.1's TOP-LEVEL CREATION URL — the browser-provided address of the top of this document's
      // navigable chain, captured on CONTENT_HTML from sender.tab.url. It is NOT sourceUrl: this document may
      // be a sub-frame, and §8.1.3.5 decides secure-context (and therefore which [SecureContext] members the
      // engine installs) from the TOP of the chain rather than from the frame's own address.
      topLevelUrl: buf.topLevelUrl || tabUrl,
      scriptUrls: scriptUrls,
      scriptOffsets: scriptOffsets,
      pageHtml: tab._pageHtml || null,
      responseHeaders: tab._responseHeaders || {},   // real CSP/Content-Type -> engine (header-CSP is the PRIMARY policy; meta-CSP is secondary)
      // Participate in the GLOBAL cross-session frontier: this engine's residue parks to IDB under RAM
      // pressure (resource-driven, host-side) and rehydrates by value order later. With headroom the page
      // runs to completion in one visit — nothing is lost to a clock; there is NO dispatch/step quantum.
      persist: true,
    });
  } catch (e) {
    /* AN INVARIANT ABORT IS NOT A DISPATCH FAILURE. Everything down this call — the bridge's result-document
       contract, the notice router's field counts, the merge's own checks — throws through here, and recording
       it as this document's `_astError` would let the zone carry on with a contract it has already proved
       broken. RETHROW_FATAL is what keeps the ONE assertion mechanism from being locally disabled. */
    RETHROW_FATAL(e);
    console.debug("[AST:combined] sendToOffscreen failed for tab=%d: %s", tabId, e.message || e);
    tab._astError = "sendToOffscreen threw: " + (e.message || String(e));
    return;
  }
  if (!response || !response.success) {
    // The Clear button terminated the engine mid-analysis. Abort cleanly — do
    // NOT fall back to per-script re-analysis, which would re-flood the freshly
    // respawned engine right after a Clear and repopulate the just-wiped store.
    if (response && response.error === "cleared") {
      console.debug("[AST:combined] tab=%d aborted — engine cleared", tabId);
      return;
    }
    console.debug("[AST:combined] analyzeJSBundle failed for tab=%d: %s", tabId,
      response ? response.error : "no response");
    if (response && response.stack) console.debug(response.stack);
    tab._astError = "offscreen unsuccessful: " + (response ? (response.error + " | " + (response.stack || "")) : "no response");
    // SURFACE the combined-analysis failure — do NOT fall back to per-script
    // analysis. A page is reviewed as the COMBINATION of all its scripts; analysing
    // them in isolation loses the cross-script interprocedural visibility (webpack
    // chunk exports, shared globals) the whole design depends on, and emitting that
    // degraded result would MASK the real failure. _astError (above) + the engine's
    // @WHY/@E are the signal to root-cause; the resumable frontier retries via replay recipes.
    return;
  }
  tab._astError = null;
  // The bin/Clear reset fired while this analysis was in the engine. Its result
  // predates the wipe, so merging it would repopulate the just-cleared store.
  // Abandon the whole tail.
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
  if (!hasFindings) {
    console.debug("[AST:combined] No findings for tab=%d", tabId);
    // The deep orphan-residue drive already ran IN the ONE scheduler (seed_orphans is continuous,
    // chunks eval'd in place) — nothing more to dispatch here.
    return;
  }

  console.debug("[AST:combined] Findings for tab=%d: %d protoEnums, %d fieldMaps, %d fetchSites, %d secSinks, %d dangerousPatterns",
    tabId, analysis.protoEnums.length, analysis.protoFieldMaps.length, analysis.fetchCallSites.length,
    (analysis.securitySinks ? analysis.securitySinks.length : 0),
    (analysis.dangerousPatterns ? analysis.dangerousPatterns.length : 0));

  // Pre-empt mergeASTResultsIntoVDD's security merge — we split findings per-script below
  analysis._securityMerged = true;

  // Build security findings locally, then swap into tab._* slots atomically, so
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

  // Idle burst of the ONE host-level attention: advance other origins' parked frontiers by value
  // (non-blocking, serialized). This page's own residue (if it parked) is now in the global frontier.
  _driveGlobalFrontierBurst();

  // Lazy chunks were already fetched + eval'd IN PLACE by the ONE scheduler during this run
  // (@CHUNK → runEngine safeFetch → qjs_provide), so their endpoints are in `analysis` already.
  // No host-side re-fetch/re-analyze round.
}
