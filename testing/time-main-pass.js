// Time the main pass traversal of index.js.
var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode + "\nglobalThis.analyzeJSBundle = analyzeJSBundle;").call(globalThis);

var code = fs.readFileSync(path.join(rootDir, "testing/harness-dumps/index.js"), "utf8");
console.log("Bundle: " + (code.length / 1024).toFixed(1) + " KB");
var t0 = Date.now();
try {
  var result = globalThis.analyzeJSBundle(code, "https://test.example.com/index.js", true, null);
  console.log("Total: " + (Date.now() - t0) + "ms");
  console.log("Phase timings:", JSON.stringify(result._phaseTimings));
  console.log("fetchSites:", (result.fetchCallSites || []).length);
  console.log("sinks:", (result.securitySinks || []).length);
} catch (e) {
  console.error("threw:", e.message, e.stack);
}
