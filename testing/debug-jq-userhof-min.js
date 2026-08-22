// TRUE general-machinery baseline (shape-matcher reverted; only Step-D
// present). Isolates the higher-order composition WITHOUT the deep ajax
// chain. ce.each is properly assigned (mirrors jQuery.extend({each:…})).
//   (a) user-HOF: callback param invoked inside each's body
//   (b) GetV join: e[r] over array-lit "e" with unknown r
//   (c) computed-member assignment ce[i]=fn, i = SetUnion("get","post")
//   (d) member-read ce.get + call + inner url-param bind
var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode + "globalThis.analyzeJSBundle = analyzeJSBundle;\n").call(globalThis);

function run(label, code) {
  console.log("=== " + label + " ===");
  var r = globalThis.analyzeJSBundle(code, "test://" + label, true, null);
  console.log("  fetchCallSites:", JSON.stringify(r.fetchCallSites || []));
  if (r.resolverErrors && r.resolverErrors.length) {
    console.log("  resolverErrors:", JSON.stringify(r.resolverErrors.map(function (e) { return e.message; })));
  }
  console.log("");
}

// A: direct-call each (FunctionDeclaration), element used as computed
// assignment key, then member-read + call binds inner url param.
run("A-direct-call-each",
  'function each(e, t){ for (var r = 0; r < e.length; r++) t(r, e[r]); }\n' +
  'var X = {};\n' +
  'each(["get","post"], function(k, i){ X[i] = function(u){ fetch(u); }; });\n' +
  'X.get("/api/profile");\n');

// A2: same but via Function.prototype.call (§ 20.2.3.3) — jQuery's form.
run("A2-call-form-each",
  'function each(e, t){ for (var r = 0; r < e.length; r++) t.call(e[r], r, e[r]); }\n' +
  'var X = {};\n' +
  'each(["get","post"], function(k, i){ X[i] = function(u){ fetch(u); }; });\n' +
  'X.get("/api/profile");\n');

// B: exact jquery-3.7.1.min `each` shape, ce.each PROPERLY assigned
// (mirrors jQuery.extend({each:function(e,t){…}})). Inner fn uses the
// element `i` directly in the URL.
run("B-min-each-assigned",
  'function c(e){ return typeof e === "object" && e !== null && typeof e.length === "number"; }\n' +
  'var ce = {};\n' +
  'ce.each = function(e, t){ var n, r = 0; if (c(e)) { for (n = e.length; r < n; r++) if (!1 === t.call(e[r], r, e[r])) break; } else for (r in e) if (!1 === t.call(e[r], r, e[r])) break; return e; };\n' +
  'ce.each(["get","post"], function(e, i){ ce[i] = function(e, t, n, r){ fetch("/api/" + i); }; });\n' +
  'ce.get("/x", {a:1});\n');

// C2: NO HOF at all — plain get→ajax→xhr.open. Isolates the
// obj-lit-arg → ajax param `s` → s.url/s.type → xhr.open chain.
run("C2-plain-get-ajax-xhr-NOHOF",
  'var ce = {};\n' +
  'ce.ajax = function(s){ var x = new XMLHttpRequest(); x.open(s.type, s.url); };\n' +
  'ce.get = function(u, d){ return ce.ajax({ url: u, type: "GET" }); };\n' +
  'ce.get("/api/profile", {a:1});\n');

// C3: HOF-written get, but inner fn calls ce.ajax DIRECTLY on a literal
// (no own-param hop) — isolates closure-written-fn → ajax → xhr.open.
run("C3-hof-get-ajax-literal",
  'function c(e){ return typeof e === "object" && e !== null && typeof e.length === "number"; }\n' +
  'var ce = {};\n' +
  'ce.each = function(e, t){ var n, r = 0; if (c(e)) { for (n = e.length; r < n; r++) if (!1 === t.call(e[r], r, e[r])) break; } else for (r in e) if (!1 === t.call(e[r], r, e[r])) break; return e; };\n' +
  'ce.ajax = function(s){ var x = new XMLHttpRequest(); x.open(s.type, s.url); };\n' +
  'ce.each(["get","post"], function(e, i){ ce[i] = function(u, d){ return ce.ajax({ url: "/api/" + i, type: i }); }; });\n' +
  'ce.get();\n');

// B-GLOBAL: B but jQuery is a GLOBAL function-ref exported UMD-style
// (window.$=$), not a local `var ce={}`. Isolates whether the each-HOF
// computed-key closure-write `$[i]=fn` lands in a GLOBAL function-ref's
// _extraProps (real jQuery's exact structural shape) vs a local obj-lit.
run("Bg-global-funcref-each",
  '(function(g){\n' +
  '  function $(sel){ return sel; }\n' +
  '  $.each = function(e, t){ var n, r = 0; for (n = e.length; r < n; r++) if (!1 === t.call(e[r], r, e[r])) break; return e; };\n' +
  '  $.each(["get","post"], function(e, i){ $[i] = function(u, d){ return fetch("/api/" + i); }; });\n' +
  '  g.$ = $;\n' +
  '})(typeof window !== "undefined" ? window : this);\n' +
  '$.get("/x");\n');

// Bg2: Bg + the inner fn is SELF-REFERENTIAL & complex exactly like
// real jQuery: $[i]=function(u,d,cb,ty){ return $.ajax($.extend({url:u,
// type:i,...})); } with $.ajax→$.xhr-style. Isolates whether the each-
// HOF closure-write of a self-referential inner fn (referencing the
// same global $) through .extend composes — real jQuery's exact shape.
run("Bg2-global-selfref-inner-ajax",
  '(function(g){\n' +
  '  function $(sel){ return sel; }\n' +
  '  $.extend = function(){ var t = arguments[0] || {}; for (var i = 1; i < arguments.length; i++) { var s = arguments[i]; for (var k in s) t[k] = s[k]; } return t; };\n' +
  '  $.ajax = function(s){ var x = new XMLHttpRequest(); x.open(s.type, s.url); return x; };\n' +
  '  $.each = function(e, t){ var n, r = 0; if (typeof e === "object") { for (n = e.length; r < n; r++) if (!1 === t.call(e[r], r, e[r])) break; } else for (r in e) if (!1 === t.call(e[r], r, e[r])) break; return e; };\n' +
  '  $.each(["get","post"], function(e, i){ $[i] = function(u, d, cb, ty){ return $.ajax($.extend({ url: u, type: i, dataType: ty, data: d })); }; });\n' +
  '  g.$ = $;\n' +
  '})(typeof window !== "undefined" ? window : this);\n' +
  '$.get("/api/profile", {a:1});\n');

// C: full composition — inner fn uses its OWN url param, returns
// ce.ajax({url:u,type:i}); minified-faithful jQuery get→ajax→xhr.open.
run("C-min-inner-url-ajax-xhr",
  'function c(e){ return typeof e === "object" && e !== null && typeof e.length === "number"; }\n' +
  'var ce = {};\n' +
  'ce.each = function(e, t){ var n, r = 0; if (c(e)) { for (n = e.length; r < n; r++) if (!1 === t.call(e[r], r, e[r])) break; } else for (r in e) if (!1 === t.call(e[r], r, e[r])) break; return e; };\n' +
  'ce.ajax = function(s){ var x = new XMLHttpRequest(); x.open(s.type, s.url); };\n' +
  'ce.each(["get","post"], function(e, i){ ce[i] = function(u, d, cb, ty){ return ce.ajax({ url: u, type: i }); }; });\n' +
  'ce.get("/api/profile", {a:1});\n');
