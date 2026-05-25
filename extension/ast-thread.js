// Web Worker thread: API/value learning by FORCED MULTI-PATH
// EXECUTION on the forked QuickJS engine (wasm). The hand-coded Babel
// value-engine (ast.js) is retired — re-implementing ECMA on Babel was
// a dead end. The language is now executed for real: QuickJS runs the
// bundle, the patched interpreter forks every branch/switch whose
// predicate derives from an opaque (unknown/attacker) input, and each
// distinct forced schedule is re-executed in a FRESH engine instance.
// Every Web-boundary call the bundle makes (XHR/fetch) is trapped by
// the host-edge model and emitted as an @H record; the union across
// all forced paths is the learned API (every branch's URL + body/query
// key AND its real computed example value). This runs in the Worker so
// neither the service worker nor the offscreen document's main thread
// blocks during analysis.
//
// importScripts (classic worker): qjs_worker.js defines self.createQJS
// (SINGLE_FILE — wasm embedded, no fetch, MV3-CSP clean); hostedge.gen
// .js sets self.__HOSTEDGE_SRC (generated from engine/qjs/hostedge.js,
// the single source of truth drive.mjs also tests); sourcemap.js keeps
// the genuinely-static sourcemap/TS helpers.
importScripts("lib/qjs/qjs_worker.js", "lib/qjs/hostedge.gen.js", "lib/sourcemap.js");

var HOSTEDGE = self.__HOSTEDGE_SRC;
var HOSTDRIVER = self.__HOSTDRIVER_SRC;

// ── Durable deep-dive state (IndexedDB) ──────────────────────────────────
// The deep orphan grind takes minutes; the MV3 service worker (and the
// offscreen doc hosting this worker) are evicted on idle, so the in-wasm
// deep cursor is volatile. The Web Worker HAS IndexedDB, so persist the
// cursor here: a fresh worker (after eviction) resumes the grind from the
// saved cursor instead of restarting it — the unused-feature surface gets
// learned eventually, across however many worker lifetimes it takes. Two
// stores: the combined bundle ("code", written ONCE — it's ~18 MB) and the
// per-batch cursor ("prog", a tiny write each batch).
var _DDB = "feDeepDB";
function _idb() {
  return new Promise(function (res, rej) {
    var r = indexedDB.open(_DDB, 2);
    r.onupgradeneeded = function () {
      var db = r.result;
      if (!db.objectStoreNames.contains("code")) db.createObjectStore("code", { keyPath: "key" });
      if (!db.objectStoreNames.contains("prog")) db.createObjectStore("prog", { keyPath: "key" });
      // Source maps for path-param name resolution (e→owner), keyed by chunk
      // URL. The offscreen worker owns this (long-lived + has IndexedDB, unlike
      // the ~30s-evicted SW): map + the chunk's original JS (sourcesContent)
      // kept until the page's grind finishes (dropped) or the bin wipes the DB.
      if (!db.objectStoreNames.contains("smaps")) db.createObjectStore("smaps", { keyPath: "key" });
    };
    r.onsuccess = function () { res(r.result); };
    r.onerror = function () { rej(r.error); };
  });
}
function _idbPut(store, rec) {
  return _idb().then(function (db) {
    return new Promise(function (res, rej) {
      var tx = db.transaction(store, "readwrite"); tx.objectStore(store).put(rec);
      tx.oncomplete = function () { res(); }; tx.onerror = function () { rej(tx.error); };
    });
  });
}
function _idbGet(store, key) {
  return _idb().then(function (db) {
    return new Promise(function (res) {
      var tx = db.transaction(store, "readonly"); var rq = tx.objectStore(store).get(key);
      rq.onsuccess = function () { res(rq.result || null); }; rq.onerror = function () { res(null); };
    });
  });
}
function _idbDel(store, key) {
  return _idb().then(function (db) {
    return new Promise(function (res) {
      var tx = db.transaction(store, "readwrite"); tx.objectStore(store).delete(key);
      tx.oncomplete = function () { res(); }; tx.onerror = function () { res(); };
    });
  });
}
function _idbAllKeys(store) {
  return _idb().then(function (db) {
    return new Promise(function (res) {
      var tx = db.transaction(store, "readonly"); var rq = tx.objectStore(store).getAllKeys();
      rq.onsuccess = function () { res(rq.result || []); }; rq.onerror = function () { res([]); };
    });
  });
}

