// Run Stripe through analyzeJSBundle but instrument the main pass
// visitor entrance with a tick counter. Print top consumers every 5K.
var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");

// Wrap each top-level function used in main pass via global assignment
// at script tail. Counters per fn.
new Function(astCode + `
  globalThis.analyzeJSBundle = analyzeJSBundle;
  globalThis._wrapStart = 0;
  globalThis._t = _t;
  function wrap(name, fn) {
    return function() {
      if (!globalThis._wrapStart) globalThis._wrapStart = Date.now();
      var t = Date.now();
      var r = fn.apply(this, arguments);
      var dt = Date.now() - t;
      if (!globalThis._wrapT) globalThis._wrapT = {};
      if (!globalThis._wrapC) globalThis._wrapC = {};
      globalThis._wrapT[name] = (globalThis._wrapT[name] || 0) + dt;
      globalThis._wrapC[name] = (globalThis._wrapC[name] || 0) + 1;
      globalThis._wrapTotalCalls = (globalThis._wrapTotalCalls || 0) + 1;
      if (globalThis._wrapTotalCalls % 5000 === 0) {
        process.stdout.write("[" + Math.floor((Date.now() - globalThis._wrapStart)/1000) + "s] " + globalThis._wrapTotalCalls + " calls\\n");
        globalThis._printTimers();
      }
      return r;
    };
  }
  // Wrap the slow candidates
  _specInstantiateAv = wrap("_specInstantiateAv", _specInstantiateAv);
  _specAnalyzePropertyFlow = wrap("_specAnalyzePropertyFlow", _specAnalyzePropertyFlow);
  _resolveAllValues = wrap("_resolveAllValues", _resolveAllValues);
  _resolveByContextSensitiveReanalysis = wrap("_resolveByContextSensitiveReanalysis", _resolveByContextSensitiveReanalysis);
  _resolveAvBySubstitutingCallerArgs = wrap("_resolveAvBySubstitutingCallerArgs", _resolveAvBySubstitutingCallerArgs);
  _avProjectToTaintDescriptor = wrap("_avProjectToTaintDescriptor", _avProjectToTaintDescriptor);
  _traceValueSource = wrap("_traceValueSource", _traceValueSource);
  _avAtPath = wrap("_avAtPath", _avAtPath);
  _avHasSubstitutableCheck = wrap("_avHasSubstitutableCheck", _avHasSubstitutableCheck);
  _specCollectParamFnNodes = wrap("_specCollectParamFnNodes", _specCollectParamFnNodes);
  _specAvHasParamLeaf = wrap("_specAvHasParamLeaf", _specAvHasParamLeaf);
  globalThis._printTimers = function() {
    var keys = Object.keys(globalThis._wrapT).sort(function(a,b){return globalThis._wrapT[b] - globalThis._wrapT[a];});
    console.log("--- timer dump ---");
    keys.forEach(function(k) {
      var t = globalThis._wrapT[k], c = globalThis._wrapC[k];
      console.log("  " + k + ": " + t + "ms (" + c + " calls, avg " + (t/c).toFixed(3) + "ms)");
    });
  };
  // Print every 30s
  setInterval(function() {
    console.log("[" + Math.floor((Date.now() - globalThis._wrapStart)/1000) + "s elapsed]");
    globalThis._printTimers();
  }, 30000).unref();
`).call(globalThis);

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
}
globalThis._printTimers();
