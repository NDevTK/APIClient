// Profile the main-pass visitor body work on Stripe's index.js.
// Wrap each _process* fn with a timer + call counter.
var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");

// Profile probe: instrument selected `_process*` functions before analysis.
var instrumentation = `
  globalThis._profStats = {};
  function _prof(name, fn) {
    return function() {
      var t0 = Date.now();
      var r;
      try { r = fn.apply(this, arguments); }
      finally {
        var entry = globalThis._profStats[name] || { calls: 0, ms: 0 };
        entry.calls++;
        entry.ms += Date.now() - t0;
        globalThis._profStats[name] = entry;
      }
      return r;
    };
  }
  _processNetworkSink = _prof("_processNetworkSink", _processNetworkSink);
  _processNewExpressionSink = _prof("_processNewExpressionSink", _processNewExpressionSink);
  _processImageSrcSink = _prof("_processImageSrcSink", _processImageSrcSink);
  _processIIFE = _prof("_processIIFE", _processIIFE);
  _processExportMethodCall = _prof("_processExportMethodCall", _processExportMethodCall);
  _processSecurityCallSink = _prof("_processSecurityCallSink", _processSecurityCallSink);
  _processSecurityNewSink = _prof("_processSecurityNewSink", _processSecurityNewSink);
  _processSecurityAssignSink = _prof("_processSecurityAssignSink", _processSecurityAssignSink);
  _processDangerousPattern = _prof("_processDangerousPattern", _processDangerousPattern);
  _processDangerousAssignment = _prof("_processDangerousAssignment", _processDangerousAssignment);
  _resolveAllValues = _prof("_resolveAllValues", _resolveAllValues);
  _traceValueSource = _prof("_traceValueSource", _traceValueSource);
  _avAtPath = _prof("_avAtPath", _avAtPath);
  _extractBodyParams = _prof("_extractBodyParams", _extractBodyParams);
  _detectEnumObject = _prof("_detectEnumObject", _detectEnumObject);
  _collectSwitchConstraints = _prof("_collectSwitchConstraints", _collectSwitchConstraints);
  _collectEqualityConstraints = _prof("_collectEqualityConstraints", _collectEqualityConstraints);
  _collectIncludesConstraints = _prof("_collectIncludesConstraints", _collectIncludesConstraints);
  _collectIterationConstraints = _prof("_collectIterationConstraints", _collectIterationConstraints);
  _trackGlobalAssignment = _prof("_trackGlobalAssignment", _trackGlobalAssignment);
  _detectProtoFieldAssignment = _prof("_detectProtoFieldAssignment", _detectProtoFieldAssignment);
  globalThis.analyzeJSBundle = analyzeJSBundle;
`;

new Function(astCode + instrumentation).call(globalThis);

var bundleName = process.argv[2] || "index.js";
var code = fs.readFileSync(path.join(rootDir, "testing/harness-dumps", bundleName), "utf8");
console.log("Bundle: " + bundleName + " (" + (code.length / 1024).toFixed(1) + " KB)");

var t0 = Date.now();
var result;
try {
  result = globalThis.analyzeJSBundle(code, "https://test.example.com/" + bundleName, true, null);
  console.log("Total: " + (Date.now() - t0) + "ms");
} catch (e) {
  console.log("FAILED after " + (Date.now() - t0) + "ms: " + e.message);
  console.log(e.stack.split("\n").slice(0, 5).join("\n"));
}

// Sort by time desc
var entries = Object.keys(globalThis._profStats || {}).map(function(k) {
  return { name: k, calls: globalThis._profStats[k].calls, ms: globalThis._profStats[k].ms };
}).sort(function(a, b) { return b.ms - a.ms; });

console.log("\n┌──────────────────────────────┬─────────┬──────────┬────────────┐");
console.log("│ Fn                           │ Calls   │ Total ms │ Avg ms     │");
console.log("├──────────────────────────────┼─────────┼──────────┼────────────┤");
entries.forEach(function(e) {
  var pad = function(s, n) { s = String(s); return s + " ".repeat(Math.max(0, n - s.length)); };
  var avg = e.calls > 0 ? (e.ms / e.calls).toFixed(3) : "0";
  console.log("│ " + pad(e.name, 28) + " │ " + pad(e.calls, 7) + " │ " + pad(e.ms, 8) + " │ " + pad(avg, 10) + " │");
});
console.log("└──────────────────────────────┴─────────┴──────────┴────────────┘");
if (result) {
  console.log("\nfetchSites: " + (result.fetchCallSites || []).length);
  console.log("sinks: " + (result.securitySinks || []).length);
}
