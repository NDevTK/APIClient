/* bridge.js — the v2 HOST BRIDGE, the ONLY irreducible trusted-zone JS (SECURITY.md).
 *
 * Extracted OUT of offscreen-brain.js so the bridge and the (to-be-deleted) analysis logic are no
 * longer in one file. This is the platform edge the lexbor+quickjs engine CANNOT be, because the engine
 * is the UNTRUSTED WASM: it loads the engine WASM (dynamic import), drives the qjs_step protocol,
 * safe-fetches replies/chunks (the safeFetch chokepoint the untrusted bundle must never bypass),
 * persists the cross-session frontier to IndexedDB, and JSON.parses the engine's ONE @RESULT. It
 * installs self.astDispatch / self.driveFrontier for the offscreen to call. NO analysis LOGIC here —
 * the C engine owns identity/dedup/detection; this is only the network/IDB/WASM edges.
 */
/* The engine WASM factory is an ES module; this bridge lives in the CLASSIC offscreen-brain.js, so load
   it via dynamic import() (cached), NOT a static top-level import. Same-origin under the offscreen CSP. */
let _createQJSp = null;
function getCreateQJS() { return (_createQJSp || (_createQJSp = import("./lib/qjs/qjs.mjs").then((m) => m.default))); }

/* A URL with an opaque HOLE ("{}"/"{tag}") is not concretely fetchable (used to gate reply/chunk fetch).
   Endpoint IDENTITY (hole-normalization, shape/concrete collapse) is the ENGINE's now — the host's
   normHoles/SEG_HOLE/pathSegs/mergeCallsites/dedupShapeConcrete were DELETED. */
const HOLE = /\{[a-z]*\}/;
const hasHole = (s) => HOLE.test(s || "");

/* Map the engine's ONE structured `@RESULT <json>` line -> the analysis object the brain consumes.
   The ENGINE builds + DEDUPS the whole result (endpoints/params/headers/body, @S sinks, chunkUrls,
   errors, park recipes) and JSON.stringifies it; the host does ONE JSON.parse and relays — NO per-line
   @H/@Q/@HDR/@BODY parsing, NO host identity/dedup (all DELETED, the engine owns it like a browser).
   @E lines (host-side protocol errors) are still surfaced so a zero-result never fails silently. Every
   sibling field the brain reads unconditionally is present (empty) so it never throws. */
function linesToAnalysis(lines, msg) {
  let result = null;
  const extraErrors = [];
  for (const raw of lines) {
    const ln = String(raw);
    if (ln.startsWith("@RESULT ")) {
      try { result = JSON.parse(ln.slice(8)); }
      catch (e) { extraErrors.push({ context: "result-parse", message: String(e && e.message || e), snippet: null, replyExample: null }); }
    } else if (ln.startsWith("@E ")) {
      extraErrors.push({ context: "engine", message: ln.slice(3), snippet: null, replyExample: null });
    } else if (ln.startsWith("@WHY ")) {
      // engine diagnostic for a zero-result/resource path (e.g. reg_oom) — surface it so an OOM or
      // aborted flow never fails SILENTLY (CLAUDE.md: every zero-result path emits @WHY).
      try { const o = JSON.parse(ln.slice(5)); extraErrors.push({ context: o.phase || "why", message: o.reason || ln.slice(5), snippet: null, replyExample: null }); }
      catch (_) { extraErrors.push({ context: "why", message: ln.slice(5), snippet: null, replyExample: null }); }
    }
  }
  result = result || {};
  return {
    fetchCallSites: result.fetchCallSites || [],
    resolverErrors: (result.resolverErrors || []).concat(extraErrors),
    chunkUrls: result.chunkUrls || [],
    securitySinks: result.securitySinks || [],
    // sibling fields the brain reads unconditionally, present + empty so it never throws:
    protoEnums: [], protoFieldMaps: [], dangerousPatterns: [],
    esmImportUrls: [], inRunModuleUrls: [], domEndpoints: [],
    sourceMapTypes: [], sourceMapsByUrl: {}, traceMapsByUrl: {}, valueConstraints: [],
    sourceMapUrl: null, sourceMap: null, sourceUrl: (msg && msg.sourceUrl) || "",
    _orphans: result._orphans || 0, _emitDone: result._emit != null ? ("emit=" + result._emit) : "",
    _replyWant: [], _park: result._park || [],
  };
}

