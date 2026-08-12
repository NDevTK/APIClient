// Diagnose what main pass actually does on Stripe.
var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");

new Function(astCode + `
  globalThis.analyzeJSBundle = analyzeJSBundle;
  globalThis._t = _t;
  // Track WHO is calling _resolveByContextSensitiveReanalysis and how often.
  var orig_rcsr = _resolveByContextSensitiveReanalysis;
  var rcsrCount = 0;
  var rcsrCallSiteCount = 0;
  _resolveByContextSensitiveReanalysis = function(initialPath, encFn) {
    rcsrCount++;
    if (encFn && encFn.node) {
      var cs = _specFindCallSites(encFn);
      rcsrCallSiteCount += (cs ? cs.length : 0);
    }
    if (rcsrCount % 50 === 0) {
      console.log("[ctx-reanalysis] " + rcsrCount + " calls, cumulative call sites visited: " + rcsrCallSiteCount);
    }
    return orig_rcsr(initialPath, encFn);
  };
  // Track _specAnalyzePropertyFlow calls
  var orig_sapf = _specAnalyzePropertyFlow;
  var sapfCount = 0;
  var sapfPartialCount = 0;
  _specAnalyzePropertyFlow = function(funcPath, force, stopAfterStmtIdx) {
    sapfCount++;
    if (typeof stopAfterStmtIdx === "number" && stopAfterStmtIdx >= 0) sapfPartialCount++;
    if (sapfCount % 1000 === 0) {
      console.log("[apf] " + sapfCount + " calls (" + sapfPartialCount + " partial)");
    }
    return orig_sapf(funcPath, force, stopAfterStmtIdx);
  };
`).call(globalThis);

var bundleName = process.argv[2] || "index.js";
var code = fs.readFileSync(path.join(rootDir, "testing/harness-dumps", bundleName), "utf8");
console.log("Bundle: " + bundleName + " (" + (code.length / 1024).toFixed(1) + " KB)");

var t0 = Date.now();
try {
  var result = globalThis.analyzeJSBundle(code, "https://test.example.com/" + bundleName, true, null);
  console.log("Total: " + (Date.now() - t0) + "ms");
  console.log("fetchSites: " + (result.fetchCallSites || []).length);
} catch (e) {
  console.log("FAILED after " + (Date.now() - t0) + "ms: " + e.message);
}
