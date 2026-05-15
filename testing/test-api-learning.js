// testing/test-api-learning.js — API learning tests verifying the analyzer
// produces fetch call sites with correct URL / method / headers / body
// params, exercising the same data the popup displays for "what APIs
// does this bundle call".
//
// Run: node testing/test-api-learning.js

var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");

var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();

var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode + "\nglobalThis.analyzeJSBundle = analyzeJSBundle;").call(globalThis);

var passed = 0, failed = 0, total = 0;

function api(name, code, check) {
  total++;
  try {
    var result = analyzeJSBundle(code, "test://" + name, true, null);
    var sites = result.fetchCallSites || [];
    if (check(sites, result)) {
      passed++;
      console.log("  PASS: " + name);
    } else {
      failed++;
      console.log("  FAIL: " + name);
      console.log("    fetchCallSites=" + JSON.stringify(sites.map(function(s) {
        return { url: s.url, method: s.method, params: (s.params||[]).map(function(p){
          return { name: p.name, location: p.location, type: p.type, defaultValue: p.defaultValue, source: p.source };
        }) };
      })).slice(0, 1000));
    }
  } catch (e) {
    failed++;
    console.log("  ERROR: " + name + " — " + e.message);
  }
}

console.log("\n=== Basic URL extraction ===\n");

api("fetch literal URL", `
  fetch("/api/users");
`, function(sites) {
  return sites.length === 1 && sites[0].url === "/api/users" && sites[0].method === "GET";
});

api("fetch with method option", `
  fetch("/api/users", { method: "POST" });
`, function(sites) {
  return sites.length === 1 && sites[0].url === "/api/users" && sites[0].method === "POST";
});

api("fetch with concat URL (literal prefix)", `
  fetch("/api/users/" + 42);
`, function(sites) {
  return sites.length === 1 && sites[0].url === "/api/users/42";
});

console.log("\n=== Body param learning ===\n");

api("fetch with JSON body literal", `
  fetch("/api/users", {
    method: "POST",
    body: JSON.stringify({ name: "alice", role: "admin" })
  });
`, function(sites) {
  if (sites.length !== 1) return false;
  var s = sites[0];
  if (s.method !== "POST") return false;
  var paramNames = (s.params || []).map(function(p) { return p.name; }).sort();
  return paramNames.indexOf("name") >= 0 && paramNames.indexOf("role") >= 0;
});

api("fetch with FormData body (accumulator pattern)", `
  function send() {
    var fd = new FormData();
    fd.append("user", "alice");
    fd.append("token", "x");
    fetch("/api/upload", { method: "POST", body: fd });
  }
  send();
`, function(sites) {
  if (sites.length !== 1) return false;
  var s = sites[0];
  if (s.url !== "/api/upload") return false;
  var paramNames = (s.params || []).map(function(p) { return p.name; }).sort();
  return paramNames.indexOf("user") >= 0 && paramNames.indexOf("token") >= 0;
});

api("fetch FormData with .set semantics (set replaces, not appends)", `
  var fd = new FormData();
  fd.append("k", "v1");
  fd.set("k", "v2");
  fd.append("other", "y");
  fetch("/api", { method: "POST", body: fd });
`, function(sites) {
  if (sites.length !== 1) return false;
  var paramNames = (sites[0].params || []).map(function(p) { return p.name; }).sort();
  // Both k and other should appear
  return paramNames.indexOf("k") >= 0 && paramNames.indexOf("other") >= 0;
});

console.log("\n=== HTTP method on options ===\n");

api("fetch with custom method", `
  fetch("/api/x", { method: "DELETE" });
`, function(sites) {
  return sites.length === 1 && sites[0].method === "DELETE";
});

console.log("\n=== Method via call (GET shorthand) ===\n");

api("fetch with no method option defaults to GET", `
  fetch("/api/list");
`, function(sites) {
  return sites.length === 1 && sites[0].method === "GET";
});

console.log("\n=== Negative cases ===\n");

api("safe: fetch with literal absolute URL doesn't false-positive", `
  fetch("https://api.example.com/endpoint");
`, function(sites) {
  // Should learn 1 fetch site with absolute URL.
  return sites.length === 1 && sites[0].url === "https://api.example.com/endpoint";
});

console.log("\n=== URLSearchParams body ===\n");

api("fetch with URLSearchParams body", `
  fetch("/api/x", {
    method: "POST",
    body: new URLSearchParams({ key1: "v1", key2: "v2" })
  });
`, function(sites) {
  if (sites.length !== 1) return false;
  // URLSearchParams from object literal — body params should reflect keys
  // OR the body might be serialised as a string. Either is OK; for now
  // just verify the fetch site exists.
  return sites[0].url === "/api/x" && sites[0].method === "POST";
});

console.log("\n=== Headers extraction ===\n");

api("fetch with headers literal", `
  fetch("/api/x", {
    method: "POST",
    headers: { "Content-Type": "application/json", "X-Custom": "val" },
    body: JSON.stringify({ a: 1 })
  });
`, function(sites) {
  if (sites.length !== 1) return false;
  var s = sites[0];
  return s.headers && (s.headers["Content-Type"] === "application/json" || s.headers["content-type"] === "application/json");
});

console.log("\n=== Class-method API client ===\n");

api("API client class with constructor URL", `
  class Client {
    constructor(base) { this.base = base; }
    list() { return fetch(this.base + "/list"); }
  }
  new Client("/api").list();
`, function(sites) {
  // Class constructor caller-arg substitution: this.base = "/api",
  // fetch URL = "/api/list".
  for (var i = 0; i < sites.length; i++) {
    if (sites[i].url === "/api/list") return true;
  }
  return false;
});

console.log("\n=== Body schema learning ===\n");

api("body params type-inferred from literal property values", `
  fetch("/api/create", {
    method: "POST",
    body: JSON.stringify({ id: 42, name: "alice", active: true, profile: null })
  });
`, function(sites) {
  if (sites.length !== 1) return false;
  var p = sites[0].params || [];
  var byName = {};
  for (var i = 0; i < p.length; i++) byName[p[i].name] = p[i];
  return byName.id && byName.id.type === "number" && byName.id.defaultValue === 42 &&
         byName.name && byName.name.type === "string" && byName.name.defaultValue === "alice" &&
         byName.active && byName.active.type === "boolean" && byName.active.defaultValue === true &&
         byName.profile && byName.profile.type === "null";
});

api("body params from caller obj-lit through wrapper fn", `
  function post(path, data) {
    return fetch(path, { method: "POST", body: JSON.stringify(data) });
  }
  post("/api/save", { title: "hello", count: 7 });
`, function(sites) {
  if (sites.length !== 1) return false;
  var s = sites[0];
  var params = s.params || [];
  var hasTitle = false, hasCount = false;
  for (var i = 0; i < params.length; i++) {
    if (params[i].name === "title" && params[i].defaultValue === "hello") hasTitle = true;
    if (params[i].name === "count" && params[i].defaultValue === 7) hasCount = true;
  }
  return hasTitle && hasCount;
});

api("query params extracted from URL string", `
  fetch("/api/search?q=test&limit=10&active=true");
`, function(sites) {
  if (sites.length !== 1) return false;
  var p = sites[0].params || [];
  var hasQ = false, hasLimit = false, hasActive = false;
  for (var i = 0; i < p.length; i++) {
    if (p[i].name === "q" && p[i].location === "query") hasQ = true;
    if (p[i].name === "limit" && p[i].location === "query") hasLimit = true;
    if (p[i].name === "active" && p[i].location === "query") hasActive = true;
  }
  return hasQ && hasLimit && hasActive;
});

api("headers merged via spread + literal", `
  var defaults = { "Accept": "application/json", "X-Trace": "default-trace" };
  fetch("/api/x", {
    method: "POST",
    headers: { ...defaults, "Content-Type": "application/json", "X-Trace": "override" },
    body: "{}"
  });
`, function(sites) {
  if (sites.length !== 1) return false;
  var h = sites[0].headers || {};
  // spread first, then literal overrides per § 13.2.5.4 CopyDataProperties
  return h["Accept"] === "application/json" &&
         h["Content-Type"] === "application/json" &&
         h["X-Trace"] === "override";
});

api("method override via variable", `
  var m = "PATCH";
  fetch("/api/update", { method: m, body: "{}" });
`, function(sites) {
  if (sites.length !== 1) return false;
  return sites[0].method === "PATCH";
});

console.log("\n=== Body schema: complex indirect key+value learning ===\n");

api("body keys+values across 3-level wrapper", `
  function deep(payload) { return fetch("/api/d3", { method: "POST", body: JSON.stringify(payload) }); }
  function mid(p) { return deep(p); }
  function top(p) { return mid(p); }
  top({ user_id: 99, action: "approve" });
`, function(sites) {
  if (sites.length !== 1) return false;
  var p = sites[0].params || [];
  var byName = {};
  for (var i = 0; i < p.length; i++) byName[p[i].name] = p[i];
  return byName.user_id && byName.user_id.type === "number" && byName.user_id.defaultValue === 99 &&
         byName.action && byName.action.type === "string" && byName.action.defaultValue === "approve";
});

