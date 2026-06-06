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
importScripts("lib/qjs/qjs_worker.js", "lib/qjs/hostedge.gen.js", "lib/sourcemap.js", "lib/priority.js", "lib/safe-fetch.js", "lib/learnstate.js");

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
  if (/^https?:\/\//i.test(smu)) return smu;
  return URL.canParse(smu, chunkUrl) ? new URL(smu, chunkUrl).href : null;
}
// @security-contract  LOADER: source map (chunkUrl's sourceMappingURL)
//   loads:    DATA (parsed for names only)           quickjs-control: NO
//   enforced: safeFetch(as:"sourcemap") -> SSRF (no CORB; not executed code);
//             principal = the grind's sourceUrl (pageOrigin, per-call). Resolved
//             names go ONLY to the findings shown in the UI, never into the engine.
//   RESIDUAL: this fetch runs in the WORKER that hosts QuickJS (outside the WASM,
//             but not the trusted offscreen). Defense-in-depth: move to the
//             offscreen (needs offscreen chunk-map pre-fetch). Primary map fetch
//             already IS in the offscreen (_fetchSourceMapForScript).
async function _smGetParsed(chunkUrl, sourceMapScripts, pageOrigin) {
  if (Object.prototype.hasOwnProperty.call(_smParsed, chunkUrl)) return _smParsed[chunkUrl];
  var parsed = null;
  try { var rec = await _idbGet("smaps", chunkUrl); if (rec && rec.json) parsed = parseSourceMap(rec.json); }
  catch (e) {
    if (!self._whyRecords) self._whyRecords = [];
    self._whyRecords.push({ phase: "smap_idb_get_throw", chunkUrl: chunkUrl, err: String(e && e.message || e) });
  }
  if (!parsed) {
    var url = _smMapUrl(chunkUrl, sourceMapScripts);
    if (url) {
      try {
        // safeFetch = the single external-fetch fn (GET, no cookies, http(s) only).
        // Time-box so a slow/hanging map server can't stall the grind's post loop.
        var _ac = (typeof AbortController !== "undefined") ? new AbortController() : null;
        var _to = _ac ? setTimeout(function () { try { _ac.abort(); } catch (e) {} }, 8000) : 0;
        // Per-call page origin (the grind's sourceUrl) — NOT a worker global, so a
        // concurrent grind for another page can't lend this fetch its origin.
        // as:"sourcemap" — DATA, not executed code (parsed for names only, outside
        // QuickJS, UI-only), so no CORB; still SSRF-gated by the page principal.
        var resp = await safeFetch(url, { pageUrl: pageOrigin || "", as: "sourcemap", signal: _ac ? _ac.signal : undefined });
        if (_to) clearTimeout(_to);
        if (resp && resp.ok && resp.body != null) {
          var json = JSON.parse(resp.body);
          parsed = parseSourceMap(json);
          _smChunksTouched.add(chunkUrl);
          try { await _idbPut("smaps", { key: chunkUrl, json: json }); }
          catch (e) {
            if (!self._whyRecords) self._whyRecords = [];
            self._whyRecords.push({ phase: "smap_idb_put_throw", chunkUrl: chunkUrl, err: String(e && e.message || e) });
          }
        }
      } catch (e) {
        if (!self._whyRecords) self._whyRecords = [];
        self._whyRecords.push({ phase: "smap_fetch_throw", chunkUrl: chunkUrl, url: url, err: String(e && e.message || e) });
      }
    }
  }
  _smParsed[chunkUrl] = parsed || null;
  return _smParsed[chunkUrl];
}
// One bundle frame (combined-line position) → its original template's path
// ${…} names, via the chunk's source map. Returns null if this frame's source
// has no path template (e.g. a shared fetch wrapper).
async function _smNamesForFrame(frame, scriptOffsets, sourceMapScripts, pageOrigin) {
  if (!frame || typeof frame.line !== "number") return null;
  var chunk = _smChunkForLine(frame.line, scriptOffsets);
  if (!chunk || !chunk.url) return null;
  var parsed = await _smGetParsed(chunk.url, sourceMapScripts, pageOrigin);
  if (!parsed) return null;
  var genLine = frame.line - chunk.lineStart + 1;
  var col0 = (frame.column != null ? frame.column : (frame.col || 1)) - 1; if (col0 < 0) col0 = 0;
  var op = _smOrigPos(parsed, genLine, col0);
  if (!op) return null;
  return _smNamesFromContent(parsed.sourcesContent && parsed.sourcesContent[op.srcIdx], op.srcLine0);
}
async function _resolveSmNames(fcs, scriptOffsets, sourceMapScripts, pageOrigin) {
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
      var fn = await _smNamesForFrame(frames[f], scriptOffsets, sourceMapScripts, pageOrigin);
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
  // "[object Object]" is the stringified opaque marker reaching a URL slot. It
  // arrives raw, or percent-encoded by WHATWG `new URL()` to varying degrees:
  // the space as %20 ("[object%20Object]") and/or the brackets as %5B/%5D
  // ("%5Bobject%20Object%5D"). Match the "%5Bobject" prefix too so the fully
  // encoded form is recognized as opaque, not emitted as a garbage endpoint.
  return !t || t.indexOf("[object Object]") >= 0 || t.indexOf("[object%20Object]") >= 0 ||
    t.indexOf("%5Bobject") >= 0;
}