// ── Source-map path-param name resolution (offscreen-owned) ──────────────────
// The worker (long-lived, has IndexedDB — unlike the ~30 s-evicted SW) fetches a
// chunk's map by the REAL sourceMappingURL the bundle ships (relative filename OR
// full address), parses it, and resolves a minified path read (e) → its declared
// name (owner): originalPositionFor(call-site) → original line → that template's
// path ${…} identifiers. Map JSON (its sourcesContent IS the chunk's original JS)
// is kept in feDeepDB "smaps" until the grind finishes or the bin wipes the DB; a
// worker-local parsed cache avoids re-parsing. Every step is failure-tolerant —
// a missing/404 map just leaves the param minified, never fabricated.
var _smParsed = {};   // chunkUrl → parsedMap | null (null = tried, none)
var _smChunksTouched = new Set();   // chunk URLs this grind fetched a map for (drop on done)
var _SMBT = String.fromCharCode(96);
function _smChunkForLine(line, scriptOffsets) {
  for (var i = scriptOffsets.length - 1; i >= 0; i--)
    if (line >= scriptOffsets[i].lineStart) return scriptOffsets[i];
  return scriptOffsets[0];
}
function _smOrigPos(parsed, genLine1, genCol0) {
  if (!parsed || !parsed.mappings) return null;
  var li = genLine1 - 1;
  if (li < 0 || li >= parsed.mappings.length) return null;
  var segs = parsed.mappings[li];
  if (!segs || !segs.length) return null;
  var lo = 0, hi = segs.length - 1, best = -1;
  while (lo <= hi) { var mid = (lo + hi) >>> 1; if (segs[mid].genCol <= genCol0) { best = mid; lo = mid + 1; } else hi = mid - 1; }
  if (best < 0) best = 0;
  var s = segs[best];
  if (typeof s.srcIdx !== "number" || typeof s.srcLine !== "number") return null;
  return { srcIdx: s.srcIdx, srcLine0: s.srcLine };
}
function _smNamesFromContent(content, srcLine0) {
  if (!content) return null;
  var lines = content.split("\n");
  var win = (lines[srcLine0] || "") + " " + (lines[srcLine0 + 1] || "") + " " + (lines[srcLine0 - 1] || "");
  var bt = win.indexOf(_SMBT); if (bt < 0) return null;
  var bt2 = win.indexOf(_SMBT, bt + 1);
  var tmpl = bt2 > bt ? win.slice(bt + 1, bt2) : win.slice(bt + 1);
  var qm = tmpl.indexOf("?"); var pathPart = qm >= 0 ? tmpl.slice(0, qm) : tmpl;
  var names = [], re = /\$\{\s*(?:[A-Za-z_$][\w$]*\s*\.\s*)*([A-Za-z_$][\w$]*)\s*\}/g, mm;
  while ((mm = re.exec(pathPart))) names.push(mm[1]);
  return names.length ? names : null;
}
function _smMapUrl(chunkUrl, sourceMapScripts) {
  var smu = null;
  if (Array.isArray(sourceMapScripts)) for (var i = 0; i < sourceMapScripts.length; i++)
    if (sourceMapScripts[i].scriptUrl === chunkUrl) { smu = sourceMapScripts[i].smUrl; break; }
  if (!smu) smu = chunkUrl + ".map";   // fallback ONLY when the chunk shipped no pragma
  try { return /^https?:\/\//i.test(smu) ? smu : new URL(smu, chunkUrl).href; } catch (e) { return null; }
}
async function _smGetParsed(chunkUrl, sourceMapScripts) {
  if (Object.prototype.hasOwnProperty.call(_smParsed, chunkUrl)) return _smParsed[chunkUrl];
  var parsed = null;
  try { var rec = await _idbGet("smaps", chunkUrl); if (rec && rec.json) parsed = parseSourceMap(rec.json); } catch (e) {}
  if (!parsed) {
    var url = _smMapUrl(chunkUrl, sourceMapScripts);
    if (url) {
      try {
        // Time-box so a slow/hanging map server can't stall the grind's post loop.
        var _ac = (typeof AbortController !== "undefined") ? new AbortController() : null;
        var _to = _ac ? setTimeout(function () { try { _ac.abort(); } catch (e) {} }, 8000) : 0;
        var resp = await fetch(url, _ac ? { method: "GET", credentials: "omit", signal: _ac.signal } : { method: "GET", credentials: "omit" });
        if (_to) clearTimeout(_to);
        if (resp && resp.ok) {
          var json = await resp.json();
          parsed = parseSourceMap(json);
          _smChunksTouched.add(chunkUrl);
          try { await _idbPut("smaps", { key: chunkUrl, json: json }); } catch (e) {}
        }
      } catch (e) {}
    }
  }
  _smParsed[chunkUrl] = parsed || null;
  return _smParsed[chunkUrl];
}
// One bundle frame (combined-line position) → its original template's path
// ${…} names, via the chunk's source map. Returns null if this frame's source
// has no path template (e.g. a shared fetch wrapper).
async function _smNamesForFrame(frame, scriptOffsets, sourceMapScripts) {
  if (!frame || typeof frame.line !== "number") return null;
  var chunk = _smChunkForLine(frame.line, scriptOffsets);
  if (!chunk || !chunk.url) return null;
  var parsed = await _smGetParsed(chunk.url, sourceMapScripts);
  if (!parsed) return null;
  var genLine = frame.line - chunk.lineStart + 1;
  var col0 = (frame.column != null ? frame.column : (frame.col || 1)) - 1; if (col0 < 0) col0 = 0;
  var op = _smOrigPos(parsed, genLine, col0);
  if (!op) return null;
  return _smNamesFromContent(parsed.sourcesContent && parsed.sourcesContent[op.srcIdx], op.srcLine0);
}
async function _resolveSmNames(fcs, scriptOffsets, sourceMapScripts) {
  if (!Array.isArray(fcs) || !Array.isArray(scriptOffsets) || !scriptOffsets.length) return;
  for (var i = 0; i < fcs.length; i++) {
    var cs = fcs[i];
    if (!cs || !Array.isArray(cs.params)) continue;
    var pathN = 0;
    for (var p = 0; p < cs.params.length; p++) if ((cs.params[p].location || "query") === "path") { if (!cs.params[p]._sourceMapName) pathN++; }
    if (pathN === 0) continue;
    // Walk the bundle call frames innermost→outermost: the host edge / a shared
    // fetch wrapper has no URL template, the caller that wrote `/${owner}/${repo}`
    // does. Use the first frame whose path-name count matches this site's path
    // params (exact structural match), else the first non-empty result.
    var frames = (Array.isArray(cs.bframes) && cs.bframes.length) ? cs.bframes : (cs.loc ? [cs.loc] : []);
    var names = null;
    for (var f = 0; f < frames.length; f++) {
      var fn = await _smNamesForFrame(frames[f], scriptOffsets, sourceMapScripts);
      if (!fn) continue;
      if (fn.length === pathN) { names = fn; break; }
      if (!names) names = fn;   // remember first non-empty as fallback
    }
    if (!names) continue;
    var spi = 0;
    for (var q = 0; q < cs.params.length; q++)
      if ((cs.params[q].location || "query") === "path") { if (spi < names.length) cs.params[q]._sourceMapName = names[spi]; spi++; }
  }
}
// Stable key for a combined bundle (content-sensitive: chunk set changes ⇒
// new key ⇒ no stale resume). djb2 over a stride sample + length — fast on
// ~18 MB, collision-safe enough for a resume cache.
function _deepKey(s) {
  var h = 5381, n = s.length, step = n > 65536 ? (n >> 16) : 1;
  for (var i = 0; i < n; i += step) h = ((h << 5) + h + s.charCodeAt(i)) | 0;
  return (h >>> 0).toString(36) + "_" + n;
}
// The cross-file source-viewer scope index was a Babel artifact; per
// the architecture decision it is dropped (the engine learns values by
// running code, not by indexing identifiers). Honest disable, never a
// fabricated map.
var _VIEWER_RETIRED = "source-viewer scope index dropped — value/API/taint learning is forced multi-path execution on QuickJS (see CLAUDE.md)";

// a=1&b=2 form-encoded query/body (same parser as drive.mjs).
function parsePairs(s) {
  var o = {};
  String(s).split("&").forEach(function (kv) {
    if (!kv) return;
    var i = kv.indexOf("=");
    var k, v;
    try { k = decodeURIComponent(i < 0 ? kv : kv.slice(0, i)); } catch (e) { k = i < 0 ? kv : kv.slice(0, i); }
    try { v = i < 0 ? "" : decodeURIComponent(kv.slice(i + 1)); } catch (e) { v = i < 0 ? "" : kv.slice(i + 1); }
    (o[k] || (o[k] = [])).push(v);
  });
  return o;
}

// sourcemap pragma — mirror background.js extractSourceMapUrl so the
// merge path can resolve TS originals exactly as before.
function sourceMapUrlOf(code) {
  var tail = code.length > 500 ? code.slice(-500) : code;
  var marker = "sourceMappingURL=";
  // LAST occurrence — the real trailing annotation, not a "sourceMappingURL="
  // that appears earlier inside a string literal.
  var idx = tail.lastIndexOf(marker);
  if (idx === -1) return null;
  var start = idx + marker.length;
  while (start < tail.length && (tail.charCodeAt(start) === 32 || tail.charCodeAt(start) === 9)) start++;
  var end = start;
  while (end < tail.length && tail.charCodeAt(end) > 32) end++;
  var url = tail.substring(start, end);
  // Block-comment form `/*# sourceMappingURL=foo.js.map*/` (github ships both
  // `//#` line and `/*# … */` block styles) — strip the trailing `*/` the
  // line form lacks, else the fetch URL ends in `*/` and 404s.
  var star = url.indexOf("*/");
  if (star >= 0) url = url.slice(0, star);
  return url.length ? url : null;
}

// An opaque value that flowed into a URL/method stringifies to
// "[object Object]" (the sentinel has no toString). That is a resolver
// gap, NOT an endpoint — surface it on resolverErrors, never emit a
// fabricated path. (Empty/whitespace url = same: nothing resolved.)
// When the opaque passed through new URL()/encodeURI before the host
// edge, the space is percent-encoded to "[object%20Object]"; that is the
// same unresolved marker and must be caught too, or the encoded form
// slips through as a fabricated endpoint. (A real structural hole from
// __feUrlShape is "{name}" and never contains this marker.)
function isUnresolved(s) {
  if (!s) return true;
  var t = String(s).trim();
  return !t || t.indexOf("[object Object]") >= 0 || t.indexOf("[object%20Object]") >= 0;
}

// One coherent forced execution per schedule; enumerate schedules from
// the trace's F (frontier) records exactly as drive.mjs proved. Each
// run is a fresh QuickJS instance (no isolate snapshotting — the
// extension's offscreen/sandbox model). Pairing of XHR open->send is
// PER RUN so every branch's body variant is kept; examples then
// aggregate across runs.
// Build the page's server-rendered DOM data islands + id'd elements
// into the virtual document BEFORE the bundle runs, so code that
// bootstraps from them (GitHub: getElementById("client-env")
// .textContent → JSON.parse) runs correctly instead of throwing
// "requested before it was loaded". This is running the code right,
// not recovering from a self-inflicted crash.
function buildPageDomSrc(scriptUrls) {
  var srcs = Array.isArray(scriptUrls) ? scriptUrls : [];
  if (!srcs.length) return null;
  return "(function(){try{\n" +
    "var SR=" + JSON.stringify(srcs) + ";\n" +
    // Real <script src> tags + document.currentScript: the page's
    // actual script elements, so a bundle that derives its base URL
    // from currentScript.src / getElementsByTagName("script") runs its
    // own real code correctly — not a model, the real DOM it expects.
    "var LS=null;for(var s=0;s<SR.length;s++){var sc=document.createElement('script');sc.src=SR[s];sc.setAttribute('src',SR[s]);(document.head||document.documentElement||document.body).appendChild(sc);LS=sc;}\n" +
    "if(LS){try{Object.defineProperty(document,'currentScript',{get:function(){return LS;},configurable:true});}catch(e){}}\n" +
    "}catch(e){}})();";
}

async function forcedAnalyze(code, sourceUrl, scriptUrls, pageHtml, seedOnly, deep, resumeCursor, visitTs, drivenIds, maxBatches, scriptOffsets, sourceMapScripts) {
  var t0 = Date.now();
  // Rotation bound: the deep grind advances at most this many batches per
  // invocation, then RETURNS (leaving its driven-set persisted) so the scheduler
  // can rotate to another page. Without it one page would grind to completion
  // while others wait ("getting stuck"). 0/undefined → effectively unbounded.
  var _maxDeep = (typeof maxBatches === "number" && maxBatches > 0) ? maxBatches : 200000;
  var _resume = (typeof resumeCursor === "number" && resumeCursor >= 0);
  // Cross-session resume is by a per-function-ID DRIVEN SET (file:line:col hash
  // the engine emits as `@DD`), not a cursor index — so the engine's usefulness
  // sort can reorder freely without skipping. `_driven` accumulates this run's
  // @DD ids on top of the persisted set; it's written to the engine's `/driven`
  // before the first deep-step so already-driven functions are skipped.
  var _driven = new Set(Array.isArray(drivenIds) ? drivenIds : []);
  // Relevance = when the PAGE was last visited (user's definition). Set once on
  // the initial grind (a real page visit ≈ now); PRESERVED across resume batches
  // so it stays the visit time, NOT bumped to "now" each batch — otherwise a
  // long-running grind for an old page would masquerade as recently-visited and
  // starve a page the user actually opened more recently.
  var _vts = (typeof visitTs === "number" && visitTs > 0) ? visitTs : Date.now();
  // Throttle: cap any single yield (ms) — the work-burst granularity is one
  // schedule/deep-batch run, so rest ≈ work gives ~50% duty, capped here so a
  // slow run doesn't stall the queue too long. DEEP_BATCH = orphans per deep
  // step (small → short bursts → the device stays cool).
  var THROTTLE_CAP_MS = 4000;
  var DEEP_BATCH = 3;
  // /h.js host-edge model, /p.js the page's server-rendered DOM data
  // islands (so the bundle bootstraps correctly), /b.js the analyzed
  // bundle, /d.js the epilogue that pumps load/ready/message + XHR
  // completion + timers so fetch code inside callbacks runs.
  //
  // /pre.js is generated when content.js shipped raw server-rendered
  // HTML: it stashes the HTML on a global so /h.js (loaded next) can
  // hand it to Lexbor's HTML5 parser via document.__feLoadPage —
  // replacing the seed `<!DOCTYPE html><html><head></head><body></
  // body></html>` document with the real page DOM. Without this,
  // bundles that wait on customElements (Catalyst/Turbo) or
  // querySelector results from the server-rendered HTML never reach
  // their fetch sites.
  var inMem = [["/h.js", HOSTEDGE], ["/b.js", code], ["/d.js", HOSTDRIVER]];
  var fileArgs = ["/h.js", "/b.js", "/d.js"];
  // globalThis (not self) — QuickJS embedded in the wasm here is running
  // script bytecode, not a Web Worker; `self` isn't defined until
  // hostedge.js's prelude maps it. Hostedge.js reads __pageHtml /
  // __pageUrl via `G.…`. __pageUrl is the real page location so the
  // engine's `location` is the actual site (origin-gated init + same-
  // origin URL builds resolve correctly), not the example.com seed.
  var preParts = [];
  if (typeof sourceUrl === "string" && sourceUrl.length > 0)
    preParts.push("globalThis.__pageUrl = " + JSON.stringify(sourceUrl) + ";");
  if (typeof pageHtml === "string" && pageHtml.length > 0)
    preParts.push("globalThis.__pageHtml = " + JSON.stringify(pageHtml) + ";");
  if (preParts.length) {
    inMem.unshift(["/pre.js", preParts.join("\n")]);
    fileArgs.unshift("/pre.js");
  }
  var pageDom = buildPageDomSrc(scriptUrls);
  if (pageDom) {
    inMem.splice(fileArgs.indexOf("/h.js") + 1, 0, ["/p.js", pageDom]);
    fileArgs.splice(fileArgs.indexOf("/h.js") + 1, 0, "/p.js");
  }
  // On a durable RESUME (fresh worker after eviction) skip the BFS entirely —
  // the reached endpoints were already learned + persisted in the original
  // run; this run only continues the deep orphan grind from the saved cursor.
  var work = _resume ? [] : [""], seen = new Set([""]);
  // X-Force Algorithm 1 §3.2 fitness hybrid (mirrors drive.mjs header
  // comment). Linear (edge prune) by default for fast saturation; on
  // a NEW @S sink, switch to Quadratic boost (all eligible frontiers
  // from the fruitful run enqueued regardless of edge coverage);
  // until any sink fires, Exponential bootstrap (all frontiers,
  // bounded by the distinct-prefix seen-set).
  var coveredEdges = new Set();     // Linear fitness: "<branchKey>:<dec>"
  var sinkSeen = new Set();         // Quadratic-boost / bootstrap-Exponential trigger
  var structural = new Map();       // JAW @T: host-edge sites in unreached code
  var runs = 0;
  var methods = new Map();          // "METHOD path" -> {method,path,kind,params,loc,chain}
  var resolverErrors = [];
  var reSeen = new Set();
  var securitySinks = [];
  var secSeen = new Set();
  var chunkUrls = new Set();         // lazy-chunk URLs the bundle's loader requested (script.src=)

  // Normalise the engine's @P (per-leaf step records) into the shape
  // background.js's _runExploitProbe expects:
  //   url{hash,search,pathname}   — persistent-source values applied
  //                                 at chrome.tabs.create({url: …})
  //   events[]                    — per-event ordered postMessage
  //                                 payloads (each .payload is a real
  //                                 nested object reconstructed from
  //                                 the engine's `data.type` /
  //                                 `data.html` dot-paths)
  //   storage[], cookies[]        — pre-injection actions
  //   verify: "marker"            — intercept.js wraps the sink and
  //                                 sets a flag when the marker text
  //                                 hits it; the only valid PoC
  //                                 evidence (CSP-blocked = invalid)
  function adaptPoc(rawPoc, sinkType, psi) {
    var out = { url: { hash: null, search: null, pathname: null },
                events: [], storage: [], cookies: [], verify: "marker" };
    var steps = (rawPoc && Array.isArray(rawPoc.steps)) ? rawPoc.steps : [];
    // Extract the sink-bearing leaf id + field path from psi.
    // psi format example: "(. (. $3:handler.event \"data\") \"html\")"
    // → root leaf id=3, field path = data.html (which adaptPoc strips
    // to just "html" since the event's `payload` IS the .data object).
    // The orchestrator weaves the marker into THIS field of THIS
    // event only; every other field keeps its Z3-solved gate value.
    var sinkLeafId = -1;
    var sinkField = null;
    if (typeof psi === "string") {
      var leafMatch = psi.match(/\$(\d+):/);
      if (leafMatch) sinkLeafId = +leafMatch[1];
      // Member accesses appear as (. <root> "key") nested innermost-first
      // (`(. (. $3 "data") "html")`). Pull the quoted segments in
      // outer-to-inner order = the OP order = the property chain.
      var keys = [];
      var re = /"([^"\\]*(?:\\.[^"\\]*)*)"/g, m;
      while ((m = re.exec(psi))) keys.push(m[1]);
      if (keys.length) sinkField = keys.join(".");
    }
    function setNested(obj, dotPath, value) {
      var parts = String(dotPath).split(".");
      var cur = obj;
      for (var i = 0; i < parts.length - 1; i++) {
        var k = parts[i];
        if (typeof cur[k] !== "object" || cur[k] === null) cur[k] = {};
        cur = cur[k];
      }
      cur[parts[parts.length - 1]] = value;
    }
    for (var i = 0; i < steps.length; i++) {
      var s = steps[i];
      if (!s) continue;
      if (s.action === "hash") out.url.hash = s.value || null;
      else if (s.action === "search") out.url.search = s.value || null;
      else if (s.action === "cookie") out.cookies.push({ value: s.value || null });
      else if (s.action === "localStorage") out.storage.push({ key: s.key || null, value: s.value || null });
      else if (s.action === "event" || s.action === "postMessage") {
        // Reassemble nested payload from dot-paths. The handler reads
        // `e.data.X`, captured as "data.X" path; strip the leading
        // "data." so payload IS what the attacker sends as .data.
        var payload;
        if (s.fields && Object.keys(s.fields).length > 0) {
          var raw = {};
          for (var k in s.fields) setNested(raw, k, s.fields[k]);
          payload = (raw.data && typeof raw.data === "object" && Object.keys(raw).length === 1)
                    ? raw.data : raw;
        } else {
          payload = s.value || "";
        }
        out.events.push({ kind: "postMessage", payload: payload, leafId: s.id });
      }
    }
    // Engine emits steps in execution order; preserve as seq. The
    // event whose leafId matches sinkLeafId carries the payload —
    // mark it so the orchestrator weaves the marker only into the
    // sink-bearing field (not all string fields, which would break
    // gate equality on `type === "render"` etc.).
    for (var ei = 0; ei < out.events.length; ei++) {
      out.events[ei].seq = ei;
    }
    if (sinkLeafId >= 0) {
      for (var ej = 0; ej < out.events.length; ej++) {
        if (out.events[ej].leafId === sinkLeafId) {
          out.events[ej].carriesPayload = true;
          // sinkField is "data.html" etc. — adaptPoc strips the
          // leading "data." for the payload object (since payload IS
          // the .data shape). Strip here too so the orchestrator can
          // directly setByPath(payload, payloadField, marker:value).
          if (sinkField) {
            out.events[ej].payloadField = sinkField.indexOf("data.") === 0
              ? sinkField.slice(5) : sinkField;
          }
        }
      }
    }
    // Fallback when psi was missing: the last event still gets the
    // marker, but only on its single string field if there is one.
    if (sinkLeafId < 0 && out.events.length) {
      out.events[out.events.length - 1].carriesPayload = true;
    }
    return out;
  }

  // Pick the source position from a captured stack (frames[0] is the
  // literal host-API call). Prefer a /b.js (analyzed-bundle) frame so
  // the line is in the combined-bundle space background.js maps via
  // scriptOffsets; fall back to the innermost frame.
  function pickSite(at) {
    if (!at || !at.length) return { loc: null, chain: [], bframes: [] };
    var pick = at[0];
    for (var i = 0; i < at.length; i++) { if (String(at[i].file).indexOf("/b.js") >= 0) { pick = at[i]; break; } }
    var chain = [];
    // bframes = every analyzed-bundle (/b.js) frame, innermost→outermost. The
    // innermost is often a shared wrapper (e.g. the patched global fetch in
    // fetch-patch.ts) whose source has NO URL template; the real ${owner}/${repo}
    // template is in a CALLER frame. Source-map name resolution walks these.
    var bframes = [];
    for (var j = 0; j < at.length; j++) {
      chain.push({ line: at[j].line, column: at[j].col });
      if (String(at[j].file).indexOf("/b.js") >= 0) bframes.push({ line: at[j].line, column: at[j].col });
    }
    return { loc: { line: pick.line, column: pick.col }, chain: chain, bframes: bframes };
  }

  function ep(method, rawUrl, body, kind, at, shape, hdrs, holes) {
    if (isUnresolved(rawUrl) || isUnresolved(method)) {
      var rk = (method || "?") + " " + (rawUrl || "");
      if (!reSeen.has(rk)) {
        reSeen.add(rk);
        resolverErrors.push({
          context: kind + " call site (" + (sourceUrl || "bundle") + ")",
          message: "request target did not resolve to a concrete string at the converged fixpoint (opaque component reached the " + kind + " URL/method): " + JSON.stringify({ method: method, url: rawUrl }),
        });
      }
      return;
    }
    var path = rawUrl, qs = "";
    var q = rawUrl.indexOf("?");
    if (q >= 0) { path = rawUrl.slice(0, q); qs = rawUrl.slice(q + 1); }
    var key = method + " " + path;
    var m = methods.get(key);
    if (!m) { m = { method: method, path: path, kind: kind, params: new Map(), loc: null, chain: [], bframes: [], headers: {} }; methods.set(key, m); }
    if (!m.loc && at) { var s = pickSite(at); m.loc = s.loc; m.chain = s.chain; m.bframes = s.bframes; }
    // requiredHeaders: the SET the bundle actually attached at the host edge
    // (fetch init.headers / XHR setRequestHeader), per-header literal-vs-opaque
    // provenance preserved ({name:{kind:"literal",value}|{kind:"opaque"}}).
    // Merge across forced runs; a literal supersedes an earlier opaque for the
    // same header (a concrete value is the better example).
    if (hdrs && typeof hdrs === "object") {
      for (var hk in hdrs) {
        var hv = hdrs[hk];
        if (!hv) continue;
        var prev = m.headers[hk];
        if (!prev || (prev.kind === "opaque" && hv.kind === "literal")) m.headers[hk] = hv;
      }
    }
    var add = function (n, loc, val, holeLoc) {
      var p = m.params.get(n);
      if (!p) { p = { name: n, location: loc, examples: new Set() }; m.params.set(n, p); }
      // Generated bundle read site of this hole (from __feUrlHoles) so the SW
      // can resolve the declared name via a source-map library. First wins.
      if (holeLoc && !p.holeLoc) p.holeLoc = holeLoc;
      // A "{name}" value is a __feUrlShape opaque marker (e.g. ?q={hash}
      // when the query value was attacker/server input), NOT a real
      // example — record the param, omit the synthetic value.
      if (val !== undefined && val !== "" && !isUnresolved(val) && !/^\{[^}]*\}$/.test(String(val))) p.examples.add(val);
    };
    // Pair a path/query hole name with its generated position from __feUrlHoles.
    var holeLocOf = function (nm) {
      if (!holes || !holes.length) return null;
      for (var i = 0; i < holes.length; i++) if (holes[i] && holes[i].name === nm && holes[i].line) return { line: holes[i].line, column: holes[i].col };
      return null;
    };
    // Path-template params: __feUrlShape renders an opaque path segment as
    // {name} (e.g. /settings/avatars/{id} — id is real attacker/server
    // input with no static value). Record each as a path param (opaque, no
    // example) so the structure is learned; the path keeps the {name}
    // template (OpenAPI style), never a fabricated value.
    var ppRe = /\{([^}\/]+)\}/g, ppm;
    while ((ppm = ppRe.exec(path))) add(ppm[1], "path", undefined, holeLocOf(ppm[1]));
    var qp = parsePairs(qs);
    for (var k in qp) for (var vi = 0; vi < qp[k].length; vi++) add(k, "query", qp[k][vi], holeLocOf(k));
    // Prefer hostedge's structured `shape` over JSON.parse(body) — the
    // shape carries per-field provenance (literal value vs opaque
    // attacker-tainted), so `{action:"favorite", target_id:<opaque>}`
    // becomes two distinct params: `action` with example "favorite"
    // and `target_id` with no examples but recorded as opaque-sourced.
    // Falls back to body-string parsing when shape isn't supplied
    // (older record format / non-object body).
    if (shape && shape.fields) {
      var _walk = function (fields, pref) {
        for (var fk in fields) {
          var fv = fields[fk];
          var pn = pref ? pref + "." + fk : fk;
          if (!fv || !fv.kind) continue;
          if (fv.kind === "literal") add(pn, "body", fv.value);
          else if (fv.kind === "opaque") add(pn, "body", undefined);   // record param name; example omitted
          else if (fv.kind === "array") {
            for (var ii = 0; ii < fv.items.length; ii++) {
              var it = fv.items[ii];
              if (it && it.kind === "literal") add(pn, "body", it.value);
              else if (it && (it.kind === "object" || it.kind === "formdata" || it.kind === "params") && it.fields) _walk(it.fields, pn);
            }
          }
          else if (fv.fields) _walk(fv.fields, pn);
        }
      };
      _walk(shape.fields, "");
    } else if (shape && shape.kind === "opaque") {
      // The whole body is a single opaque blob (e.g. fetch(url,{body:
      // someOpaqueValue})) — there are no field names to learn. Do NOT
      // fall through to string-parsing String(opaque)="[object Object]",
      // which would invent a body param literally named "[object Object]".
    } else if (body != null && body !== "" && !isUnresolved(body)) {
      var bj = null;
      try { bj = JSON.parse(body); } catch (e) {}
      if (bj && typeof bj === "object" && !Array.isArray(bj)) {
        for (var bk in bj) add(bk, "body", typeof bj[bk] === "object" ? JSON.stringify(bj[bk]) : String(bj[bk]));
      } else {
        var bp = parsePairs(body);
        for (var k2 in bp) for (var vj = 0; vj < bp[k2].length; vj++) add(k2, "body", bp[k2][vj]);
      }
    }
  }

  // wasm linear memory growth is MONOTONIC — `memory.grow` never
  // returns pages to the OS within an instance. Reusing one instance
  // across the BFS therefore accumulates each schedule's footprint and
  // climbs until Chrome refuses to grow it and emscripten aborts
  // (crash-dump confirmed: e0000008, ~18 GB). A single github run
  // plateaus at ~920 MB (measured), so the only way to keep peak
  // bounded is to RECYCLE to a fresh instance once the current one has
  // accumulated past a ratio of its first-run footprint — a memory
  // watchdog (robustness), NOT an analysis cap: every queued schedule
  // still runs and coverage is unchanged; we just move the work to a
  // fresh engine and let GC reclaim the old one's whole linear memory.
  var stdout = [];
  var stderr = [];
  // emscripten abort() (e.g. the JS_FreeRuntime debug assert at engine
  // teardown, which a heavy __feDriveStatic run on a large bundle can trip)
  // POISONS the wasm instance — every later callMain on it throws. The run's
  // @H/@T/@S were already captured via print BEFORE teardown, so the schedule
  // loop just recycles to a fresh instance and keeps going; no coverage is
  // lost. Same disposable-instance principle as the memory watchdog below.
  var instAborted = false;
  async function freshInstance() {
    instAborted = false;
    var inst = await self.createQJS({
      noInitialRun: true,
      print: function (s) { stdout.push(s); },
      // Surface engine/bundle errors — never swallow. A bundle that
      // throws is a signal the host model is incomplete; the detail
      // (qjsmain emits "@E <file> :: <msg>" + stack here) becomes a
      // visible resolverError so the gap gets fixed, not hidden.
      printErr: function (s) { stderr.push(s); },
      onAbort: function () { instAborted = true; },
    });
    for (var i = 0; i < inMem.length; i++) inst.FS.writeFile(inMem[i][0], inMem[i][1]);
    return inst;
  }
  function wasmBytes(inst) {
    try { return inst.HEAPU8 ? inst.HEAPU8.buffer.byteLength : 0; } catch (e) { return 0; }
  }
  var m = await freshInstance();
  var baselineBytes = 0;        // first run's footprint; recycle ratio is relative to it
  // Bytecode-cache the bundle ONCE: re-parsing the multi-MB /b.js on every
  // schedule's fresh runtime dominates per-run cost (~850 ms on github);
  // compile it to /b.bc once and run the compiled form each schedule
  // (measured ~1.6× faster/run, identical @H). Same wasm INSTANCE keeps
  // its MEMFS across callMain, so /b.bc persists; on recycle, freshInstance
  // re-writes it from inMem. If compile fails, fall back to the source.
  try {
    m.callMain(["--fe-emit-bc", "/b.js", "/b.bc"]);
    var _bc = m.FS.readFile("/b.bc");
    if (_bc && _bc.length > 0) {
      var _bi = fileArgs.indexOf("/b.js");
      if (_bi >= 0) fileArgs[_bi] = "/b.bc";
      inMem = inMem.filter(function (e) { return e[0] !== "/b.js"; });
      inMem.push(["/b.bc", _bc]);
      try { m.FS.unlink("/b.js"); } catch (e) {}   // drop the 7 MB source from this instance
    }
  } catch (e) { /* keep /b.js source path on any compile failure */ }
  // Aggregate ONE run's stdout (@E/@T/@H/@S/@P/@Z) into the shared learning
  // state. Reads the closure `stdout` (each run clears + fills it first).
  // Shared by the BFS schedule runs AND the throttled deep-step batches, so
  // the deep pass's @H/@S/@T are learned identically to the reached ones.
  function processStdout() {
    var rh = [];
    var pendingSec = null;                        // @S waiting for paired @Z verdict
    var pendingPoC = null;                        // @P arriving between @S and @Z
    for (var li = 0; li < stdout.length; li++) {
      var line = stdout[li];
      if (line.slice(0, 3) === "@E ") {
        var ej; try { ej = JSON.parse(line.slice(3)); } catch (e) { ej = { message: line.slice(3) }; }
        var emsg = String(ej.message || "(throw)");
        var ekey = "E:" + emsg.slice(0, 120);
        if (!reSeen.has(ekey)) {
          reSeen.add(ekey);
          var firstFrame = String(ej.stack || "").split("\n").filter(function (s) { return s.indexOf("/b.js") >= 0; })[0] || "";
          var snippet = null;
          var fm = /\/b\.js:(\d+):(\d+)/.exec(firstFrame);
          if (fm) {
            var ln = +fm[1], cl = +fm[2];
            var srcLine = String(code).split("\n")[ln - 1];
            if (srcLine != null) {
              var a = Math.max(0, cl - 240);
              snippet = { line: ln, column: cl,
                text: srcLine.slice(a, Math.min(srcLine.length, cl + 180)) };
            }
          }
          resolverErrors.push({
            context: "host-model gap (" + (sourceUrl || "bundle") + ")",
            message: "bundle threw under the host model — a Web API it reached is not modelled correctly: " + emsg + (firstFrame ? "  @ " + firstFrame.trim() : "") + (snippet ? "\n  src: …" + snippet.text + "…" : ""),
            snippet: snippet,
          });
        }
        continue;
      }
      if (line.slice(0, 3) === "@T ") {
        try {
          var tr = JSON.parse(line.slice(3));
          structural.set(tr.api + "@" + tr.file + ":" + tr.line + ":" + tr.col,
            { api: tr.api, file: tr.file, line: tr.line, col: tr.col, args: tr.args || [] });
        } catch (e) {}
        continue;
      }
      if (line.slice(0, 3) === "@H ") {
        try { rh.push(JSON.parse(line.slice(3))); } catch (e) {}
      } else if (line.slice(0, 3) === "@S ") {
        try { pendingSec = JSON.parse(line.slice(3)); pendingPoC = null; } catch (e) { pendingSec = null; pendingPoC = null; }
      } else if (line.slice(0, 3) === "@P ") {
        try { pendingPoC = JSON.parse(line.slice(3)); } catch (e) { pendingPoC = null; }
      } else if (line.slice(0, 3) === "@Z ") {
        var zr; try { zr = JSON.parse(line.slice(3)); } catch (e) { pendingSec = null; pendingPoC = null; continue; }
        if (!pendingSec) continue;
        if (zr.verdict === "INFEASIBLE") { pendingSec = null; pendingPoC = null; continue; }
        var ss = pickSite(pendingSec.at);
        var sk = pendingSec.type + "|" + pendingSec.sink + "|" + (ss.loc ? ss.loc.line + ":" + ss.loc.column : "?");
        if (secSeen.has(sk)) { pendingSec = null; pendingPoC = null; continue; }
        secSeen.add(sk);
        sinkSeen.add(sk);
        var sev = pendingSec.type === "code-exec" ? "critical" : "high";
        if (zr.verdict === "TAINT_REACH") sev = "medium";
        var poc = null;
        if (pendingPoC && Array.isArray(pendingPoC.steps) && pendingPoC.steps.length) {
          poc = adaptPoc(pendingPoC, pendingSec.type, zr.psi);
        }
        securitySinks.push({
          type: pendingSec.type,
          sink: pendingSec.sink,
          source: "host-unknown attacker input reached the sink (forced multi-path execution)",
          severity: sev,
          location: ss.loc || { line: 0, column: 0 },
          taintPath: ss.chain.map(function (c) { return { at: c }; }),
          value: pendingSec.value != null ? String(pendingSec.value) : null,
          verdict: zr.verdict,
          witness: zr.witness || null,
          psi: zr.psi || null,
          phi: zr.phi || null,
          poc: poc,
        });
        pendingSec = null;
        pendingPoC = null;
      }
    }
    var pend = null;
    for (var ri = 0; ri < rh.length; ri++) {
      var r = rh[ri];
      if (r.api === "XMLHttpRequest.open") { if (pend) ep(pend.m, pend.u, null, "xhr", pend.at, undefined, undefined, pend.holes); pend = { m: r.args[0], u: r.args[1], at: r.at, holes: r.args[2] }; }
      else if (r.api === "XMLHttpRequest.send") { if (pend) { ep(pend.m, pend.u, r.args[0], "xhr", pend.at, r.args[1], r.args[2], pend.holes); pend = null; } }
      else if (r.api === "fetch") ep(r.args[1] || "GET", r.args[0], r.args[2], "fetch", r.at, r.args[3], r.args[4], r.args[5]);
      else if (r.api === "script" && r.args && r.args[0] && !isUnresolved(r.args[0])) chunkUrls.add(String(r.args[0]));
    }
    if (pend) ep(pend.m, pend.u, null, "xhr", pend.at, undefined, undefined, pend.holes);
  }
  while (work.length) {
    var job = work.shift();
    var sched = typeof job === "string" ? job : job.sched;
    // Pop-time gate (mirrors drive.mjs). During bootstrap (no sink
    // fired anywhere) every queued job runs — pruning here would
    // defeat the bootstrap search. After bootstrap, Linear's edge
    // prune applies unless `job.deep` (Quadratic-boost bypass). Edge-
    // prune non-deep jobs whenever their edge is already covered — NOT
    // gated on a security sink having fired. Gating on `sinkSeen.size>0`
    // meant an API-only page (no @S ever) never edge-pruned and ran every
    // queued schedule: the X-Force path explosion that OOM-crashed github.
    if (job && job.key && coveredEdges.has(job.key + ":1")) continue;
    runs++;
    var _runT0 = Date.now();
    stdout.length = 0;
    stderr.length = 0;
    try {
      m.callMain(["--fe-exec", "--fe-sched=" + sched, "--fe-trace=/t.tr"].concat(fileArgs));
    } catch (e) { /* engine may longjmp on exit; trace + stdout still valid */ }
    var trace = "";
    try { trace = m.FS.readFile("/t.tr", { encoding: "utf8" }); } catch (e) {}
    // Memory watchdog: capture the first run's footprint as the
    // baseline; once the (monotonic) wasm memory has grown past 2×
    // that baseline, the instance is accumulating across schedules —
    // recycle. The 2× ratio means "one extra run's worth beyond the
    // first has stuck"; it is relative to the measured per-bundle
    // footprint (tiny for jQuery, ~920 MB for github), not an absolute
    // constant. Yield first so V8 GC reclaims the dropped instance's
    // linear memory before the fresh one allocates.
    var memNow = wasmBytes(m);
    if (instAborted) {
      // The engine aborted this run at teardown — the instance is poisoned,
      // recycle before the next schedule (output for THIS run was already
      // captured in stdout/trace above).
      m = null;
      await new Promise(function (r) { setTimeout(r, 0); });
      m = await freshInstance();
      baselineBytes = 0;
    } else if (baselineBytes === 0) {
      baselineBytes = memNow;
    } else if (memNow > baselineBytes * 2) {
      m = null;
      await new Promise(function (r) { setTimeout(r, 0); });
      m = await freshInstance();
      baselineBytes = 0;
    }

    processStdout();

    // X-Force §3.2 hybrid: Linear edge prune by default; Quadratic
    // boost when this run was fruitful; bootstrap-Exponential while
    // no sink has fired anywhere yet. Frontiers enqueued under any
    // boost variant carry `deep:true` so the pop-time gate also
    // bypasses them.
    var decisions = [], frontiers = [], tl = trace.split("\n");
    for (var ti = 0; ti < tl.length; ti++) {
      var b = tl[ti].match(/^B (\d+) (\d) (\d+)/);
      if (b) { decisions[+b[1]] = b[2]; coveredEdges.add(b[3] + ":" + b[2]); continue; }
      var f = tl[ti].match(/^F (\d+) (\d+)/);
      if (f) frontiers.push({ i: +f[1], key: f[2] });
    }
    // Pure Linear edge-coverage: enqueue a frontier ONLY if its flipped
    // edge (key,1) is uncovered, so N is bounded by the distinct host-
    // relevant branch-edge set (polynomial) — the BFS terminates and can't
    // OOM. The Quadratic "deepen" (re-enqueue COVERED edges from a fruitful
    // run to chase branch COMBINATIONS) is removed: combination exploration
    // is exponential and, without a cap (forbidden), explodes — measured on
    // github the deepen queue grew 46→819 in 20 runs. Per-field value
    // spreads (_gate role/tier) come from each branch's OWN edge so they
    // survive; the cost is deep multi-condition-gated endpoints
    // (`if(a)if(b)fetch`) — the inherent X-Force path-explosion limit under
    // no-cap/no-snapshot, not a coverage silently dropped.
    // seedOnly: the chunk re-analysis pass. The combined eager+chunks bundle
    // is ~4× the eager size; running the full value-spread BFS over it (33+
    // schedules × a ~18 MB boot each) is the minutes-long cliff. The chunk
    // endpoints are recovered structurally by the seed's loader/static drive
    // + cascade (their args are opaque anyway — no per-value spread to chase),
    // so the seed run alone suffices; skip frontier enqueue. The reached
    // EAGER endpoints already got their value spread from round 1's full BFS.
    if (!seedOnly) for (var ki = 0; ki < frontiers.length; ki++) {
      var fr = frontiers[ki];
      if (coveredEdges.has(fr.key + ":1")) continue;
      var ns = decisions.slice(0, fr.i).map(function (d) { return d || "0"; }).join("") + "1";
      if (!seen.has(ns)) { seen.add(ns); work.push({ sched: ns, key: fr.key }); }
    }
    // THROTTLE: yield the core ~as long as this schedule ran (≈50% duty), so
    // the analysis never PINS a core — it runs cool in the background. Time is
    // free (this is a queued background reviewer); a maxed core is not.
    if (work.length) await new Promise(function (r) { setTimeout(r, Math.min(THROTTLE_CAP_MS, Date.now() - _runT0)); });
  }

  // DEEP pass (throttled, resumable): learn the unused/render-gated chunk
  // endpoints (login-gated preheat) without pinning the core. The bundle
  // boots ONCE into qjsmain's persistent runtime; each --fe-deep-step drives
  // a small batch of orphan @T functions and reports how many remain, and we
  // sleep between batches. One instance (bounded memory), low CPU duty.
  var _deepStats = { steps: 0, rem: -1, stop: "n/a", total: -1,
    combinedKB: Math.round((code ? code.length : 0) / 1024),
    hasPreheatSrc: !!(code && code.indexOf("issues/preheat/index") >= 0) };
  if (deep && m) {
    var _dkey = _deepKey(code);
    var _drem = 1, _dguard = 0, _dcur = _resume ? resumeCursor : 0, _lastPartialN = methods.size, _lastPartialSinks = securitySinks.length;
    _deepStats.stop = "done";
    // Persist the combined bundle ONCE (heavy, ~18 MB) so a fresh worker can
    // resume; the cursor is a tiny per-batch write below.
    try { await _idbPut("code", { key: _dkey, code: code, scriptUrls: scriptUrls || null, pageHtml: pageHtml || null, sourceUrl: sourceUrl || "", scriptOffsets: scriptOffsets || null, sourceMapScripts: sourceMapScripts || null }); } catch (e) {}
    try { await _idbPut("prog", { key: _dkey, cur: _dcur, ts: Date.now(), vts: _vts, driven: Array.from(_driven) }); } catch (e) {}
    // Hand the engine the already-driven ids (from a prior session/batch) so its
    // first deep-step skips them — cross-session resume by driven-SET, not cursor.
    if (_driven.size) { try { m.FS.writeFile("/driven", Array.from(_driven).join("\n")); } catch (e) {} }
    while (_drem > 0 && _dguard++ < _maxDeep) {
      stdout.length = 0; stderr.length = 0;
      var _dT0 = Date.now();
      var _args = ["--fe-deep-step=" + DEEP_BATCH];   // resume is via /driven (written above), not a cursor seek
      try { m.callMain(_args.concat(fileArgs)); }
      catch (e) { _deepStats.stop = "throw:" + ((e && e.message) || e); break; }
      _drem = -1;
      for (var _di = 0; _di < stdout.length; _di++) {
        var _ln = stdout[_di];
        if (_ln.slice(0, 4) === "@DS ") { try { var _ds = JSON.parse(_ln.slice(4)); _drem = _ds.rem; if (typeof _ds.cur === "number") _dcur = _ds.cur; } catch (e) { _drem = 0; } }
        else if (_ln.slice(0, 4) === "@DD ") { var _did = _ln.slice(4).trim(); if (_did) _driven.add(_did); }   // function driven → driven-set (persisted for resume)
      }
      processStdout();                       // aggregate this batch's @H/@S/@T
      _deepStats.steps++; _deepStats.rem = _drem;
      if (_deepStats.total < 0 && _drem >= 0) _deepStats.total = _drem + DEEP_BATCH;   // ≈ residue size
      try { await _idbPut("prog", { key: _dkey, cur: _dcur, ts: Date.now(), vts: _vts, driven: Array.from(_driven) }); } catch (e) {}   // durable cursor
      // Stream what this batch learned to the UI instead of waiting for the
      // whole (minutes-long) grind. Fire on: (a) new endpoints OR new SECURITY
      // SINKS (the highest-value growth — sink-reaching fns can fire @S without
      // an endpoint, so an endpoint-only trigger would miss them); OR (b) a step
      // interval / completion, so the deep-status PROGRESS bar keeps advancing
      // even through long runs of orphans that add nothing learnable (otherwise
      // the bar freezes at the last new-endpoint step and looks stalled). The
      // merge is cheap for partials (doNames=false → no source-map fetch).
      var _progressTick = (_deepStats.steps % 8 === 0) || _drem === 0;
      if (methods.size > _lastPartialN || securitySinks.length > _lastPartialSinks || _progressTick) {
        _lastPartialN = methods.size;
        _lastPartialSinks = securitySinks.length;
        try { var _pres = _buildResult(); await _resolveSmNames(_pres.fetchCallSites, scriptOffsets, sourceMapScripts); postMessage({ _partial: true, sourceUrl: sourceUrl || "", response: { success: true, result: _pres } }); } catch (e) {}
      }
      if (_yieldDeep) { _deepStats.stop = "yielded@step" + _deepStats.steps; break; }   // a live review preempts — cursor saved, resume after
      if (instAborted) { _deepStats.stop = "abort@step" + _deepStats.steps; break; }
      if (_drem < 0) { _deepStats.stop = "no-@DS@step" + _deepStats.steps; _drem = 0; }
      if (_drem > 0) await new Promise(function (r) { setTimeout(r, Math.min(THROTTLE_CAP_MS, Date.now() - _dT0)); });
    }
    try { m.callMain(["--fe-deep-end"]); } catch (e) {}
    if (_drem === 0) { try { await _idbDel("prog", _dkey); await _idbDel("code", _dkey); } catch (e) {}   // done → drop resume state
      try { var _tc = Array.from(_smChunksTouched); for (var _tci = 0; _tci < _tc.length; _tci++) await _idbDel("smaps", _tc[_tci]); } catch (e) {}   // grind finished all stages → drop this page's source maps + their JS (kept only during learning)
    }
  }

  // Release the wasm module + its captured arrays after BFS exits.
  m = null;
  stdout.length = 0;
  stderr.length = 0;

  // Execution-grounded focused view: the spans a security researcher
  // actually wants — every endpoint (with the example-value spread
  // that PROVES it sits behind a forced gate) and every tainted sink,
  // each at its real bundle source position. This replaces the dropped
  // Babel viewer index: the engine knows what's interesting because it
  // executed it, not because it indexed identifiers.
  // Hoisted so the deep loop can build a PARTIAL result from the
  // accumulated state (methods/sinks/structural) after each batch and stream
  // it to the UI — the deep grind runs for minutes, so without this the
  // unused-feature endpoints would only appear once the whole run finished.
  function _buildResult() {
  var fetchCallSites = [];
  var focusedView = [];
  methods.forEach(function (mm) {
    var params = [];
    var gated = [];
    mm.params.forEach(function (p) {
      var vv = Array.from(p.examples);
      params.push({ name: p.name, location: p.location, validValues: vv, holeLoc: p.holeLoc || null });
      if (vv.length > 1) gated.push(p.name + "∈{" + vv.join(",") + "}");
    });
    fetchCallSites.push({
      method: mm.method, url: mm.path, params: params,
      headers: mm.headers || {}, loc: mm.loc, callChain: mm.chain,
      bframes: mm.bframes || [], enclosingFunction: null, kind: mm.kind,
    });
    focusedView.push({
      kind: "endpoint", title: mm.method + " " + mm.path,
      location: mm.loc, callChain: mm.chain,
      // >1 distinct value for a param == it was set differently on
      // different forced paths == a real branch/gate decides it.
      gateEvidence: gated,
    });
  });
  securitySinks.forEach(function (s) {
    focusedView.push({
      kind: "sink", title: s.type + " → " + s.sink,
      location: s.location, callChain: s.taintPath.map(function (t) { return t.at; }),
      severity: s.severity, value: s.value,
    });
  });
  // JAW static half: host-edge sites in code no forced path reached
  // (unrequired modules). Network -> structural fetchCallSites
  // (value unresolved, NEVER fabricated; distinct provenance so the
  // learner can rank observed > structural). Sink -> focusedView
  // review item, NOT a securitySink (no taint proven; @S stays
  // taint-only — provenance-split keeps findings sound).
  var NETW = { fetch: 1, XMLHttpRequest: 1, send: 1, sendBeacon: 1, WebSocket: 1, EventSource: 1 };
  structural.forEach(function (s) {
    var loc = { line: s.line || 0, column: s.col || 0 };
    if (NETW[s.api]) {
      // Sound location-only: a host-edge API used in unreached code.
      // url stays null (value unresolved, never a static guess —
      // tested: static arg-attribution mis-leads). Resolution is the
      // dynamic/concolic side's job.
      fetchCallSites.push({
        method: s.api === "fetch" ? "GET" : (s.api === "WebSocket" || s.api === "EventSource" ? s.api.toUpperCase() : "POST"),
        url: null, params: [], headers: {}, loc: loc, callChain: [],
        enclosingFunction: null, kind: s.api,
        structural: true, valueSource: "unreached",
      });
      focusedView.push({ kind: "endpoint", title: "~" + s.api + " (unreached; value unresolved)",
        location: loc, callChain: [], gateEvidence: [], structural: true });
    } else {
      focusedView.push({ kind: "sink", title: "~" + s.api + " (unreached site; review)",
        location: loc, callChain: [], severity: "review", value: null, structural: true });
    }
  });

  return {
    fetchCallSites: fetchCallSites,
    securitySinks: securitySinks,
    dangerousPatterns: [],
    resolverErrors: resolverErrors,
    focusedView: focusedView,
    protoEnums: [],
    protoFieldMaps: [],
    domEndpoints: [],
    // Lazy-chunk URLs the bundle's own code-splitter requested (webpack
    // b.l `script.src=`). background.js fetches each, then re-runs this
    // analyzer on the chunk — the unused-feature API surface (most of a
    // complex app's endpoints) lives there. Engine-grounded discovery.
    chunkUrls: chunkUrls ? Array.from(chunkUrls) : [],
    sourceUrl: sourceUrl || "",
    sourceMapUrl: sourceMapUrlOf(code),
    _timings: { ms: Date.now() - t0, runs: runs },
    _deepStats: _deepStats,
  };
  }
  var _fr = _buildResult();
  try { await _resolveSmNames(_fr.fetchCallSites, scriptOffsets, sourceMapScripts); } catch (e) {}
  return _fr;
}