/* Run one page through a fresh v2 engine instance, capturing @H/@CHUNK stdout. The ENGINE parses the
   page HTML with its in-wasm Lexbor DOM and runs the document's scripts in order (against the real
   DOM) — the bridge no longer scrapes scripts. code (argv[1]) is any brain-assembled extra scripts
   (usually empty); html (argv[2]) is the page. */
function originOf(u) { try { return new URL(u).origin; } catch (_) { return ""; } }
/* Stable BUNDLE IDENTITY for the frontier key: the EXTERNAL <script src> set (content-hash filenames
   like main.abc123.js ARE the app version), NOT the volatile HTML wrapper (per-request nonces/CSRF
   tokens would change the key every visit -> the frontier would never resume). A redeploy changes a src
   -> new key -> stale frontier invalidated. Inline-only pages fall back to the HTML hash (rare; they're
   small, finish in one visit, never park). */
/* The document's bundle IDENTITY is computed by the ENGINE (qjs_bundle_id, a real Lexbor <script> scan) —
   NOT a host-side regex. The frontier key = origin | that id, is only a RELEVANCE grouping for which parked
   recipes to pull on resume (recipes self-identify by function SOURCE hash, so a coarse key is sound). */
/* Cross-session flow FRONTIER (IndexedDB): the learned surface (globalStore) already persists; this
   persists the UNFINISHED frontier as compact replay recipes, keyed by origin+bundle-hash (a changed
   bundle invalidates stale orphan indices). A parked frontier resumes next visit/session -> ONE
   continuous attention across sessions, extracting more breadth each time until fully explored. */
function idbOpen() {
  return new Promise((res, rej) => {
    const r = indexedDB.open("apiclient-frontier", 1);
    r.onupgradeneeded = () => { r.result.createObjectStore("frontier"); };
    r.onsuccess = () => res(r.result); r.onerror = () => rej(r.error);
  });
}
/* A frontier entry (the GLOBAL union spans all origins): { key: origin|hash, sourceUrl, html, code,
   recipes: "idx,dec;...", emit, visits, ts, credentialed }. Rehydration re-runs (html,code) + resumes
   recipes -- so a parked flow on ANY site can be advanced later, even when that page isn't open. */
async function frontierGet(key) {
  try { const db = await idbOpen(); return await new Promise((res) => { const t = db.transaction("frontier").objectStore("frontier").get(key); t.onsuccess = () => res(t.result || null); t.onerror = () => res(null); }); }
  catch (_) { return null; }
}
async function frontierPut(key, entry) {
  try { const db = await idbOpen(); await new Promise((res) => { const s = db.transaction("frontier", "readwrite").objectStore("frontier"); const t = (entry && entry.recipes) ? s.put(entry, key) : s.delete(key); t.onsuccess = () => res(); t.onerror = () => res(); }); }
  catch (_) {}
}
async function frontierAll() {
  try { const db = await idbOpen(); return await new Promise((res) => { const t = db.transaction("frontier").objectStore("frontier").getAll(); t.onsuccess = () => res(t.result || []); t.onerror = () => res([]); }); }
  catch (_) { return []; }
}
/* The ONE WFQ weight at the HOST level: a parked frontier's value-of-information = epRate (emits per
   visit) + an explore/fairness floor for under-visited frontiers. Same formula the C engine owns
   (flow_weight) -- one attention, two levels; the JS mirror lib/priority.js was DELETED. */
function frontierWeight(e) {
  const epRate = (e && e.visits) ? (e.emit || 0) / e.visits : (e && e.emit || 0);
  const exploreBonus = 1 / ((e && e.visits || 0) + 1);
  return 1 + epRate + exploreBonus;   // same WFQ formula the C engine owns (flow_weight); lib/priority.js DELETED
}
/* THE HOST-LEVEL GLOBAL WFQ ARBITER: advance the globally-highest-value parked frontier across ALL
   origins (rehydrate its bundle + resume its recipes for one quantum, re-park the residue), `rounds`
   times. This is the "single ATTENTION across all sites" -- a high-value parked flow on site A outranks
   a low-value fresh residue on site B by ONE value order. Returns the endpoints learned (for the brain
   to merge into the GLOBAL surface). */
