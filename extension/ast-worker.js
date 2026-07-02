/* APIClient v2 host bridge (offscreen document).
 *
 * The old ast-worker.js + ast-thread.js (a Web Worker owning the emscripten glue) were DELETED with
 * the fresh fork. This is their replacement: it instantiates the v2 forced-execution engine
 * (lib/qjs/qjs.mjs + qjs.wasm, built by engine/build.mjs from engine/host/main.c) and exposes the one
 * seam the brain calls — `self.astDispatch({type:"AST_ANALYZE", code, ...}) -> {success, result}`.
 *
 * The v2 engine is a CLI: main() evals the bundle as <boot>, runs the ONE scheduler (BFS over forced
 * paths + orphans), and PRINTS `@H <url>` per learned endpoint (plus @ORPHANS/@DONE/@WHY/@E). This
 * bridge captures that stdout and maps @H -> the `analysis.fetchCallSites` shape offscreen-brain.js
 * consumes (which populates globalStore.endpoints with source:"ast_analysis", what `netdiff --unused`
 * reads). A module script so it can `import` the ES6 wasm factory under the offscreen CSP.
 *
 * NOTE: a fresh wasm instance per analysis (main() runs exactly once per instance; its scheduler
 * globals are single-shot). Reusing one instance across analyses needs a re-runnable analyze() export
 * from the engine — a later optimization. Network is still the in-engine stub (emits @H, opaque body);
 * routing fetch to lib/safe-fetch.js is HOST WIRE step 3.
 */
import createQJS from "./lib/qjs/qjs.mjs";

/* An opaque HOLE in a shape: "{}" (generic) or "{tag}" (source-tagged: {hash}/{search}/{pm}/{reply}).
   A URL with any hole is not concretely fetchable; a path segment that IS a hole is a path placeholder. */
const HOLE = /\{[a-z]*\}/;
const HOLE_G = /\{[a-z]*\}/g;
const SEG_HOLE = /^\{[a-z]*\}$/;
const hasHole = (s) => HOLE.test(s || "");
const normHoles = (s) => (s || "").replace(HOLE_G, "{}");   // endpoint IDENTITY is hole-source-insensitive ({search}=={hash}=={})

/* Parse a learned URL's query string into per-param example values (the moat wants keys+values).
   A value of "{}" is the opaque SHAPE (external-input param); a concrete value is a real example. */
function parseQueryParams(url) {
  const params = [];
  const q = url.indexOf("?");
  if (q < 0) return params;
  for (const pair of url.slice(q + 1).split("&")) {
    if (!pair) continue;
    const eq = pair.indexOf("=");
    const name = eq >= 0 ? pair.slice(0, eq) : pair;
    const val = eq >= 0 ? pair.slice(eq + 1) : "";
    let dn = name, dv = val;
    try { dn = decodeURIComponent(name); } catch (_) {}
    try { dv = decodeURIComponent(val); } catch (_) {}
    params.push({ name: dn, location: "query", validValues: dv ? [dv] : [] });
  }
  return params;
}

/* Map captured stdout lines -> the analysis object the brain consumes. Every sibling field the brain
   reads unconditionally is present (empty) so it never throws; fetchCallSites carries the @H records. */