onmessage = function (e) {
  var id = e.data._id;
  var msg = e.data.msg;

  function done(response) { postMessage({ _id: id, response: response }); }

  if (msg.type === "AST_ANALYZE") {
    // High-priority live page review. Queue it and, if the background deep
    // grind is running, signal it to YIELD at the next batch boundary (its
    // cursor is already persisted to IndexedDB, so it resumes afterward). The
    // review never waits for the deep dive to finish, and the two never run
    // concurrently — so the device never carries two wasm instances / two
    // drives at once (the overheating failure mode).
    _reviewQ.push({ id: id, msg: msg });
    // Preempt ANY in-progress deep grind — the background resume rotation
    // (`_deepRunning`) AND a live review now running to completion (`_busy`).
    // The deep loop checks `_yieldDeep` each batch and breaks (cursor/driven-set
    // already persisted), so the newer page's review starts within one batch
    // instead of waiting out the prior page's full ~100 s grind.
    if (_deepRunning || _busy) _yieldDeep = true;
    _deepRound.length = 0;   // a new page visit changes recency → rebuild the rotation round so it leads next
    _pump();
    return;
  }

  if (msg.type === "AST_ANALYZE_BATCH") {
    // Not used by background.js; analyze each independently if ever called.
    var batch = Array.isArray(msg.scripts) ? msg.scripts : [];
    Promise.all(batch.map(function (s) {
      return forcedAnalyze(String(s.code || ""), s.sourceUrl || "")
        .then(function (r) { return { success: true, result: r }; })
        .catch(function (err) { return { success: false, error: (err && err.message) || String(err) }; });
    })).then(function (results) { done({ success: true, result: results }); });
    return;
  }

  var response;
  if (msg.type === "AST_BUILD_DEFINITION_MAP") {
    response = { success: false, error: _VIEWER_RETIRED };
  } else if (msg.type === "AST_PARSE_SOURCEMAP") {
    try { response = { success: true, result: parseSourceMap(msg.sourceMapJson) }; }
    catch (err) { response = { success: false, error: err.message, stack: err.stack }; }
  } else if (msg.type === "AST_EXTRACT_TYPES") {
    try { response = { success: true, result: extractTypesFromSources(msg.sourcesContent, msg.sources) }; }
    catch (err) { response = { success: false, error: err.message, stack: err.stack }; }
  } else {
    response = { success: false, error: "Unknown message type: " + msg.type };
  }
  done(response);
};