async function driveFrontier(rounds, opts) {
  opts = opts || {};
  const advances = [];   // per-origin: { sourceUrl, result } so the brain merges each to its OWN origin
  for (let n = 0; n < (rounds || 1); n++) {
    const all = (await frontierAll()).filter((e) => e && e.recipes);
    if (!all.length) break;
    all.sort((a, b) => frontierWeight(b) - frontierWeight(a));   // ONE global value order
    const top = all[0];
    const m2 = { type: "AST_ANALYZE", pageHtml: top.html, code: top.code, sourceUrl: top.sourceUrl, quantum: opts.quantum || top.quantum || 8, credentialed: top.credentialed };
    const r = await runEngine(top.code || "", top.html || "", m2, m2.quantum);   // engine re-derives id, resumes recipes, DEDUPS
    advances.push({ sourceUrl: top.sourceUrl, result: r });
    top.emit = (top.emit || 0) + (r.fetchCallSites || []).length;
    top.visits = (top.visits || 0) + 1;
    top.recipes = (r._park || []).join(";");
    top.ts = Date.now();
    await frontierPut(top.key, top);   // empty recipes -> deleted (fully explored)
  }
  return advances;
}
self.driveFrontier = driveFrontier;   // the brain drives this on idle / after analysis; merges each per-origin
/* Drive ONE persistent engine instance through the step protocol. fromReply is now IN PLACE: when the
   engine parks awaiting a reply (qjs_step -> NEED_FETCH), the TRUSTED offscreen safe-fetches each pending
   url (GET, page-origin SSRF) and qjs_provide()s the body, resuming the flow in the SAME instance -- no
   re-instantiate, no re-run of the whole page. */
async function runEngine(code, html, msg, quantum) {
  const lines = [];
  let fkey = null, prior = null;   // frontier key (origin | engine bundle-id) + the parked entry, set in phase 1
  const createQJS = await getCreateQJS();
  const M = await createQJS({ print: (s) => lines.push(s), printErr: (s) => lines.push(s), noInitialRun: true });
  const cstr = (s) => { const n = M.lengthBytesUTF8(s || "") + 1; const p = M._malloc(n); M.stringToUTF8(s || "", p, n); return p; };
  const ptrs = [];
  const arg = (s) => { const p = cstr(s); ptrs.push(p); return p; };
  try {
    // PHASE 1 — parse + boot (Lexbor+quickjs): the engine runs the page's own scripts and computes the stable
    // bundle IDENTITY from its REAL Lexbor <script> scan (no host regex). argv[6] recipes empty; seeded below.
    M.ccall("qjs_init", "number", ["number", "number", "number", "number", "number", "number"],
      [arg(code || ""), arg(html || ""), arg(originOf(msg && msg.sourceUrl)), arg(""), quantum || 0, arg("")]);
    // The host's ONLY job here is the IDB bridge: look up this document's parked frontier by the ENGINE's
    // bundle id (origin | id), then hand the recipes back for phase-2 seeding.
    const _bid = (M.ccall("qjs_bundle_id", "number", [], []) >>> 0).toString(36);
    fkey = originOf(msg && msg.sourceUrl) + "|" + _bid;
    prior = quantum ? await frontierGet(fkey) : null;
    // PHASE 2 — seed the frontier (fresh visit, or resume the parked recipes) + fix the COW baseline.
    M.ccall("qjs_begin", "void", ["string"], [(prior && prior.recipes) ? prior.recipes : ""]);
    const canFetch = typeof self.safeFetch === "function" && msg && msg.sourceUrl;
    const fetched = async (u, asScript) => {   // safe-fetch a pending reply/chunk url -> body ("" if unavailable)
      if (!canFetch || hasHole(u)) return "";
      try {
        const abs = new URL(u, msg.sourceUrl).href;
        // chunks: as-script (CORB), never credentialed. replies: opt-in credentialed -> the AUTHENTICATED
        // logged-in reply (the moat headline), gated by safeFetch's own SOP/CORS + GET-only. Default off.
        const opts = asScript ? { pageUrl: msg.sourceUrl, as: "script" }
                              : { pageUrl: msg.sourceUrl, credentialed: !!(msg && msg.credentialed) };
        const r = await self.safeFetch(abs, opts);
        return (r && r.ok && typeof r.body === "string") ? r.body : "";
      } catch (_) { return ""; }
    };
    // NO step-cap: qjs_step returns 1 only while genuine fetch/chunk work is pending, and the engine's
    // work-based QUANTUM parks the residue under resource pressure (a single deep await-loop yields too),
    // so the loop terminates by PROGRESS + park, never a counter. A hang here = a quantum bug to root-fix.
    while (M.ccall("qjs_step", "number", [], []) === 1) {
      const replies = String(M.ccall("qjs_pending", "string", [], [])).split("\n").filter(Boolean);
      for (const u of replies) M.ccall("qjs_provide", "void", ["string", "string"], [u, await fetched(u, false)]);   // reply body -> resume in place
      const chunks = String(M.ccall("qjs_chunks", "string", [], [])).split("\n").filter(Boolean);
      for (const u of chunks) M.ccall("qjs_provide", "void", ["string", "string"], [u, await fetched(u, true)]);      // chunk JS -> eval in place
    }
    M.ccall("qjs_teardown", "void", [], []);
  } catch (e) {
    lines.push('@E {"phase":"protocol","err":' + JSON.stringify(String(e && e.message || e)) + "}");
  } finally {
    for (const p of ptrs) { try { M._free(p); } catch (_) {} }
  }
  const _result = linesToAnalysis(lines, msg);
  _result._fkey = fkey; _result._prior = prior;   // engine-computed frontier key + parked entry -> astDispatch persists
  return _result;
}