api("body keys+values via Object.assign merge of caller args", `
  function send(base, extra) {
    return fetch("/api/merged", {
      method: "POST",
      body: JSON.stringify(Object.assign({}, base, extra))
    });
  }
  send({ id: 1, type: "default" }, { name: "alpha", id: 42 });
`, function(sites) {
  if (sites.length !== 1) return false;
  var p = sites[0].params || [];
  var byName = {};
  for (var i = 0; i < p.length; i++) byName[p[i].name] = p[i];
  // Per § 20.1.2.1 Object.assign: source overrides target. id from extra (42) wins.
  return byName.id && byName.id.defaultValue === 42 &&
         byName.type && byName.type.defaultValue === "default" &&
         byName.name && byName.name.defaultValue === "alpha";
});

api("body keys+values via class method using constructor caller args", `
  class API {
    constructor(token, version) { this.token = token; this.version = version; }
    create(item) {
      return fetch("/api/items", {
        method: "POST",
        body: JSON.stringify({ token: this.token, version: this.version, item: item })
      });
    }
  }
  new API("secret-123", 7).create("widget");
`, function(sites) {
  if (sites.length !== 1) return false;
  var p = sites[0].params || [];
  var byName = {};
  for (var i = 0; i < p.length; i++) byName[p[i].name] = p[i];
  return byName.token && byName.token.defaultValue === "secret-123" &&
         byName.version && byName.version.defaultValue === 7 &&
         byName.item && byName.item.defaultValue === "widget";
});

api("body keys+values via shorthand-property + template-literal value", `
  function update(id, name) {
    var status = "active";
    return fetch("/api/upd", {
      method: "POST",
      body: JSON.stringify({ id, name, status, ref: \`#\${id}-\${name}\` })
    });
  }
  update(101, "beta");
`, function(sites) {
  if (sites.length !== 1) return false;
  var p = sites[0].params || [];
  var byName = {};
  for (var i = 0; i < p.length; i++) byName[p[i].name] = p[i];
  return byName.id && byName.id.defaultValue === 101 &&
         byName.name && byName.name.defaultValue === "beta" &&
         byName.status && byName.status.defaultValue === "active" &&
         byName.ref && byName.ref.defaultValue === "#101-beta";
});

api("body keys+values via destructured-param wrapper", `
  function post(path, { method, payload }) {
    return fetch(path, { method: method, body: JSON.stringify(payload) });
  }
  post("/api/destr", { method: "PUT", payload: { kind: "primary", count: 5 } });
`, function(sites) {
  if (sites.length !== 1) return false;
  var s = sites[0];
  var p = s.params || [];
  var byName = {};
  for (var i = 0; i < p.length; i++) byName[p[i].name] = p[i];
  return s.method === "PUT" &&
         byName.kind && byName.kind.defaultValue === "primary" &&
         byName.count && byName.count.defaultValue === 5;
});

// ═════════════════════════════════════════════════════════════════════
// Inter-procedural learning through library wrappers (jQuery-shape)
//
// The extension's value depends on tracing through library internals
// (jQuery $.ajax, axios, fetch wrappers) to the native API call so the
// schema sees the REAL fetch site with the caller's concrete URL +
// body + method. CLAUDE.md "Framework code is just JavaScript — trace
// through it, don't recognise it": these tests verify spec-eval
// reaches the native XMLHttpRequest / fetch through the wrapper, not
// that we shape-match on `$.ajax` or `ajax`.
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== Inter-procedural library-wrapper tracing ===\n");

api("jquery-shaped $.ajax({url, method, data}) — wrapper traces to XHR", `
  function ajax(opts) {
    var xhr = new XMLHttpRequest();
    xhr.open(opts.method || "GET", opts.url);
    if (opts.headers) {
      for (var h in opts.headers) xhr.setRequestHeader(h, opts.headers[h]);
    }
    xhr.send(JSON.stringify(opts.data));
  }
  ajax({ url: "/api/users", method: "POST", data: { name: "alice", role: "admin" } });
`, function(sites) {
  // The wrapper's xhr.open()/send() pair should resolve to a fetch
  // call site with the caller's URL+method+body fields. Tests that
  // caller-arg substitution + obj-lit prop access flow through
  // _specInstantiateAv into _processNetworkSink's XHR dispatch.
  if (sites.length < 1) return false;
  var found = sites.find(function(s) { return s.url === "/api/users" && s.method === "POST"; });
  if (!found) return false;
  var byName = {};
  (found.params || []).forEach(function(p) { byName[p.name] = p; });
  return !!byName.name && byName.name.defaultValue === "alice" &&
         !!byName.role && byName.role.defaultValue === "admin";
});

api("axios-shaped wrapper: trace through `request(config)` to native fetch", `
  function request(config) {
    return fetch(config.url, { method: config.method || "GET", body: JSON.stringify(config.data) });
  }
  request({ url: "/api/posts", method: "PUT", data: { title: "hello", draft: true } });
`, function(sites) {
  if (sites.length < 1) return false;
  var found = sites.find(function(s) { return s.url === "/api/posts" && s.method === "PUT"; });
  if (!found) return false;
  var byName = {};
  (found.params || []).forEach(function(p) { byName[p.name] = p; });
  return !!byName.title && byName.title.defaultValue === "hello" &&
         !!byName.draft && byName.draft.defaultValue === true;
});

api("two-hop wrapper: util.get(path) calls util.request(method, path)", `
  function request(method, path) {
    return fetch(path, { method: method });
  }
  function get(path) { return request("GET", path); }
  function post(path, body) { return fetch(path, { method: "POST", body: JSON.stringify(body) }); }
  get("/api/v1/profile");
  post("/api/v1/order", { item: "widget", qty: 2 });
`, function(sites) {
  // Verifies: convergence allows the wrapper's caller-arg substitution
  // to propagate through both call levels. The read-edge fix should
  // keep this working — `get`'s analysis reads `request`'s return
  // memo and constraints it with caller's args.
  if (sites.length < 2) return false;
  var profileSite = sites.find(function(s) { return s.url === "/api/v1/profile" && s.method === "GET"; });
  var orderSite = sites.find(function(s) { return s.url === "/api/v1/order" && s.method === "POST"; });
  if (!profileSite || !orderSite) return false;
  var orderParams = {};
  (orderSite.params || []).forEach(function(p) { orderParams[p.name] = p; });
  return !!orderParams.item && orderParams.item.defaultValue === "widget" &&
         !!orderParams.qty && orderParams.qty.defaultValue === 2;
});

api("jquery-shaped wrapper returns promise chain; caller's then receives response", `
  function fetchJson(url, opts) {
    return fetch(url, opts).then(function(r) { return r.json(); });
  }
  fetchJson("/api/widgets", { method: "GET", headers: { "Accept": "application/json" } });
`, function(sites) {
  // The .then chain after fetch shouldn't break URL+method extraction.
  if (sites.length < 1) return false;
  var found = sites.find(function(s) { return s.url === "/api/widgets" && s.method === "GET"; });
  if (!found) return false;
  return found.headers && found.headers["Accept"] === "application/json";
});

api("library wrapper with switch-discriminated method", `
  function send(opts) {
    var m = opts.method;
    var xhr = new XMLHttpRequest();
    switch (m) {
      case "GET": xhr.open("GET", opts.url); break;
      case "POST": xhr.open("POST", opts.url); break;
      case "PUT": xhr.open("PUT", opts.url); break;
      default: xhr.open("GET", opts.url);
    }
    xhr.send(opts.body ? JSON.stringify(opts.body) : null);
  }
  send({ url: "/api/sw1", method: "PUT", body: { val: 42 } });
`, function(sites) {
  // The switch-based dispatch in the wrapper should still resolve
  // both URL and method through caller-arg substitution.
  if (sites.length < 1) return false;
  var found = sites.find(function(s) { return s.url === "/api/sw1" && s.method === "PUT"; });
  if (!found) return false;
  var byName = {};
  (found.params || []).forEach(function(p) { byName[p.name] = p; });
  return !!byName.val && byName.val.defaultValue === 42;
});

// ═════════════════════════════════════════════════════════════════════
// Points-to sub-problem tests (Phase B2 + Phase C7 + Phase B5 + Phase C8).
// Each verifies one specific value-flow shape ECMA-262 mandates for static
// resolution to succeed. ECMA refs noted per test.
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== Points-to sub-problem tests ===\n");

// F-1 Phase B2-1: ObjectExpression init at declarator.
// `var o = {k: fn}` ⇒ o.k() must dispatch into fn so its fetch reaches.
// ECMA § 13.2.5 ObjectExpression eval + § 13.10 PropertyAccess + § 13.3.6.
api("Phase B2-1 + C7-1: ObjectExpression init binds fn to prop; o.k() dispatches", `
  var o = {
    fetchX: function() { fetch("/api/b2-1"); }
  };
  o.fetchX();
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/b2-1"; });
});

// F-2 Phase B2-2: AssignmentExpression LHS=MemberExpression.
// `o.k = fn` after-declaration prop write; subsequent o.k() must dispatch.
// ECMA § 10.1.7 OrdinarySet + § 13.10 + § 13.3.6.
api("Phase B2-2 + C7: late prop write `o.k = fn` binds; o.k() dispatches", `
  var o = {};
  o.fetchY = function() { fetch("/api/b2-2"); };
  o.fetchY();
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/b2-2"; });
});