// ── Two-tier preemptible scheduler ───────────────────────────────────────
// The worker is single-threaded, so exactly one task runs at a time. Live
// page reviews (_reviewQ) are HIGH priority; the background deep grind (learn
// the unused/login-gated chunk surface across MV3 worker lifetimes, resumed
// from the IndexedDB cursor) is LOW priority and PREEMPTIBLE. A review
// arriving mid-grind sets _yieldDeep → the grind stops at the next batch
// boundary (cursor saved) → the review runs → the grind resumes from its
// saved cursor. Nothing overlaps, so the device never runs two wasm instances
// / two drives at once. This is the "low-effort findings on a new visit show
// even while a past site's deep dive is in flight, then it resumes" tier.
var _busy = false;          // a live review is running
var _deepRunning = false;   // the background deep grind is running
var _yieldDeep = false;     // preempt signal: deep grind stops at next batch
var _reviewQ = [];          // pending high-priority reviews
var DEEP_ROUND = 20;        // batches a grind advances per rotation cycle before yielding to the next page
var _deepRound = [];        // keys remaining in the current recency-ordered rotation round (cross-page fairness)

function _pump() {
  if (_busy || _deepRunning) return;       // a task owns the worker; it re-pumps when it finishes
  if (_reviewQ.length) { var t = _reviewQ.shift(); _runReview(t.id, t.msg); return; }
  setTimeout(_resumeIncompleteDeep, 300);  // idle → pick up any incomplete deep grind
}

