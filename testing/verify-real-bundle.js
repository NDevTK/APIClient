// Run analyzeJSBundle on a cached real-site bundle and report stats:
//   - fetchCallSites count and resolution sources
//   - resolverErrors count
//   - sinks/dangers counts (security)
// Used to verify that spec-eval improvements actually fire on real JS.

var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");

var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();

var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode + "\nglobalThis.analyzeJSBundle = analyzeJSBundle;").call(globalThis);

var bundleName = process.argv[2];
if (!bundleName) {
  console.log("usage: node testing/verify-real-bundle.js <bundle-filename>");
  console.log("available bundles in testing/harness-dumps/:");
  for (var f of fs.readdirSync(path.join(rootDir, "testing/harness-dumps"))) {
    var p = path.join(rootDir, "testing/harness-dumps", f);
    var sz = fs.statSync(p).size;
    console.log("  " + f + " (" + (sz / 1024).toFixed(1) + " KB)");
  }
  process.exit(1);
}

var bundlePath = path.join(rootDir, "testing/harness-dumps", bundleName);
var code = fs.readFileSync(bundlePath, "utf8");
console.log("Analyzing " + bundleName + " (" + (code.length / 1024).toFixed(1) + " KB)");

var t0 = Date.now();
var result = analyzeJSBundle(code, "https://test.example.com/" + bundleName, true);
var t1 = Date.now();

console.log("\n=== Analysis: " + (t1 - t0) + "ms ===");
console.log("fetchCallSites: " + (result.fetchCallSites || []).length);
console.log("resolverErrors: " + (result.resolverErrors || []).length);
console.log("sinks: " + (result.sinks || []).length);
console.log("dangers: " + (result.dangers || []).length);
console.log("interProc: " + (result.interProc || []).length);
console.log("fieldMaps: " + Object.keys(result.fieldMaps || {}).length);
console.log("enums: " + Object.keys(result.enums || {}).length);

if (result.fetchCallSites && result.fetchCallSites.length > 0) {
  console.log("\nfetch URLs (first 20):");
  result.fetchCallSites.slice(0, 20).forEach(function(s) {
    console.log("  " + s.method + " " + s.url + " (" + (s.params || []).map(function(p) {
      return p.name + ":" + (p.location || "?");
    }).join(",") + ")");
  });
}

if (result.resolverErrors && result.resolverErrors.length > 0) {
  console.log("\nresolverErrors (first 5):");
  result.resolverErrors.slice(0, 5).forEach(function(e) {
    console.log("  " + (e.identifier || e.reason) + " at line " + (e.line || "?"));
  });
}