function linesToAnalysis(lines, msg) {
  const fetchCallSites = [];
  const resolverErrors = [];
  const chunkUrls = [];
  const replyWant = [];
  const park = [];
  const securitySinks = [];
  let emitDone = null, orphans = 0;
  const seen = new Set();
  for (const raw of lines) {
    const ln = String(raw);
    if (ln.startsWith("@REPLYWANT ")) { const u = ln.slice(11).trim(); if (u && replyWant.indexOf(u) < 0) replyWant.push(u); continue; }
    if (ln.startsWith("@H ")) {
      // format: "@H <METHOD> <url>" (js_fetch) or "@H <tag>" (__emit). First token = method if it's a verb.
      let rest = ln.slice(3).trim(), method = "GET";
      const sp = rest.indexOf(" ");
      if (sp > 0) {
        const tok = rest.slice(0, sp).toUpperCase();
        if (/^(GET|POST|PUT|DELETE|PATCH|HEAD|OPTIONS)$/.test(tok)) { method = tok; rest = rest.slice(sp + 1).trim(); }
      }
      const url = rest;
      if (!url || url === "?") continue;
      const key = method + " " + url;
      if (seen.has(key)) continue;            // metric-layer dedup: distinct (method,url)
      seen.add(key);
      fetchCallSites.push({ url, method, params: parseQueryParams(url), source: "ast_analysis" });
    } else if (ln.startsWith("@CHUNK ")) {
      const u = ln.slice(7).trim(); if (u && chunkUrls.indexOf(u) < 0) chunkUrls.push(u);   // external <script src> discovered
    } else if (ln.startsWith("@S ")) {
      // security view: an XSS/injection SINK reached by tainted (opaque) data. sp[0]=sink, rest=taint shape.
      const rest = ln.slice(3).trim(); const sp = rest.indexOf(" ");
      const sink = sp > 0 ? rest.slice(0, sp) : rest;
      const shape = sp > 0 ? rest.slice(sp + 1) : "{}";   // the tainted value's concrete/opaque interleaving (PoC seed)
      securitySinks.push({ type: sink, sink: sink, taint: "opaque", shape: shape, evidence: rest, source: "ast_analysis" });
    } else if (ln.startsWith("@PARK ")) {
      const p = ln.slice(6).trim().split(/\s+/); park.push(p[0] + "," + (p[1] || ""));   // recipe: orphan_idx,decbits
    } else if (ln.startsWith("@ORPHANS ")) {
      orphans += parseInt(ln.slice(9).trim(), 10) || 0;
    } else if (ln.startsWith("@WHY ")) {
      try { const o = JSON.parse(ln.slice(5)); resolverErrors.push({ context: o.phase || "why", message: o.reason || ln, snippet: null, replyExample: null }); }
      catch (_) { resolverErrors.push({ context: "why", message: ln.slice(5), snippet: null, replyExample: null }); }
    } else if (ln.startsWith("@E ")) {
      resolverErrors.push({ context: "engine", message: ln.slice(3), snippet: null, replyExample: null });
    } else if (ln.startsWith("@DONE")) {
      emitDone = ln;
    }
  }
  return {
    fetchCallSites,
    resolverErrors,
    chunkUrls,
    securitySinks,
    // all sibling fields the brain reads unconditionally, present + empty so it never throws:
    protoEnums: [], protoFieldMaps: [], dangerousPatterns: [],
    esmImportUrls: [], inRunModuleUrls: [], domEndpoints: [],
    sourceMapTypes: [], sourceMapsByUrl: {}, traceMapsByUrl: {}, valueConstraints: [],
    sourceMapUrl: null, sourceMap: null, sourceUrl: msg.sourceUrl || "",
    _orphans: orphans, _emitDone: emitDone, _replyWant: replyWant, _park: park,
  };
}

/* Run one page through a fresh v2 engine instance, capturing @H/@CHUNK stdout. The ENGINE parses the
   page HTML with its in-wasm Lexbor DOM and runs the document's scripts in order (against the real
   DOM) — the bridge no longer scrapes scripts. code (argv[1]) is any brain-assembled extra scripts
   (usually empty); html (argv[2]) is the page. */
function originOf(u) { try { return new URL(u).origin; } catch (_) { return ""; } }
function strHash(s) { let h = 5381; for (let i = 0; i < s.length; i++) h = ((h << 5) + h + s.charCodeAt(i)) | 0; return (h >>> 0).toString(36); }
/* Stable BUNDLE IDENTITY for the frontier key: the EXTERNAL <script src> set (content-hash filenames
   like main.abc123.js ARE the app version), NOT the volatile HTML wrapper (per-request nonces/CSRF
   tokens would change the key every visit -> the frontier would never resume). A redeploy changes a src
   -> new key -> stale frontier invalidated. Inline-only pages fall back to the HTML hash (rare; they're
   small, finish in one visit, never park). */
function bundleId(html, code) {
  const srcs = [];
  const re = /<script\b[^>]*\bsrc\s*=\s*["']([^"']+)["']/gi;
  let m; while ((m = re.exec(html || "")) !== null) srcs.push(m[1]);
  return srcs.length ? strHash(srcs.join("|")) : strHash((html || "") + (code || ""));
}
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
/* The ONE WFQ weight (priority.js) at the HOST level: a parked frontier's value-of-information =
   epRate (emits per visit) + an explore/fairness floor for under-visited frontiers. Same policy as the
   within-page scheduler -- one attention, two levels. */