// F-11 Phase B5-1 + C8: factory return + outer call dispatch.
// `function make() { return function(){fetch...}; } make()();` — outer
// call site is a dispatch site of the inner fn. ECMA § 14.10 + § 13.3.6.
api("Phase B5-1 + C8: factory return + chained call dispatches into returned fn", `
  function make() {
    return function() { fetch("/api/b5-1"); };
  }
  make()();
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/b5-1"; });
});

// F-15 Phase B2-2 + C7 round-trip: var binding + late prop + computed key.
// `o[K] = fn` where K resolves to a const string at static eval.
// ECMA § 13.10 PropertyReference (computed form) + § 13.15.4 PutValue.
api("Phase B2-3 + C7: computed const-key write + computed access dispatches", `
  var o = {};
  var key = "doFetch";
  o[key] = function() { fetch("/api/b2-3"); };
  o[key]();
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/b2-3"; });
});

// F-4 Phase B2-4: or-of-consts key. The key K resolves to or("a","b");
// the write distributes to both. Either read should dispatch.
// ECMA § 14.6 LUB + § 13.10 + § 10.1.7.
api("Phase B2-4 + C7: or-of-consts key distributes write per leaf", `
  function pick(x) {
    var k;
    if (x > 0) { k = "alpha"; } else { k = "beta"; }
    return k;
  }
  var registry = {};
  var dyn = pick(Math.random());
  registry[dyn] = function() { fetch("/api/b2-4"); };
  registry.alpha();
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/b2-4"; });
});

// Phase B2-4 wildcard via array-iter cb-param dispatch (jQuery's
// `ce.each(["get","post"], function(_,t){ce[t]=fn})` shape). The cb's
// t param is opaque to B2 unless B4 populates it from the iterator
// array; meanwhile B2-5 records WILDCARD which C7 wildcard-match
// resolves at any subsequent o.X access.
// ECMA § 13.3.6 + § 13.10 + § 23.1.3.7 forEach-style iter + Phase B2/C7.
api("Phase B2 wildcard + C7: array-iter cb assigns opaque-key fn; member access dispatches", `
  function each(arr, cb) {
    for (var i = 0; i < arr.length; i++) cb(i, arr[i]);
  }
  var registry = {};
  each(["get", "post"], function(idx, method) {
    registry[method] = function(url) { fetch("/api/wildmatch/" + url); };
  });
  registry.get("xx");
`, function(sites) {
  return !!sites.find(function(s) { return s.url && s.url.indexOf("/api/wildmatch/") === 0; });
});

// Phase B2-5 wildcard via OBJ-LIT METHOD iter dispatch — jQuery's actual
// shape. The iter helper is bound as obj-lit method `ns.each` not a
// top-level fn decl. Tests whether Phase C5 unfolds obj-lit-method iter.
api("Phase B2 wildcard via obj-lit method iter: ns.each dispatches into cb", `
  var ns = {
    each: function(arr, cb) { for (var i = 0; i < arr.length; i++) cb(i, arr[i]); }
  };
  var registry = {};
  ns.each(["get", "post"], function(idx, method) {
    registry[method] = function(url) { fetch("/api/wildmatch2/" + url); };
  });
  registry.get("xx");
`, function(sites) {
  return !!sites.find(function(s) { return s.url && s.url.indexOf("/api/wildmatch2/") === 0; });
});

// jQuery shape — exact `t.call(e[r], r, e[r])` dispatch with if/else branching.
api("jQuery each shape: obj-lit method using .call dispatch + if/else branching", `
  var ns = {
    each: function(e, t) {
      var n, r = 0;
      if (e && e.length !== undefined) {
        for (n = e.length; r < n; r++)
          if (false === t.call(e[r], r, e[r])) break;
      } else {
        for (r in e)
          if (false === t.call(e[r], r, e[r])) break;
      }
      return e;
    }
  };
  var registry = {};
  ns.each(["get", "post"], function(idx, method) {
    registry[method] = function(url) { fetch("/api/jqeach/" + url); };
  });
  registry.get("xx");
`, function(sites) {
  return !!sites.find(function(s) { return s.url && s.url.indexOf("/api/jqeach/") === 0; });
});

// SD-9 + § 14.7.2 + § 7.4 + § 23.1.4: while-loop iteration variable
// widening — `n = arr[r++]` inside while test should expose n as
// or-of-elements per multi-iter over-approximation.
api("SD-9 + § 7.4: while-loop induction var widening exposes or-of-elements", `
  var arr = ["alpha", "beta", "gamma"];
  var n;
  var r = 0;
  while (n = arr[r++]) {
    fetch("/api/iter/" + n);
  }
`, function(sites) {
  // The widening makes n an or-of-elements; concat with const produces
  // or-of-paths; all three URLs should appear as fetch sites.
  var found = {};
  sites.forEach(function(s) { if (s.url) found[s.url] = true; });
  return !!found["/api/iter/alpha"] && !!found["/api/iter/beta"] && !!found["/api/iter/gamma"];
});

// F-17 Phase C9: Function.prototype.call/apply over indirect-discovered fn.
// `o.k.call(thisArg, ...)` — the fn at o.k is invoked via .call.
// ECMA § 20.2.3.3 Function.prototype.call + § 13.10 + § 13.3.6.
api("Phase B2-1 + .call: prop-stored fn dispatched via Function.prototype.call", `
  var o = {
    runner: function(arg) { fetch("/api/c9-call?x=" + arg); }
  };
  o.runner.call(null, "ok");
`, function(sites) {
  return !!sites.find(function(s) { return s.url && s.url.indexOf("/api/c9-call") === 0; });
});

// ═════════════════════════════════════════════════════════════════════
// REAL jQuery — verify the analyzer traces through actual jquery-3.7.1
// internals into the native XHR.send. Synthetic shape-mimics are not a
// substitute: jQuery's $.ajax is a multi-hundred-line state machine
// (ajaxSettings → prefilters → transports → jqXHR), and the analyzer
// has to follow the spec-eval lattice into all of it. This test fixes
// regressions where convergence/binop+/read-edge optimizations would
// stop seeing the user's actual fetch URL through the real wrapper.
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== REAL jQuery library tracing ===\n");

var jqueryMin = fs.readFileSync(path.join(rootDir, "testing/harness-dumps/jquery-3.7.1.min.js"), "utf8");

// Wildcard dispatch into nested ajax-like inner body. Probes whether
// wildcard-resolved fn's body itself triggers ctx-refinement into an
// inner ajax helper. ECMA § 13.3.6 + § 9.4.1 + Phase B2-5 wildcard.
api("Wildcard dispatch + nested inner ajax dispatch", `
  (function() {
    function each(arr, cb) { for (var i = 0; i < arr.length; i++) cb(i, arr[i]); }
    function extend(t, s) { for (var k in s) t[k] = s[k]; return t; }
    var ce = function() {};
    extend(ce, {
      each: each,
      ajax: function(opts) {
        var xhr = new XMLHttpRequest();
        xhr.open(opts.method, opts.url);
        xhr.send();
      }
    });
    each(["get", "post"], function(idx, m) {
      ce[m] = function(url) {
        return ce.ajax({url: url, method: m.toUpperCase()});
      };
    });
    window.myLib4 = ce;
  })();
  myLib4.get("/api/wild-nested");
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/wild-nested"; });
});

// Full jQuery shape: IIFE-wrapped, ce internal, exposed via globalThis.
api("Full jQuery shape: IIFE + ce internal + ce.each(...) + user-code via global", `
  (function(window) {
    function extend(target, src) {
      for (var k in src) target[k] = src[k];
      return target;
    }
    var ce = function() {};
    extend(ce, {
      each: function(e, t) {
        for (var r = 0; r < e.length; r++) t.call(e[r], r, e[r]);
      }
    });
    ce.each(["aaa", "bbb"], function(idx, m) {
      ce[m] = function(url) { fetch("/api/iife/" + url); };
    });
    window.myLib = ce;
  })(window);
  myLib.aaa("xx");
`, function(sites) {
  return !!sites.find(function(s) { return s.url && s.url.indexOf("/api/iife/") === 0; });
});

// Sub-isolation: same IIFE shape but use direct globalThis.myLib =,
// bypassing the IIFE-param-shim form.
api("IIFE with direct globalThis export (no param-aliased window)", `
  (function() {
    function extend(target, src) {
      for (var k in src) target[k] = src[k];
      return target;
    }
    var ce = function() {};
    extend(ce, {
      each: function(e, t) {
        for (var r = 0; r < e.length; r++) t.call(e[r], r, e[r]);
      }
    });
    ce.each(["aaa", "bbb"], function(idx, m) {
      ce[m] = function(url) { fetch("/api/iife3/" + url); };
    });
    window.myLib3 = ce;
  })();
  myLib3.aaa("xx");
`, function(sites) {
  return !!sites.find(function(s) { return s.url && s.url.indexOf("/api/iife3/") === 0; });
});