// A URL whose addressable BASE is itself an opaque __feUrlShape hole has no
// concrete origin to issue against — it is a FULLY-opaque URL, which per the
// structural-learning rule is a resolverError, never a fabricated {name}
// endpoint. Two shapes: (1) the whole URL is a hole at position 0
// ({refEndpoint}, {currentTarget}/title — relative with an opaque base), or
// (2) the host/authority inside scheme://… or protocol-relative //… is a hole
// (https://{host}/api). An opaque SEGMENT under a concrete base is NOT this —
// "/repos/{id}" and "/{t}/refs" are root-relative (page origin is the concrete
// base), so the leading "/" anchors them and they remain real path templates.
function isOpaqueBaseUrl(s) {
  if (!s) return false;
  var t = String(s).trim();
  if (t.charAt(0) === "{") return true;
  // A URL-template's host expressions that evaluated to JS undefined/null
  // coerce the AUTHORITY to the literal string "undefined"/"null" (or a
  // concatenation, e.g. `https://${h}${p}/` with h,p undefined →
  // "https://undefinedundefined/"). That authority can never address a real
  // host — the host-building values weren't computed — so it is an UNRESOLVED
  // base (a resolverError, never the fabricated "POST /" endpoint it would
  // otherwise display as). Same class of JS-coercion artifact as the
  // "[object Object]" opaque marker. Tight, not fuzzy: a real authority always
  // has a label separator (a dot, or is a known host); an authority made up
  // ONLY of "undefined"/"null" never does. Whole-URL "undefined"/"null" (a
  // bare `fetch(undefinedVar)`) is the same artifact. Observed live on
  // apple.com (POST / + SENDBEACON / were both https://undefinedundefined/).
  if (t === "undefined" || t === "null") return true;
  var m = /^[a-zA-Z][a-zA-Z0-9+.-]*:\/\/([^\/?#]*)/.exec(t);
  if (m && m[1].indexOf("{") >= 0) return true;
  if (m && /^(undefined|null)+$/.test(m[1])) return true;
  var m2 = /^\/\/([^\/?#]*)/.exec(t);
  if (m2 && m2[1].indexOf("{") >= 0) return true;
  if (m2 && /^(undefined|null)+$/.test(m2[1])) return true;
  return false;
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
    // Real <script src> tags + per-script document.currentScript: bundles
    // (MDN airgap.js / Catalyst loaders / webpack publicPath probes)
    // dereference `document.currentScript.src` to learn their own URL.
    // Build a basename→element map; document.currentScript is a live
    // getter that consults globalThis.__feCurFile (set per-script by
    // qjs_eval_script in qjsmain.c) so EACH script sees ITSELF as
    // currentScript during its own eval — matching browser semantics.
    "var SR_EL={};var LS=null;for(var s=0;s<SR.length;s++){\n" +
    "  var sc=document.createElement('script');sc.src=SR[s];sc.setAttribute('src',SR[s]);\n" +
    "  (document.head||document.documentElement||document.body).appendChild(sc);LS=sc;\n" +
    "  try{var bn=String(SR[s]).split('?')[0].split('/').pop();SR_EL['/'+bn]=sc;}catch(e){}\n" +
    "}\n" +
    "globalThis.__feScriptElMap=SR_EL;globalThis.__feLastScript=LS;\n" +
    "}catch(e){}})();";
}

async function forcedAnalyze(code, sourceUrl, scriptUrls, pageHtml, seedOnly, deep, resumeCursor, visitTs, drivenIds, scriptOffsets, sourceMapScripts, scriptSources) {
  var t0 = Date.now();
  // PROPER taint TRACE from the engine's psi term (the data-flow QuickJS
  // computed), NOT the @S call-STACK. The popup's _extract*FromTaintPath helpers
  // expect data-flow hops {kind:"source"|"member"|"call-arg", desc} — that IS
  // the psi term: `$id:label` (source leaf), `(. base "name")` (member),
  // `(fn base arg)` (call). _psiToHops parses the Lisp-y psi (serializer in
  // quickjs.c qjs_sb_term0) and emits hops SOURCE-FIRST along the tainted spine,
  // so the popup shows `responseText → JSON.parse → .webRes → sink` — verifiable
  // from the trace alone (essential for sources like XHR responseText that have
  // no automated probe strategy). Robust to junk: any parse failure → [].
  function _psiToHops(psi) {
    if (typeof psi !== "string" || !psi) return [];
    var i = 0, n = psi.length;
    function ws() { while (i < n && psi[i] === " ") i++; }
    function str() { ws(); if (psi[i] !== '"') return ""; i++; var s = ""; while (i < n && psi[i] !== '"') { if (psi[i] === "\\") i++; s += psi[i++]; } if (psi[i] === '"') i++; return s; }
    function skip() { // skip one balanced term without recording (for non-spine args)
      ws(); if (i >= n) return;
      if (psi[i] === "(") { var d = 0; do { if (psi[i] === "(") d++; else if (psi[i] === ")") d--; i++; } while (i < n && d > 0); }
      else if (psi[i] === '"') { str(); }
      else { while (i < n && psi[i] !== " " && psi[i] !== ")") i++; }
    }
    function parse() { // returns source-first hops for the term at i
      ws(); if (i >= n) return [];
      var c = psi[i];
      if (c === "(") {
        i++; ws(); var head = ""; while (i < n && psi[i] !== " " && psi[i] !== ")") head += psi[i++];
        if (head === ".") { var bh = parse(); var name = str(); ws(); if (psi[i] === ")") i++; bh.push({ kind: "member", desc: "." + name }); return bh; }
        // (fn base arg): follow whichever child carries the source leaf
        var save = i; var bh2 = parse(); ws();
        var argStart = i; var ah = parse(); ws(); if (psi[i] === ")") i++;
        var spine = bh2.length ? bh2 : ah;
        spine.push({ kind: "call-arg", desc: head + "()" });
        return spine;
      }
      if (c === "$") { i++; var id = ""; while (i < n && psi[i] !== ":") id += psi[i++]; if (psi[i] === ":") i++; var lab = ""; while (i < n && /[\w.$]/.test(psi[i])) lab += psi[i++]; return [{ kind: "source", desc: lab }]; }
      skip(); return []; // "lit" / num / ? → no source hop
    }
    try { return parse(); } catch (e) { return []; }
  }
  // Pretty-print one Lisp-y term (psi/phi sub-term; serializer qjs_sb_term0 in
  // quickjs.c) into a readable JS-ish expression — source-grounded, no guessing:
  // `$id:label`→label, `(. b "n")`→b.n, `(fn b a)`→fn(b, a), `(op a b)`→a OP b
  // (op chars exactly qjs_cmp_opchar's set), `(! b)`→!b.
  function _termToStr(s, st) {
    function ws() { while (st.i < s.length && s[st.i] === " ") st.i++; }
    function rdStr() { st.i++; var o = ""; while (st.i < s.length && s[st.i] !== '"') { if (s[st.i] === "\\") st.i++; o += s[st.i++]; } if (s[st.i] === '"') st.i++; return o; }
    function opStr(c) { return ({ "=": "===", "!": "!==", "<": "<", "l": "<=", ">": ">", "g": ">=" })[c] || c; }
    ws();
    if (st.i >= s.length) return "?";
    var c = s[st.i];
    if (c === "(") {
      st.i++; ws();
      var head = ""; while (st.i < s.length && s[st.i] !== " " && s[st.i] !== ")") head += s[st.i++];
      if (head === ".") { var base = _termToStr(s, st); ws(); var name = s[st.i] === '"' ? rdStr() : ""; ws(); if (s[st.i] === ")") st.i++; return base + "." + name; }
      var a = _termToStr(s, st); ws();
      var b = null;
      if (s[st.i] !== ")") b = _termToStr(s, st);
      ws(); if (s[st.i] === ")") st.i++;
      if (/^[A-Za-z_$]/.test(head)) return head + "(" + a + (b != null && b !== "?" ? ", " + b : "") + ")";
      if (b == null) return (head === "!" ? "!" : head) + a;  // unary logical NOT (op 'U')
      return a + " " + opStr(head) + " " + b;            // infix binary
    }
    if (c === '"') { var k = rdStr(); return JSON.stringify(k); }
    if (c === "$") { st.i++; while (st.i < s.length && s[st.i] !== ":") st.i++; if (s[st.i] === ":") st.i++; var lab = ""; while (st.i < s.length && /[\w.$]/.test(s[st.i])) lab += s[st.i++]; return lab; }
    var tok = ""; while (st.i < s.length && s[st.i] !== " " && s[st.i] !== ")") tok += s[st.i++];
    return tok;
  }
  function _termPretty(s) { if (typeof s !== "string" || !s) return ""; try { return _termToStr(s, { i: 0 }); } catch (e) { return ""; } }
  // Φ (path constraints) → the CONDITIONALS on the source→sink path. The engine
  // serializes Φ as `[d:term,d:term,...]` (qjs_sb_pc) where d∈{0,1} is the
  // branch the forced run took. Each is a condition a reviewer reads to judge
  // whether a gate PINS the value (likely-safe / FP) vs leaves it free. The
  // term may reference the tainted source (taintRel) or be source-independent
  // (an unrelated control-flow gate); both are surfaced. Robust to junk → [].
  function _phiToConditions(phi) {
    if (typeof phi !== "string") return [];
    var s = phi.trim();
    if (s[0] !== "[") return [];
    var out = [], i = 1, n = s.length;
    while (i < n && s[i] !== "]") {
      while (i < n && (s[i] === " " || s[i] === ",")) i++;
      if (i >= n || s[i] === "]") break;
      var dec = ""; while (i < n && s[i] !== ":" && s[i] !== "]" && s[i] !== ",") dec += s[i++];
      if (s[i] !== ":") { continue; }
      i++;
      var st = { i: i }, expr = "";
      try { expr = _termToStr(s, st); } catch (e) { expr = ""; }
      i = st.i;
      if (expr) out.push({ decision: (dec.trim() === "1") ? 1 : 0, expr: expr });
    }
    return out;
  }
  // Classify the Z3 verdict so a reviewer can DISTINGUISH the cases the bare
  // verdict conflates: a candidate FALSE-POSITIVE (gates pin the value → not
  // exploitable) from a PoC-GENERATION FAILURE (Z3 errored, or no exploit shape
  // for the sink family → exploitability UNKNOWN). REAL_EXPLOIT+witness = a
  // generated PoC, untested until the dynamic probe fires it.
  function _classifyVerdict(verdict, conditions, witness) {
    var nc = conditions ? conditions.length : 0;
    if (verdict === "REAL_EXPLOIT") return { reason: "poc", text: witness ? "Z3 generated an exploit witness (PoC) under " + nc + " path condition" + (nc === 1 ? "" : "s") + " — UNTESTED until Verify fires it" : "Z3: exploit shape satisfiable, no concrete witness captured" };
    if (verdict === "Z3_ERROR") return { reason: "gen-failed", text: "PoC generation FAILED — Z3 solver error; exploitability UNKNOWN (not a clean verdict)" };
    if (verdict === "EXPLOIT_UNPROVEN") return { reason: "unproven", text: "Taint reaches an exploit-shaped sink, but the exploit is satisfiable only under an UNSOUND over-approximation (an unmodeled transform or a replace() that could sanitize) — NOT a proven PoC, so no PoC is offered. Review the trace manually." };
    if (verdict === "TAINT_REACH") {
      if (nc) return { reason: "pinned", text: "Taint reaches the sink but " + nc + " path condition" + (nc === 1 ? "" : "s") + " pin the value — no exploit shape satisfies them (candidate FALSE POSITIVE / sanitized)" };
      return { reason: "no-shape", text: "Taint reaches the sink but no exploit shape is defined for this sink family — PoC generation not attempted (exploitability UNKNOWN)" };
    }
    return { reason: "unknown", text: String(verdict || "") };
  }
  // Phase-timing instrumentation — to MEASURE (not assert) where real-bundle
  // time goes: boot (the one snapshot boot), memcpy (cumulative restore cost,
  // scales with image size), the BFS phase, and the deep grind. Surfaced via
  // _deepStats so the popup can read the breakdown on a real page.
  var _tBootMs = 0, _tMemcpyMs = 0, _tBcMs = 0;
  // No JS-side rotation bound: the deep grind runs ONE callMain that drives
  // every remaining orphan, JSPI-yielding per orphan via qjs_host_yield, so
  // the host scheduler can rotate to a higher-priority fiber (live review,
  // another page's grind) at every orphan boundary via priority.js's
  // flowCmp. A page no longer "completes in this booted instance" exclusive
  // of other pages; instead, the suspended-fiber queue interleaves them by
  // priority at orphan granularity.
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
  // Throttle: schedule-loop yields rest ≈ work (~50% duty) capped here so a
  // slow run doesn't stall the queue too long. The deep grind has no JS-side
  // batch loop — its cool-CPU duty lives in the JSPI scheduler's macrotask
  // sleep (priority.js pickFromFiberQueue + _yieldMC), invoked by the
  // engine's per-orphan qjs_host_yield (compiled in for the JSPI worker
  // build). Drain + persist cadence is the per-orphan JSPI yield itself —
  // every yield runs _drainDeepStdout below before the suspending Promise
  // resolves, so there is NO interval-based heartbeat (which would be a
  // magic time-cap masquerading as observability).
  // Schedule-loop throttle cap (ms). The popup's "Yield throttle" slider
  // writes self._throttleCapMs via SET_ANALYSIS_OPTS — 0 means no sleep
  // (max throughput), higher means longer rest = cooler.
  // DEFAULT = 0 (no throttle). This is a security-RESEARCH tool: the user
  // actively opened it and is WAITING on the API surface — leaving the
  // analysis core idle at 50% duty to be "polite" optimised for the wrong
  // thing. A single page's grind is pinned to ONE worker (sticky routing) =
  // one core, so full-tilt uses that one core and leaves the rest for the
  // system. Cooling is now an OPT-IN (raise the slider), not a forced
  // default; even then the active page stays hot via the _isActive exemption
  // below. (Was 4000 = ~50% duty — made every page, incl. the one on screen,
  // crawl.)
  var THROTTLE_CAP_MS = (typeof self._throttleCapMs === "number" && self._throttleCapMs >= 0) ? self._throttleCapMs : 0;
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
  // Split the analyzed bundle along the per-page-script boundaries the
  // brain captured (`scriptOffsets[i].lineStart`, 1-based). Each script
  // is its own MEMFS file (/b/0.js, /b/1.js, …) so qjsmain.c can run
  // JS_DetectModule on the per-script slice and eval each with the
  // right mode (GLOBAL for classic, MODULE for ESM). A real page mixes
  // both kinds: classic inline scripts that use sloppy-mode features
  // alongside an rspack/webpack ESM bundle with top-level `export`;
  // their concatenation parses as neither and the old single-/b.js
  // path threw "unsupported keyword: export" the moment the ESM chunk
  // landed, learning ZERO endpoints on MDN/GitHub. JS_Eval2 carries
  // `line_num = scriptOffsets[i].lineStart` so stack frames inside a
  // slice still report combined-bundle line numbers (downstream source
  // map resolution by `_findScriptForLine` keeps working unchanged).
  // /b.0.js etc. all keep the `/b.` prefix so the stack-frame filter
  // below still picks them out from /h.js / /d.js frames.
  // Name each MEMFS slice by its real script-URL basename (no /b. prefix)
  // so the libc ESM module loader resolves `import "runtime.X.js"` —
  // rspack/webpack runtime-chunk static imports — to THIS slice's file
  // (`/runtime.X.js`), making them the SAME module record the
  // per-file argv eval handles. Without this, each cross-chunk import
  // creates a NEW module record, the webpack runtime initializes
  // twice, and __webpack_require__ identity diverges so the runtime's
  // chunk-load wiring breaks. Browser semantics: one <script> = one
  // module record. Slices with no basename (inline) keep the numbered
  // form. Our infrastructure files /h.js, /d.js, /pre.js, /p.js are
  // the only non-bundle MEMFS paths; the stack-frame filter excludes
  // those explicitly.
  var bundleFiles = [];
  var _slicePathToUrl = {};   // slice MEMFS path → its real script URL, for per-importer ESM relative-import resolution
  var INFRA_PATHS = { "/h.js": 1, "/d.js": 1, "/pre.js": 1, "/p.js": 1 };
  if (scriptOffsets && scriptOffsets.length) {
    var lines = String(code).split("\n");
    var pathSeen = Object.create(null);
    for (var si = 0; si < scriptOffsets.length; si++) {
      var startLn = (scriptOffsets[si].lineStart | 0);
      var endLn = (si + 1 < scriptOffsets.length)
        ? (scriptOffsets[si + 1].lineStart | 0) - 1
        : lines.length;
      var sliceLines = lines.slice(startLn - 1, endLn);
      var scriptUrl = scriptOffsets[si].url || "";
      var path;
      if (scriptUrl && /^https?:\/\//i.test(scriptUrl)) {
        // Encode the URL → "/x/<host><path><?query>": a collision-free module identity
        // the engine's qjs_module_normalize matches. The old basename scheme only worked
        // for distinct .js basenames (firebase/gstatic); esm.sh ships many .mjs/no-ext
        // modules with colliding basenames + deep paths, so identity must be host+path.
        // The engine's abs-path branch concatenates "/x/<host>" with the RAW importer
        // specifier — INCLUDING ?query + a literal caret (esm.sh imports redirect stubs
        // as "/@supabase/phoenix@^0.4.2?target=es2022"). new URL() percent-encodes ^→%5E
        // and drops the query, so the slice wrote ".../phoenix@%5E0.4.2" while the engine
        // looked up ".../phoenix@^0.4.2?target=es2022" → the stub never linked → the whole
        // supabase-js import chain threw → createClient never ran → 0 endpoints. Decode the
        // pathname + keep the query so the slice identity matches the engine lookup.
        try {
          var _u = new URL(scriptUrl);
          var _pn = _u.pathname; try { _pn = decodeURIComponent(_pn); } catch (e3) {}
          path = "/x/" + _u.host + _pn + _u.search;
        }
        catch (e2) { path = "/b." + si + ".js"; }
        if (pathSeen[path]) path = "/b." + si + ".js";
      } else {
        var basename = scriptUrl ? scriptUrl.split("?")[0].split("/").pop() : "";
        if (basename && basename.endsWith(".js") && !pathSeen["/" + basename] && !INFRA_PATHS["/" + basename]) {
          path = "/" + basename;
        } else {
          path = "/b." + si + ".js";
        }
      }
      pathSeen[path] = 1;
      bundleFiles.push({ path: path, src: sliceLines.join("\n"), startLine: startLn });
      if (scriptUrl) _slicePathToUrl[path] = scriptUrl;
    }
  } else {
    bundleFiles.push({ path: "/b.0.js", src: String(code), startLine: 1 });
  }
  var inMem = [["/h.js", HOSTEDGE]];
  var fileArgs = ["/h.js"];
  for (var bfi = 0; bfi < bundleFiles.length; bfi++) {
    inMem.push([bundleFiles[bfi].path, bundleFiles[bfi].src]);
    fileArgs.push(bundleFiles[bfi].path);
  }
  inMem.push(["/d.js", HOSTDRIVER]);
  fileArgs.push("/d.js");
  /* @WHY observability: every forcedAnalyze invocation logs the bundleFiles/
     fileArgs counts so a downstream "main eval processed only 10 of N args"
     symptom is distinguishable from "the worker was only given 10 to start
     with". Surfaces via GET_LAST_STDERR alongside the C-side per-eval @WHY. */
  if (!self._whyRecords) self._whyRecords = [];
  self._whyRecords.push({
    phase: "forced_analyze_entry",
    seedOnly: !!seedOnly, deep: !!deep,
    sourceUrl: sourceUrl || "",
    scriptOffsets: scriptOffsets ? scriptOffsets.length : 0,
    bundleFiles: bundleFiles.length,
    fileArgs: fileArgs.length,
    codeKB: Math.round((code ? code.length : 0) / 1024),
  });
  // Pass start-line offsets to qjsmain so JS_Eval2 reports stack-frame
  // line numbers in combined-bundle space (matches scriptOffsets[].lineStart
  // exactly, so the brain's _findScriptForLine still resolves correctly).
  // Per-path (`/b.N.js=line` or `/b.basename.js=line`) because the slice
  // naming switches between numbered and basename to satisfy ESM imports.
  var startLineArg = "--fe-script-start-lines=" + bundleFiles.map(function (b) { return b.path + "=" + b.startLine; }).join(",");
  fileArgs.unshift(startLineArg);
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
  // url->source map of the page's OWN external <script src>, safeFetched by the
  // offscreen and handed back so the engine RUNS them in document order (the
  // one-message-per-document model — hostedge's SSR phase reads G.__feScriptSources).
  if (scriptSources && typeof scriptSources === "object" && Object.keys(scriptSources).length)
    preParts.push("globalThis.__feScriptSources = " + JSON.stringify(scriptSources) + ";");
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
  // Work items: {sched, key?, deep?, frontierSig?, productivity?, ts?}. seen
  // dedups by the decision string (sched).
  var work = _resume ? [] : [{ sched: "" }], seen = new Set([""]);
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
  var secByKey = new Map();         // sk → finding, so distinct forced paths to
                                    // the SAME sink aggregate as multiple
                                    // interprocedural paths (not first-wins drop)
  var chunkUrls = new Set();         // lazy-chunk URLs the bundle's loader requested (script.src=)
  var scriptSrcUrls = [];            // the page's OWN external <script src> URLs (@SCRIPTSRC), engine-discovered from the Lexbor DOM — [{url, module}]
  var esmImportUrls = new Set();     // ESM `import` URLs (@MODURL) — a BOUNDED dependency tree,
                                     // so the offscreen follows these MULTI-ROUND (transitive) to
                                     // fixpoint, unlike the webpack script.src graph (one-round).

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
    // PREFER the engine-emitted (sinkLeaf, sinkField): the engine identified the
    // sink-bearing field from Ψ's spine exactly. Scraping quoted segments from
    // the psi STRING breaks for a concat Ψ ("<div class='" + e.data.cls + "'>…")
    // — the literal wrapper segments get mixed into the field path, so the proof
    // hook is woven into a nonexistent field and the payload keeps alert(...).
    if (rawPoc && typeof rawPoc.sinkLeaf === "number" && rawPoc.sinkLeaf >= 0) {
      sinkLeafId = rawPoc.sinkLeaf;
      sinkField = (typeof rawPoc.sinkField === "string" && rawPoc.sinkField) ? rawPoc.sinkField : null;
    } else if (typeof psi === "string") {
      // Fallback (older engine / no spine): single-leaf reflections only.
      var leafMatch = psi.match(/\$(\d+):/);
      if (leafMatch) sinkLeafId = +leafMatch[1];
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
  // A frame is a "bundle frame" when its file matches a path the worker
  // wrote into MEMFS for one of the page's scripts — anything else is
  // infra (/h.js host-edge, /d.js driver, /pre.js page-globals, /p.js
  // page-DOM seed) or native. The slice paths are basenames (`/foo.js`)
  // for scripts that carry one (so ESM imports resolve), or
  // /b.N.js fallbacks for inline scripts.
  var bundleFileSet = Object.create(null);
  for (var bfsi = 0; bfsi < bundleFiles.length; bfsi++) bundleFileSet[bundleFiles[bfsi].path] = 1;
  function isBundleFrame(fname) { return !!bundleFileSet[String(fname)]; }
  function pickSite(at) {
    if (!at || !at.length) return { loc: null, chain: [], bframes: [] };
    var pick = at[0];
    for (var i = 0; i < at.length; i++) { if (isBundleFrame(at[i].file)) { pick = at[i]; break; } }
    var chain = [];
    var bframes = [];
    for (var j = 0; j < at.length; j++) {
      chain.push({ line: at[j].line, column: at[j].col, name: at[j].name, file: at[j].file });
      if (isBundleFrame(at[j].file)) bframes.push({ line: at[j].line, column: at[j].col, name: at[j].name });
    }
    return { loc: { line: pick.line, column: pick.col }, chain: chain, bframes: bframes };
  }

  /* Magic-byte protocol sniffer (never URL-suffix). hex is the body's
     full byte sequence as ASCII hex (from hostedge bodyShape). Returns
     a string tag — the caller stores it on the endpoint so the popup
     and the brain's decoders know which wire format to apply.
     Recognised signatures (every byte from the spec/wire format, not
     a name guess):
       - gRPC-Web:  first byte is 0x00 (uncompressed) or 0x01
         (compressed) and the NEXT 4 bytes are a big-endian length that
         exactly matches the remaining bytes. Spec: gRPC over HTTP §6.
       - Protobuf:  first varint byte's low 3 bits are a valid wire
         type (0 varint / 1 i64 / 2 LEN / 5 i32). Wire types 3, 4 are
         reserved → reject. High bit can be 0 or 1 (continuation).
       - gzip:      magic 1f 8b.
       - zlib:      78 da | 78 9c | 78 01.
       - JSON:      starts with whitespace or { / [ / " / digit / t / f / n.
     Multiple candidates can match (Protobuf bytes might also be JSON
     when starting with 0x7b/0x5b); precedence: gRPC-Web > gzip/zlib >
     JSON > Protobuf > bytes. */
  function _classifyBinary(hex) {
    if (typeof hex !== "string" || hex.length < 2) return "bytes";
    var b0 = parseInt(hex.slice(0, 2), 16);
    var byteLen = hex.length >> 1;
    // gRPC-Web frame: 5-byte header (1 flag + 4 BE length)
    if (byteLen >= 5 && (b0 === 0x00 || b0 === 0x01)) {
      var declared = parseInt(hex.slice(2, 10), 16);
      if (declared === byteLen - 5) return "grpc-web";
    }
    // gzip magic
    if (b0 === 0x1f && byteLen >= 2 && parseInt(hex.slice(2, 4), 16) === 0x8b) return "gzip";
    // zlib magic
    if (b0 === 0x78 && byteLen >= 2) {
      var b1 = parseInt(hex.slice(2, 4), 16);
      if (b1 === 0xda || b1 === 0x9c || b1 === 0x01) return "zlib";
    }
    // JSON / NDJSON — leading byte is '{', '[', '"', digit, t/f/n, or whitespace
    if (b0 === 0x7b || b0 === 0x5b || b0 === 0x22 ||
        (b0 >= 0x30 && b0 <= 0x39) || b0 === 0x74 || b0 === 0x66 || b0 === 0x6e ||
        b0 === 0x20 || b0 === 0x09 || b0 === 0x0a || b0 === 0x0d) return "json";
    // Protobuf — varint tag, low 3 bits ∈ {0,1,2,5}
    var wireType = b0 & 0x07;
    if (wireType === 0 || wireType === 1 || wireType === 2 || wireType === 5) {
      var fieldNum = (b0 & 0x78) >> 3;   // single-byte tag; multi-byte varints have high bit set
      if (fieldNum > 0) return "protobuf";
    }
    return "bytes";
  }

  function ep(method, rawUrl, body, kind, at, shape, hdrs, holes) {
    // __feUrlShape renders an opaque URL segment as the template marker
    // {name}. When the bundle builds the URL through WHATWG `new URL()`
    // (Lexbor), the spec percent-encodes the braces ({->%7B, }->%7D) before
    // urlOf records the resolved .href — so /{t}/refs arrives here as
    // /%7Bt%7D/refs and the {name} path-param regex below misses it, leaving
    // the segment unlearned and the path mangled. Un-mangle our OWN marker's
    // encoded form so the segment is learned as an opaque path param and the
    // stored path keeps the clean OpenAPI template. A real API path never
    // carries a literal percent-encoded brace pair, so this only ever touches
    // the synthetic hole marker, never a concrete URL component.
    if (typeof rawUrl === "string" && rawUrl.indexOf("%7") >= 0)
      rawUrl = rawUrl.replace(/%7[Bb]/g, "{").replace(/%7[Dd]/g, "}");
    var opaqueBase = isOpaqueBaseUrl(rawUrl);
    if (isUnresolved(rawUrl) || isUnresolved(method) || opaqueBase) {
      var rk = (method || "?") + " " + (rawUrl || "");
      if (!reSeen.has(rk)) {
        reSeen.add(rk);
        // Attach the call-site location + chain (from the @H record's
        // Error().stack) so a reached-but-opaque endpoint is SELF-DIAGNOSING —
        // CLAUDE.md makes this a P1 the reviewer must SEE and act on, but the
        // record carried only a generic page-URL context, leaving the reviewer
        // unable to find WHICH of a 2.3 MB bundle's fetch sites went opaque
        // (9 such on learn.microsoft.com, all locationless). `loc` is in
        // combined-bundle space (background.js maps it to file:line:col via
        // scriptOffsets); `chain` is the full call stack to the host edge.
        var _reSite = pickSite(at);
        var _reLoc = _reSite.loc;
        // Cold-orphan drives invoke the function synthetically (no real caller
        // chain), so pickSite finds no bundle frame → null loc. Fall back to the
        // orphan's OWN bytecode position — self._currentOrphan, set from the
        // @DSTART the deep grind flushes before each drive — so even a cold-driven
        // opaque URL names its source function. Only set during the deep grind
        // (BFS emits no @DSTART, so _currentOrphan is null there → no wrong loc).
        if (!_reLoc && self._currentOrphan && typeof self._currentOrphan.line === "number")
          _reLoc = { line: self._currentOrphan.line, column: self._currentOrphan.col, file: self._currentOrphan.file };
        // Suppress a REDUNDANT opaque resolverError when the SAME call site was
        // ALSO learned CONCRETELY — a config client driven both cold (opaque
        // this → opaque URL) and with its real receiver (concrete URL). The
        // concrete record wins; the opaque is cold-drive residue, not a gap.
        // Genuine opaque-only sinks (no concrete sibling at this loc) still
        // surface. (Handles concrete-first; the concrete branch handles the
        // opaque-first order by retiring the resolverError when it learns one.)
        if (_reLoc) {
          var _dupConcrete = false;
          methods.forEach(function (m) { if (m.loc && m.loc.line === _reLoc.line && m.loc.column === _reLoc.column) _dupConcrete = true; });
          if (_dupConcrete) return;
        }
        resolverErrors.push({
          context: kind + " call site (" + (sourceUrl || "bundle") + ")",
          message: opaqueBase
            ? "request target has an opaque base/origin (the addressable anchor is an attacker/server-input hole, not a concrete host or root-relative path) — fully-opaque URL, recorded as a driver gap not a fabricated endpoint: " + JSON.stringify({ method: method, url: rawUrl })
            : "request target did not resolve to a concrete string at the converged fixpoint (opaque component reached the " + kind + " URL/method): " + JSON.stringify({ method: method, url: rawUrl }),
          loc: _reLoc,
          chain: _reSite.chain,
          // The URL TEMPLATE (e.g. "{apiHost}/api/widgets") when partially
          // concrete — opaque-base flagged it, but the literal path is here, so
          // the reply-example seed can substitute the field value and keep the
          // path instead of dropping it.
          rawUrl: typeof rawUrl === "string" ? rawUrl : undefined,
          // The opaque URL's symbolic fields + their SOURCE-LEAF provenance
          // (from __feUrlHoles). A reply->request chain (oidc
          // metadata.token_endpoint) shows src "fetch.body.json" — the
          // server-response the URL came from; `fromReply` is the honest signal
          // that a fetch flows into THIS fetch (what a bounded reply-GET would
          // seed). location.search/cookie/etc. show their own src. Empty = bare.
          opaqueFields: (holes && holes.length) ? holes.map(function (h) { return h && h.name; }).filter(Boolean) : undefined,
          opaqueSources: (holes && holes.length) ? holes.map(function (h) { return h && h.src; }).filter(Boolean) : undefined,
          fromReply: (holes && holes.some(function (h) { return h && h.src && (h.src.indexOf("fetch.") === 0 || h.src.indexOf("XHR.") === 0); })) || undefined,
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
    if (!m.loc && at) {
      var s = pickSite(at); m.loc = s.loc; m.chain = s.chain; m.bframes = s.bframes;
      // A concrete endpoint at this loc RETIRES any earlier opaque resolverError
      // for the same call site (the cold-drive sibling) — keep the signal, drop
      // the redundant noise. Genuine opaque-only sinks have no concrete loc here.
      if (m.loc) for (var _ri = resolverErrors.length - 1; _ri >= 0; _ri--) {
        var _rl = resolverErrors[_ri].loc;
        if (_rl && _rl.line === m.loc.line && _rl.column === m.loc.column) resolverErrors.splice(_ri, 1);
      }
    }
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
    // The concrete example the engine recovered for this hole (a real
    // location.search / URLSearchParams.get value that rode a concat into the
    // URL) — surfaces /api/user/{userId} WITH example 42, not a bare template.
    var holeExampleOf = function (nm) {
      if (!holes || !holes.length) return undefined;
      for (var i = 0; i < holes.length; i++) if (holes[i] && holes[i].name === nm && holes[i].example !== undefined) return holes[i].example;
      return undefined;
    };
    // Path-template params: __feUrlShape renders an opaque path segment as
    // {name} (e.g. /settings/avatars/{id} — id is real attacker/server input).
    // Record each as a path param so the structure is learned; the path keeps
    // the {name} template (OpenAPI style). A concrete example is attached only
    // when the engine recovered one (a real location.search / query value that
    // rode a concat) — never a fabricated value.
    var ppRe = /\{([^}\/]+)\}/g, ppm;
    while ((ppm = ppRe.exec(path))) add(ppm[1], "path", holeExampleOf(ppm[1]), holeLocOf(ppm[1]));
    var qp = parsePairs(qs);
    for (var k in qp) for (var vi = 0; vi < qp[k].length; vi++) {
      // A "{tab}" query value is a __feUrlShape hole marker — resolve it to the
      // engine's recovered example for that hole (fetch("?section="+location
      // .search value)) so the query param carries the concrete example, not
      // the synthetic marker; holeLoc follows the hole name too.
      var qv = qp[k][vi], qm = /^\{([^}]+)\}$/.exec(qv);
      add(k, "query", qm ? holeExampleOf(qm[1]) : qv, holeLocOf(qm ? qm[1] : k));
    }
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
          else if (fv.kind === "binary" && typeof fv.hex === "string") {
            /* Binary value INSIDE an object field (e.g. {file: <Uint8Array>}).
               The bytes are the example value — record as a 0x-prefixed hex
               literal so the popup can render and the user can edit it.
               Wire protocol of this sub-field would also be classifyable,
               but we keep this concise — full classification is for
               top-level binary bodies (handled above). */
            add(pn, "body", "0x" + fv.hex);
          }
          else if (fv.kind === "array") {
            for (var ii = 0; ii < fv.items.length; ii++) {
              var it = fv.items[ii];
              if (it && it.kind === "literal") add(pn, "body", it.value);
              else if (it && it.kind === "binary" && typeof it.hex === "string") add(pn, "body", "0x" + it.hex);
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
    } else if (shape && shape.kind === "binary" && typeof shape.hex === "string") {
      /* Binary body (Protobuf/gRPC-Web/file upload/etc.) — hostedge
         captured every byte as hex. Classify the wire format from the
         leading bytes so the popup + protocol decoder know what to do
         with it. Per CLAUDE.md #7 ("magic-byte sniffing + content-type,
         never URL-suffix heuristics"): identifier is the bytes, never
         the URL. Field decoding (Protobuf tag/wire-type walk) happens
         in the brain via lib/protobuf.js, which already has the decoder
         — we just need to surface the bytes + the protocol guess so
         downstream can pick the decoder. */
      m.bodyBinary = { byteLength: shape.byteLength | 0, hex: shape.hex, protocol: _classifyBinary(shape.hex) };
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
  /* huntContext = {pageKey, type, vts} captured by this instance's qjs_host_yield
     closure. The yield Promise stores this context in self._fiberQ so the
     scheduler can pick the highest-priority entry across ALL suspended fibers
     of ALL pages — not in arrival order. */
  var huntContext = {
    pageKey: (sourceUrl || "").split("#")[0] || "default",
    type: deep ? "deep" : "review",      // live review preempts deep grind by default
    vts: (typeof visitTs === "number" && visitTs > 0) ? visitTs : Date.now(),
    lastReaches: 1,                       // engine sets via @Y reaches=<bit> before each yield; 1 = conservative default
    /* Per-goal emission counters (per CLAUDE.md goal priority: endpoints
       are #1, security is #10). flowCmp uses `recentEndpoints` as the
       load-bearing productivity tier so a fiber that just emitted real
       network endpoints (@H) outranks one emitting structural candidates
       (@T) or security sinks (@S) of equal count. recentSecondary is
       the tiebreaker so a fiber emitting @S still beats one emitting
       nothing. */
    totalEndpoints: 0,                    // @H only — fetch / XHR.open / WebSocket / etc.
    totalSecondary: 0,                    // @T (structural candidates) + @S (security sinks)
    endpointsAtLastYield: 0,
    secondaryAtLastYield: 0,
    /* Legacy aggregate kept so existing read sites (BFS fruitfulness
       check at _lastEmittedHS) keep working. = endpoints + secondary,
       same as the previous "all emissions" counter. */
    totalEmissions: 0,
    emissionsAtLastYield: 0,
    lastResumed: Date.now(),
    /* WEIGHTED FAIR QUEUEING (CPU allocation across concurrent page fibers).
       flowVt = this flow's accumulated VIRTUAL TIME; the scheduler picks the
       runnable fiber with the SMALLEST flowVt, and after a fiber runs a slice
       of wall-time `c` advances its flowVt by c / weight. Higher weight ⇒
       slower virtual-time growth ⇒ MORE real CPU — so CPU share is
       proportional to weight (= marginal value). A fresh flow starts at the
       current system virtual time (self._sysVt), NOT 0, so a newly-opened tab
       can't hoard CPU "catching up" from virtual time it was never present
       for (Start-time Fair Queueing's max-with-system-V rule). epRate = an
       EWMA of endpoints produced per resume slice = the observed marginal
       value that feeds the weight. */
    flowVt: (typeof self._sysVt === "number" ? self._sysVt : 0),
    epRate: 0,
  };
  async function freshInstance() {
    instAborted = false;
    var inst = await self.createQJS({
      noInitialRun: true,
      print: function (s) {
        /* Per-line tap: the engine emits structured @Y / @H / @T / @S
           records on stdout. Update huntContext live so the priority
           comparator sees the latest reachability + emission counts.
           Then push to stdout for the BFS / drive logic below. */
        if (s.length > 3) {
          var p3 = s.charCodeAt(0) === 64 /* @ */ ? s.substr(0, 3) : "";
          if (p3 === "@Y ") {
            var m = /reaches=([01])/.exec(s);
            if (m) huntContext.lastReaches = +m[1];
          } else if (p3 === "@H ") {
            huntContext.totalEndpoints++;
            huntContext.totalEmissions++;
          } else if (p3 === "@T " || p3 === "@S ") {
            huntContext.totalSecondary++;
            huntContext.totalEmissions++;
          }
        }
        stdout.push(s);
      },
      printErr: function (s) {
        stderr.push(s); if (!self._lastStderr) self._lastStderr = []; self._lastStderr.push(s); if (self._lastStderr.length > 500) self._lastStderr.shift();
        /* Promote the engine's leak diagnostic to the non-rotating @WHY log:
           the FreeRuntime_residue line (printed by JS_FreeRuntime right before
           the gc_obj_list assert, naming the leaked object TYPE) scrolls off the
           500-line stderr buffer before it can be read, so the abort looked
           unattributed. Keep it durable so the specific leaked type (obj /
           bytecode / varref / async / context) is always recoverable. */
        if (s.indexOf("FreeRuntime_residue") >= 0) {
          if (!self._whyRecords) self._whyRecords = [];
          try { self._whyRecords.push(Object.assign({ phase: "freeruntime_residue_raw" }, JSON.parse(s.slice(s.indexOf("{"))))); }
          catch (e) { self._whyRecords.push({ phase: "freeruntime_residue_raw", line: s.slice(0, 200) }); }
        }
        /* Promote each bundle-slice's module-vs-classic dispatch to the durable
           @WHY log: the breadcrumb scrolls off the 500-line stderr buffer
           (deep-grind re-boots flood it), but its isModule bit IS the
           cross-slice-global signal — a CDN <script src> defining `var Sentry`
           runs in isolated MODULE scope iff JS_DetectModule flags the slice,
           hiding that global from a later inline slice. bc_emit = the bytecode
           compile decision (the bundle's real path); eval_script = the non-bc
           path, infra files (/h,/p,/d,/pre) excluded as noise. */
        if (s.indexOf("bc_emit") >= 0 ||
            (s.indexOf("eval_script") >= 0 && s.indexOf("/h.js") < 0 && s.indexOf("/p.js") < 0 && s.indexOf("/d.js") < 0 && s.indexOf("/pre.js") < 0)) {
          if (!self._whyRecords) self._whyRecords = [];
          try { self._whyRecords.push(JSON.parse(s.slice(s.indexOf("{")))); }
          catch (e) { self._whyRecords.push({ phase: "bc_emit", line: s.slice(0, 200) }); }
        }
      },
      onAbort: function (msg) {
        instAborted = true;
        /* Capture the emscripten abort reason — without this it lands on the
           default emscripten printErr which our worker shim wires up, but on
           wasm-builtin aborts (RuntimeError, integer overflow, OOM) the message
           may bypass printErr and arrive only here. Record it on @WHY so a
           silent abort no longer looks like a clean return. */
        if (!self._whyRecords) self._whyRecords = [];
        self._whyRecords.push({ phase: "wasm_abort", reason: String(msg || "(no reason)") });
      },
      /* JSPI cooperative-yield import: each yield is enqueued in
         self._fiberQ with this instance's huntContext (pageKey/type/vts).
         self._yieldDrain (the global scheduler) picks which entry to
         resume next BY PRIORITY across all pages — not by arrival. The
         wasm stack is preserved by JSPI's real stack switching while
         this Promise is pending. */
      qjs_host_yield: function () {
        /* Per-orphan drain (deep grind) — invoke this instance's drain
           hook synchronously BEFORE the suspending Promise resolves, so
           @DD/@H/@T/@S printed since the last yield are aggregated and
           persisted while this fiber is paused. Hook is set by the deep
           grind on huntContext.onYieldDrain; absent for review fibers
           (their print loop is post-callMain). The hook itself is an
           async function but returning a Promise from a Promise body is
           ignored — we fire-and-forget; the IDB put it awaits resolves
           on its own microtask. Reentrancy is gated inside the drain. */
        if (huntContext.onYieldDrain) {
          try { huntContext.onYieldDrain(); }
          catch (e) {
            /* A drain throw is an observability failure for THIS yield
               (@DD/@H/etc. since last yield not aggregated). Surface so
               a hook bug doesn't silently lose endpoints. The wasm
               resume below still runs so the grind continues. */
            if (!self._whyRecords) self._whyRecords = [];
            self._whyRecords.push({ phase: "yield_drain_throw", err: String(e && e.message || e) });
          }
        }
        return new Promise(function (resolve) {
          if (!self._fiberQ) self._fiberQ = [];
          var entry = { resolve: resolve, ts: Date.now(), ctx: huntContext };
          self._fiberQ.push(entry);
          if (typeof self._yieldDrain === "function") {
            self._yieldDrain(entry);
          } else {
            queueMicrotask(function () {
              var i = self._fiberQ.indexOf(entry);
              if (i >= 0) { self._fiberQ.splice(i, 1); resolve(); }
            });
          }
        });
      },
    });
    for (var i = 0; i < inMem.length; i++) {
      // Create parent dirs first — FS.writeFile won't (ENOENT on nested paths).
      // Flat slices ("/firebase-app.js") need none; deep encoded module-identity
      // paths ("/x/<host>/@scope/pkg.mjs") do, or the slice silently fails to write
      // and the module can't load (wedges an ESM-app boot).
      var _ip = inMem[i][0], _sl = _ip.lastIndexOf("/");
      if (_sl > 0) { try { inst.FS.mkdirTree(_ip.slice(0, _sl)); } catch (e) {} }
      inst.FS.writeFile(_ip, inMem[i][1]);
    }
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
  // Compile each per-script bundle file to bytecode so re-parsing the
  // multi-MB sources on every schedule (~850 ms each on github) collapses
  // to a fast JS_ReadObject. qjs_eval_bc dispatches MODULE vs GLOBAL on
  // the value tag, so ESM vs classic stays correct after the compile.
  // start-lines are passed in so JS_Eval2's line_num bakes combined-bundle
  // line numbers into the .bc's debug info.
  //
  // DEFER source unlinking until every BC is compiled: a later script's
  // BC compile may issue `import "<earlier>.js"` which the libc loader
  // reads from MEMFS. Unlinking the source after each compile would
  // make the import fail for cross-chunk references (rspack/webpack
  // runtime chunks). Sources stay in MEMFS for the entire compile loop;
  // also stay in inMem so freshInstance re-creates them on schedule
  // recycle.
  var _bcOk = 0, _bcEmpty = 0, _bcThrew = 0, _bcAborted = 0;
  var _bcBaselineBytes = 0;   // first chunk's footprint; recycle ratio is relative
  var _bc0 = Date.now();
  for (var bce = 0; bce < bundleFiles.length; bce++) {
    var _src = bundleFiles[bce].path;
    var _dst = _src.replace(/\.js$/, ".bc");
    // .mjs / no-extension module ids (esm.sh: "/x/<host>/auth-js.mjs", ".../supabase-js@2")
    // don't match /\.js$/, so _dst would EQUAL _src and the emitted bytecode would OVERWRITE
    // the source in MEMFS + inMem. The import-triggered module loader then reads that BC
    // (version byte 0x1A) AS SOURCE → "SyntaxError: unexpected token ''" at 1:1, the esm.sh
    // 0-endpoints blocker. Keep _dst distinct so the source survives for the loader.
    if (_dst === _src) _dst = _src + ".bc";
    var _bcCallThrew = false;
    try {
      await m.callMain([startLineArg, "--fe-emit-bc", _src, _dst]);
      var _bc = null;
      try { _bc = m.FS.readFile(_dst); } catch (e) { _bc = null; }
      if (_bc && _bc.length > 0) {
        var _bi = fileArgs.indexOf(_src);
        if (_bi >= 0) fileArgs[_bi] = _dst;
        inMem.push([_dst, _bc]);
        _bcOk++;
      } else {
        /* @WHY observability: the bytecode file is missing or empty even though
           callMain returned without throwing — qjsmain's --fe-emit-bc printed @E
           or wrote zero bytes. Recording the source keeps a per-script trail so
           a host-model gap (e.g. an unmodelled JS feature breaking the parse
           ONLY for this slice) is visible. NOT a silent skip. */
        _bcEmpty++;
        if (!self._whyRecords) self._whyRecords = [];
        self._whyRecords.push({ phase: "bc_compile_empty", file: _src, len: bundleFiles[bce].src.length });
      }
    } catch (e) {
      _bcCallThrew = true;
      _bcThrew++;
      if (instAborted) _bcAborted++;
      if (!self._whyRecords) self._whyRecords = [];
      self._whyRecords.push({ phase: "bc_compile_throw", file: _src, err: String(e && e.message || e), aborted: !!instAborted });
    }
    /* Memory watchdog over the bc-compile loop. Each --fe-emit-bc invocation
       creates+frees a JSRuntime; wasm `memory.grow` is monotonic so the freed
       memory is never returned to the OS — over 646 github chunks the
       instance climbs past Chrome's per-instance wasm cap and the next
       emit-bc throws a wasm trap ("memory access out of bounds"). Note the
       trap does NOT set instAborted (onAbort fires for emscripten abort()
       calls only, not for wasm RuntimeErrors); we must recycle on ANY throw,
       not just abort. The recycle reuses the SAME instance-disposable
       principle as the BFS schedule loop's ratio-based watchdog at the
       bottom of this function; reusing it here is structural, not a
       workaround. inMem already carries every .bc produced so far + every
       still-uncompiled source, so freshInstance reconstitutes MEMFS
       verbatim. */
    if (instAborted || _bcCallThrew) {
      if (!self._whyRecords) self._whyRecords = [];
      self._whyRecords.push({ phase: "bc_recycle", reason: instAborted ? "abort" : "wasm_trap", after_bce: bce });
      m = null;
      await new Promise(function (r) { setTimeout(r, 0); });
      m = await freshInstance();
      _bcBaselineBytes = 0;
    } else {
      var _bcMemNow = wasmBytes(m);
      if (_bcBaselineBytes === 0) _bcBaselineBytes = _bcMemNow;
      else if (_bcMemNow > _bcBaselineBytes * 2) {
        if (!self._whyRecords) self._whyRecords = [];
        self._whyRecords.push({ phase: "bc_recycle", reason: "mem_growth", after_bce: bce, baseline: _bcBaselineBytes, now: _bcMemNow });
        m = null;
        await new Promise(function (r) { setTimeout(r, 0); });
        m = await freshInstance();
        _bcBaselineBytes = 0;
      }
    }
  }
  if (!self._whyRecords) self._whyRecords = [];
  _tBcMs = Date.now() - _bc0;
  self._whyRecords.push({ phase: "bc_compile_done", total: bundleFiles.length, ok: _bcOk, empty: _bcEmpty, threw: _bcThrew, aborted: _bcAborted });
  // Aggregate stdout (@E/@T/@H/@S/@P/@Z) into the shared learning state.
  // BFS schedule runs pass startIdx=0 (each run resets stdout first); the
  // deep grind's drain passes a per-yield startIdx so each @H/@T/@S line
  // is aggregated exactly once across the long single-callMain. _pendingSec
  // / _pendingPoC are CLOSURE-LEVEL (declared here, used by the loop below) so
  // an @S/@P whose paired @Z arrived in a LATER yield/drain batch is not
  // orphaned by the drain boundary. (These were previously undeclared implicit
  // globals while a dead `__`-prefixed pair was reset here — so cross-batch
  // pairing silently dropped the Z3 @P plan; declaring the names the loop
  // actually uses fixes it.)
  var _pendingSec = null;       // @S waiting for paired @Z verdict (spans yields)
  var _pendingPoC = null;       // @P arriving between @S and @Z (spans yields)
  var _codeLines = null;        // lazy split of the combined bundle for caught_throw snippet enrichment
  var _caughtSites = new Set(); // dedup: enrich the snippet once per throw site (a hot site re-emits per drive)
  function processStdout(startIdx) {
    var rh = [];
    var li0 = (typeof startIdx === "number" && startIdx > 0) ? startIdx : 0;
    // startIdx==0 means a fresh stdout buffer (BFS schedule OR first deep-
    // drain on a recycled instance), so any dangling _pendingSec from a
    // PRIOR buffer must not pair into the new run.
    if (li0 === 0) { _pendingSec = null; _pendingPoC = null; }
    for (var li = li0; li < stdout.length; li++) {
      var line = stdout[li];
      if (line.slice(0, 8) === "@MODURL ") {
        /* A CDN-URL ESM import (`import x from "https://cdn/auth.js"`) the module
           loader couldn't resolve from MEMFS — feed it into chunkUrls so the same
           discover→fetch→re-run loop that handles webpack lazy chunks pulls the
           imported module in. Without this, modular ESM apps (Vite/Rollup, modular
           firebase) analyze to total:0 — their dep modules are fetched by the live
           page's module loader, never shipped by content.js. */
        var _mu = line.slice(8);
        var _tab = _mu.indexOf("\t");
        if (_tab >= 0) {
          // <importer-module-name>\t<relative-specifier> — a fetched CDN module's
          // relative/absolute-path import. WHATWG-resolve the specifier against the
          // importer's REAL URL (not the page base) so esm.sh-style transitive
          // imports like `/@supabase/auth-js.mjs` reach the right origin.
          var _imp = _mu.slice(0, _tab).trim(), _spec = _mu.slice(_tab + 1).trim();
          var _impUrl = _slicePathToUrl[_imp];
          if (_impUrl && _spec) { try { esmImportUrls.add(new URL(_spec, _impUrl).href); } catch (e) {} }
        } else {
          _mu = _mu.trim();
          if (_mu && !isUnresolved(_mu)) esmImportUrls.add(_mu);
        }
        continue;
      }
      if (line.slice(0, 11) === "@SCRIPTSRC ") {
        /* The engine discovered an external <script src> from the Lexbor DOM (the
           one-message-per-document model — the page's OWN bundle, not a lazy chunk).
           Format: "@SCRIPTSRC <c|m> <url>". Collect for the offscreen to safeFetch +
           feed back to be RUN in order (classic global eval, or the module path for
           type=module). Surfaced on self for the brain + verification. */
        var _ss = line.slice(11);
        var _ssp = _ss.indexOf(" ");
        if (_ssp > 0) {
          var _ssUrl = _ss.slice(_ssp + 1).trim();
          if (_ssUrl && !isUnresolved(_ssUrl)) scriptSrcUrls.push({ url: _ssUrl, module: _ss.slice(0, _ssp) === "m" });
        }
        continue;
      }
      if (line.slice(0, 5) === "@WHY ") {
        /* Diagnostic record from a phase that finished without producing
           its expected output. Surface on self so the brain can expose
           it to the popup's diagnostic view. */
        try {
          var why = JSON.parse(line.slice(5));
          // Enrich a caught_throw (a swallowed exception in a host-edge-reaching
          // fn — typically a MISSING host/DOM stub the path called as a
          // function) with the SOURCE at the throw, so the missing capability
          // is self-identifying without dumpscripts/worker line-mapping
          // archaeology (the combined `code` lines match the engine's reported
          // combined line/col). Deduped per site — a hot site re-emits per drive.
          if (why && why.phase === "caught_throw" && (why.line | 0) > 0 && !why.snippet) {
            var _cs = (why.file || "") + ":" + why.line + ":" + (why.col || 0);
            if (!_caughtSites.has(_cs)) {
              _caughtSites.add(_cs);
              try {
                if (!_codeLines) _codeLines = String(code || "").split("\n");
                var _cl = _codeLines[(why.line | 0) - 1] || "";
                var _cc = (why.col | 0);
                why.snippet = _cl.slice(Math.max(0, _cc - 70), _cc + 90);
              } catch (e2) {}
            }
          }
          if (!self._whyRecords) self._whyRecords = [];
          self._whyRecords.push(why);
        } catch (e) {
          /* A malformed @WHY emission is itself an observability gap — the
             whole point of @WHY is to surface failures, and dropping the
             malformed one means a downstream phase that emitted bad JSON is
             ALSO invisible. Record the raw line and the parse error so the
             engine-side serialization bug is self-diagnosing. */
          if (!self._whyRecords) self._whyRecords = [];
          self._whyRecords.push({ phase: "why_parse_throw", raw: line.slice(0, 300), err: String(e && e.message || e) });
        }
        continue;
      }
      if (line.slice(0, 3) === "@E ") {
        var ej; try { ej = JSON.parse(line.slice(3)); } catch (e) { ej = { message: line.slice(3) }; }
        var emsg = String(ej.message || "(throw)");
        var ekey = "E:" + emsg.slice(0, 120);
        if (!reSeen.has(ekey)) {
          reSeen.add(ekey);
          var bundleFileNames = bundleFiles.map(function (b) { return b.path; });
          var firstFrame = String(ej.stack || "").split("\n").filter(function (s) {
            for (var bfn = 0; bfn < bundleFileNames.length; bfn++) if (s.indexOf(bundleFileNames[bfn] + ":") >= 0) return true;
            return false;
          })[0] || "";
          var snippet = null;
          var fmre = null;
          for (var bfn2 = 0; bfn2 < bundleFileNames.length && !fmre; bfn2++) {
            var name = bundleFileNames[bfn2].replace(/[.+*?^${}()|[\]\\]/g, "\\$&");
            fmre = new RegExp(name + ":(\\d+):(\\d+)").exec(firstFrame);
          }
          if (fmre) {
            var ln = +fmre[1], cl = +fmre[2];
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
        } catch (e) {
          /* Malformed @T from the engine — without surfacing, a corrupt
             structural-candidate emission silently halves the JAW set the
             reviewer sees. Engine-side serialization bug if observed. */
          if (!self._whyRecords) self._whyRecords = [];
          self._whyRecords.push({ phase: "at_parse_throw", line: line.slice(0, 200), err: String(e && e.message || e) });
        }
        continue;
      }
      if (line.slice(0, 3) === "@H ") {
        try { rh.push(JSON.parse(line.slice(3))); }
        catch (e) {
          /* Malformed @H (host-edge call site) — silently dropping it means
             a real fetch the bundle reached is lost from the learned set. */
          if (!self._whyRecords) self._whyRecords = [];
          self._whyRecords.push({ phase: "ah_parse_throw", line: line.slice(0, 200), err: String(e && e.message || e) });
        }
      } else if (line.slice(0, 3) === "@S ") {
        try { _pendingSec = JSON.parse(line.slice(3)); _pendingPoC = null; }
        catch (e) {
          /* @S (security sink) parse failure — without surfacing, a real
             tainted-sink finding is silently lost AND the verdict pairing
             (next @Z) attaches to a stale or null _pendingSec. */
          _pendingSec = null; _pendingPoC = null;
          if (!self._whyRecords) self._whyRecords = [];
          self._whyRecords.push({ phase: "as_parse_throw", line: line.slice(0, 200), err: String(e && e.message || e) });
        }
      } else if (line.slice(0, 3) === "@P ") {
        try { _pendingPoC = JSON.parse(line.slice(3)); }
        catch (e) {
          _pendingPoC = null;
          if (!self._whyRecords) self._whyRecords = [];
          self._whyRecords.push({ phase: "ap_parse_throw", line: line.slice(0, 200), err: String(e && e.message || e) });
        }
      } else if (line.slice(0, 3) === "@Z ") {
        var zr;
        try { zr = JSON.parse(line.slice(3)); }
        catch (e) {
          /* @Z (Z3 verdict) parse failure — the security verdict is lost,
             and the paired _pendingSec must drop too (no verdict means we
             can't classify the @S). */
          _pendingSec = null; _pendingPoC = null;
          if (!self._whyRecords) self._whyRecords = [];
          self._whyRecords.push({ phase: "az_parse_throw", line: line.slice(0, 200), err: String(e && e.message || e) });
          continue;
        }
        if (!_pendingSec) continue;
        // DIAG: capture whether a Z3 @P plan is paired at each REAL_EXPLOIT @Z,
        // to pin why the live PoC falls back to a template (plan dropped before
        // finding.poc). Read via the worker harness cmd's self._whyRecords.
        if (zr.verdict === "REAL_EXPLOIT") {
          if (!self._whyRecords) self._whyRecords = [];
          self._whyRecords.push({ phase: "poc_pair_diag", sink: _pendingSec.sink,
            hasPendingPoC: !!_pendingPoC,
            pocSteps: (_pendingPoC && _pendingPoC.steps && _pendingPoC.steps.length) || 0,
            pocSample: _pendingPoC ? JSON.stringify(_pendingPoC).slice(0, 200) : null });
        }
        if (zr.verdict === "INFEASIBLE") { _pendingSec = null; _pendingPoC = null; continue; }
        var ss = pickSite(_pendingSec.at);
        var sk = _pendingSec.type + "|" + _pendingSec.sink + "|" + (ss.loc ? ss.loc.line + ":" + ss.loc.column : "?");
        // The conditionals (Φ) the forced run took to reach this sink, and the
        // interprocedural call chain (which functions the taint passed through).
        var conditions = _phiToConditions(zr.phi);
        var dataFlow = _psiToHops(zr.psi);
        var verdictReason = _classifyVerdict(zr.verdict, conditions, zr.witness);
        // Each forced run reaching the SAME sink is a DISTINCT interprocedural
        // path (different Φ / call chain). Keyed by the Φ string so genuinely
        // identical paths dedup but alternate gating-paths accumulate.
        var pathRec = {
          verdict: zr.verdict,
          verdictReason: verdictReason,
          conditions: conditions,
          dataFlow: dataFlow,
          callChain: ss.chain,
          witness: zr.witness || null,
          psi: zr.psi || null,
          phi: zr.phi || null,
        };
        if (secSeen.has(sk)) {
          var existing = secByKey.get(sk);
          if (existing) {
            var pkey = String(zr.phi || "") + "||" + String(zr.psi || "");
            if (!existing._pathKeys) existing._pathKeys = {};
            if (!existing._pathKeys[pkey]) {
              existing._pathKeys[pkey] = 1;
              existing.paths.push(pathRec);
              // Promote the finding to the strongest verdict any path proves.
              // Only REAL_EXPLOIT is a proven PoC; EXPLOIT_UNPROVEN ranks above
              // bare TAINT_REACH but is NOT a PoC (no badge / no PoC offer).
              var rank = { REAL_EXPLOIT: 3, EXPLOIT_UNPROVEN: 2, Z3_ERROR: 2, TAINT_REACH: 1 };
              if ((rank[zr.verdict] || 0) > (rank[existing.verdict] || 0)) {
                existing.verdict = zr.verdict;
                existing.verdictReason = verdictReason;
                existing.severity = _pendingSec.type === "code-exec" ? "critical" : ((zr.verdict === "TAINT_REACH" || zr.verdict === "EXPLOIT_UNPROVEN") ? "medium" : "high");
                existing.taintPath = dataFlow.length ? dataFlow : ss.chain.map(function (c) { return { at: c }; });
                existing.conditions = conditions;
                existing.callChain = ss.chain;
                existing.witness = zr.witness || existing.witness;
                existing.psi = zr.psi || existing.psi;
                existing.phi = zr.phi || existing.phi;
              }
            }
          }
          _pendingSec = null; _pendingPoC = null;
          continue;
        }
        secSeen.add(sk);
        sinkSeen.add(sk);
        var sev = _pendingSec.type === "code-exec" ? "critical" : "high";
        // TAINT_REACH (exploit shape UNSAT) and EXPLOIT_UNPROVEN (exploit SAT but
        // only under an unsound over-approximation — NOT a proven PoC) are both
        // medium: real taint reach, but no confidently-solvable exploit.
        if (zr.verdict === "TAINT_REACH" || zr.verdict === "EXPLOIT_UNPROVEN") sev = "medium";
        var poc = null;
        if (_pendingPoC && Array.isArray(_pendingPoC.steps) && _pendingPoC.steps.length) {
          poc = adaptPoc(_pendingPoC, _pendingSec.type, zr.psi);
        }
        // Recover the attacker source from Ψ. LEAF terms serialize as
        // `$<id>:<label>` where <label> is the taint-source name the engine
        // assigned at OPQ() (location.hash, postMessage.data, document.cookie,
        // …) — the engine already emits it in psi, so surface it instead of a
        // generic host-unknown fallback. The popup maps it to a probe strategy.
        var srcLabel = null;
        if (zr.psi && typeof zr.psi === "string") {
          var _labels = [], _re = /\$\d+:([A-Za-z][\w.]*)/g, _m;
          while ((_m = _re.exec(zr.psi))) _labels.push(_m[1]);
          for (var _li = 0; _li < _labels.length; _li++) {
            if (/^location\.(hash|search|pathname|href)$/.test(_labels[_li]) ||
                /postMessage|event\.data/i.test(_labels[_li])) { srcLabel = _labels[_li]; break; }
          }
          if (!srcLabel && _labels.length) srcLabel = _labels[0];
        }
        var finding = {
          type: _pendingSec.type,
          sink: _pendingSec.sink,
          source: srcLabel || "host-unknown attacker input reached the sink (forced multi-path execution)",
          sourceType: srcLabel ? "user-controlled" : undefined,
          severity: sev,
          location: ss.loc || { line: 0, column: 0 },
          // Prefer the engine's psi data-flow (source→ops→sink, verifiable +
          // feeds the probe's field-path/decoder extractors); fall back to the
          // @S call-stack positions only when there's no psi term.
          taintPath: dataFlow.length ? dataFlow : ss.chain.map(function (c) { return { at: c }; }),
          // The interprocedural call chain + the conditionals (Φ) + the
          // verdict classification — so the trace shows WHICH functions, WHICH
          // gates, and WHY it's a PoC vs pinned-FP vs gen-failure. `paths` holds
          // every distinct forced path reaching this sink (multi-path view).
          callChain: ss.chain,
          conditions: conditions,
          verdictReason: verdictReason,
          paths: [pathRec],
          _pathKeys: (function () { var o = {}; o[String(zr.phi || "") + "||" + String(zr.psi || "")] = 1; return o; })(),
          value: _pendingSec.value != null ? String(_pendingSec.value) : null,
          verdict: zr.verdict,
          witness: zr.witness || null,
          psi: zr.psi || null,
          phi: zr.phi || null,
          poc: poc,
        };
        securitySinks.push(finding);
        secByKey.set(sk, finding);
        _pendingSec = null;
        _pendingPoC = null;
      }
    }
    var pend = null;
    for (var ri = 0; ri < rh.length; ri++) {
      var r = rh[ri];
      if (r.api === "XMLHttpRequest.open") { if (pend) ep(pend.m, pend.u, null, "xhr", pend.at, undefined, undefined, pend.holes); pend = { m: r.args[0], u: r.args[1], at: r.at, holes: r.args[2] }; }
      else if (r.api === "XMLHttpRequest.send") { if (pend) { ep(pend.m, pend.u, r.args[0], "xhr", pend.at, r.args[1], r.args[2], pend.holes); pend = null; } }
      else if (r.api === "fetch") ep(r.args[1] || "GET", r.args[0], r.args[2], "fetch", r.at, r.args[3], r.args[4], r.args[5]);
      /* html-attr: declared endpoint extracted from the Lexbor document
         ([action] / [formaction] / [data-*-url|-href|-src]). No body, no
         headers — the URL itself IS the captured fact. Args layout
         matches the fetch shape so ep() reads url/method from args[0]/
         args[1]; the shape carries kind:"declared" so the brain sees
         it's from HTML, not from a forced fetch call. */
      else if (r.api === "html-attr") ep(r.args[1] || "GET", r.args[0], null, "html-attr", r.at, null, null, null);
      /* WebSocket / EventSource / sendBeacon: the bundle CONSTRUCTED a
         real connection / event-source URL during forced execution. The
         structural-residue path above already handles UNREACHED instances
         (url:null), but a REACHED ws:// or wss:// URL was being dropped
         because the dispatch had no case — the @H record was emitted by
         hostedge but nothing folded it into the endpoint set. The "method"
         field is the protocol verb (WEBSOCKET/EVENTSOURCE/SENDBEACON) to
         match the structural variant's labeling. */
      else if (r.api === "WebSocket") ep("WEBSOCKET", r.args[0], null, "websocket", r.at, null, null, null);
      else if (r.api === "EventSource") ep("EVENTSOURCE", r.args[0], null, "eventsource", r.at, null, null, null);
      else if (r.api === "sendBeacon") ep("SENDBEACON", r.args[0], r.args[1], "sendbeacon", r.at, r.args[2], null, null);
      /* script/Worker/SharedWorker URLs are ALL additional JS bundle
         locations: the brain downloads them and re-analyses to recover
         lazy-loaded endpoints. Without including Worker/SharedWorker in
         chunkUrls, bundles that move their fetch surface into a Web
         Worker (Google Drive, Apple Maps, Office 365) lose every fetch
         the Worker makes — those scripts were never analysed. */
      else if ((r.api === "script" || r.api === "Worker" || r.api === "SharedWorker") &&
               r.args && r.args[0] && !isUnresolved(r.args[0])) chunkUrls.add(String(r.args[0]));
    }
    if (pend) ep(pend.m, pend.u, null, "xhr", pend.at, undefined, undefined, pend.holes);
  }
  /* Within-page priority: BFS pops schedules by priority, not FIFO. Each
     pending job has a `key` (its uncovered-edge identity) and a `deep` flag
     (Quadratic-boost bypass — fruitful run's frontier). Priority order
     lexicographic, no magic weights:
       1. `deep` bypass jobs first (they came from a fruitful run that
          just emitted a NEW @H/@S — its frontiers are likely productive).
       2. shorter prefix first (shallower depth → may unlock more frontiers
          via the existing X-Force expansion).
       3. enqueue order (anti-starvation, oldest waiting beats newest tied).
     Each pop is O(n) over `work`; n is bounded by the distinct uncovered
     branch-edge set per CLAUDE.md's "Linear edge prune" → polynomial. */
  var _lastEmittedHS = huntContext.totalEmissions | 0;   // post-run delta marks fruitfulness
  /* Comparator delegated to lib/priority.js so the heuristic lives in the
     dedicated file (ORDER only, never COVERAGE). The schedCmp returns the
     <,=,> ordering for two work entries on (deep, prefix-length); the
     enqueue-order tiebreaker is handled here via the array index since the
     comparator is pure and doesn't see positions. */
  function _pickJob() {
    var bestI = 0, best = work[0];
    for (var k = 1; k < work.length; k++) {
      if (self._priorityCmp.schedCmp(work[k], best) < 0) { best = work[k]; bestI = k; }
      // schedCmp returns 0 on equal-priority — the earlier array index already
      // held by bestI is the anti-starvation tiebreaker (oldest enqueued wins).
    }
    work.splice(bestI, 1);
    return best;
  }
  // === Snapshot-restore schedule execution (Wizer-style linear-memory imaging,
  // validated in mdrive.mjs SNAP=1: identical results, ~7-8× lower per-schedule
  // cost). REPLACES re-boot-per-schedule: boot the bundle ONCE into the
  // persistent g_boot_ctx, image HEAPU8, then restore the image + drive per
  // schedule (no per-schedule re-eval). The driver epilogue (/d.js, last
  // fileArg) is the per-schedule DRIVE; everything before it (page DOM, /h.js,
  // bundle .bc) is the one-time BOOT. The restore resets the bundle's mutated
  // JS state AND footprint to post-boot every drive, so the memory accumulation
  // the old per-schedule watchdog guarded against cannot happen — the watchdog
  // is gone, replaced by abort-recovery (a wasm abort poisons the instance; the
  // JS-side ABORT flag survives a memcpy, so re-boot a fresh instance). */
  var driveArg = fileArgs[fileArgs.length - 1];   // /d.js
  var bootArgs = fileArgs.slice(0, -1);           // /pre.js, --fe-script-start-lines, /h.js, /p.js, bundle .bc
  // Boot the bundle ONCE, image HEAPU8, then restore + drive per schedule (no
  // per-schedule re-eval). The restore resets the bundle's state + footprint to
  // post-boot every drive, so the cross-schedule memory climb the old watchdog
  // guarded against can't happen (watchdog deleted). A schedule's decision
  // string drives the FUNCTIONS the driver invokes; opaque branches at module
  // top-level run during boot (before the image) and are SURFACED, not explored
  // (bootSnapshot) — this model does NOT re-boot for them.
  var _snap = null;   // post-boot linear-memory image (Uint8Array)
  async function bootSnapshot() {
    stdout.length = 0; stderr.length = 0;
    try { m.FS.writeFile("/boot.tr", new Uint8Array(0)); } catch (e) {}
    var _bt0 = Date.now();
    try { await m.callMain(["--fe-boot", "--fe-trace=/boot.tr"].concat(bootArgs)); }
    catch (e) {
      if (!self._whyRecords) self._whyRecords = [];
      self._whyRecords.push({ phase: "boot_callmain_throw", err: String(e && e.message || e), instAborted: !!instAborted });
    }
    _tBootMs += Date.now() - _bt0;
    processStdout();   // module-init @H/@S/@Z, aggregated once
    var _img = (m.HEAPU8 && m.HEAPU8.length) ? m.HEAPU8.slice() : null;
    var _bt = ""; try { _bt = m.FS.readFile("/boot.tr", { encoding: "utf8" }); } catch (e) {}
    var _bd = [], _bfr = [], _btl = _bt.split("\n");
    for (var _bi = 0; _bi < _btl.length; _bi++) {
      var _bb = _btl[_bi].match(/^B (\d+) (\d)/); if (_bb) _bd[+_bb[1]] = _bb[2];
      var _bff = _btl[_bi].match(/^F (\d+)/); if (_bff) _bfr.push(+_bff[1]);
    }
    if (_bfr.length) { if (!self._whyRecords) self._whyRecords = []; self._whyRecords.push({ phase: "boot_frontiers_unexplored", count: _bfr.length }); }
    // Boot-frontier coverage. A top-level `if(opaque){sink/fetch}` runs during the
    // seed boot eval — BEFORE the snapshot image — so the pure-snapshot BFS can't
    // flip it (the snapshot images post-boot). Re-boot once per frontier with that
    // decision flipped (--fe-boot --fe-sched), driving the sink/fetch-bearing arm.
    // The boot-forced decision PUSHES its predicate into Φ, so the security verdict
    // is correct (TAINT_REACH for a value-pinning gate `===`, REAL_EXPLOIT for a
    // bypass) — this only became safe to re-enable once the gated-sink Z3 false-
    // positive was fixed (fresh exploit solver + UNDEF-gated breakout, quickjs.c).
    // SINGLE-LEVEL: a frontier revealed only AFTER flipping another (a NESTED boot
    // gate) is not yet re-explored; the complete form is a boot-frontier BFS that
    // enqueues each re-boot trace's new frontiers to fixpoint. The boot_frontiers
    // count above surfaces the residual so the gap is visible, never silent. Boot
    // frontiers are ~0 on real bundles (host-edge taint is read INSIDE functions,
    // not at module top level — _idxdocs: 0), so this is inert there.
    var _rebooted = 0;
    for (var _fi = 0; _fi < _bfr.length && !instAborted; _fi++) {
      var _p = _bfr[_fi];
      var _bs = "";
      for (var _j = 0; _j < _p; _j++) _bs += (_bd[_j] != null ? _bd[_j] : "0");
      _bs += (_bd[_p] === "1" ? "0" : "1");
      stdout.length = 0; stderr.length = 0;
      try { m.FS.writeFile("/boot.tr", new Uint8Array(0)); } catch (e) {}
      try { await m.callMain(["--fe-boot", "--fe-sched=" + _bs, "--fe-trace=/boot.tr"].concat(bootArgs)); }
      catch (e) { if (!self._whyRecords) self._whyRecords = []; self._whyRecords.push({ phase: "reboot_frontier_throw", pos: _p, err: String(e && e.message || e) }); }
      processStdout();
      _rebooted++;
    }
    if (_rebooted && !instAborted) {
      stdout.length = 0; stderr.length = 0;
      try { await m.callMain(["--fe-boot"].concat(bootArgs)); } catch (e) {}
      var _img2 = (m.HEAPU8 && m.HEAPU8.length) ? m.HEAPU8.slice() : null;
      if (_img2) _img = _img2;
    }
    return _img;
  }
  if (work.length) _snap = await bootSnapshot();
  while (work.length) {
    var job = _pickJob();
    var sched = job.sched;
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
    /* BFS throughput telemetry (observability): runs done, schedules still
       queued, endpoints found so far. The deep grind (where most unused-API
       endpoints come from) only starts AFTER this loop drains, so on a massive
       bundle a queue that GROWS while endpoints stay flat = X-Force path
       explosion starving the deep grind — the signal that tells "converging
       slowly" from "exploding". Order-only diagnostic. */
    self._bfsState = { runs: runs, workLen: work.length, eps: methods.size, sinks: securitySinks.length };
    var _runT0 = Date.now();
    stdout.length = 0;
    stderr.length = 0;
    var _mc0 = Date.now();
    if (_snap) m.HEAPU8.set(_snap);                       // restore the post-boot image (resets bundle state + footprint)
    _tMemcpyMs += Date.now() - _mc0;
    try { m.FS.writeFile("/t.tr", new Uint8Array(0)); } catch (e) {}   // fresh trace per drive
    try {
      await m.callMain(["--fe-drive=" + sched, "--fe-trace=/t.tr", driveArg]);
    } catch (e) {
      /* The engine may longjmp on exit; trace + stdout still valid. But a
         RuntimeError from a wasm trap (stack overflow, unreachable, OOM) needs
         to be surfaced — without this it's swallowed and the symptom looks like
         a clean completion that just learned nothing. */
      if (!self._whyRecords) self._whyRecords = [];
      self._whyRecords.push({
        phase: "schedule_callmain_throw",
        sched: sched,
        runs: runs,
        err: String(e && e.message || e),
        instAborted: !!instAborted,
      });
    }
    var trace = "";
    try { trace = m.FS.readFile("/t.tr", { encoding: "utf8" }); }
    catch (e) {
      /* The forced-exec trace file is the BFS's frontier signal — without it,
         no `F <i> <key>` lines are decoded, no new schedules enqueue, and the
         BFS exits after this run as if the bundle had no branches. A trace
         read failure is a host-FS gap (e.g. the wasm aborted before
         flushing) and must be visible: the symptom otherwise looks like
         "the engine just stops finding endpoints after schedule N". */
      if (!self._whyRecords) self._whyRecords = [];
      self._whyRecords.push({ phase: "trace_read_throw", sched: sched, err: String(e && e.message || e) });
    }
    // Abort recovery: a wasm abort (e.g. an emscripten teardown assert) POISONS
    // the instance — every later callMain throws, and HEAPU8.set can't clear
    // the JS-side ABORT flag — so spin up a fresh instance and re-boot + re-
    // image. NO memory watchdog: the per-drive restore resets the bundle's
    // footprint to post-boot every schedule, so the cross-schedule accumulation
    // the old 2×-baseline watchdog guarded against cannot happen (the restore
    // IS the reclamation; the climb to ~920 MB on github was caused by the
    // re-boot-per-schedule model this replaces). Output for THIS run was
    // already captured in stdout/trace above.
    if (instAborted) {
      m = null;
      await new Promise(function (r) { setTimeout(r, 0); });
      m = await freshInstance();
      _snap = await bootSnapshot();        // re-image on the fresh instance
    }

    processStdout();

    /* OBSERVABILITY: if driver.js never reached its @WHY driver_entry
       breadcrumb, the bundle aborted mid per-script-eval (wasm
       Aborted, typically from list_empty(&rt->gc_obj_list) assertion
       triggered by ESM cross-chunk module-record cycles the GC can't
       collect). Without surfacing this, the brain sees "0 endpoints + 0
       errors" — indistinguishable from "didn't run". */
    var sawDriverEntry = false;
    for (var _wi = 0; _wi < stdout.length; _wi++) {
      if (stdout[_wi].indexOf("\"phase\":\"driver_entry\"") >= 0) { sawDriverEntry = true; break; }
    }
    if (!sawDriverEntry && !reSeen.has("driver_never_entered")) {
      reSeen.add("driver_never_entered");
      resolverErrors.push({
        context: "engine teardown (" + (sourceUrl || "bundle") + ")",
        message: "wasm aborted during per-script eval BEFORE driver.js ran (no driver_entry @WHY record). Most likely: a module-record cycle GC couldn't collect, triggering list_empty(&rt->gc_obj_list) assertion at JS_FreeRuntime. Look at self._lastStderr for the 'Aborted(' line.",
      });
    }

    // X-Force §3.2 hybrid: Linear edge prune by default; Quadratic
    // boost when this run was fruitful; bootstrap-Exponential while
    // no sink has fired anywhere yet. Frontiers enqueued under any
    // boost variant carry `deep:true` so the pop-time gate also
    // bypasses them.
    var decisions = [], frontiers = [], tl = trace.split("\n");
    for (var ti = 0; ti < tl.length; ti++) {
      var b = tl[ti].match(/^B (\d+) (\d) (\d+)/);
      if (b) { decisions[+b[1]] = b[2]; coveredEdges.add(b[3] + ":" + b[2]); continue; }
      var f = tl[ti].match(/^F (\d+) (\d+)(?: (\d+))?/);
      // 3rd field = reach-signature of the arm this frontier's flip explores
      // (2 = reaches a network edge, 1 = host-but-not-net, 0/absent = unknown
      // from a legacy/non-per-arm branch). schedCmp runs net-reaching
      // frontiers first so the most-likely-endpoint internal path is explored
      // ahead of the rest — intra-function priority, not FIFO over branches.
      if (f) frontiers.push({ i: +f[1], key: f[2], sig: f[3] ? +f[3] : 0 });
    }
    /* Per-schedule branch-density observation: how many opaque branches did
       this schedule traverse (B-records) and how many frontiers remain
       (F-records, post-SMT-prune). The brain uses this to estimate per-page
       continuation density — pages with many opaque branches deep in the
       reach graph have more potential continuations to schedule against
       priority signals (active-tab affinity, click affinity, etc.). Pure
       observability; doesn't change BFS scheduling. */
    if (!self._whyRecords) self._whyRecords = [];
    self._whyRecords.push({ phase: "schedule_branch_density", sched: sched.slice(0, 24), branches: decisions.length, frontiers: frontiers.length, uncoveredFrontiers: frontiers.filter(function (f) { return !coveredEdges.has(f.key + ":1"); }).length });
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
    /* Productivity tag — frontiers from a run that JUST emitted new @H/@S/@T
       records are MORE LIKELY to lead to further discoveries when their
       flipped arm runs, so they pop ahead of frontiers from unproductive
       runs (the _pickJob comparator's existing `deep` dimension). Without
       this, every frontier is FIFO-flat by the prior structural sort —
       fruitful arms wait behind boring ones, and on a large bundle the
       reviewer sees endpoints trickle in slowly even though the analyzer
       just discovered a productive seam. _lastEmittedHS captures the per-
       schedule emission count we tracked above; the comparison against
       the per-instance huntContext.totalEmissions reflects @H/@S only,
       which is the productivity signal we care about for endpoint
       learning (a schedule that found new sinks/endpoints is the seam). */
    var _emissionsDelta = (huntContext.totalEmissions | 0) - _lastEmittedHS;
    var _runFruitful = _emissionsDelta > 0;
    _lastEmittedHS = huntContext.totalEmissions | 0;
    var _enqTs = Date.now();
    if (!seedOnly) for (var ki = 0; ki < frontiers.length; ki++) {
      var fr = frontiers[ki];
      if (coveredEdges.has(fr.key + ":1")) continue;
      var ns = decisions.slice(0, fr.i).map(function (d) { return d || "0"; }).join("") + "1";
      if (!seen.has(ns)) {
        seen.add(ns);
        /* Carry the PARENT-run productivity onto every frontier this run
           emits — schedCmp uses `productivity` as tier 2 (replacing the
           old shorter-prefix depth heuristic) so frontiers from a run
           that just emitted N new @H/@S/@T pop before frontiers from a
           run that emitted fewer. ts is anti-starvation across equal-
           productivity frontiers (oldest enqueued wins). */
        work.push({ sched: ns, key: fr.key, deep: _runFruitful, frontierSig: fr.sig | 0, productivity: _emissionsDelta, ts: _enqTs });
      }
    }
    // Per-schedule wall-clock samples for surfacing in the offscreen brain via
    // a JS-readable list (`_schedTimings`). Lets us see per-schedule cost from
    // outside the worker without needing a console capture that puppeteer
    // doesn't reliably forward. Bounded to last 50 to cap memory.
    try {
      if (!self._schedTimings) self._schedTimings = [];
      self._schedTimings.push({ sched: sched.slice(0, 16), runs: runs, ms: Date.now() - _runT0 });
      if (self._schedTimings.length > 50) self._schedTimings.shift();
    } catch (e) {
      /* Per-schedule timing-record append failure — pure observability path,
         the analysis result is unaffected, but a failure here means the
         GET_SCHED_TIMINGS RPC will under-report. Surface so a corrupt timing
         buffer or out-of-memory on self._schedTimings is visible. */
      if (!self._whyRecords) self._whyRecords = [];
      self._whyRecords.push({ phase: "sched_timing_push_throw", err: String(e && e.message || e) });
    }
    // THROTTLE: yield the core ~as long as this schedule ran (≈50% duty) so a
    // BACKGROUND analysis never PINS a core. EXCEPT the page the user is actively
    // viewing (self._activePageKey): the researcher is WAITING on its results, so
    // it runs hot (no per-run sleep) — making the focused page fast was the point
    // of the question, and a flat throttle made every page (incl. the one on
    // screen) crawl at half speed. Relevance-aware, mirroring flowCmp's active-
    // page-focus tier: background stays cool, the focused page is responsive.
    // Time is free for background work; the focused page's latency is what the
    // user feels. THROTTLE_CAP_MS=0 (popup slider) still forces hot everywhere.
    var _isActive = self._activePageKey && String(sourceUrl || "").split("#")[0] === self._activePageKey;
    if (work.length && !_isActive) await new Promise(function (r) { setTimeout(r, Math.min(THROTTLE_CAP_MS, Date.now() - _runT0)); });
  }
  // Schedule BFS done. If the deep grind follows, RECYCLE to a fresh instance
  // for it rather than an explicit --fe-boot-end free of g_boot_ctx: tearing
  // down MS's async-heavy g_boot_ctx can trip the gc-leak assert and POISON the
  // instance, so the deep grind's first callMain throws and the page learns
  // ZERO @T→@H (observed: astLearned=0 on learn.microsoft.com). Dropping the
  // old instance lets V8 reclaim its whole linear memory (the g_boot_ctx image
  // included), and the deep grind boots its own g_deep_ctx on a CLEAN instance
  // — exactly the pre-snapshot model (which left no persistent runtime before
  // the deep grind). Review-only runs need no free: `m` is dropped on return.
  _snap = null;
  if (deep && m) {
    m = null;
    await new Promise(function (r) { setTimeout(r, 0); });
    m = await freshInstance();
  }

  // DEEP pass: learn the unused/render-gated chunk endpoints (login-gated
  // preheat). ONE callMain drives the entire residue; the engine yields per
  // orphan via JSPI (qjs_host_yield), so the host scheduler can rotate to a
  // higher-priority fiber (live review, another page's grind) at every
  // orphan boundary. Drain + persist run DURING each JSPI yield via
  // _drainDeepStdout below — no JS-side outer batch loop, no DEEP_BATCH
  // size knob, no DEEP_ROUND batch-count cap, no _maxDeep guard. A wasm
  // trap mid-callMain throws → recycle to a fresh instance from the
  // persisted driven-set and retry (the residue is whatever's not in
  // /driven, so the recycle resumes the right tail).
  // Phase breakdown captured at the BFS→deep boundary: bfsMs = whole schedule
  // phase (boot + memcpy + drives), of which bootMs is the one snapshot boot,
  // memcpyMs the cumulative restore cost, bcMs the one-time bytecode-compile.
  // deepMs is filled in after the deep grind. Lets the popup show where real-
  // bundle time actually goes (is the BFS or the deep grind dominant?).
  var _bfsMs = Date.now() - t0;
  var _deepStats = { steps: 0, rem: -1, stop: "n/a", total: -1, dnfThrew: 0, dnfRet: 0,
    bfsMs: _bfsMs, bootMs: _tBootMs, memcpyMs: _tMemcpyMs, bcMs: _tBcMs, deepMs: 0, runs: runs,
    combinedKB: Math.round((code ? code.length : 0) / 1024),
    hasPreheatSrc: !!(code && code.indexOf("issues/preheat/index") >= 0) };
  self._lastDeepStats = _deepStats;   // liveness: expose the in-flight grind's steps/total/rem to the heartbeat
  // Per-page liveness: with _grindCap=2 two grinds share _lastDeepStats (last to
  // update wins), so a learnstate read for the ACTIVE page can show the OTHER
  // page's counters (e.g. github read returning MS Learn's total:14411 while both
  // grind concurrently). Index by pageKey so _learningState reports the active
  // page's own grind, not whichever updated _lastDeepStats last.
  if (!self._deepStatsByPage) self._deepStatsByPage = {};
  self._deepStatsByPage[String(sourceUrl || "").split("#")[0]] = _deepStats;
  if (deep && m) {
    var _dkey = _deepKey(code);
    /* Register this LIVE grind in the SAME in-flight set the resume launcher
       (_resumeIncompleteDeep) uses, keyed by the same _deepKey(code). Without
       this, a live grind (which persists its prog record with rem>0 below) is
       INVISIBLE to _resumeIncompleteDeep, so the resume launcher could pick the
       very key being live-ground and spawn a DUPLICATE grind in a second wasm
       instance — double-driving the same orphans + breaking the concurrency
       cap (which must bound TOTAL live instances = reviews + resumes). The
       finally guarantees removal on every exit (normal, break, or an uncaught
       throw from freshInstance). */
    if (!self._deepGrindRunning) self._deepGrindRunning = new Set();
    self._deepGrindRunning.add(_dkey);
    try {
    var _dcur = _resume ? resumeCursor : 0;
    var _lastPartialN = methods.size, _lastPartialSinks = securitySinks.length;
    var _lastProgTs = 0;              // last time the live deep-grind progress was streamed (UI cadence, not a learning bound)
    var _stdoutCursor = 0;            // index into stdout: lines before this are already processed
    var _lastPersistDrivenN = -1;     // driven set size at last successful prog-put
    var _drainBusy = false;           // reentrancy guard (drain awaits IDB; another yield may fire)
    var _drem = -1;                   // last @DS rem we saw
    _deepStats.stop = "done";
    // Persist the combined bundle ONCE (heavy, ~18 MB) so a fresh worker can
    // resume. IDB failures must be visible — a silent QuotaExceededError on
    // the 18 MB code-put leaves the resume state unwriteable and breaks
    // cross-session continuity, but looks identical to "the grind finished"
    // on the next session. Same for the prog-put: without it, the
    // driven-set doesn't survive a worker eviction.
    try { await _idbPut("code", { key: _dkey, code: code, scriptUrls: scriptUrls || null, pageHtml: pageHtml || null, sourceUrl: sourceUrl || "", scriptOffsets: scriptOffsets || null, sourceMapScripts: sourceMapScripts || null }); }
    catch (e) {
      if (!self._whyRecords) self._whyRecords = [];
      self._whyRecords.push({ phase: "idb_put_code_throw", key: _dkey, codeKB: Math.round((code ? code.length : 0) / 1024), err: String(e && e.message || e) });
    }
    try { await _idbPut("prog", { key: _dkey, cur: _dcur, ts: Date.now(), vts: _vts, driven: Array.from(_driven) }); _lastPersistDrivenN = _driven.size; }
    catch (e) {
      if (!self._whyRecords) self._whyRecords = [];
      self._whyRecords.push({ phase: "idb_put_prog_throw", key: _dkey, driven: _driven.size, err: String(e && e.message || e) });
    }
    // Hand the engine the already-driven ids (from a prior session/batch) so its
    // first deep-step skips them — cross-session resume by driven-SET, not cursor.
    if (_driven.size) {
      try { m.FS.writeFile("/driven", Array.from(_driven).join("\n")); }
      catch (e) {
        if (!self._whyRecords) self._whyRecords = [];
        self._whyRecords.push({ phase: "driven_file_write_throw", n: _driven.size, err: String(e && e.message || e) });
      }
    }
    /* Drain stdout incrementally from _stdoutCursor onward: aggregate new
       @DD into _driven, parse the latest @DS for `rem`, run processStdout
       on the new tail to capture @H/@T/@S/@WHY/@E/@P/@Z, and persist the
       prog record + partial result when the driven set advanced. Bound
       to this forcedAnalyze's closure so per-instance state (stdout,
       methods, _driven, _dkey, …) is the right one. The qjs_host_yield
       Promise body invokes this synchronously on every per-orphan yield,
       and the post-callMain final-drain call below catches the tail. */
    async function _drainDeepStdout() {
      if (_drainBusy) return;   // reentrancy: an awaited IDB put yields → another fiber's yield may invoke us before we finish
      _drainBusy = true;
      try {
        for (var _di = _stdoutCursor; _di < stdout.length; _di++) {
          var _ln = stdout[_di];
          if (_ln.slice(0, 4) === "@DS ") {
            try { var _ds = JSON.parse(_ln.slice(4)); _drem = _ds.rem; if (typeof _ds.cur === "number") _dcur = _ds.cur;
              // Driving-completeness frontier counts: host-bearing @T functions
              // directed-driven that fired no host call (threw before fetch, or
              // returned without one). Per-grind cumulative — surfaced in
              // deep-status so the non-firing residue is visible, not silent.
              if (typeof _ds.dnfThrew === "number") _deepStats.dnfThrew = _ds.dnfThrew;
              if (typeof _ds.dnfRet === "number") _deepStats.dnfRet = _ds.dnfRet; }
            catch (e) {
              /* @DS JSON parse failure — leave _drem at its last value rather
                 than falling back to 0 (which would falsely look like a
                 completed grind to the cleanup branch). Surface so engine-
                 side serialization bugs are diagnosable. */
              if (!self._whyRecords) self._whyRecords = [];
              self._whyRecords.push({ phase: "ds_parse_throw", line: _ln.slice(0, 200), err: String(e && e.message || e) });
            }
          } else if (_ln.slice(0, 4) === "@DD ") {
            var _did = _ln.slice(4).trim();
            if (_did) _driven.add(_did);
            // Orphan completed → clear the "currently driving" marker. If this
            // marker is STILL set (same id) on later heartbeats, that orphan's
            // drive never returned = the non-terminating-orphan freeze, now
            // named with its source loc (per the OBSERVABILITY policy).
            if (_did && self._currentOrphan && self._currentOrphan.id === _did) self._currentOrphan = null;
          } else if (_ln.slice(0, 8) === "@DSTART ") {
            try { self._currentOrphan = JSON.parse(_ln.slice(8)); self._currentOrphan.ts = Date.now(); }
            catch (e) {
              if (!self._whyRecords) self._whyRecords = [];
              self._whyRecords.push({ phase: "dstart_parse_throw", line: _ln.slice(0, 200), err: String(e && e.message || e) });
            }
          } else if (_ln.slice(0, 5) === "@WHY ") {
            // Deep-grind @WHY (spin_nonterminating, etc.) on STDOUT — capture into
            // _whyRecords so the engine's grind-phase diagnostics are observable
            // from the harness (were DROPPED: only @H/@DD/@DS were extracted, so a
            // stdout @WHY never surfaced — the gap that blocked diagnosing the
            // multi-orphan net:0 residual). Capped to avoid a re-arming-probe flood.
            if (!self._whyRecords) self._whyRecords = [];
            if (self._whyRecords.length < 300) {
              try { self._whyRecords.push(JSON.parse(_ln.slice(5))); }
              catch (e) { self._whyRecords.push({ phase: "why_raw", line: _ln.slice(0, 200) }); }
            }
          } else if (_ln.slice(0, 8) === "@DTOTAL ") {
            // Residue size emitted at grind start → live `total` so the popup
            // shows done/total/% DURING the grind, not just the vague cross-tab
            // line until @DS at the end.
            var _tt = parseInt(_ln.slice(8), 10);
            if (_tt >= 0) _deepStats.total = _tt;
          } else if (_ln.slice(0, 7) === "@DHEAD ") {
            // Head size = net-reaching (endpoint) orphan count, sorted first.
            // The continuous-session scheduler drives every page's HEAD before
            // any page's completeness TAIL; this is the head/tail boundary.
            var _hh = parseInt(_ln.slice(7), 10);
            if (_hh >= 0) _deepStats.head = _hh;
          }
        }
        // Aggregate this drain's @H/@T/@S/@WHY/@E/@P/@Z into the shared
        // learning state. processStdout reads from _stdoutCursor through
        // stdout.length, then advances the cursor — so each line is
        // processed exactly once across the grind.
        processStdout(_stdoutCursor);
        _stdoutCursor = stdout.length;
        if (_driven.size !== _lastPersistDrivenN) {
          _deepStats.steps++;
          // Stamp wall-clock of the LAST real grind progress so the liveness
          // heartbeat (a separate timer) can report how long the grind has
          // been stuck even while the event loop is otherwise alive — the
          // signal that distinguishes a fiber-scheduling stall from a
          // blocked-event-loop freeze.
          self._lastGrindProgressTs = Date.now();
          // LIVE progress: with @DTOTAL (residue size) known from grind start,
          // rem = total - driven count, recomputed each drain so the popup
          // shows done/total/% DURING the grind. Fall back to the end-of-grind
          // @DS rem (and derive total from it) when no @DTOTAL was seen
          // (older engine / step mode).
          if (_deepStats.total >= 0) _deepStats.rem = Math.max(0, _deepStats.total - _driven.size);
          else { _deepStats.rem = _drem; if (_drem >= 0) _deepStats.total = _drem + _driven.size; }
          try { await _idbPut("prog", { key: _dkey, cur: _dcur, ts: Date.now(), vts: _vts, driven: Array.from(_driven) }); _lastPersistDrivenN = _driven.size; }
          catch (e) {
            /* Durable-cursor write — silently failing here means resume
               across MV3 eviction restarts the grind from an EARLIER
               driven-set, redriving fns that already fired @H. The grind
               eventually converges via dedup, but the engine wastes
               callMains. Surface so quota/IDB-locked is diagnosable. */
            if (!self._whyRecords) self._whyRecords = [];
            self._whyRecords.push({ phase: "idb_put_prog_step_throw", step: _deepStats.steps, driven: _driven.size, err: String(e && e.message || e) });
          }
          // Stream what this drain learned to the UI. Fire when new endpoints
          // or sinks appeared (the highest-value growth) — the live-pick
          // order means useful network-reaching orphans come early, so the
          // UI fills in fast even through long-running grinds. The merge is
          // cheap for partials (doNames=false → no source-map fetch).
          // Stream on endpoint/sink growth (the high-value signal) OR on a
          // ~2s cadence once the grind is active (total>=0) so the live
          // progress %/driven count keeps refreshing through the long
          // unproductive-orphan tail — otherwise the popup % freezes between
          // endpoint-growths and the grind looks stuck. The 2s is a UI refresh
          // cadence, NOT a learning bound: every orphan still drives; this only
          // paces how often progress is posted.
          var _nowMs = Date.now();
          if (methods.size > _lastPartialN || securitySinks.length > _lastPartialSinks ||
              (_deepStats.total >= 0 && _nowMs - _lastProgTs >= 2000)) {
            _lastPartialN = methods.size;
            _lastPartialSinks = securitySinks.length;
            _lastProgTs = _nowMs;
            try { var _pres = _buildResult(); await _resolveSmNames(_pres.fetchCallSites, scriptOffsets, sourceMapScripts, sourceUrl); postMessage({ _partial: true, sourceUrl: sourceUrl || "", response: { success: true, result: _pres } }); }
            catch (e) {
              /* Partial-result emission failed — surface so a serialization
                 gap (e.g. an opaque marker reaching JSON.stringify, or a
                 name-resolver throwing on a malformed sourcemap) is
                 visible. The grind continues. */
              if (!self._whyRecords) self._whyRecords = [];
              self._whyRecords.push({ phase: "partial_emit_throw", step: _deepStats.steps, err: String(e && e.message || e) });
            }
          }
        }
      } finally {
        _drainBusy = false;
      }
    }
    /* Register the drain on this instance's huntContext so qjs_host_yield
       can call it BEFORE pushing the suspending Promise resolve into
       _fiberQ. The yield body in freshInstance reads ctx.onYieldDrain;
       a null/missing hook (review fibers) makes it a no-op. */
    huntContext.onYieldDrain = _drainDeepStdout;
    /* Outer loop iterates only on recycle (wasm trap / abort / cumulative
       memory growth past 2× baseline) — the engine's own loop drives every
       remaining orphan in one callMain, JSPI-yielding per orphan. Without
       a recycle path, an OOM mid-grind would lose the rest of the
       residue. The /driven file is rewritten into the fresh instance, so
       the retry skips already-driven fns. */
    var _dpBaselineBytes = 0;
    var _grindDone = false;
    /* HEAD-FIRST (continuous-session scheduler, increment 2): the FIRST grind
       callMain drives ONLY the net-reaching HEAD (--fe-deep-grind-head) — on
       github that's ~405 of 98,445 orphans (0.4%), ~4s, surfacing essentially
       all endpoints — then RETURNS so the page is immediately useful and the
       scheduler can rotate to another open page's head before this page's
       ~6-min completeness tail. Subsequent iterations drive the full residue
       (the tail) with --fe-deep-grind. The driven-set carries the head's
       progress into the tail run (per-fn-id, no re-drive). NOT a cap: the tail
       still runs to rem=0; head-first only reorders WHICH runs first so
       endpoints stream early. */
    var _headPhase = true;
    var _noProgRecycles = 0, _lastRecycleDriven = -1;   // a recycle loop with driven flat across many recycles is a provable no-progress spin
    while (!_grindDone) {
      stdout.length = 0; stderr.length = 0;
      _stdoutCursor = 0;
      var _dpCallThrew = false;
      var _grindArg = _headPhase ? "--fe-deep-grind-head" : "--fe-deep-grind";
      try { await m.callMain([_grindArg].concat(fileArgs)); }
      catch (e) {
        /* Wasm trap (e.g. monotonic memory.grow saturating Chrome's per-
           instance cap, abort from JS_FreeRuntime's debug assert on heavy
           teardown). The residue is whatever's not in /driven; recycle the
           instance and retry. */
        _dpCallThrew = true;
        if (!self._whyRecords) self._whyRecords = [];
        var _co = self._currentOrphan;
        var _culpritId = (_co && _co.id) || null;
        // Capture the culprit's source loc (@DSTART carries file/line/col) — names
        // WHICH function overflowed, the context the recursion-collapse fixpoint
        // needs to bound the shape it is currently missing (a stack overflow that
        // gets abandoned today instead of being made to terminate).
        var _culpritLoc = _co ? ((_co.file || "?") + ":" + _co.line + ":" + _co.col) : null;
        // Capture the overflow stack TOP (the deepest, repeating frames name the
        // recursion cycle) — distinguishes a worker-JS re-drive recursion (HOSTDRIVER
        // frames) from a wasm/QuickJS one, deciding WHERE the fix goes.
        self._whyRecords.push({ phase: "deep_callmain_throw", step: _deepStats.steps, err: String(e && e.message || e), drivenN: _driven.size, culprit: _culpritId || "(unknown)", culpritLoc: _culpritLoc, stack: String((e && e.stack) || "").slice(0, 1100) });
        // A culprit that throws the SAME way on every re-drive is provably
        // unhelpable (forced exec is deterministic) — count repeats so the recycle
        // below can abandon it (mark driven) rather than re-drive it forever.
        // Terminate-on-provable-pointlessness, NOT a cap: a one-off trap (count 1)
        // still recycles and retries; only a deterministic RE-throw is abandoned.
        if (_culpritId) { if (!self._deepThrowCounts) self._deepThrowCounts = {}; self._deepThrowCounts[_culpritId] = (self._deepThrowCounts[_culpritId] || 0) + 1; }
      }
      // Final drain for any lines after the last JSPI yield (e.g. the
      // closing @DS the engine emits before returning from main).
      await _drainDeepStdout();
      /* Head phase returned cleanly (not a throw/abort): the net-reaching HEAD
         is driven (endpoints surfaced). Transition to the TAIL — continue the
         SAME instance with --fe-deep-grind (full residue; driven-set skips the
         head). This is NOT a recycle and NOT done: rem>0 is EXPECTED here (the
         tail remains), so bypass the no-progress/recycle logic below. The
         scheduler (increment 3) will later interleave OTHER pages' heads here
         before this tail; for now the same page proceeds head→tail. */
      if (_headPhase && !_dpCallThrew && !instAborted) {
        _headPhase = false;
        /* HEAD done = the page's endpoint-surfacing work is finished; only the
           low-value completeness tail remains. Drop this flow's strict
           active-focus override (flowCmp reads ctx.headDone) so its tail
           competes by plain WFQ — a freshly-opened background tab's high-VOI
           head can now preempt this tail instead of waiting for it. */
        huntContext.headDone = true;
        if (_drem === 0) { _grindDone = true; break; }   // head WAS the whole residue (small page)
        continue;   // run the tail on the same instance
      }
      var _dpRecycleReason = null;
      if (instAborted) _dpRecycleReason = "abort";
      else if (_dpCallThrew) { _dpRecycleReason = "wasm_trap"; _headPhase = false; }
      else if (_drem > 0) {
        /* callMain returned cleanly but residue still has uncovered fns —
           memory recycle proactively if we crossed the 2× baseline threshold
           (same monotonic-memory ratchet as the BFS schedule loop). */
        var _dpMemNow = wasmBytes(m);
        if (_dpBaselineBytes === 0) _dpBaselineBytes = _dpMemNow;
        else if (_dpMemNow > _dpBaselineBytes * 2) _dpRecycleReason = "mem_growth";
        else _dpRecycleReason = "callmain_returned_with_residue";   // shouldn't happen on the grind path
      }
      if (_drem === 0) { _grindDone = true; break; }
      if (_dpRecycleReason) {
        if (!self._whyRecords) self._whyRecords = [];
        self._whyRecords.push({ phase: "deep_recycle", reason: _dpRecycleReason, step: _deepStats.steps, rem: _drem });
        /* No-progress recycle break — MADE to terminate (CLAUDE.md), not spun. A
           recycle loop where driven hasn't advanced across many recycles is a
           provably-pointless spin: a SYSTEMIC trap (e.g. an async-frame corruption
           hitting a VARYING orphan each re-drive) the per-culprit abandon below
           can't catch. Stop the grind so the head's endpoints persist instead of
           looping forever; the underlying trap stays surfaced in _whyRecords. */
        if (_driven.size === _lastRecycleDriven) _noProgRecycles++;
        else { _noProgRecycles = 0; _lastRecycleDriven = _driven.size; }
        if (_noProgRecycles >= 12) {
          self._whyRecords.push({ phase: "deep_recycle_noprogress_stop", recycles: _noProgRecycles, driven: _driven.size, rem: _drem, step: _deepStats.steps });
          _deepStats.stop = "recycle-noprogress";
          break;
        }
        m = null;
        await new Promise(function (r) { setTimeout(r, 0); });
        m = await freshInstance();
        huntContext.onYieldDrain = _drainDeepStdout;
        /* Abandon a culprit that has thrown identically ≥2× — re-driving it only
           re-throws (forced exec is deterministic), which is what wedges the grind
           in a recycle loop (esm.sh: one orphan overflowed 1231×, driven flat).
           Mark it driven so the fresh instance advances to the remaining residue;
           its OWN endpoints stay forfeit until the recursion-collapse fixpoint
           bounds the overflow (the deeper root). Recorded, never silent. */
        if (_dpCallThrew && self._currentOrphan && self._currentOrphan.id &&
            self._deepThrowCounts && self._deepThrowCounts[self._currentOrphan.id] >= 2 &&
            !_driven.has(self._currentOrphan.id)) {
          var _ab = self._currentOrphan.id;
          var _abLoc = (self._currentOrphan.file || "?") + ":" + self._currentOrphan.line + ":" + self._currentOrphan.col;
          _driven.add(_ab);
          self._whyRecords.push({ phase: "deep_orphan_abandoned", culprit: _ab, loc: _abLoc, throws: self._deepThrowCounts[_ab], step: _deepStats.steps });
        }
        if (_driven.size) {
          try { m.FS.writeFile("/driven", Array.from(_driven).join("\n")); }
          catch (e) {
            if (!self._whyRecords) self._whyRecords = [];
            self._whyRecords.push({ phase: "driven_file_recycle_throw", n: _driven.size, step: _deepStats.steps, err: String(e && e.message || e) });
          }
        }
        _dpBaselineBytes = 0;
        instAborted = false;
      } else {
        /* No recycle reason and residue not zero — engine returned without
           making progress. Surface so a stuck callMain (e.g. host stub
           hanging the grind) is visible, then exit so we don't spin. */
        if (!self._whyRecords) self._whyRecords = [];
        self._whyRecords.push({ phase: "deep_callmain_no_progress", drem: _drem, drivenN: _driven.size });
        _deepStats.stop = "no-progress";
        break;
      }
    }
    // FINAL drain after the grind loop. The reentrancy guard runs one async
    // drain at a time and DROPS a request that arrives while a prior drain is
    // mid-IDB-await. Mid-grind a later yield re-drains the dropped lines, but
    // the FINAL drain — a SHORT grind converging while a drain is still awaiting
    // IDB — has no later yield, so it is dropped and the tail's @H is lost (the
    // cold-ctor arrow re-drive's concrete endpoint vanished this way). An async
    // drain runs its for-loop + processStdout SYNCHRONOUSLY at invocation and
    // only awaits AFTER (the IDB put), so an in-flight drain has already
    // advanced _stdoutCursor past its own lines — a direct synchronous pass
    // over the unread tail here cannot race the cursor and guarantees the last
    // @H is learned before the result is built.
    if (_stdoutCursor < stdout.length) { processStdout(_stdoutCursor); _stdoutCursor = stdout.length; }
    await _drainDeepStdout();
    huntContext.onYieldDrain = null;
    try { await m.callMain(["--fe-deep-end"]); }
    catch (e) {
      /* Final --fe-deep-end on a recycled/aborted instance — the engine
         call to release the deep-cache (qjs_deep_free) can throw if the
         wasm was poisoned. wasm GC reclaims everything on instance
         disposal, so functionally fine, but a throw on a HEALTHY instance
         is a host-model gap worth seeing. */
      if (!self._whyRecords) self._whyRecords = [];
      self._whyRecords.push({ phase: "deep_end_throw", err: String(e && e.message || e), instAborted: !!instAborted });
    }
    // Finalize the remaining count at grind exit (the final drain above fully
    // populated _driven). Without this, a grind that RAN but never hit the
    // `@DS rem===0` break — e.g. a page whose whole residue was driven via the
    // head/value-spread, or whose @DTOTAL was known but the closing @DS lost —
    // leaves _deepStats.rem at the -1 init, so the heartbeat reports learnstate
    // "unknown" (the PRE-grind sentinel) forever instead of the honest
    // "complete"/"stalled". rem is now total−driven (or 0 with total=driven when
    // the grind completed with no @DTOTAL), so "missing vs still-looking" stays
    // truthful. Mirrors the live update at the per-drain rem computation.
    if (_deepStats.total >= 0) _deepStats.rem = Math.max(0, _deepStats.total - _driven.size);
    else if (_grindDone) { _deepStats.rem = 0; _deepStats.total = _driven.size; }
    if (_grindDone) {
      try { await _idbDel("prog", _dkey); await _idbDel("code", _dkey); }
      catch (e) {
        /* Cleanup-on-completion IDB delete failed — the resume state stays
           in IDB, so on next session the worker will REPLAY a completed
           grind (re-driving every function). Functionally idle — the
           driven-SET prevents re-work — but wastes a wasm boot. */
        if (!self._whyRecords) self._whyRecords = [];
        self._whyRecords.push({ phase: "idb_del_done_throw", key: _dkey, err: String(e && e.message || e) });
      }
      try { var _tc = Array.from(_smChunksTouched); for (var _tci = 0; _tci < _tc.length; _tci++) await _idbDel("smaps", _tc[_tci]); }
      catch (e) {
        if (!self._whyRecords) self._whyRecords = [];
        self._whyRecords.push({ phase: "idb_del_smaps_throw", n: _smChunksTouched.size, err: String(e && e.message || e) });
      }
    }
    } finally {
      self._deepGrindRunning.delete(_dkey);
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
      // Binary body bytes + magic-byte-sniffed protocol guess. Present
      // only when the bundle sent an ArrayBuffer/TypedArray body. The
      // brain runs the protocol-specific decoder (lib/protobuf.js for
      // protobuf/grpc-web) to extract per-field values from the bytes.
      bodyBinary: mm.bodyBinary || null,
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

  _deepStats.deepMs = (Date.now() - t0) - _bfsMs;   // deep-grind phase (total minus the BFS phase)
  /* Observability: expose the discovered chunk/script URLs (incl. the SSR
     <script src> now emitted by hostedge.js) so the harness can see whether
     a missing endpoint's defining script was DISCOVERED — the first layer of
     the chunk pipeline (discover → safeFetch → re-run → define → upgrade). */
  self._lastChunkUrls = chunkUrls ? Array.from(chunkUrls) : [];
  self._lastScriptSrcs = scriptSrcUrls;
  return {
    fetchCallSites: fetchCallSites,
    scriptSrcUrls: scriptSrcUrls,
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
    esmImportUrls: esmImportUrls ? Array.from(esmImportUrls) : [],
    sourceUrl: sourceUrl || "",
    sourceMapUrl: sourceMapUrlOf(code),
    _timings: { ms: Date.now() - t0, runs: runs },
    _deepStats: _deepStats,
  };
  }
  var _fr = _buildResult();
  try { await _resolveSmNames(_fr.fetchCallSites, scriptOffsets, sourceMapScripts, sourceUrl); }
  catch (e) {
    /* Final source-map name resolution failed — surface the diagnostic so a
       parse error (malformed map JSON, missing sourcesContent, traceMapping
       version skew) is visible. Without this the popup shows minified path
       params (e/a) and the reviewer can't tell whether they're genuinely
       unresolvable (no map shipped) or a fixable resolver gap. */
    if (!self._whyRecords) self._whyRecords = [];
    self._whyRecords.push({ phase: "resolve_sm_names_throw", err: String(e && e.message || e), nFcs: (_fr.fetchCallSites || []).length });
  }
  return _fr;
}


onmessage = function (e) {
  var id = e.data._id;
  var msg = e.data.msg;

  function done(response) { postMessage({ _id: id, response: response }); }

  if (msg.type === "SET_ANALYSIS_OPTS") {
    /* Cooling + pool-size knobs from the popup. yieldThrottleMs caps the
       schedule-loop's per-run sleep (the existing self._throttleCapMs;
       0 = no throttle = max throughput). maxWorkers is a pool-shape
       knob owned by ast-worker.js (the dispatcher) — workers don't know
       their own pool size, so we just record it on self for telemetry. */
    var opts = msg.opts || {};
    if (typeof opts.yieldThrottleMs === "number" && opts.yieldThrottleMs >= 0) {
      self._throttleCapMs = opts.yieldThrottleMs | 0;
    }
    if (typeof opts.maxWorkers === "number") {
      self._poolMaxWorkers = opts.maxWorkers | 0;
    }
    if (typeof opts.grindCap === "number" && opts.grindCap > 0) {
      // WFQ admission cap: max concurrent grind wasm instances (memory vs
      // multi-tab breadth). CPU among them is fair-shared by flowCmp's
      // virtual-time tier regardless of this value.
      self._grindCap = opts.grindCap | 0;
    }
    return;   // no _id reply expected (broadcast from dispatcher)
  }

  if (msg.type === "AST_ANALYZE") {
    // A live review IS a focus signal — the brain dispatches this on
    // page-loaded for the active tab, so the page becoming reviewable
    // IS the page the user is now on. Update _activePageKey so the
    // cross-page priority scheduler's first lexicographic dimension
    // resumes this page's fibers preferentially over background deep
    // grinds for older pages. The URL identity is browser-verified
    // (the brain's NAV handler set it from chrome.webNavigation).
    if (msg.sourceUrl) self._activePageKey = String(msg.sourceUrl).split("#")[0];
    _reviewQ.push({ id: id, msg: msg });
    // No need for a JS-side preempt flag: a new review's fiber competes in
    // the suspended-fiber queue, and flowCmp's active-page-focus dimension
    // wins over background-page grinds at the next per-orphan JSPI yield.
    // The IDB-resume launcher (_resumeIncompleteDeep) already skips keys
    // whose grind fiber is in flight (_deepGrindRunning), so a new review
    // here doesn't re-kick its own grind on top of the running one.
    _pump();
    return;
  }

  if (msg.type === "GET_SCHED_TIMINGS") {
    done({ success: true, result: self._schedTimings || [] });
    return;
  }

  if (msg.type === "GET_FIBER_STATE") {
    /* Read-only snapshot of the scheduler state so the harness can verify
       that paused fibers are actually being picked by priority, not
       resumed FIFO. The fiber-queue is the cross-page priority queue. */
    var fq = (self._fiberQ || []).map(function (e) {
      return {
        pageKey: e.ctx && e.ctx.pageKey,
        type: e.ctx && e.ctx.type,
        vts: e.ctx && e.ctx.vts,
        reaches: e.reaches,
        recentEmissions: e.recentEmissions,
        ts: e.ts,
        // WFQ observability: the virtual time + weight inputs the fair-share
        // pick is based on, so the harness can confirm CPU is split by weight.
        flowVt: e.ctx && e.ctx.flowVt,
        epRate: e.ctx && e.ctx.epRate,
        weight: self._priorityCmp.flowWeight(e),
      };
    });
    // Per-flow cumulative resume tally (CPU actually granted) so fairness is
    // verifiable as resumes-proportional-to-weight, not just queue state.
    done({ success: true, result: {
      activePageKey: self._activePageKey || null,
      fiberQ: fq,
      totalSuspended: fq.length,
      sysVt: self._sysVt || 0,
      resumeCount: self._resumeCount || 0,
      resumesByFlow: self._resumesByFlow || {},
    } });
    return;
  }

  if (msg.type === "DEEP_FOCUS") {
    // Tab switched to an already-loaded page: bump that page's visit recency
    // (vts) so _resumeIncompleteDeep's recency-ordered round puts its incomplete
    // grind FIRST next round — relevance follows the tab you're actually viewing,
    // not only the last one navigated. Resume is by driven-SET (ids), so the
    // reorder never re-drives or skips. Match on the code record's sourceUrl.
    // Also set _activePageKey so the cross-page priority scheduler's FIRST
    // lexicographic dimension (active-page-focus) preempts all other flows
    // for the currently-viewed page — JSPI yields between the focused page's
    // wasm and any others give the focused page every resume until it yields.
    self._activePageKey = msg.pageUrl ? String(msg.pageUrl).split("#")[0] : null;
    done({ success: true });
    (async function () {
      try {
        var keys = await _idbAllKeys("code");
        for (var i = 0; i < keys.length; i++) {
          var cr = await _idbGet("code", keys[i]);
          if (cr && cr.sourceUrl && String(cr.sourceUrl).split("#")[0] === msg.pageUrl) {
            var pr = await _idbGet("prog", keys[i]);
            if (pr) { pr.vts = Date.now(); await _idbPut("prog", pr); }
            break;
          }
        }
      } catch (e) {
        /* DEEP_FOCUS handler — bumping `vts` for the focused page's resumable
           grind so the cross-page rotation picks it next. An IDB failure here
           means the focused page DOESN'T jump the rotation queue, so the
           reviewer sees the WRONG page being ground when they tab-switch.
           Diagnose so a quota / lock condition is visible. */
        if (!self._whyRecords) self._whyRecords = [];
        self._whyRecords.push({ phase: "deep_focus_vts_throw", pageUrl: msg.pageUrl, err: String(e && e.message || e) });
      }
      _pump();   // idle → pick up the now-leading focused grind
    })();
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
  } else if (msg.type === "GET_LAST_STDERR") {
    response = { success: true, result: { lines: (self._lastStderr || []).slice(-200), why: self._whyRecords || [] } };
  } else {
    response = { success: false, error: "Unknown message type: " + msg.type };
  }
  done(response);
};

// ── Preemptible JSPI-fiber scheduler ─────────────────────────────────────
// One worker thread runs wasm at a time, but MULTIPLE wasm fibers can be
// suspended concurrently — each forcedAnalyze's qjs_host_yield enqueues an
// entry in self._fiberQ. priority.js's flowCmp picks the next paused fiber
// to resume on each macrotask turn: active-page-focus > reaches host edge >
// recent emissions > visit recency > anti-starvation ts. A new live review
// arrives → _reviewQ.push + _pump kicks the review's forcedAnalyze; its
// fiber competes in _fiberQ with whatever deep grinds are already paused
// there, and flowCmp's active-page focus dimension picks the review next
// at every JSPI yield of the running fiber. No JS-side preempt flag, no
// batch loop — preemption is the comparator's job.
var _reviewQ = [];          // pending high-priority reviews

/* Fiber yield drain. The wasm's JSPI suspension creates a Promise per
   yield; this hook resolves it after the worker's MACROTASK queue gets
   a turn — i.e., after pending message events (new AST_ANALYZE, brain
   RPC replies) dispatch. queueMicrotask was wrong
   (the wasm's resume queues another microtask immediately, the worker's
   message queue never dispatches); setTimeout(0) was wrong (clamps to
   1-4ms per yield → 100%+ overhead at 1000 yields/sec). MessageChannel
   posts a real macrotask without the timeout clamp, ~10us each — the
   worker's message queue gets a turn between every wasm yield and
   resume, while overhead stays acceptable. The orchestrator layer can
   replace this with a priority-aware version that DELAYS resume for
   lower-priority fibers. */
/* Cross-page paused-fiber comparator lives in lib/priority.js — see that file
   for the lexicographic dimensions. This module-level binding stays so call
   sites here read `_flowCmp(a, b)` unchanged; the activePageKey closure is
   provided once per call. */
self._activePageKey = null;   // updated by DEEP_FOCUS / NAV: the user's current page wins all ties
function _flowCmp(a, b) { return self._priorityCmp.flowCmp(a, b, self._activePageKey); }
var _yieldMC = new MessageChannel();
_yieldMC.port1.onmessage = function () {
  /* Resolve the SINGLE highest-priority paused fiber per macrotask turn.
     Others stay suspended (their wasm stacks preserved by JSPI). When
     the resumed fiber yields next, we run the comparator again — at
     that point the resumed fiber's recentEmissions / reaches values
     have shifted, possibly changing the winner. */
  /* Delegate pick to priority.js — same comparator as inline before, but
     now the ORDER decision lives in the dedicated picker file. The fiber's
     `wake` is whatever resume mechanism the entry was created with (Promise
     resolve here; would be a postMessage in a multi-worker setup). */
  var best = self._priorityCmp.pickFromFiberQueue(self._fiberQ, self._activePageKey);
  if (!best) return;
  self._resumeCount = (self._resumeCount || 0) + 1;   // liveness: # of fiber resumes (forks "not resumed" vs "resumed-but-not-driving")
  if (best.ctx) {
    best.ctx.lastResumed = Date.now();
    // WFQ system virtual time = the virtual time of the flow now being served.
    // A flow created LATER inits its flowVt to this (huntContext), so a newly-
    // opened tab joins at "now" in virtual time and can't hoard CPU catching
    // up from virtual time it was never present for (SFQ's max-with-V rule).
    if (typeof best.ctx.flowVt === "number") self._sysVt = best.ctx.flowVt;
    // Per-flow CPU-grant tally (observability: verify resumes ∝ weight).
    if (!self._resumesByFlow) self._resumesByFlow = {};
    var _pk = best.ctx.pageKey || "default";
    self._resumesByFlow[_pk] = (self._resumesByFlow[_pk] || 0) + 1;
  }
  best.resolve();
  /* If MORE fibers are still queued, schedule another drain — the
     just-resumed one will run until ITS next yield (op-poll heartbeat or
     per-orphan), which gives the comparator new state. */
  if (self._fiberQ.length > 0) _yieldMC.port2.postMessage(null);
};
self._yieldDrain = function (entry) {
  /* Attach the most recent engine signals to this entry. The engine
     emits @Y reaches=<bit> JUST BEFORE the JS-side yield call — the
     per-instance stdout collector parses that line and stashes the
     reachability bit on the instance's last-yield context, which we
     read here. recentEndpoints / recentSecondary are computed by
     comparing the instance's per-class emission counts to the values
     at last yield, so flowCmp can prioritize endpoint-producing
     fibers (goal #1) ahead of sink-producing ones (goal #10) while
     keeping recentEmissions for any legacy consumer. */
  if (entry.ctx) {
    entry.reaches = entry.ctx.lastReaches ? 1 : 0;
    var nowEp = (entry.ctx.totalEndpoints | 0);
    var nowSec = (entry.ctx.totalSecondary | 0);
    var nowE = (entry.ctx.totalEmissions | 0);
    entry.recentEndpoints = nowEp - (entry.ctx.endpointsAtLastYield | 0);
    entry.recentSecondary = nowSec - (entry.ctx.secondaryAtLastYield | 0);
    entry.recentEmissions = nowE - (entry.ctx.emissionsAtLastYield | 0);
    entry.ctx.endpointsAtLastYield = nowEp;
    entry.ctx.secondaryAtLastYield = nowSec;
    entry.ctx.emissionsAtLastYield = nowE;
    /* WFQ accounting for the slice that just ran (resume → this yield).
       epRate = EWMA of @H endpoints produced per resume slice — the
       marginal-value signal that feeds flowWeight. Decay 0.75 keeps ~4
       slices of memory so a page that just went flat decays toward the floor
       within a few yields (CPU then flows to a fresher page), while a steadily
       productive page holds a high rate. Then advance the flow's virtual time
       by sliceCost / weight: a higher-weight (more productive / host-edge-
       reaching) flow advances SLOWER ⇒ is picked more ⇒ gets proportionally
       more CPU. sliceCost is wall-time so the share is CPU-time-proportional,
       not merely resume-count-proportional. */
    entry.ctx.epRate = entry.ctx.epRate * 0.75 + entry.recentEndpoints * 0.25;
    /* UCB1 explore bonus = √(2·ln N / nᵢ): N = total resumes across all flows,
       nᵢ = this flow's resumes. Large when this flow is under-sampled (a fresh
       tab nᵢ≈0 ⇒ bonus ≈ √(2 ln N), the highest-VOI thing to run), shrinks as
       it is sampled ⇒ weight converges to the observed epRate. Recomputed each
       yield so a never-yet-picked new flow (nᵢ=0) gets the burst on its FIRST
       yield and isn't starved waiting to be picked. ln is floored at 1 (N<e)
       so the bonus is finite and positive from the first resume. */
    var _ni = (self._resumesByFlow && self._resumesByFlow[entry.ctx.pageKey]) || 0;
    var _ntot = self._resumeCount || 1;
    entry.ctx.exploreBonus = Math.sqrt(2 * Math.max(1, Math.log(_ntot)) / Math.max(1, _ni));
    /* Per-flow scheduler telemetry (observability): record each flow's explore
       bonus at its FIRST yield (smallest nᵢ ⇒ the burst) and the peak, so the
       UCB explore burst for a fresh background tab is verifiable after the fact
       (the fiber spends most of its life RUNNING, rarely caught in _fiberQ at a
       probe instant). Order-only data, never affects coverage. */
    if (!self._flowSeen) self._flowSeen = {};
    var _fk = entry.ctx.pageKey || "default";
    if (!self._flowSeen[_fk]) self._flowSeen[_fk] = { firstExplore: Math.round(entry.ctx.exploreBonus * 100) / 100, firstNtot: _ntot, maxExplore: entry.ctx.exploreBonus };
    var _fs = self._flowSeen[_fk];
    if (entry.ctx.exploreBonus > _fs.maxExplore) _fs.maxExplore = entry.ctx.exploreBonus;
    var _sliceCost = Date.now() - (entry.ctx.lastResumed || Date.now());
    if (_sliceCost < 0) _sliceCost = 0;
    var _w = self._priorityCmp.flowWeight(entry);
    entry.ctx.flowVt = (typeof entry.ctx.flowVt === "number" ? entry.ctx.flowVt : (self._sysVt || 0)) + _sliceCost / _w;
    /* Cumulative REAL CPU time (ms) granted to each flow — the true fairness
       metric (resume COUNT misleads: a small bundle does many cheap yields, a
       big one fewer heavy slices). With WFQ, two flows backlogged at once
       should accrue cpuMs in proportion to their weights. */
    if (!self._cpuMsByFlow) self._cpuMsByFlow = {};
    var _fpk = entry.ctx.pageKey || "default";
    self._cpuMsByFlow[_fpk] = (self._cpuMsByFlow[_fpk] || 0) + _sliceCost;
  } else {
    entry.reaches = 1;   // unknown context → assume reachable so it gets a fair turn
    entry.recentEndpoints = 0;
    entry.recentSecondary = 0;
    entry.recentEmissions = 0;
  }
  if (self._fiberQ.length === 1) _yieldMC.port2.postMessage(null);
};

/* PER-PAGE WASM, prioritized-hunting scheduler. Each page gets its own
   wasm instance (forcedAnalyze's freshInstance) with isolated memory +
   JSRuntime — pages NEVER share state. Each instance's callMain is a
   JSPI fiber; when it calls qjs_host_yield the wasm stack is preserved
   intact. The scheduler maintains a queue of suspended fibers across
   ALL pages and resolves the HIGHEST-PRIORITY one's yield Promise next
   — not the most-recent yield, not in arrival order. Concurrent flows
   on the same wasm thread interleave at JSPI yield boundaries, not at
   callMain boundaries.

   Removed: _busy gate. Multiple forcedAnalyze can be in flight, one per
   page; each has its own instance + own callMain Promise. The yield
   queue (self._fiberQ) is GLOBAL — shared across pages. The drain hook
   picks across all pages by priority. */
function _pump() {
  // Live reviews start AS SOON AS they arrive — no gate.
  while (_reviewQ.length) {
    var t = _reviewQ.shift();
    _runReview(t.id, t.msg);
  }
  // Kick the IDB-resume scan on the next macrotask. _resumeIncompleteDeep
  // is re-entrant via the _deepGrindRunning set: keys whose fiber is
  // already in flight are skipped, so launching from every _pump tick
  // never double-spawns.
  setTimeout(_resumeIncompleteDeep, 0);
}

function _runReview(id, msg) {
  function fin(resp) { postMessage({ _id: id, response: resp }); _pump(); }
  // The review's deep grind runs ONE callMain that drives every remaining
  // orphan; the engine JSPI-yields per orphan so the host scheduler can
  // rotate to another page's grind via priority.js's flowCmp at every
  // orphan boundary. No `maxBatches` slice, no JS-side batch loop — the
  // active-page focus tier in flowCmp keeps this review's fiber winning
  // ties until it pauses on its own JSPI yield, then another page's
  // grind can advance one orphan before the active-page fiber gets the
  // next pick.
  forcedAnalyze(String(msg.code || ""), msg.sourceUrl || "", msg.scriptUrls || null,
    typeof msg.pageHtml === "string" ? msg.pageHtml : null, !!msg.seedOnly, !!msg.deep,
    undefined, undefined, undefined, msg.scriptOffsets || null, msg.sourceMapScripts || null, msg.scriptSources || null)
    .then(function (result) { fin({ success: true, result: result }); })
    .catch(function (err) { fin({ success: false, error: (err && err.message) || String(err), stack: err && err.stack }); });
}

async function _resumeIncompleteDeep() {
  /* Default: ONE deep grind running at a time. Multiple concurrent grinds
     would mean N concurrent wasm instances (each holds its bundle's
     ~hundreds-of-MB linear memory + Z3 + Lexbor + JSRuntime), which on a
     multi-page session blows the MV3 worker's memory ceiling. The cross-
     page priority signal still works: switching tabs bumps that page's
     vts via DEEP_FOCUS → the NEXT grind launch (after the current one
     exits) picks the most-recent-vts incomplete grind. A live REVIEW for
     a newly-visited page spawns its own short-lived wasm instance and
     runs CONCURRENTLY with whatever grind is in flight (2 instances at
     most: 1 review + 1 grind), and the JSPI fiber-queue interleaves them
     per orphan via flowCmp's active-page focus tier. Parallel grinds
     across pages are the UI option (cooling vs. throughput) — not the
     default. */
  if (!self._deepGrindRunning) self._deepGrindRunning = new Set();
  /* ADMISSION (WFQ part 1 — memory bound): up to _grindCap concurrent grind
     instances may be live at once (each wasm instance holds its bundle's
     ~hundreds-of-MB linear memory + Z3 + Lexbor + JSRuntime, so this caps
     peak memory). The set contains BOTH live-review grinds (registered in
     forcedAnalyze's deep block) and resume grinds, so the cap bounds the
     TOTAL — and a resume never duplicates a key already being live-ground.
     CPU among the admitted instances is then allocated by WEIGHTED FAIR
     QUEUEING at the JSPI yield boundary (flowCmp's virtual-time tier), so a
     flat page still gets a proportional slice instead of only anti-starvation
     crumbs. Default 2 (active + 1 background); user-growable via
     SET_ANALYSIS_OPTS.grindCap for more multi-tab breadth (more memory). */
  var _grindCap = (typeof self._grindCap === "number" && self._grindCap > 0) ? self._grindCap : 2;
  if (self._deepGrindRunning.size >= _grindCap) return;
  var keys;
  try { keys = await _idbAllKeys("prog"); }
  catch (e) {
    if (!self._whyRecords) self._whyRecords = [];
    self._whyRecords.push({ phase: "deep_resume_idb_keys_throw", err: String(e && e.message || e) });
    return;
  }
  if (!keys || !keys.length) return;
  var withTs = [];
  for (var ki = 0; ki < keys.length; ki++) {
    var pr = null;
    try { pr = await _idbGet("prog", keys[ki]); }
    catch (e) {
      if (!self._whyRecords) self._whyRecords = [];
      self._whyRecords.push({ phase: "deep_resume_idb_read_throw", key: keys[ki], err: String(e && e.message || e) });
      continue;
    }
    if (pr) withTs.push({ k: keys[ki], vts: pr.vts || pr.ts || 0 });
  }
  if (!withTs.length) return;
  withTs.sort(self._priorityCmp.deepRoundCmp);   // most-recently-visited first
  // Pick the highest-priority key NOT already running (concurrent admission:
  // the top key may already be in flight — live or resume — so scan for the
  // best not-yet-running one to fill the next cap slot).
  var key = null;
  for (var _wi = 0; _wi < withTs.length; _wi++) {
    if (!self._deepGrindRunning.has(withTs[_wi].k)) { key = withTs[_wi].k; break; }
  }
  if (key === null) return;   // every incomplete grind is already running
  var prog = null, codeRec = null;
  try { prog = await _idbGet("prog", key); codeRec = await _idbGet("code", key); }
  catch (e) {
    if (!self._whyRecords) self._whyRecords = [];
    self._whyRecords.push({ phase: "deep_resume_idb_read_throw", key: key, err: String(e && e.message || e) });
    return;
  }
  if (!prog || !codeRec || !codeRec.code) {
    try { await _idbDel("prog", key); await _idbDel("code", key); }
    catch (e) {
      if (!self._whyRecords) self._whyRecords = [];
      self._whyRecords.push({ phase: "deep_resume_idb_cleanup_throw", key: key, err: String(e && e.message || e) });
    }
    setTimeout(_resumeIncompleteDeep, 0);   // try the next-most-recent key
    return;
  }
  self._deepGrindRunning.add(key);
  /* Concurrent fill: this grind suspends at its first JSPI yield, so kick
     another launch NOW to admit the next page's grind up to _grindCap — they
     then run as concurrent fibers fair-shared by flowCmp's WFQ virtual-time
     tier, instead of this one having to finish first. The cap check + not-
     running scan make the kick idempotent (no-ops when the cap is full). */
  setTimeout(_resumeIncompleteDeep, 0);
  try {
    var result = await forcedAnalyze(codeRec.code, codeRec.sourceUrl || "", codeRec.scriptUrls || null, codeRec.pageHtml || null, true, true, prog.cur || 0, prog.vts || prog.ts || 0, prog.driven || [], codeRec.scriptOffsets || null, codeRec.sourceMapScripts || null, codeRec.scriptSources || null);
    postMessage({ _resumed: true, sourceUrl: codeRec.sourceUrl || "", response: { success: true, result: result } });
  } catch (e) {
    /* The grind fiber threw — prog/code IDB records remain so the next
       _resumeIncompleteDeep tick retries. Surface so a stuck fiber is
       visible instead of being silently re-queued. */
    if (!self._whyRecords) self._whyRecords = [];
    self._whyRecords.push({ phase: "deep_resume_grind_throw", key: key, err: String(e && e.message || e) });
  } finally {
    self._deepGrindRunning.delete(key);
  }
  setTimeout(_resumeIncompleteDeep, 0);   // chain to the next-most-recent incomplete grind
}
// Initial IDB-resume kick on worker start: if a prior session left
// incomplete grinds, launch them immediately. Re-entrancy is guarded by
// _deepGrindRunning, so a NAV-triggered review that arrives concurrently
// just adds its own fiber to the queue rather than double-launching.
setTimeout(_resumeIncompleteDeep, 0);

// Liveness heartbeat (OBSERVABILITY — bounds/terminates nothing, not a
// watchdog). A timer-driven post, independent of the grind's per-yield
// drain, so the offscreen (ast-worker.js records it per slot, exposed via
// _poolLiveness for the harness `offscreen` command) can tell apart the two
// freeze classes that look identical from outside:
//   • heartbeat STOPS advancing (its ts goes stale) ⇒ the wasm thread is
//     parked inside ONE long synchronous C call (regex backtrack, Lexbor
//     parse, a huge --fe-emit-bc) — no qjs_host_yield can fire there, so the
//     whole event loop is blocked. THIS is the live freeze.
//   • heartbeat KEEPS advancing but lastGrindProgressTs is stale ⇒ event loop
//     alive, but the grind fiber isn't being resumed — a scheduling stall.
// Before this, the two were indistinguishable and a freeze could only be
// guessed at (three wrong guesses this session). The grind's own per-yield
// drain only emits while it's RUNNING, so it cannot report its own freeze;
// a separate timer can.
/* Honest learning-state verdict — the SINGLE signal the harness needs to tell
   "a finding is genuinely MISSING (driven to completion, never found)" from
   "still being LOOKED FOR (orphans remain / a fiber is mid-flight)". Without
   this, a network-vs-AST diff is unfalsifiable: a missing endpoint could be a
   real forced-exec gap OR just not-yet-reached, and the two are
   indistinguishable from rem/stop alone (rem>0 with stop="done" looked
   finished while 61k orphans were undriven). Classifies into:
     • "complete"  — rem===0: every orphan driven; a finding absent NOW is a
                      genuine gap (or correctly-not-an-endpoint, e.g. a
                      <include-fragment src> whose connectedCallback never
                      fetched — NOT an endpoint, by design).
     • "analyzing" — rem>0 AND (a grind is running OR a fiber is queued OR the
                      grind made progress within the staleness window): still
                      looking; absence is NOT yet a gap.
     • "stalled"   — rem>0 but nothing running/queued AND no progress for the
                      staleness window: the priority frontier failed to advance
                      (the prioritization bug class) — absence here is a
                      SCHEDULING failure to surface, distinct from a real gap.
   The distinction is the whole point: "missing" is only truthful at
   "complete"; at "analyzing" the harness must say "still looking", and
   "stalled" flags a prioritization defect, not a coverage gap. */
function _learningState() {
  // Delegate to the shared lib/learnstate.js classifier (DRY — the popup UI
  // uses the same one) so the harness verdict and the user-facing status never
  // drift on what "complete"/"analyzing"/"stalled" mean. This just gathers the
  // worker-local counters and hands them off.
  // Prefer the ACTIVE page's own grind counters over the global last-updated
  // ones (two grinds run concurrently at _grindCap=2), so the "missing vs
  // still-looking" verdict reflects the page the harness/popup is asking about.
  var d = (self._activePageKey && self._deepStatsByPage && self._deepStatsByPage[self._activePageKey]) || self._lastDeepStats || {};
  var progressTs = self._lastGrindProgressTs || 0;
  return self.LearnState.learningStateOf({
    rem: (typeof d.rem === "number") ? d.rem : -1,
    total: (typeof d.total === "number") ? d.total : -1,
    head: (typeof d.head === "number") ? d.head : -1,
    running: self._deepGrindRunning ? self._deepGrindRunning.size : 0,
    queued: (self._fiberQ || []).length,
    msSinceProgress: progressTs ? (Date.now() - progressTs) : -1
  });
}
self._learningState = _learningState;

setInterval(function () {
  try {
    var ls = _learningState();
    postMessage({ _heartbeat: true, ts: Date.now(),
      fiberQ: (self._fiberQ || []).length,
      grindRunning: (self._deepGrindRunning ? self._deepGrindRunning.size : 0),
      lastGrindProgressTs: self._lastGrindProgressTs || 0,
      resumeCount: self._resumeCount || 0,
      deepSteps: self._lastDeepStats ? self._lastDeepStats.steps : -1,
      deepRem: self._lastDeepStats ? self._lastDeepStats.rem : -1,
      deepTotal: self._lastDeepStats ? self._lastDeepStats.total : -1,
      learningState: ls.state, learningDriven: ls.driven, learningRem: ls.rem,   // honest "missing vs still-looking" verdict
      currentOrphan: self._currentOrphan || null,   // the orphan being driven; if unchanged while steps stalls = the non-terminating culprit
      activePageKey: self._activePageKey || null });
  } catch (e) {
    /* postMessage only throws while the worker is being torn down (the port
       is closing) — there is no live offscreen to surface a diagnostic to,
       and _whyRecords dies with the worker. Nothing to report. */
  }
}, 2000);