function _runReview(id, msg) {
  _busy = true;
  _yieldDeep = false;   // fresh review owns the worker; any preempt flag set AFTER this point (by a newer visit) is honored, so a stale yield can't kill this grind on its first batch
  function fin(resp) { _busy = false; postMessage({ _id: id, response: resp }); _pump(); }
  // The active page IS the most-relevant work, so its deep grind runs to
  // COMPLETION in this one booted instance (maxBatches=0 → unbounded) rather
  // than a DEEP_ROUND slice. Capping the live review at one rotation cycle meant
  // every later cycle RE-BOOTED the ~18 MB bundle (~3 s each) before driving its
  // next 20 batches — so an endpoint deep in the residue (github's `preheat` sits
  // at orphan ~1008/1108) needed ~17 re-boots and was never reached before the
  // user gave up, even though native drives all 1108 in ONE boot (~65 s). It
  // stays preemptible: `_yieldDeep` breaks the batch loop the instant a newer
  // page's review arrives (cursor/driven-set persisted), and OTHER pages still
  // make progress via the DEEP_ROUND-bounded `_resumeIncompleteDeep` rotation.
  forcedAnalyze(String(msg.code || ""), msg.sourceUrl || "", msg.scriptUrls || null,
    typeof msg.pageHtml === "string" ? msg.pageHtml : null, !!msg.seedOnly, !!msg.deep,
    undefined, undefined, undefined, 0, msg.scriptOffsets || null, msg.sourceMapScripts || null)
    .then(function (result) { fin({ success: true, result: result }); })
    .catch(function (err) { fin({ success: false, error: (err && err.message) || String(err), stack: err && err.stack }); });
}

