// Wrap each visitor at the source level and emit periodic progress + cumulative time per visitor.
var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");

// Append instrumentation after the file load. Use a `_origAnalyze` wrapper
// so we can install the visitor wrappers at start-of-analyze time.
var instrumentation = `
  globalThis.analyzeJSBundle = analyzeJSBundle;
  globalThis._vCount = {};
  globalThis._vTime = {};
  function _instrumented(name, fn) {
    return function(p, r) {
      var t0 = Date.now();
      var ret = fn(p, r);
      globalThis._vTime[name] = (globalThis._vTime[name] || 0) + (Date.now() - t0);
      globalThis._vCount[name] = (globalThis._vCount[name] || 0) + 1;
      var total = globalThis._vCount._all = (globalThis._vCount._all || 0) + 1;
      if (total % 5000 === 0) {
        console.log("[T+" + (Date.now() - globalThis._anStart) + "ms] " + total + " visitor calls. By time:");
        var keys = Object.keys(globalThis._vTime).sort(function(a,b){ return globalThis._vTime[b] - globalThis._vTime[a]; });
        keys.slice(0, 6).forEach(function(k) {
          console.log("    " + k + ": " + globalThis._vTime[k] + "ms (" + globalThis._vCount[k] + " calls)");
        });
      }
      return ret;
    };
  }
  _specAnalyzePropertyFlow = _instrumented("_specAnalyzePropertyFlow", _specAnalyzePropertyFlow);
  _resolveAllValues = _instrumented("_resolveAllValues", _resolveAllValues);
  _avAtPath = _instrumented("_avAtPath", _avAtPath);
  _processNetworkSink = _instrumented("_processNetworkSink", _processNetworkSink);
  _processExportMethodCall = _instrumented("_processExportMethodCall", _processExportMethodCall);
  _processSecurityCallSink = _instrumented("_processSecurityCallSink", _processSecurityCallSink);
  _processDangerousPattern = _instrumented("_processDangerousPattern", _processDangerousPattern);
  _processNewExpressionSink = _instrumented("_processNewExpressionSink", _processNewExpressionSink);
  _processSecurityNewSink = _instrumented("_processSecurityNewSink", _processSecurityNewSink);
  _trackGlobalAssignment = _instrumented("_trackGlobalAssignment", _trackGlobalAssignment);
  _detectProtoFieldAssignment = _instrumented("_detectProtoFieldAssignment", _detectProtoFieldAssignment);
  _processImageSrcSink = _instrumented("_processImageSrcSink", _processImageSrcSink);
  _processSecurityAssignSink = _instrumented("_processSecurityAssignSink", _processSecurityAssignSink);
  _processDangerousAssignment = _instrumented("_processDangerousAssignment", _processDangerousAssignment);
  _detectEnumObject = _instrumented("_detectEnumObject", _detectEnumObject);
  _collectSwitchConstraints = _instrumented("_collectSwitchConstraints", _collectSwitchConstraints);
  _collectEqualityConstraints = _instrumented("_collectEqualityConstraints", _collectEqualityConstraints);
  _collectIncludesConstraints = _instrumented("_collectIncludesConstraints", _collectIncludesConstraints);
  _collectIterationConstraints = _instrumented("_collectIterationConstraints", _collectIterationConstraints);
  _collectObjectLiteralConstraints = _instrumented("_collectObjectLiteralConstraints", _collectObjectLiteralConstraints);
  _traceValueSource = _instrumented("_traceValueSource", _traceValueSource);
  _resolveAllValues = _instrumented("_resolveAllValues", _resolveAllValues);
`;

new Function(astCode + instrumentation).call(globalThis);

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
console.log("\nFinal visitor stats:");
var keys = Object.keys(globalThis._vTime).sort(function(a,b){ return globalThis._vTime[b] - globalThis._vTime[a]; });
keys.forEach(function(k) {
  console.log("    " + k + ": " + globalThis._vTime[k] + "ms (" + globalThis._vCount[k] + " calls, " + (globalThis._vTime[k] / globalThis._vCount[k]).toFixed(3) + "ms avg)");
});
