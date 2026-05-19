// GENERALIZATION SWEEP — proves the landed native rules (F4b jump-fn
// composition, cont.⁸/⁹ IIFE+fn-ref wrapper composition, Step-1
// §13.1.3/§14.2 param-derived local-alias keys, Step-2 §13.15.2 loop-
// test-bound keys) are GENERAL ECMAScript value-flow rules, NOT jQuery-
// corpus-fitted. Each probe is a faithful mirror of a DIFFERENT real
// library's internal shape (axios / redux / lodash / fetch-wrapper).
// If a probe resolves the user's URL, the rule generalised; if not,
// that's a real grounding gap to close (no papering).
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;").call(globalThis);

function probe(label, code) {
  console.log("\n=== " + label + " ===");
  var r;
  try { r = globalThis.analyzeJSBundle(code, "https://ex.com/app", "https://ex.com", null); }
  catch (e) { console.log("  threw: " + e.message); return; }
  var sites = (r.fetchCallSites || []).map(function (s) { return s.method + " " + s.url; });
  console.log("  sites=" + JSON.stringify(sites));
}

// G1 — AXIOS-shape: createInstance returns a `request` wrapper that
// returns dispatchRequest(mergeConfig(defaults, cfg)); mergeConfig is a
// `for(k in src)t[k]=src[k]` merger; dispatchRequest does XHR. Exercises
// F4b + cont.⁹ wrapper-returns-merge-call composition (no jQuery names).
probe("G1-axios-createInstance-mergeConfig-dispatch", `
function mergeConfig(a, b){ var t = {}; for (var k in a) t[k] = a[k]; for (var k2 in b) t[k2] = b[k2]; return t; }
function dispatchRequest(config){ var xhr = new XMLHttpRequest(); xhr.open(config.method, config.url); xhr.send(config.data); return xhr; }
function createInstance(defaults){
  return function request(cfg){ return dispatchRequest(mergeConfig(defaults, cfg)); };
}
var axios = createInstance({ method: "GET", baseURL: "/api" });
axios({ method: "POST", url: "/api/g1-users", data: "x" });
`);

// G2 — LODASH-shape: param-derived local-alias key into a registry,
// then dispatch reads it. Exercises Step-1 §13.1.3 IdentifierReference
// + §14.2 dominating-var-decl replay (alias chain n←path[0]←path).
probe("G2-lodash-baseSet-alias-key-dispatch", `
var registry = {};
function register(spec){
  var path = spec.type.split(".");
  var key = path[0];
  (registry[key] = registry[key] || []).push(spec.handler);
}
register({ type: "http.get", handler: function(o){ var x = new XMLHttpRequest(); x.open(o.verb, o.endpoint); } });
function run(name, o){ var hs = registry[name] || []; for (var i = 0; i < hs.length; i++) hs[i](o); }
run("http", { verb: "GET", endpoint: "/api/g2-resource" });
`);

// G3 — REDUX-shape: applyMiddleware composes via `while` loop-test
// `dispatch = chain[i++](dispatch)`; final dispatch issues the request.
// Exercises Step-2 §13.15.2 AssignmentExpression-value + §14.7.
probe("G3-redux-applyMiddleware-while-compose", `
function thunk(next){ return function(action){ if (typeof action === "function") return action(); return next(action); }; }
function api(next){ return function(action){ if (action.type === "CALL") { var x = new XMLHttpRequest(); x.open(action.method, action.url); } return next(action); }; }
function applyMiddleware(mws){
  var dispatch = function(a){ return a; };
  var i = 0, m;
  while (m = mws[i++]) dispatch = m(dispatch);
  return dispatch;
}
var store = applyMiddleware([thunk, api]);
store({ type: "CALL", method: "GET", url: "/api/g3-data" });
`);

// G4 — FETCH-WRAPPER-shape (cont.⁸ IIFE composition): an IIFE returns a
// merged-config object consumed by a request. Exercises the IIFE
// standalone-summary fallback + forward composition.
probe("G4-iife-merged-config-fetch", `
function extend(a, b){ var t = {}; for (var k in a) t[k] = a[k]; for (var k2 in b) t[k2] = b[k2]; return t; }
var cfg = (function(base, over){ return extend(extend({}, base), over); })({ method: "GET" }, { url: "/api/g4-endpoint" });
fetch(cfg.url);
`);

// G5 — PROMISE-CHAIN: a fetch wrapper built by `.then` composition; the
// request fn is reached through the resolved chain. Exercises §27.2
// thenable value-flow as native code (no Promise special-casing).
probe("G5-promise-then-chain", `
function get(url){ var x = new XMLHttpRequest(); x.open("GET", url); return Promise.resolve(x); }
function withBase(base){ return function(p){ return base + p; }; }
Promise.resolve("/api").then(withBase("/api")).then(function(full){ return get(full + "/g5-data"); });
`);

// G6 — GETTER/defineProperty config (axios/Vue-shape): the URL is
// behind an Object.defineProperty getter. Exercises §10.1.6.2 accessor
// property resolution through the AV graph.
probe("G6-defineProperty-getter-config", `
function makeCfg(u){ var c = {}; Object.defineProperty(c, "url", { get: function(){ return u; }, enumerable: true }); return c; }
var cfg = makeCfg("/api/g6-endpoint");
var xhr = new XMLHttpRequest(); xhr.open("GET", cfg.url);
`);

// G7 — CLASS builder (fetch-wrapper / superagent-shape): chained
// setters on `this`, terminal method issues the request. Exercises
// §15.7 constructor-chain `this.X` resolution + method dispatch.
probe("G7-class-builder-chained-setters", `
function Request(){ this.opts = {}; }
Request.prototype.method = function(m){ this.opts.method = m; return this; };
Request.prototype.url = function(u){ this.opts.url = u; return this; };
Request.prototype.end = function(){ var x = new XMLHttpRequest(); x.open(this.opts.method, this.opts.url); return x; };
new Request().method("POST").url("/api/g7-submit").end();
`);
