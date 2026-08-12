// GENERAL UMD-bootstrap capability test (NOT jQuery bytes). A library
// defines itself inside a factory invoked by a wrapper IIFE that passes
// the global; the factory writes the export onto the global param; a
// top-level user call reads it. This is the general shape real jQuery's
// $.get bottoms out on. If the demand-gated forward engine resolves the
// fetch URL here, the jQuery gap is scale/secondary-mechanism; if not,
// a GENERAL UMD-bootstrap refinement rule is the fix (in the engine,
// not a jQuery-shaped cone seed).
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
globalThis.__DEMAND_PROBE = true;
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;").call(globalThis);

function run(label, code, expectUrl) {
  console.log("\n=== " + label + " (expect url " + expectUrl + ") ===");
  var r = globalThis.analyzeJSBundle(code, "t://" + label, true, null);
  var sites = (r && r.fetchCallSites) || [];
  var urls = sites.map(function (s) { return (s.method || "?") + " " + (s.url || "?"); });
  console.log("fetchSites=" + sites.length + " " + JSON.stringify(urls) +
    (urls.some(function (u) { return u.indexOf(expectUrl) >= 0; }) ? "  [RESOLVED]" : "  [GAP]"));
}

// U1 — minimal UMD: wrapper IIFE passes global to factory; factory writes
// export onto the global param; static-member entry.
run("U1-umd-static-member", `
(function(g, f){ f(g); })(window, function(C){
  C.lib = { get: function(u){ return fetch(u); } };
});
window.lib.get("/api/u1");
`, "/api/u1");

// U2 — UMD + dynamic computed-key augmentation (jQuery's ce.each shape,
// general): factory builds the export via a forEach over a name array
// assigning computed keys.
run("U2-umd-each-augment", `
(function(g, f){ f(g); })(window, function(C){
  var lib = {};
  ["get","post"].forEach(function(m){
    lib[m] = function(u){ return fetch(u, { method: m }); };
  });
  C.lib = lib;
});
window.lib.get("/api/u2");
`, "/api/u2");

// U3 — UMD + factory returns the lib, wrapper assigns the global
// (jQuery's `C.jQuery=C.$=x; return x` + chained-assign shape, general).
run("U3-umd-return-and-assign", `
var L = (function(g, f){ var x = f(); g.lib = g.L = x; return x; })(window, function(){
  var o = {};
  o.req = function(u){ return fetch(u); };
  o.get = function(u){ return o.req(u); };
  return o;
});
window.lib.get("/api/u3");
`, "/api/u3");
