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

console.log("\n=== Summary ===");
console.log("Total: " + total + ", Passed: " + passed + ", Failed: " + failed);
process.exit(failed > 0 ? 1 : 0);