function frontierWeight(e) {
  const epRate = (e && e.visits) ? (e.emit || 0) / e.visits : (e && e.emit || 0);
  const exploreBonus = 1 / ((e && e.visits || 0) + 1);
  const P = self._priorityCmp;
  return (P && P.flowWeight) ? P.flowWeight({ ctx: { epRate, exploreBonus } }) : (1 + epRate + exploreBonus);
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
    const r = await runEngine(top.code || "", top.html || "", m2, m2.quantum, top.recipes);
    dedupShapeConcrete(new Map((r.fetchCallSites || []).map((s) => [(s.method || "GET") + " " + s.url, s])));
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
async function runEngine(code, html, msg, quantum, recipes) {
  const lines = [];
  const M = await createQJS({ print: (s) => lines.push(s), printErr: (s) => lines.push(s), noInitialRun: true });
  const cstr = (s) => { const n = M.lengthBytesUTF8(s || "") + 1; const p = M._malloc(n); M.stringToUTF8(s || "", p, n); return p; };
  const ptrs = [];
  const arg = (s) => { const p = cstr(s); ptrs.push(p); return p; };
  try {
    // argv: [extra-code, pageHtml, real-origin, fromReply(unused, in-place now), quantum, resume-recipes]
    M.ccall("qjs_init", "number", ["number", "number", "number", "number", "number", "number"],
      [arg(code || ""), arg(html || ""), arg(originOf(msg && msg.sourceUrl)), arg(""), quantum || 0, arg(recipes || "")]);
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
  return linesToAnalysis(lines, msg);
}

/* A @CHUNK URL is fetchable only if its PATH is concrete — an opaque path segment ("/chunks/{}.js")
   can't be resolved. An opaque QUERY ("?v={}") is fine: strip it (version params are optional). */
function chunkFetchUrl(u) {
  const q = u.indexOf("?");
  const path = q >= 0 ? u.slice(0, q) : u;
  if (hasHole(path)) return null;   // opaque path -> unresolvable
  return path;                                 // concrete path, query dropped
}
function mergeCallsites(map, sites) {
  for (const s of sites || []) { const k = (s.method || "GET") + " " + normHoles(s.url); if (!map.has(k)) map.set(k, s); }
}
function pathSegs(u) { const q = u.indexOf("?"); const p = q >= 0 ? u.slice(0, q) : u; return { segs: p.split("/"), query: q >= 0 ? u.slice(q) : "" }; }
/* A concrete endpoint that is a SHAPE instantiated (e.g. /api/org/acme-42/members vs /api/org/{}/members)
   is the SAME endpoint with its path placeholder filled — not a distinct one. Attach the concrete segment
   as the shape's path-param example (the moat's KEY+VALUE) and drop the redundant concrete, so the surface
   isn't double-counted. Only collapses a concrete INTO an existing shape (never merges two real resources). */
function dedupShapeConcrete(map) {
  const arr = [...map.values()];
  const shapes = arr.filter((e) => hasHole(e.url));
  if (!shapes.length) return;
  for (const c of arr) {
    if (hasHole(c.url)) continue;                 // c must be concrete
    for (const s of shapes) {
      if ((s.method || "GET") !== (c.method || "GET")) continue;
      const ss = pathSegs(s.url), cs = pathSegs(c.url);
      if (ss.segs.length !== cs.segs.length || ss.query !== cs.query) continue;
      let ok = true; const ex = [];
      for (let i = 0; i < ss.segs.length; i++) {
        if (SEG_HOLE.test(ss.segs[i])) { if (cs.segs[i] && !hasHole(cs.segs[i])) ex.push([i, cs.segs[i]]); else { ok = false; break; } }
        else if (ss.segs[i] !== cs.segs[i]) { ok = false; break; }
      }
      if (ok && ex.length) {
        for (const [i, v] of ex) {
          let pp = (s.params || []).find((p) => p.location === "path" && p.name === "arg" + i);
          if (!pp) { pp = { name: "arg" + i, location: "path", validValues: [] }; (s.params = s.params || []).push(pp); }
          if (pp.validValues.indexOf(v) < 0) pp.validValues.push(v);   // concrete example for the path param
        }
        map.delete((c.method || "GET") + " " + c.url);
        break;
      }
    }
  }
}

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
    const fkey = originOf(msg.sourceUrl) + "|" + bundleId(html, code);   // STABLE across visits (bundle version, not volatile HTML)
    const prior = quantum ? await frontierGet(fkey) : null;      // resume this bundle's parked frontier
    const result = await runEngine(code, html, msg, quantum, prior && prior.recipes);
    if (quantum) {   // persist the residue into the GLOBAL frontier (rich entry for later rehydration)
      await frontierPut(fkey, {
        key: fkey, sourceUrl: msg.sourceUrl, html, code, credentialed: !!msg.credentialed, quantum,
        recipes: (result._park || []).join(";"),
        emit: ((prior && prior.emit) || 0) + (result.fetchCallSites || []).length,
        visits: ((prior && prior.visits) || 0) + 1, ts: Date.now(),
      });
    }
    const endpoints = new Map();
    mergeCallsites(endpoints, result.fetchCallSites);

    dedupShapeConcrete(endpoints);   // collapse concrete instantiations into their shape (path-param examples)
    result.fetchCallSites = [...endpoints.values()];
    return { success: true, result };
  } catch (e) {
    return { success: false, error: String(e && e.message || e), stack: e && e.stack };
  }
};

console.debug("[ast-worker v2] bridge ready (self.astDispatch installed)");
