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
  var idx = tail.indexOf(marker);
  if (idx === -1) return null;
  var start = idx + marker.length;
  while (start < tail.length && (tail.charCodeAt(start) === 32 || tail.charCodeAt(start) === 9)) start++;
  var end = start;
  while (end < tail.length && tail.charCodeAt(end) > 32) end++;
  return end > start ? tail.substring(start, end) : null;
}

// An opaque value that flowed into a URL/method stringifies to
// "[object Object]" (the sentinel has no toString). That is a resolver
// gap, NOT an endpoint — surface it on resolverErrors, never emit a
// fabricated path. (Empty/whitespace url = same: nothing resolved.)
function isUnresolved(s) {
  return !s || !String(s).trim() || String(s).indexOf("[object Object]") >= 0;
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
function buildPageDomSrc(domIslands, domContext, scriptUrls) {
  var islands = Array.isArray(domIslands) ? domIslands : [];
  var byId = (domContext && domContext.byId && typeof domContext.byId === "object") ? domContext.byId : {};
  var srcs = Array.isArray(scriptUrls) ? scriptUrls : [];
  if (!islands.length && !Object.keys(byId).length && !srcs.length) return null;
  return "(function(){try{\n" +
    "var IS=" + JSON.stringify(islands) + ",BY=" + JSON.stringify(byId) + ",SR=" + JSON.stringify(srcs) + ";\n" +
    "for(var i=0;i<IS.length;i++){var it=IS[i];var e=document.createElement(it.tag==='template'?'template':'script');" +
    "if(it.type)e.setAttribute('type',it.type);if(it.id)e.id=it.id;" +
    "if(it.dataTarget)e.setAttribute('data-target',it.dataTarget);e.textContent=it.text;" +
    "(document.head||document.documentElement||document.body).appendChild(e);}\n" +
    "for(var id in BY){if(!Object.prototype.hasOwnProperty.call(BY,id)||document.getElementById(id))continue;" +
    "var b=BY[id],el=document.createElement('div');el.id=id;" +
    "if(b&&b.href)el.setAttribute('href',b.href);if(b&&b.src)el.setAttribute('src',b.src);" +
    "if(b&&b.action)el.setAttribute('action',b.action);" +
    "if(b&&b.dataAttrs)for(var k in b.dataAttrs)el.setAttribute('data-'+k,b.dataAttrs[k]);" +
    "(document.body||document.documentElement).appendChild(el);}\n" +
    // Real <script src> tags + document.currentScript, so webpack's
    // auto-publicPath (currentScript.src / getElementsByTagName) finds
    // a real URL instead of throwing — running the code correctly, not
    // recovering from a self-inflicted throw.
    "var LS=null;for(var s=0;s<SR.length;s++){var sc=document.createElement('script');sc.src=SR[s];sc.setAttribute('src',SR[s]);(document.head||document.documentElement||document.body).appendChild(sc);LS=sc;}\n" +
    "if(LS){try{Object.defineProperty(document,'currentScript',{get:function(){return LS;},configurable:true});}catch(e){}}\n" +
    "}catch(e){}})();";
}

async function forcedAnalyze(code, sourceUrl, domContext, domIslands, scriptUrls) {
  var t0 = Date.now();
  // /h.js host-edge model, /p.js the page's server-rendered DOM data
  // islands (so the bundle bootstraps correctly), /b.js the analyzed
  // bundle, /d.js the epilogue that pumps load/ready/message + XHR
  // completion + timers so fetch code inside callbacks runs.
  var inMem = [["/h.js", HOSTEDGE], ["/b.js", code], ["/d.js", HOSTDRIVER]];
  var fileArgs = ["/h.js", "/b.js", "/d.js"];
  var pageDom = buildPageDomSrc(domIslands, domContext, scriptUrls);
  if (pageDom) {
    inMem.splice(1, 0, ["/p.js", pageDom]);
    fileArgs.splice(1, 0, "/p.js");
  }
  var work = [""], seen = new Set([""]);
  var runs = 0;
  var methods = new Map();          // "METHOD path" -> {method,path,kind,params,loc,chain}
  var resolverErrors = [];
  var reSeen = new Set();
  var securitySinks = [];
  var secSeen = new Set();

  // Pick the source position from a captured stack (frames[0] is the
  // literal host-API call). Prefer a /b.js (analyzed-bundle) frame so
  // the line is in the combined-bundle space background.js maps via
  // scriptOffsets; fall back to the innermost frame.
  function pickSite(at) {
    if (!at || !at.length) return { loc: null, chain: [] };
    var pick = at[0];
    for (var i = 0; i < at.length; i++) { if (String(at[i].file).indexOf("/b.js") >= 0) { pick = at[i]; break; } }
    var chain = [];
    for (var j = 0; j < at.length; j++) chain.push({ line: at[j].line, column: at[j].col });
    return { loc: { line: pick.line, column: pick.col }, chain: chain };
  }

  function ep(method, rawUrl, body, kind, at) {
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
    if (!m) { m = { method: method, path: path, kind: kind, params: new Map(), loc: null, chain: [] }; methods.set(key, m); }
    if (!m.loc && at) { var s = pickSite(at); m.loc = s.loc; m.chain = s.chain; }
    var add = function (n, loc, val) {
      var p = m.params.get(n);
      if (!p) { p = { name: n, location: loc, examples: new Set() }; m.params.set(n, p); }
      if (val !== undefined && val !== "" && !isUnresolved(val)) p.examples.add(val);
    };
    var qp = parsePairs(qs);
    for (var k in qp) for (var vi = 0; vi < qp[k].length; vi++) add(k, "query", qp[k][vi]);
    if (body != null && body !== "") {
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

  while (work.length) {
    var sched = work.shift();
    runs++;
    var stdout = [];
    var stderr = [];
    var m = await self.createQJS({
      noInitialRun: true,
      print: function (s) { stdout.push(s); },
      // Surface engine/bundle errors — never swallow. A bundle that
      // throws is a signal the host model is incomplete; the detail
      // (qjsmain emits "@E <file> :: <msg>" + stack here) becomes a
      // visible resolverError so the gap gets fixed, not hidden.
      printErr: function (s) { stderr.push(s); },
    });
    for (var fi = 0; fi < inMem.length; fi++) m.FS.writeFile(inMem[fi][0], inMem[fi][1]);
    try {
      m.callMain(["--fe-exec", "--fe-sched=" + sched, "--fe-trace=/t.tr"].concat(fileArgs));
    } catch (e) { /* engine may longjmp on exit; trace + stdout still valid */ }
    var trace = "";
    try { trace = m.FS.readFile("/t.tr", { encoding: "utf8" }); } catch (e) {}

    var rh = [];
    for (var li = 0; li < stdout.length; li++) {
      var line = stdout[li];
      if (line.slice(0, 3) === "@E ") {
        // Uncaught bundle exception = host-model gap. Surface the real
        // message + stack (structured JSON from qjsmain) as a
        // resolverError so the specific unmodelled Web API gets fixed,
        // never ignored. Dedup by message (same gap repeats per run).
        var ej; try { ej = JSON.parse(line.slice(3)); } catch (e) { ej = { message: line.slice(3) }; }
        var emsg = String(ej.message || "(throw)");
        var ekey = "E:" + emsg.slice(0, 120);
        if (!reSeen.has(ekey)) {
          reSeen.add(ekey);
          var firstFrame = String(ej.stack || "").split("\n").filter(function (s) { return s.indexOf("/b.js") >= 0; })[0] || "";
          // A "/b.js:line:col" location is useless to fix the gap
          // unless the actual offending source is shown — the combined
          // bundle isn't reconstructable later (lazy chunks shift the
          // line numbers). The worker still holds `code` (the exact
          // /b.js it just ran), so slice the failing line here.
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
      if (line.slice(0, 3) === "@H ") {
        try { rh.push(JSON.parse(line.slice(3))); } catch (e) {}
      } else if (line.slice(0, 3) === "@S ") {
        // Attacker-tainted security sink (XSS class). Dedup across
        // forced runs by type|sink|position.
        var sr; try { sr = JSON.parse(line.slice(3)); } catch (e) { continue; }
        var ss = pickSite(sr.at);
        var sk = sr.type + "|" + sr.sink + "|" + (ss.loc ? ss.loc.line + ":" + ss.loc.column : "?");
        if (secSeen.has(sk)) continue;
        secSeen.add(sk);
        securitySinks.push({
          type: sr.type,
          sink: sr.sink,
          source: "host-unknown attacker input reached the sink (forced multi-path execution)",
          severity: sr.type === "code-exec" ? "critical" : "high",
          location: ss.loc || { line: 0, column: 0 },
          taintPath: ss.chain.map(function (c) { return { at: c }; }),
          value: sr.value != null ? String(sr.value) : null,
        });
      }
    }
    var pend = null;
    for (var ri = 0; ri < rh.length; ri++) {
      var r = rh[ri];
      if (r.api === "XMLHttpRequest.open") { if (pend) ep(pend.m, pend.u, null, "xhr", pend.at); pend = { m: r.args[0], u: r.args[1], at: r.at }; }
      else if (r.api === "XMLHttpRequest.send") { if (pend) { ep(pend.m, pend.u, r.args[0], "xhr", pend.at); pend = null; } }
      else if (r.api === "fetch") ep(r.args[1] || "GET", r.args[0], r.args[2], "fetch", r.at);
    }
    if (pend) ep(pend.m, pend.u, null, "xhr", pend.at);

    var decisions = [], frontiers = [], tl = trace.split("\n");
    for (var ti = 0; ti < tl.length; ti++) {
      var b = tl[ti].match(/^B (\d+) (\d)/); if (b) decisions[+b[1]] = b[2];
      var f = tl[ti].match(/^F (\d+)/); if (f) frontiers.push(+f[1]);
    }
    for (var ki = 0; ki < frontiers.length; ki++) {
      var idx = frontiers[ki];
      var ns = decisions.slice(0, idx).map(function (d) { return d || "0"; }).join("") + "1";
      if (!seen.has(ns)) { seen.add(ns); work.push(ns); }
    }
  }

  // Execution-grounded focused view: the spans a security researcher
  // actually wants — every endpoint (with the example-value spread
  // that PROVES it sits behind a forced gate) and every tainted sink,
  // each at its real bundle source position. This replaces the dropped
  // Babel viewer index: the engine knows what's interesting because it
  // executed it, not because it indexed identifiers.
  var fetchCallSites = [];
  var focusedView = [];
  methods.forEach(function (mm) {
    var params = [];
    var gated = [];
    mm.params.forEach(function (p) {
      var vv = Array.from(p.examples);
      params.push({ name: p.name, location: p.location, validValues: vv });
      if (vv.length > 1) gated.push(p.name + "∈{" + vv.join(",") + "}");
    });
    fetchCallSites.push({
      method: mm.method, url: mm.path, params: params,
      headers: {}, loc: mm.loc, callChain: mm.chain,
      enclosingFunction: null, kind: mm.kind,
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

  return {
    fetchCallSites: fetchCallSites,
    securitySinks: securitySinks,
    dangerousPatterns: [],
    resolverErrors: resolverErrors,
    focusedView: focusedView,
    protoEnums: [],
    protoFieldMaps: [],
    domEndpoints: [],
    sourceUrl: sourceUrl || "",
    sourceMapUrl: sourceMapUrlOf(code),
    _timings: { ms: Date.now() - t0, runs: runs },
  };
}

onmessage = function (e) {
  var id = e.data._id;
  var msg = e.data.msg;

  function done(response) { postMessage({ _id: id, response: response }); }

  if (msg.type === "AST_ANALYZE") {
    forcedAnalyze(String(msg.code || ""), msg.sourceUrl || "", msg.domContext || null, msg.domIslands || null, msg.scriptUrls || null)
      .then(function (result) { done({ success: true, result: result }); })
      .catch(function (err) {
        done({ success: false, error: (err && err.message) || String(err), stack: err && err.stack });
      });
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