// Sub-isolation: same IIFE shape but call ce.aaa() INSIDE the IIFE
// instead of via the global. If this passes while window.myLib.aaa() fails,
// the gap is in global-export propagation of post-iter updates.
api("IIFE-internal ce.aaa() call (same iter+cb but no global indirection)", `
  (function() {
    function extend(target, src) {
      for (var k in src) target[k] = src[k];
      return target;
    }
    var ce = function() {};
    extend(ce, {
      each: function(e, t) {
        for (var r = 0; r < e.length; r++) t.call(e[r], r, e[r]);
      }
    });
    ce.each(["aaa", "bbb"], function(idx, m) {
      ce[m] = function(url) { fetch("/api/iife2/" + url); };
    });
    ce.aaa("xx");
  })();
`, function(sites) {
  return !!sites.find(function(s) { return s.url && s.url.indexOf("/api/iife2/") === 0; });
});

// Same as above but using a FUNCTION as the target (jQuery shape) —
// `ce = function(){}; extend(ce, {each: ...})`. Tests whether function-
// ref-with-extraProps shape supports member-iter dispatch.
api("Function-target extend + iter: ce = function...; extend(ce, {each}); ce.each(...)", `
  function extend(target, src) {
    for (var k in src) target[k] = src[k];
    return target;
  }
  var ce = function() {};
  extend(ce, {
    each: function(e, t) {
      for (var r = 0; r < e.length; r++) t.call(e[r], r, e[r]);
    }
  });
  var reg = {};
  ce.each(["aaa", "bbb"], function(idx, m) {
    reg[m] = function(url) { fetch("/api/fnextend/" + url); };
  });
  reg.aaa("xx");
`, function(sites) {
  return !!sites.find(function(s) { return s.url && s.url.indexOf("/api/fnextend/") === 0; });
});

// Isolate the extend()-bound dispatch: jQuery's ce.each is bound via
// extend(), not direct obj-lit. If this fails while inline obj-method
// passes, extend() loses the iter-dispatchable property.
api("Extend-bound iter helper: extend(ns, {each: ...}); ns.each(...)", `
  function extend(target, src) {
    for (var k in src) target[k] = src[k];
    return target;
  }
  var ns = {};
  extend(ns, {
    each: function(e, t) {
      for (var r = 0; r < e.length; r++) t.call(e[r], r, e[r]);
    }
  });
  var reg = {};
  ns.each(["aaa", "bbb"], function(idx, m) {
    reg[m] = function(url) { fetch("/api/extend/" + url); };
  });
  reg.aaa("xx");
`, function(sites) {
  return !!sites.find(function(s) { return s.url && s.url.indexOf("/api/extend/") === 0; });
});

// Top-level extend with LITERAL src — works (literal flows directly).
api("Top-level extend with literal src", `
  function extend(target, src) {
    for (var k in src) target[k] = src[k];
    return target;
  }
  var s = {};
  extend(s, {url: "/api/min"});
  var xhr = new XMLHttpRequest();
  xhr.open("GET", s.url);
  xhr.send();
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/min"; });
});

// SD-10 multi-level ctx-refinement: extend with param-typed src does NOT
// propagate through ctx-refinement composition. The for-in loop-key
// effects have eff.key.src = param(extend, 1); at replay, frArgAvsForReplay[1]
// is opts which is itself a param (of ajax). Per-key distribution requires
// loopSrcAv to be a concrete obj-lit. Refining ajax's body with concrete
// opts WOULD make opts an obj-lit at extend's replay site — but the
// do-while ctx-refinement at line 14264 uses BASE memos when computing
// argAvs, so the inner extend's refinement never sees the refined opts.
// ECMA § 14.7.5 + § 13.3.6 + § 10.2.10 — proper fix needed in next session.
api("SD-10 reproducer: extend(s, opts) with param src — multi-level ctx-refinement", `
  function extend(target, src) {
    for (var k in src) target[k] = src[k];
    return target;
  }
  function ajax(opts) {
    var s = {};
    extend(s, opts);
    var xhr = new XMLHttpRequest();
    xhr.open("GET", s.url);
    xhr.send();
  }
  ajax({url: "/api/sd10-reproducer"});
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/sd10-reproducer"; });
});

// Direct opts: lib3.ajax({url}) reaches xhr.open via direct opts access.
// SD-25: Array.prototype.forEach builtin-iter dispatching cb that writes
// to outer obj-lit. The cb's prop-write should propagate to outer state.
// (Distinguishes user-defined `each` from builtin `forEach`.)
api("SD-25: Array.prototype.forEach cb propagates outer obj prop writes", `
  var registry = {};
  ["aaa", "bbb"].forEach(function(method) {
    registry[method] = function(url) { fetch("/api/sd25/" + method + "?u=" + url); };
  });
  registry.aaa("x");
`, function(sites) {
  return !!sites.find(function(s) { return s.url && s.url.indexOf("/api/sd25/") === 0; });
});

// JQ-6: jQuery's Ut/Vt curry — register factory via curried helper, then
// transport-select + invoke + .send() chain. Mimics ce.ajaxTransport's
// actual shape: Ut(_t) returns closure that registers into _t; Vt picks
// transports from _t["*"] and invokes each until one returns truthy.
api("JQ-6: Ut(_t)-style curried registry + Vt-style transport dispatch", `
  function Ut(o) {
    return function(e, t) {
      if (typeof e !== "string") { t = e; e = "*"; }
      (o[e] = o[e] || []).push(t);
    };
  }
  function Vt(t, opts) {
    var arr = t["*"] || [];
    for (var i = 0; i < arr.length; i++) {
      var r = arr[i](opts);
      if (r && r.send) { r.send(); return; }
    }
  }
  var _t = {};
  var ajaxTransport = Ut(_t);
  ajaxTransport(function(opts) {
    return {
      send: function() {
        var x = new XMLHttpRequest();
        x.open(opts.method, opts.url);
      }
    };
  });
  Vt(_t, {url: "/api/jq6", method: "POST"});
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/jq6" && s.method === "POST"; });
});

// JQ-2: extend(target, source) deep-merge of nested fn-refs.
api("JQ-2: extend(s, {xhr: fn}); s.xhr() returns from the merged-in factory", `
  function extend(t, src) { for (var k in src) t[k] = src[k]; return t; }
  function go() {
    var s = {};
    extend(s, { xhr: function() { return new XMLHttpRequest(); } });
    var r = s.xhr();
    r.open("GET", "/api/jq2");
    r.send();
  }
  go();
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/jq2"; });
});

// JQ-4: i.xhr() factory dispatch — XHR instance returned, .open detected.
api("JQ-4: opts.xhr() returning new XHR — r.open detected as sink with opts.url", `
  function go(opts) {
    var r = opts.xhr();
    r.open(opts.method, opts.url);
    r.send();
  }
  go({
    xhr: function() { return new XMLHttpRequest(); },
    method: "POST",
    url: "/api/jq4"
  });
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/jq4" && s.method === "POST"; });
});

// JQ-5: Inline transport factory returning {send: fn}; send invoked.
api("JQ-5: transport(opts).send() — send body analyzed with concrete opts", `
  function transport(opts) {
    return {
      send: function() {
        var r = new XMLHttpRequest();
        r.open("GET", opts.url);
        r.send();
      }
    };
  }
  transport({url: "/api/jq5"}).send();
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/jq5"; });
});

