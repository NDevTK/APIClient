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
    // all sibling fields the brain reads unconditionally, present + empty so it never throws:
    protoEnums: [], protoFieldMaps: [], securitySinks: [], dangerousPatterns: [],
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
/* Drive ONE persistent engine instance through the step protocol. fromReply is now IN PLACE: when the
   engine parks awaiting a reply (qjs_step -> NEED_FETCH), the TRUSTED offscreen safe-fetches each pending
   url (GET, page-origin SSRF) and qjs_provide()s the body, resuming the flow in the SAME instance -- no
   re-instantiate, no re-run of the whole page. */
async function runEngine(code, html, msg) {
  const lines = [];
  const M = await createQJS({ print: (s) => lines.push(s), printErr: (s) => lines.push(s), noInitialRun: true });
  const cstr = (s) => { const n = M.lengthBytesUTF8(s || "") + 1; const p = M._malloc(n); M.stringToUTF8(s || "", p, n); return p; };
  const ptrs = [];
  const arg = (s) => { const p = cstr(s); ptrs.push(p); return p; };
  try {
    M.ccall("qjs_init", "number", ["number", "number", "number", "number", "number", "number"],
      [arg(code || ""), arg(html || ""), arg(originOf(msg && msg.sourceUrl)), arg(""), 0, arg("")]);
    let guard = 0;
    while (M.ccall("qjs_step", "number", [], []) === 1 && guard++ < 5000) {
      const pend = String(M.ccall("qjs_pending", "string", [], [])).split("\n").filter(Boolean);
      for (const u of pend) {
        let body = "";
        if (typeof self.safeFetch === "function" && msg && msg.sourceUrl && u.indexOf("{}") < 0) {
          try { const abs = new URL(u, msg.sourceUrl).href; const r = await self.safeFetch(abs, { pageUrl: msg.sourceUrl }); if (r && r.ok && typeof r.body === "string") body = r.body; } catch (_) {}
        }
        M.ccall("qjs_provide", "void", ["string", "string"], [u, body]);   // concrete body, or "" -> opaque shape
      }
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
  if (path.indexOf("{}") >= 0) return null;   // opaque path -> unresolvable
  return path;                                 // concrete path, query dropped
}
function mergeCallsites(map, sites) {
  for (const s of sites || []) { const k = (s.method || "GET") + " " + s.url; if (!map.has(k)) map.set(k, s); }
}
function pathSegs(u) { const q = u.indexOf("?"); const p = q >= 0 ? u.slice(0, q) : u; return { segs: p.split("/"), query: q >= 0 ? u.slice(q) : "" }; }
/* A concrete endpoint that is a SHAPE instantiated (e.g. /api/org/acme-42/members vs /api/org/{}/members)
   is the SAME endpoint with its path placeholder filled — not a distinct one. Attach the concrete segment
   as the shape's path-param example (the moat's KEY+VALUE) and drop the redundant concrete, so the surface
   isn't double-counted. Only collapses a concrete INTO an existing shape (never merges two real resources). */
function dedupShapeConcrete(map) {
  const arr = [...map.values()];
  const shapes = arr.filter((e) => e.url.indexOf("{}") >= 0);
  if (!shapes.length) return;
  for (const c of arr) {
    if (c.url.indexOf("{}") >= 0) continue;                 // c must be concrete
    for (const s of shapes) {
      if ((s.method || "GET") !== (c.method || "GET")) continue;
      const ss = pathSegs(s.url), cs = pathSegs(c.url);
      if (ss.segs.length !== cs.segs.length || ss.query !== cs.query) continue;
      let ok = true; const ex = [];
      for (let i = 0; i < ss.segs.length; i++) {
        if (ss.segs[i] === "{}") { if (cs.segs[i] && cs.segs[i].indexOf("{}") < 0) ex.push([i, cs.segs[i]]); else { ok = false; break; } }
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

    const result = await runEngine(code, html, msg);   // fromReply handled IN PLACE inside the step-loop
    const endpoints = new Map();
    mergeCallsites(endpoints, result.fetchCallSites);

    /* CHUNK LOOP (the moat's "learn computed JS files loaded via a code path"): the untrusted engine
       DISCOVERS chunk URLs (static or JS-computed, via <script src> / createElement+.src+appendChild);
       the TRUSTED offscreen safe-fetches each (GET, page-origin SSRF, JS-typed) and the engine
       forced-executes it WITH the page DOM -> its endpoints are learned. Iterate until no NEW chunk
       (dedup by URL; disk is the floor, not a count). */
    if (typeof self.safeFetch === "function" && msg.sourceUrl) {
      const seen = new Set();
      let frontier = (result.chunkUrls || []).slice();
      const chunkUrlsAll = new Set(result.chunkUrls || []);
      while (frontier.length) {
        const next = [];
        for (const cu of frontier) {
          if (seen.has(cu)) continue; seen.add(cu);
          const furl = chunkFetchUrl(cu);
          if (!furl) continue;
          let abs; try { abs = new URL(furl, msg.sourceUrl).href; } catch (_) { continue; }   // resolve relative -> absolute for safeFetch
          let r; try { r = await self.safeFetch(abs, { pageUrl: msg.sourceUrl, as: "script" }); } catch (_) { continue; }
          if (!r || !r.ok || !r.body) continue;
          const cr = await runEngine(r.body, html, msg);   // forced-execute the chunk against the page DOM (fromReply in-place)
          mergeCallsites(endpoints, cr.fetchCallSites);
          for (const nu of cr.chunkUrls || []) if (!chunkUrlsAll.has(nu)) { chunkUrlsAll.add(nu); next.push(nu); }  // nested chunks
        }
        frontier = next;
      }
      result.chunkUrls = [...chunkUrlsAll];
    }

    dedupShapeConcrete(endpoints);   // collapse concrete instantiations into their shape (path-param examples)
    result.fetchCallSites = [...endpoints.values()];
    return { success: true, result };
  } catch (e) {
    return { success: false, error: String(e && e.message || e), stack: e && e.stack };
  }
};

console.debug("[ast-worker v2] bridge ready (self.astDispatch installed)");