/* Endpoint IDENTITY (exact dedup by method+hole-normalized-url + shape/concrete collapse with path-param
   examples) is the ENGINE's job now — it emits an already-deduped fetchCallSites in @RESULT. The host
   mergeCallsites/dedupShapeConcrete/pathSegs were DELETED. */

self.astDispatch = async function astDispatch(msg) {
  try {
    if (!msg || msg.type !== "AST_ANALYZE") return { success: false, error: "unknown type " + (msg && msg.type) };
    const html = msg.pageHtml || "";
    const code = msg.code || "";           // any brain-assembled scripts (usually empty); the DOM carries the page
    if (!html && !code) return { success: true, result: linesToAnalysis([], msg) };

    /* ONE persistent instance runs the WHOLE analysis: the page + its chunks (fetched + eval'd in place
       via qjs_chunks/qjs_provide) + its fromReply consumes (in place). No re-run, no separate loops.
       CROSS-SESSION FRONTIER: with a host quantum, park the residue to IDB and resume it next visit. */
    const quantum = msg.quantum || 0;   // 0 = run to completion (default); >0 = per-visit CPU slice
    // runEngine does phase-1 init (engine computes the bundle id via Lexbor), the frontierGet by that id,
    // and phase-2 seeding. It returns the ENGINE-computed frontier key (_fkey) + parked entry (_prior).
    const result = await runEngine(code, html, msg, quantum);
    if (quantum && result._fkey) {   // persist the residue into the GLOBAL frontier (rich entry for later rehydration)
      const prior = result._prior;
      await frontierPut(result._fkey, {
        key: result._fkey, sourceUrl: msg.sourceUrl, html, code, credentialed: !!msg.credentialed, quantum,
        recipes: (result._park || []).join(";"),
        emit: ((prior && prior.emit) || 0) + (result.fetchCallSites || []).length,
        visits: ((prior && prior.visits) || 0) + 1, ts: Date.now(),
      });
    }
    return { success: true, result };   // result.fetchCallSites is already deduped by the engine
  } catch (e) {
    return { success: false, error: String(e && e.message || e), stack: e && e.stack };
  }
};

console.debug("[ast-worker v2] bridge ready (self.astDispatch installed)");