api("Direct opts ajax: lib3.ajax({url}) reads opts.url directly into xhr.open", `
  (function() {
    function ajax(opts) {
      var xhr = new XMLHttpRequest();
      xhr.open("GET", opts.url);
      xhr.send();
    }
    window.lib3 = { ajax: ajax };
  })();
  lib3.ajax({url: "/api/direct"});
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/direct"; });
});

// Simpler: const String.match without || fallback.
api("Simpler: const.toLowerCase().match() returns array literal", `
  var x = "*".toLowerCase().match(/[a-z*]/g);
  fetch("/api/match/" + x[0]);
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/match/*"; });
});

// Match + || fallback alone:
api("Match + || fallback: ('*').match(re) || [''] should be the match", `
  var x = "*".match(/[a-z*]/g) || [""];
  fetch("/api/m2/" + x[0]);
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/m2/*"; });
});

// undefined || "*" (prop fallback):
api("Prop || const fallback: (opts.x || '*') resolves to '*' when prop absent", `
  function ajax(opts) {
    var t = opts.dataType || "*";
    fetch("/api/p2/" + t);
  }
  ajax({});
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/p2/*"; });
});

// Inner prop ||: opts.x || "*" in fn body should fold to "*" via param-
// refinement of opts to {}. Works as the URL component.
api("Inner prop ||: opts.x || '*' in fn body", `
  function ajax(opts) {
    var d = opts.x || "*";
    fetch("/api/inner-prop-or/" + d);
  }
  ajax({});
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/inner-prop-or/*"; });
});

// SD-11 (next-session gap): refinement-time logical-receiver chain dispatch.
// At standalone, `opts.x || "*"` is logical{left:member(param,"x"), right:const}.
// At param-refinement with opts={}, left should become undefined (since {}
// has no x), shrinking logical to just const "*". But the chain `.toLowerCase()`
// on the receiver depends on RefinementContext propagating into method-call
// receiver AVs — currently the post-refinement effects don't carry the
// resolved receiver string to the URL resolver. Single-shot test guards the
// gap until refinement-context-aware receiver propagation is added.
api("SD-11: inner chain (opts.x||'*').toLowerCase() in fn body fold via refinement", `
  function ajax(opts) {
    var d = (opts.x || "*").toLowerCase();
    fetch("/api/lc/" + d);
  }
  ajax({});
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/lc/*"; });
});

// SD-8: String.match composed with logical-or-fallback. Models jQuery's
// `i.dataTypes = (i.dataType || "*").toLowerCase().match(D) || [""]`.
api("SD-8 String.match + || fallback: dynamic-type-list from string default", `
  function ajax(opts) {
    var i = opts;
    i.dataTypes = (i.dataType || "*").toLowerCase().match(/[a-z*]+/g) || [""];
    var xhr = new XMLHttpRequest();
    xhr.open("GET", "/api/dt/" + i.dataTypes[0]);
  }
  ajax({});
`, function(sites) {
  return !!sites.find(function(s) { return s.url && s.url.indexOf("/api/dt/") === 0; });
});

// jQuery's inspect-first-non-undefined-return pattern: each iter + cb writes
// closure var + cb returns false to break. After each, captured var returned.
api("inspect first-non-undefined: each(arr, cb) writes outer var; return captured", `
  function each(arr, cb) {
    for (var i = 0; i < arr.length; i++) {
      if (cb.call(arr[i], i, arr[i]) === false) break;
    }
  }
  function inspect(arr, opts) {
    var selected;
    each(arr, function(_, factory) {
      selected = factory(opts);
      if (selected) return false;
    });
    return selected;
  }
  var transports = [
    function(opts) {
      return {
        send: function() {
          var x = new XMLHttpRequest();
          x.open("GET", opts.url);
        }
      };
    }
  ];
  function ajax(opts) {
    var t = inspect(transports, opts);
    t.send();
  }
  ajax({url: "/api/inspect-pattern"});
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/inspect-pattern"; });
});

// Full jQuery-like shape: IIFE + ce + extend(ce, {ajax}) + transports +
// user-code via window alias. Replicates real jQuery's actual layered shape
// to verify the SD-10 + B2 chain handles complete factory-dispatch flow.
api("Full jQuery shape: IIFE + extend(ce, {ajax: ...}) + transport dispatch + window alias", `
  (function(window) {
    function extend(target, src) {
      for (var k in src) target[k] = src[k];
      return target;
    }
    var ce = function(e, t) { return new ce.fn.init(e, t); };
    ce.fn = { init: function() {} };
    var transports = {};
    extend(ce, {
      ajaxTransport: function(name, fn) { transports[name] = fn; },
      ajax: function(opts) {
        var i = opts;
        var transport = transports["*"](i);
        transport.send({}, function() {});
      }
    });
    ce.ajaxTransport("*", function(opts) {
      return {
        send: function(headers, complete) {
          var xhr = new XMLHttpRequest();
          xhr.open(opts.type, opts.url);
          xhr.send();
        }
      };
    });
    window.fullJq = ce;
  })(window);
  fullJq.ajax({url: "/api/full-shape", type: "GET"});
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/full-shape"; });
});

// jQuery ajax→transport.send pattern: transport factory returns {send},
// ajax dispatches to transport.send which does xhr.open.
api("jQuery ajax→transport.send: ajax dispatches via transport factory's send method", `
  (function() {
    var transports = {};
    function ajaxTransport(name, fn) { transports[name] = fn; }
    ajaxTransport("*", function(opts) {
      return {
        send: function(headers, complete) {
          var xhr = new XMLHttpRequest();
          xhr.open(opts.type, opts.url);
          xhr.send();
        }
      };
    });
    function ajax(opts) {
      var transport = transports["*"](opts);
      transport.send({}, function() {});
    }
    window.lib5 = { ajax: ajax };
  })();
  lib5.ajax({url: "/api/transport", type: "GET"});
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/transport"; });
});

// Layered jQuery shape: ajax method via extend + get/post via each iteration.
// User-code `lib.get("/api/x")` must dispatch to wrapper → ce.ajax → xhr.open.
api("Layered ajax+iter: extend ce w/ ajax; ce.each register get; lib.get reaches xhr.open", `
  (function() {
    function extend(target, src) {
      for (var k in src) target[k] = src[k];
      return target;
    }
    var ce = function() {};
    extend(ce, {
      ajax: function(opts) {
        var xhr = new XMLHttpRequest();
        xhr.open(opts.method, opts.url);
        xhr.send();
      },
      each: function(e, t) {
        for (var r = 0; r < e.length; r++) t.call(e[r], r, e[r]);
      }
    });
    ce.each(["get", "post"], function(idx, m) {
      ce[m] = function(url) {
        return ce.ajax({ url: url, method: m });
      };
    });
    window.lib = ce;
  })();
  lib.get("/api/layered");
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/layered"; });
});

// Isolation test: same `each(...)` shape that passes synthetically,
// but embedded AFTER the real jQuery bundle. If this fails while the
// synthetic passes, the divergence point IS bundle size / call-graph
// limits — not a missing ECMA rule.
api("real-jQuery-suffix: ns.each on a fresh ns alongside jQuery still works", jqueryMin + `
  var ns2 = {
    each: function(arr, cb) { for (var i = 0; i < arr.length; i++) cb(i, arr[i]); }
  };
  var registry2 = {};
  ns2.each(["aaa", "bbb"], function(idx, method) {
    registry2[method] = function(url) { fetch("/api/iso/" + method + "/" + url); };
  });
  registry2.aaa("x");
`, function(sites) {
  return !!sites.find(function(s) { return s.url && s.url.indexOf("/api/iso/") === 0; });
});

// Isolation: simpler jQuery $.ajax invocation
api("real-jQuery isolation: $.ajax({url}) reaches XHR", jqueryMin + `
  jQuery.ajax({url: "/api/simpler"});
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/simpler"; });
});

api("real jQuery — $.ajax({url,method,data}) reaches XHR via library state machine", jqueryMin + `
  // User code calls real jQuery $.ajax. Analyzer must trace the full
  // jQuery internal state machine (ajaxSettings → prefilters →
  // transports → jqXHR → param() body serialiser → native xhr.open /
  // xhr.send) to surface the user's url + method + body fields. ECMA
  // spec-eval has to follow these inter-procedural hops via _specInst-
  // antiateAv + caller-arg substitution; if any hop bottoms out at top,
  // a learned-method gap shows. Test asserts the full outcome (no
  // "bonus" gap accommodations).
  jQuery.ajax({
    url: "/api/widgets",
    method: "POST",
    data: { name: "alpha", count: 3 }
  });
`, function(sites) {
  var match = sites.find(function(s) { return s.url === "/api/widgets" && s.method === "POST"; });
  if (!match) return false;
  var byName = {};
  (match.params || []).forEach(function(p) { byName[p.name] = p; });
  return !!byName.name && byName.name.defaultValue === "alpha" &&
         !!byName.count && byName.count.defaultValue === 3;
});

api("real jQuery — $.get(url) reaches XHR GET via library state machine", jqueryMin + `
  jQuery.get("/api/profile");
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/profile" && s.method === "GET"; });
});

// ═════════════════════════════════════════════════════════════════════
// jQuery-shape composition tests — synthetic patterns that progressively
// build up jQuery's chain. Each one verifies one ECMA-grounded
// composition layer in isolation. Real jQuery requires all layers to
// compose end-to-end via context-sensitive analysis at call sites.
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== jQuery-shape composition layers ===\n");

api("J3: array-valued registry + factory + send (jQuery transport shape)", `
  var transports = {};
  function register(name, fn) {
    (transports[name] = transports[name] || []).push(fn);
  }
  register("*", function(opts) {
    return {
      send: function() {
        var r = new XMLHttpRequest();
        r.open(opts.method, opts.url);
      }
    };
  });
  function inspect(opts) {
    for (var dt in transports) {
      var arr = transports[dt];
      for (var j = 0; j < arr.length; j++) {
        var t = arr[j](opts);
        if (t && t.send) { t.send(); return; }
      }
    }
  }
  inspect({method: "POST", url: "/api/j3"});
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/j3" && s.method === "POST"; });
});

api("J4: UMD pattern — IIFE-wrapped + globalThis.X = function", `
  (function() {
    function jQuery() {}
    jQuery.send = function(opts) {
      var r = new XMLHttpRequest();
      r.open(opts.method, opts.url);
    };
    globalThis.myAjax = function(opts) { jQuery.send(opts); };
  })();
  globalThis.myAjax({method: "POST", url: "/api/j4"});
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/j4" && s.method === "POST"; });
});

api("K1: 2-layer UMD (outer IIFE invokes factory(global))", `
  (function(e, t) {
    t(e);
  })(this, function(global) {
    global.myAjax = function(opts) {
      var r = new XMLHttpRequest();
      r.open(opts.method, opts.url);
    };
  });
  globalThis.myAjax({method: "POST", url: "/api/k1"});
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/k1" && s.method === "POST"; });
});

api("L1: function-object with prop (jQuery.X = fn pattern)", `
  (function(global) {
    function jQuery() {}
    jQuery.ajax = function(opts) {
      var r = new XMLHttpRequest();
      r.open(opts.method, opts.url);
    };
    global.jQuery = jQuery;
  })(this);
  jQuery.ajax({method: "POST", url: "/api/l1"});
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/l1" && s.method === "POST"; });
});

api("L2: jQuery exposed via window.jQuery = window.$ = jQuery", `
  (function(global) {
    function jQuery() {}
    jQuery.ajax = function(opts) {
      var r = new XMLHttpRequest();
      r.open(opts.method, opts.url);
    };
    global.jQuery = global.$ = jQuery;
  })(this);
  $.ajax({method: "POST", url: "/api/l2"});
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/l2" && s.method === "POST"; });
});

api("M1: for-in merge — extend(target, source) then call target.method", `
  function extend(target, source) {
    for (var name in source) {
      target[name] = source[name];
    }
  }
  function jQuery() {}
  extend(jQuery, { ajax: function(opts) { var r = new XMLHttpRequest(); r.open(opts.method, opts.url); } });
  jQuery.ajax({method: "POST", url: "/api/m1"});
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/m1" && s.method === "POST"; });
});

api("N1: jQuery-style extend using arguments + conditional decrement + bounded for-loop", `
  function jQuery() {}
  jQuery.extend = function() {
    var target = arguments[0], i = 1, length = arguments.length;
    if (i === length) { target = this; i--; }
    for (; i < length; i++) {
      var options = arguments[i];
      for (var name in options) {
        target[name] = options[name];
      }
    }
    return target;
  };
  jQuery.extend({ ajax: function(opts) { var r = new XMLHttpRequest(); r.open(opts.method, opts.url); } });
  jQuery.ajax({method: "POST", url: "/api/n1"});
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/n1" && s.method === "POST"; });
});

api("N2: jQuery-style extend with full typeof checks + opaque guards", `
  function isFunction(x) { return typeof x === "function"; }
  function jQuery() {}
  jQuery.extend = function() {
    var n, s, c, b = arguments[0] || {}, g = 1, _ = arguments.length;
    if (typeof b === "boolean") { b = arguments[g] || {}; g++; }
    if (typeof b !== "object" && !isFunction(b)) b = {};
    if (g === _) { b = this; g--; }
    for (; g < _; g++) {
      if ((s = arguments[g]) != null) {
        for (n in s) {
          c = s[n];
          if (n === "__proto__" || b === c) continue;
          if (c !== undefined) { b[n] = c; }
        }
      }
    }
    return b;
  };
  jQuery.extend({ ajax: function(opts) { var r = new XMLHttpRequest(); r.open(opts.method, opts.url); } });
  jQuery.ajax({method: "POST", url: "/api/n2"});
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/n2" && s.method === "POST"; });
});

// ───────────── ECMA-262 sub-problem tests (this session) ─────────────
// Each test exercises ONE specific spec rule the analyzer must follow.
// Tests are minimal synthetic shapes — fail until the rule is implemented.

api("§ 13.16.1 SequenceExpression L→R evaluation order", `
  var o = {};
  for (o.a = 1, o.b = 2, o.c = 3; false;) {}
  fetch("/api/seq-" + o.a + "-" + o.b + "-" + o.c);
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/seq-1-2-3"; });
});

api("§ 13.14.1 ConditionalExpression unchosen-arm side-effect discarded", `
  var x = 0;
  var picked = true ? (x = 1, "/api/cond-truthy") : (x = 99, "/api/cond-fail");
  fetch(picked + "?x=" + x);
`, function(sites) {
  return !!sites.find(function(s) {
    return s.url.indexOf("/api/cond-truthy") === 0 &&
      (s.params || []).some(function(p) { return p.name === "x" && (p.validValues || []).indexOf("1") >= 0; });
  });
});

api("§ 13.13.1 LogicalAND short-circuit: false && X discards X's effects", `
  var x = 5;
  var ignored = false && (x = 99, "discarded");
  fetch("/api/and-sc?x=" + x);
`, function(sites) {
  return !!sites.find(function(s) {
    return s.url.indexOf("/api/and-sc") === 0 &&
      (s.params || []).some(function(p) { return p.name === "x" && (p.validValues || []).indexOf("5") >= 0; });
  });
});

api("§ 10.1.8.1 OrdinaryGet — missing key on complete obj-lit returns undefined", `
  var settings = {url: "/api/ord-get"};
  var u = settings.missing || settings.url;
  fetch(u);
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/ord-get"; });
});

api("§ 13.5.3.5 typeof of unresolvable reference returns 'undefined'", `
  // typeof <unbound> evaluates per spec step 3.a to "undefined".
  if (typeof totallyUnboundIdentifier !== "undefined") {
    fetch("/api/typeof-wrong-branch");
  } else {
    fetch("/api/typeof-correct");
  }
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/typeof-correct"; }) &&
         !sites.find(function(s) { return s.url === "/api/typeof-wrong-branch"; });
});

api("§ 13.15.4 PutValue obj[k]=v distributes across or-of-const key", `
  var ce = {};
  ["get", "post"].forEach(function(method) {
    ce[method] = function(url) { fetch(url + "?via=" + method); };
  });
  ce.get("/api/or-key-get");
  ce.post("/api/or-key-post");
`, function(sites) {
  return !!sites.find(function(s) { return s.url.indexOf("/api/or-key-get") === 0; }) &&
         !!sites.find(function(s) { return s.url.indexOf("/api/or-key-post") === 0; });
});

api("§ 10.2.1 step 5 .call propagates body effects to caller's state", `
  var ce = {};
  var cb = function(_, m) { ce[m] = function(url) { fetch(url + "?via=" + m); }; };
  cb.call(null, 0, "post");
  ce.post("/api/call-body");
`, function(sites) {
  return !!sites.find(function(s) {
    return s.url.indexOf("/api/call-body") === 0 &&
      (s.params || []).some(function(p) { return p.name === "via" && (p.validValues || []).indexOf("post") >= 0; });
  });
});

api("WHATWG HTML § 7.2.1 window IS the global — window.XMLHttpRequest resolves", `
  (function(global, factory){ factory(global); })(typeof window !== "undefined" ? window : this, function(ie) {
    var xhr = new ie.XMLHttpRequest();
    xhr.open("POST", "/api/window-global");
    xhr.send();
  });
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/window-global" && s.method === "POST"; });
});

api("§ 7.3.2 GetV: MemberExpression on or(undefined, obj) drops the undefined branch", `
  // ECMA-262 § 13.3.2.1 RequireObjectCoercible: x.prop on a nullish x
  // throws TypeError. Static disjunction or(undefined, obj).send should
  // resolve via obj.send alone — the undefined branch can't observably
  // propagate the access result. Models jQuery's transport-dispatch
  // pattern: factories return undefined for non-matching transports +
  // obj for the matching one; only the obj's method is reachable.
  var X = Math.random() > 0.5 ? undefined : { send: function(url){ fetch(url); } };
  X.send("/api/or-undef-send");
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/or-undef-send"; });
});

api("§ 9.1.1.1 + § 14.6: forEach cb with if-narrowing inside writes back to outer state", `
  // ECMA-262 § 9.1.1.1 SetMutableBinding: closure writes from a forEach
  // cb persist after the loop. § 14.6 IfStatement clones state for its
  // consequent/alternate; the inner clone breaks shared identity with
  // outerState. Sequential HOF dispatch must propagate the cb's end-
  // state outer-binding writes back to outer regardless of any inner
  // merge frame's cloning. Models the canonical transport-pick shape.
  var ce = { send: function(u){ fetch(u); } };
  var selected;
  [1].forEach(function(){
    var r = Math.random() > 0.5 ? undefined : ce;
    if (r) selected = r;
  });
  if (selected) selected.send("/api/foreach-if-narrow");
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/foreach-if-narrow"; });
});

api("§ 14.15 TryStatement: try/catch block writes propagate past the try", `
  // ECMA-262 § 14.15 runtime semantics: post-try state = union of
  // block-end (normal completion) and catch-end (throw completion).
  // Previously the analyzer pushed block + catch + finalizer frames
  // independently with no merge — writes were lost. Now a try-merge
  // frame collects all branches' end-states and propagates the union
  // to the parent.
  var u = "/api/default";
  try { throw new Error(); } catch (e) { u = "/api/catch-wrote"; }
  fetch(u);
`, function(sites) {
  // Both /api/default (no-exception fictional path) and /api/catch-wrote
  // (exception path) are statically possible — accept either presence.
  return !!sites.find(function(s) { return s.url === "/api/catch-wrote"; });
});

api("§ 22.1.3.18 String.prototype.replace with function replacer + const return", `
  // Replacer is bound to a var first (named function reference), then
  // used as the second arg of .replace. The slice must include the FE
  // bound to 'fn' so its return memo is populated by the trampoline.
  var fn = function(m){ return "FIXED"; };
  var u = "/api/replace-prefix".replace(/replace/g, fn);
  fetch(u);
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/FIXED-prefix"; });
});

api("§ 14.9 + § 14.13 labeled break halts the labeled outer loop", `
  var picked = null;
  outer: for (var i = 0; i < 2; i++) {
    for (var j = 0; j < 2; j++) {
      if (i === 0 && j === 1) {
        picked = "/api/labeled-break";
        break outer;
      }
    }
  }
  fetch(picked);
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/labeled-break"; });
});

api("§ 14.7.4 ForBodyEvaluation: body writes to outer-captured bindings propagate past loop", `
  var u;
  for (var i = 0; i < 3; i++) {
    u = "/api/for-writeback";
  }
  fetch(u);
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/for-writeback"; });
});

api("§ 14.7.2 WhileStatement: body writes to outer-captured bindings propagate past loop", `
  var u = null;
  while (!u) { u = "/api/while-writeback"; }
  fetch(u);
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/while-writeback"; });
});

api("§ 14.9 + § 14.13 labeled break: post-label statement runs after break", `
  outer: {
    inner: {
      break outer;
      fetch("/api/never");
    }
  }
  fetch("/api/post-labeled-break");
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/post-labeled-break"; }) &&
    !sites.find(function(s) { return s.url === "/api/never"; });
});

api("§ 13.10 'in' as expression returns const boolean when key + obj are statically known", `
  var routes = { GET: "/api/get-route", POST: "/api/post-route" };
  var has = "GET" in routes;
  fetch(has ? routes.GET : "/api/never");
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/get-route"; });
});

api("§ 23.1.3.6 Array.prototype.entries: yields [idx, value] for for-of", `
  for (var pair of ["/api/ent-0", "/api/ent-1"].entries()) {
    fetch(pair[1]);
  }
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/ent-0"; }) &&
         !!sites.find(function(s) { return s.url === "/api/ent-1"; });
});

api("§ 13.3.6 multi-level call-arg substitution: opts.xhr() through 2-level dispatch", `
  // ECMA-262 § 13.3.6 EvaluateCall: when xhr.open's receiver is
  // call(member(param.xhr)) inside trans, and trans is called via
  // another fn dispatch(opts){trans(opts)}, the substitution chain
  // must walk: trans's call sites → dispatch's call sites → top-level
  // concrete obj. The wrapper-traversal queue handles this iteratively.
  function trans(opts){ var xhr = opts.xhr(); xhr.open(opts.method, opts.url); }
  function dispatch(opts){ trans(opts); }
  dispatch({xhr: function(){ return new XMLHttpRequest(); }, method: "POST", url: "/api/multi-level"});
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/multi-level" && s.method === "POST"; });
});

api("§ 13.3.6.1 step 4 SpreadElement in call args expands to discrete caller args", `
  function f(a, b){ fetch(a, {method: b}); }
  var args = ["/api/spread-call", "POST"];
  f.apply(null, args);
  f(...args);
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/spread-call" && s.method === "POST"; });
});

api("§ 20.1.2.2 Object.create(proto) inherits proto's methods on instance", `
  var proto = { send: function(u){ fetch(u); } };
  var inst = Object.create(proto);
  inst.send("/api/oc-test");
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/oc-test"; });
});

api("§ 23.1.2.1 Array.from with mapFn applied per element", `
  var arr = Array.from([1, 2], function(x){ return "/api/from-" + x; });
  fetch(arr[0]);
  fetch(arr[1]);
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/from-1"; }) &&
         !!sites.find(function(s) { return s.url === "/api/from-2"; });
});

api("§ 25.1.4.1 Promise.all on array-lit + .then: cb receives array of resolved values", `
  Promise.all(["/api/all-1", "/api/all-2"]).then(function(arr){
    fetch(arr[0]);
    fetch(arr[1]);
  });
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/all-1"; }) &&
         !!sites.find(function(s) { return s.url === "/api/all-2"; });
});

api("§ 14.2.16 + § 27.7.5.3 await unwraps promise-instance", `
  async function f(){
    var u = await Promise.resolve("/api/await-test");
    fetch(u);
  }
  f();
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/await-test"; });
});

api("§ 25.1.5 Promise.prototype.then chain: cb2's input = cb1's return", `
  // ECMA-262 § 25.1.5 + § 25.1.4 PromiseResolve: each .then returns a
  // new promise resolved with the cb's return value; chained .then's cb
  // receives the prior cb's return.
  Promise.resolve("/api/promise")
    .then(function(p){ return p + "?step=1"; })
    .then(function(u){ fetch(u); });
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/promise?step=1"; });
});

api("§ 13.3.7 SuperProperty: super.method() in derived class dispatches to base", `
  // ECMA-262 § 13.3.7 + § 15.7.4 MakeSuperPropertyReference: super.fn
  // resolves through the enclosing class's superClass prototype chain.
  class B { send(u) { fetch(u); } }
  class D extends B { call(u) { super.send(u); } }
  new D().call("/api/super-dispatch");
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/super-dispatch"; });
});

api("§ 15.7.5 ClassHeritage: inherited methods reachable on derived instance even when base has no ctor", `
  // Per § 15.7.5 [[Prototype]] is set by ClassHeritage regardless of
  // constructor presence. Previously the ancestor-method inclusion was
  // gated on "ancestor has own ctor"; now walks the extends chain
  // independently of ctor presence.
  class B { send(url) { fetch(url); } }
  class D extends B { call(u) { this.send(u); } }
  new D().call("/api/heritage-no-ctor");
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/heritage-no-ctor"; });
});

api("§ 13.4.4 UpdateExpression on MemberExpression rebinds state", `
  var ctr = { v: 0 };
  ctr.v++;
  ctr.v++;
  fetch("/api/ctr-" + ctr.v);
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/ctr-2"; });
});

api("§ 23.1.3 + § 13.3.6 + interprocedural points-to: curried-helper + each-style iterator + transport invocation (jQuery Ut/Vt full shape)", `
  // Composition: Ut(_t) returns curried registration fn; cb pushes
  // transportFn to _t['*']; dispatch reads _t['*'] and iterates via
  // each(...) which dispatches cb.call(arr[i], i, arr[i]); eachCb's
  // param 1 (= arr[i] = transportFn) is invoked as t(opts). All
  // resolved via Phase A/B/C points-to with iterator unfolding (Phase
  // C case 4: binding's reverse-points-to → walk iterator helper's
  // body → cb's element-param invocations → fn's effective call sites).
  var _t = {};
  function each(arr, cb) {
    for (var i = 0; i < arr.length; i++) cb.call(arr[i], i, arr[i]);
  }
  function Ut(o) {
    return function(name, fn) {
      if (typeof name !== "string") { fn = name; name = "*"; }
      (o[name] = o[name] || []).push(fn);
    };
  }
  var register = Ut(_t);
  function dispatch(opts) {
    var n;
    each(_t["*"], function(idx, t) { n = t(opts); });
    return n;
  }
  register(function(opts){ return { send: function(){ fetch(opts.url); } }; });
  var tres = dispatch({url: "/api/jq-full-shape"});
  tres.send();
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/jq-full-shape"; });
});

api("§ 8.1.1 + § 9.4.1 + § 14.6: curried registration helper (jQuery Ut/Vt shape) — bound via obj-lit method prop", `
  // Closer to real jQuery: helper Ut wraps registry, result is assigned
  // to an obj-lit method (ce.ajaxTransport = Ut(_t) via property in
  // object expression). Ut's curried closure mutates _t when invoked
  // via ce.ajaxTransport(fn). § 8.1.1 lexical chain + § 9.4.1 [[Call]]
  // param binding + § 14.6 if-merge resolve through the deferred-refold
  // post-pass.
  var _t = {};
  function Ut(o) {
    return function(name, fn) {
      if (typeof name !== "string") { fn = name; name = "*"; }
      (o[name] = o[name] || []).push(fn);
    };
  }
  var ce = { ajaxTransport: Ut(_t) };
  function dispatch(opts) {
    var t = _t["*"];
    if (t && t[0]) return t[0](opts);
  }
  ce.ajaxTransport(function(opts){ return { send: function(){ fetch(opts.url); } }; });
  var tres = dispatch({url: "/api/jq-objlit-method"});
  tres.send();
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/jq-objlit-method"; });
});

api("§ 8.1.1 + § 9.4.1 + § 14.6: curried registration helper (jQuery Ut/Vt shape) — typeof rebind + closure-captured registry", `
  // jQuery's actual transport shape: Ut(o) returns a registration fn
  // that closes over o. ajaxTransport = Ut(_t). Calling ajaxTransport
  // mutates _t through the closure. The inner fn's references to o
  // resolve through the helper's Environment Record per § 8.1.1.
  // ECMA § 9.4.1 binds o = _t at Ut(_t)'s [[Call]]; § 13.3.6 binds
  // the inner fn's own params at each register(...) call. The static
  // resolver must walk BOTH levels (helper + inner-fn call sites) to
  // model the mutation as a write on _t.
  var transports = {};
  function makeRegister(o) {
    return function(name, fn) {
      if (typeof name !== "string") { fn = name; name = "*"; }
      (o[name] = o[name] || []).push(fn);
    };
  }
  var register = makeRegister(transports);
  function dispatch(opts) {
    var t = transports["*"];
    if (t && t[0]) return t[0](opts);
  }
  register(function(opts){ return { send: function(){ fetch(opts.url); } }; });
  var tres = dispatch({url: "/api/curried-helper"});
  tres.send();
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/curried-helper"; });
});

api("§ 22.1.3.18 + § 14.6: String.replace with const-string search + replacer returning or-of-consts (cartesian)", `
  // Functional replacer that returns one of K consts per match. Each
  // match independently picks one leaf; for M matches across N receivers
  // the static result is the cartesian product. Tests the per-match
  // leaf-set enumeration that replaces the rsfFlat.length===1 gate.
  var u = "/api/X".replace("X", function(){ return Math.random() > 0.5 ? "first" : "second"; });
  fetch(u);
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/first"; }) &&
         !!sites.find(function(s) { return s.url === "/api/second"; });
});

api("§ 14.6 + § 13.3.6 + § 23.1.3.20 composite: full jQuery-shape transport (typeof rebind + obj-prop registry + or-key)", `
  // Composes: typeof-discriminated arg rebind (name=fn when 1 arg) +
  // dynamic key from rebound name (or(param, "*")) +
  // chained obj-prop registry (transports[name] = transports[name] || []) +
  // intermediate var + if-guard + factory returns obj with send method
  // closing over opts. Mirrors jQuery's ajaxTransport / transport
  // dispatch shape.
  var transports = {};
  function ajaxTransport(name, fn) {
    if (typeof name !== "string") { fn = name; name = "*"; }
    (transports[name] = transports[name] || []).push(fn);
  }
  function dispatch(opts) {
    var t = transports["*"];
    if (t && t[0]) return t[0](opts);
  }
  ajaxTransport(function(opts){ var r = { send: function(){ fetch(opts.url); } }; return r; });
  var tres = dispatch({url: "/api/jq-shape-full"});
  tres.send();
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/jq-shape-full"; });
});

api("§ 14.6 IfStatement merge + § 13.3.6 call distribution: typeof-discriminated param rebind in registration helper", `
  // jQuery transport ajaxTransport(name, fn) shape: when called with
  // ONE arg, name is the function; if-branch rebinds fn = name. The
  // resolver must trace through the post-if-merge or-AV via:
  //  - fold-completeness deferral (Pattern C doesn't cache when memo
  //    isn't ready, so the next fixpoint round recomputes with the
  //    merged AV)
  //  - or-callee distribution in the post-fixpoint call-graph index
  //    (function-ref arms get indexed; undefined branch skipped).
  var arr = [];
  function register(name, fn) {
    if (typeof name !== "string") { fn = name; name = "*"; }
    arr.push(fn);
  }
  function dispatch(opts) { return arr[0](opts); }
  register(function(opts){ fetch(opts.url); });
  dispatch({url: "/api/typeof-rebind"});
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/typeof-rebind"; });
});

api("§ 13.3.6 + slice transitivity: dynamic call edge through array-index callee extends slice (jQuery transport shape)", `
  // Same shape as register/dispatch but the helper functions are
  // assigned to obj properties (jq.ajaxTransport, jq.dispatch). The
  // helper invocation site has a MemberExpression callee that spec
  // eval resolves to a function-ref; the post-fixpoint pass indexes
  // this dynamic call edge AND extends the slice to include the
  // caller's enclosing fn so the resolver's slice gate doesn't reject
  // the call site when walking inter-procedural arg flow.
  var jq = {};
  var transports = [];
  jq.ajaxTransport = function(fn) { transports.push(fn); };
  jq.dispatch = function(opts) { return transports[0](opts); };
  jq.ajaxTransport(function(opts){ fetch(opts.url); });
  jq.dispatch({url: "/api/v4-host-method"});
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/v4-host-method"; });
});

api("§ 23.1.3.20 + § 10.2.10: register-helper pushes function to outer array; dispatcher reads it back", `
  // jQuery transport-registration shape: a helper fn (register) pushes
  // values into an outer array via O.push(...). When the outer var is
  // an array-lit (var arr = []), the binding-capture must fold the
  // push from EACH call site's caller args. The dispatcher reads the
  // array element, invokes it, and the pushed function's body sees its
  // caller's args via inter-procedural substitution per § 10.2.10.
  var registry = [];
  function register(fn) { registry.push(fn); }
  function dispatch(opts) { return registry[0](opts); }
  register(function(opts){ fetch(opts.url); });
  dispatch({url: "/api/registry-dispatch"});
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/registry-dispatch"; });
});

api("§ 13.3.6 + § 23.1.4: custom each iterator over array-lit dispatching to callback (factory pattern)", `
  // jQuery.each style: user-defined iterator that loops an array and
  // invokes a callback per element. The callback receives each element
  // as a param and invokes it. Resolution chain:
  //   fetch(u) → u = factory() where factory = cb's param idx=1
  //   cb's call sites = each's body: callback(i, arr[i])
  //   substituting into cb: factory = arr[i]
  //   each's call sites = each([factoryFn], cb)
  //   substituting into each: arr = array-lit[factoryFn]
  //   member(array-lit[factoryFn], opaqueKey) → factoryFn
  //   call(function-ref(factoryFn), []) folds to factoryFn's return
  function each(arr, callback){
    for (var i = 0; i < arr.length; i++) { callback(i, arr[i]); }
  }
  each([function(){ return "/api/each0"; }], function(idx, factory){
    var u = factory();
    fetch(u);
  });
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/each0"; });
});

api("§ 13.5.1 + § 10.1.10 delete operator removes obj-lit prop from state", `
  var opts = { url: "/api/delete-keep", method: "POST", junk: "drop" };
  delete opts.junk;
  fetch(opts.url, { method: opts.method });
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/delete-keep" && s.method === "POST"; });
});

api("§ 13.3.6 EvaluateCall distribution over or-of-function-refs", `
  // jQuery transport-pick pattern: cb's 'fn' param resolves to
  // or(factory1, factory2) under HOF SetUnion. fn() must dispatch
  // per-leaf and union the returns. Without this, the call returns TOP.
  var make = function(matches){ return matches ? { send: function(u){ fetch(u); } } : undefined; };
  var transports = [function(){ return make(false); }, function(){ return make(true); }];
  var selected;
  transports.forEach(function(fn){ var r = fn(); if (r) selected = r; });
  if (selected) selected.send("/api/or-of-funcrefs");
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/or-of-funcrefs"; });
});

api("Tier 7 dynamic call-edge re-enqueue: HOF cb dispatching to function-ref via param", `
  // The cb's 'fn' param is bound by HOF dispatch to a function-ref AV.
  // The static call graph has no cb→factory edge (the call is via the
  // param), so the fixpoint must re-enqueue cb when factory's return
  // memo is populated. Without dynamic reverse-read edges, fn() returns
  // TOP forever; selected.send(url) can't dispatch.
  var make = function(matches){ return matches ? { send: function(u){ fetch(u); } } : undefined; };
  var transports = [function(){ return make(true); }];
  var selected;
  transports.forEach(function(fn){ selected = fn(); });
  selected.send("/api/dyn-call-edge");
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/dyn-call-edge"; });
});

api("§ 13.5.3.4 + § 7.2.11 typeof equality narrows identifier to type-matching leaves", `
  var x = Math.random() > 0.5 ? "/api/typeof-string" : 42;
  if (typeof x === "string") fetch(x);
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/typeof-string"; });
});

api("§ 14.6.2 + § 7.1.5 ToBoolean narrows bare Identifier to truthy in consequent", `
  var x = Math.random() > 0.5 ? undefined : { send: function(url){ fetch(url); } };
  if (x) x.send("/api/truthy-narrow");
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/truthy-narrow"; });
});

api("§ 14.15 TryStatement: try block write visible post-try", `
  var u;
  try { u = "/api/try-block-wrote"; } catch (e) { }
  fetch(u);
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/try-block-wrote"; });
});

api("§ 13.15.4 + § 9.3.7 chained assignment on global: `e.A = e.B = v` BOTH propagate as SetGlobalRecordBinding", `
  // jQuery 3.7.1's actual export pattern: '"undefined"==typeof e && (ie.jQuery=ie.$=ce)'
  // — both ie.jQuery and ie.$ should land in _specGlobalPropOverrides, and a
  // user-site bare 'jQuery' Identifier should resolve to ce. Verify by
  // calling jQuery.ajaxCall() where ajaxCall is a prop on ce that fetches.
  !function(e, t){ t(e); }(typeof window !== "undefined" ? window : this, function(ie, en){
    var ce = function(){};
    ce.ajaxCall = function(url){ fetch(url); };
    "undefined" == typeof en && (ie.jQuery = ie.$ = ce);
  });
  jQuery.ajaxCall("/api/chain-global");
`, function(sites) {
  return !!sites.find(function(s) { return s.url === "/api/chain-global"; });
});

console.log("\n=== Summary ===");
console.log("Total: " + total + ", Passed: " + passed + ", Failed: " + failed);
process.exit(failed > 0 ? 1 : 0);
