// Run analyzeJSBundle on a bundle but inject phase markers via instrumentation.
var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");

// Inject phase markers:
// - Just before slice build, after slice build, after fixpoint, after main pass.
var injected = astCode.replace(
  /var _slicePass = _babelTraverse\(ast, \{\s*Program: function\(p\) \{\s*_specBuildSlice\(p\);/,
  'console.log("[T+" + (Date.now() - globalThis._anStart) + "ms] starting slice build");$&'
).replace(
  /var _t_slice_end = \(typeof performance/,
  'console.log("[T+" + (Date.now() - globalThis._anStart) + "ms] slice + fixpoint done");$&'
).replace(
  /\/\/ ── Export constraints for background\.js ──/,
  'console.log("[T+" + (Date.now() - globalThis._anStart) + "ms] main pass done");$&'
).replace(
  /\/\/ Pre-pass: collect global assignments/,
  'console.log("[T+" + (Date.now() - globalThis._anStart) + "ms] parse done");$&'
);

// Also instrument fixpoint trampoline to print every 500 analyses.
injected = injected.replace(
  /(_specAnalyzePropertyFlow\(fp, true\);\s+_worklistAnalyses\+\+;)/,
  '$1 if (_worklistAnalyses % 500 === 0) console.log("[T+" + (Date.now() - globalThis._anStart) + "ms] tramp analysed " + _worklistAnalyses);'
);

new Function(injected + "\nglobalThis.analyzeJSBundle = analyzeJSBundle;").call(globalThis);

var bundleName = process.argv[2] || "index.js";
var code = fs.readFileSync(path.join(rootDir, "testing/harness-dumps", bundleName), "utf8");
console.log("Bundle: " + bundleName + " (" + (code.length / 1024).toFixed(1) + " KB)");

globalThis._anStart = Date.now();
console.log("[T+0ms] starting analyzeJSBundle");
var result;
try {
  result = globalThis.analyzeJSBundle(code, "https://test.example.com/" + bundleName, true, null);
  console.log("[T+" + (Date.now() - globalThis._anStart) + "ms] DONE");
} catch (e) {
  console.log("[T+" + (Date.now() - globalThis._anStart) + "ms] ERROR: " + e.message);
}
if (result) {
  console.log("fetchSites: " + (result.fetchCallSites || []).length);
  console.log("sinks: " + (result.securitySinks || []).length);
}