async function _resumeIncompleteDeep() {
  if (_busy || _deepRunning || _reviewQ.length) return;   // reviews take priority; never overlap
  // ROTATION across pages so no single page gets stuck running to completion
  // while others wait. A "round" is every incomplete grind, ordered by visit
  // recency (`vts`, set on the page visit, preserved across batches — NOT the
  // per-batch `ts`). Each call advances ONE grind by a BOUNDED batch (DEEP_ROUND)
  // then returns; the next call takes the next page in the round; when the round
  // empties it's rebuilt from whatever's still incomplete (re-ordered by recency,
  // picking up newly-visited pages). So most-recent goes first each round, every
  // page makes progress every round, and each completes eventually (its record
  // drops at rem=0). Most-useful-first WITHIN a page is the engine's usefulness
  // sort; this is the cross-page half.
  if (!_deepRound.length) {
    var keys;
    try { keys = await _idbAllKeys("prog"); } catch (e) { return; }
    if (!keys || !keys.length) return;
    var withTs = [];
    for (var ki = 0; ki < keys.length; ki++) {
      try { var pr = await _idbGet("prog", keys[ki]); } catch (e) { continue; }
      if (pr) withTs.push({ k: keys[ki], vts: pr.vts || pr.ts || 0 });
    }
    if (!withTs.length) return;
    withTs.sort(function (a, b) { return b.vts - a.vts; });   // most-recently-visited first
    _deepRound = withTs.map(function (x) { return x.k; });
  }
  var key = _deepRound.shift();
  var prog = null, codeRec = null;
  try { prog = await _idbGet("prog", key); codeRec = await _idbGet("code", key); } catch (e) { _pump(); return; }
  if (!prog || !codeRec || !codeRec.code) { try { await _idbDel("prog", key); await _idbDel("code", key); } catch (e) {} _pump(); return; }
  _deepRunning = true; _yieldDeep = false;
  try {
    // Bounded to DEEP_ROUND batches: advance this page, then yield to the next.
    var result = await forcedAnalyze(codeRec.code, codeRec.sourceUrl || "", codeRec.scriptUrls || null, codeRec.pageHtml || null, true, true, prog.cur || 0, prog.vts || prog.ts || 0, prog.driven || [], DEEP_ROUND, codeRec.scriptOffsets || null, codeRec.sourceMapScripts || null);
    postMessage({ _resumed: true, sourceUrl: codeRec.sourceUrl || "", response: { success: true, result: result } });
  } catch (e) { /* leave the record for a later retry */ }
  finally { _deepRunning = false; }
  _pump();   // → next page in the round (rotation), or a queued review, or refill the round
}
setTimeout(_resumeIncompleteDeep, 8000);
