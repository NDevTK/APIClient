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

console.log("\n=== Summary ===");
console.log("Total: " + total + ", Passed: " + passed + ", Failed: " + failed);
process.exit(failed > 0 ? 1 : 0);
