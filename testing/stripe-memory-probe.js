// Track heap usage during Stripe analysis. Print periodically.
var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");

// Inject a hook that prints memory + AV counter every N _specInstantiateAv calls.
new Function(astCode + `
  globalThis.analyzeJSBundle = analyzeJSBundle;
  var origApf = _specAnalyzePropertyFlow;
  globalThis._apfCount = 0;
  _specAnalyzePropertyFlow = function(a, b, c) {
    globalThis._apfCount++;
    if (globalThis._apfCount % 200 === 0) {
      var m = process.memoryUsage();
      console.log("[apf=" + globalThis._apfCount + "] heap=" + Math.round(m.heapUsed/1024/1024) + "MB rss=" + Math.round(m.rss/1024/1024) + "MB");
    }
    return origApf(a, b, c);
  };
`).call(globalThis);

var bundleName = process.argv[2] || "index.js";
var code = fs.readFileSync(path.join(rootDir, "testing/harness-dumps", bundleName), "utf8");
console.log("Bundle: " + bundleName + " (" + (code.length / 1024).toFixed(1) + " KB)");
console.log("Heap at start: " + Math.round(process.memoryUsage().heapUsed/1024/1024) + "MB");

var t0 = Date.now();
try {
  var result = globalThis.analyzeJSBundle(code, "https://test.example.com/" + bundleName, true, null);
  console.log("Total: " + (Date.now() - t0) + "ms");
  console.log("Heap at end: " + Math.round(process.memoryUsage().heapUsed/1024/1024) + "MB");
  console.log("Total ia calls: " + globalThis._iaCount);
} catch (e) {
  console.log("FAILED after " + (Date.now() - t0) + "ms: " + e.message);
}
