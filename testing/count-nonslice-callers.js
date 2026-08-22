// Measure: for each slice fn, how many of its (post-fixpoint discovered)
// callers are NOT in slice? Tells us whether the lazy-caller-analysis fix
// is the right bottleneck attack.
var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode + "\nglobalThis.analyzeJSBundle = analyzeJSBundle; globalThis._specCallSitesByFn = _specCallSitesByFn; globalThis._specSliceFns = _specSliceFns; globalThis.BabelBundle = BabelBundle; globalThis._specBuildSlice = _specBuildSlice; globalThis._specAnalyzeProgramWithFixpoint = _specAnalyzeProgramWithFixpoint; globalThis._babelTraverse = _babelTraverse; globalThis._t = _t;").call(globalThis);

var bundleName = process.argv[2] || "index.js";
var code = fs.readFileSync(path.join(rootDir, "testing/harness-dumps", bundleName), "utf8");
console.log("Bundle: " + bundleName + " (" + (code.length / 1024).toFixed(1) + " KB)");

// Parse + run slice + fixpoint
var ast = globalThis.BabelBundle.parse(code, { sourceType: "unambiguous", errorRecovery: true });
var programPath = null;
globalThis._babelTraverse(ast, { Program: function(p) { programPath = p; p.stop(); } });
console.log("Running slice build + fixpoint...");
globalThis._specBuildSlice(programPath);
globalThis._specAnalyzeProgramWithFixpoint(programPath);
console.log("Fixpoint done.");

// Walk all fns and count those in slice
var allFns = [];
programPath.traverse({
  "FunctionDeclaration|FunctionExpression|ArrowFunctionExpression|ObjectMethod|ClassMethod|ClassPrivateMethod": function(p) {
    allFns.push(p);
  }
});
var sliceCount = 0;
for (var i = 0; i < allFns.length; i++) if (globalThis._specSliceFns.has(allFns[i].node)) sliceCount++;
console.log("Total fns: " + allFns.length + ", slice fns: " + sliceCount + ", non-slice: " + (allFns.length - sliceCount));

// For each slice fn, count call sites in _specCallSitesByFn whose enclosing fn is non-slice
var sliceCallSiteCount = 0;
var nonSliceCallerCount = 0;
var sliceCallSiteFnsWithNonSliceCallers = 0;
var maxNonSliceCallers = 0;
var totalCallSites = 0;
for (var si = 0; si < allFns.length; si++) {
  var fnNode = allFns[si].node;
  if (!globalThis._specSliceFns.has(fnNode)) continue;
  if (!globalThis._specCallSitesByFn.has(fnNode)) continue;
  var callSites = globalThis._specCallSitesByFn.get(fnNode);
  totalCallSites += callSites.length;
  var fnNonSliceCallers = 0;
  for (var csi = 0; csi < callSites.length; csi++) {
    sliceCallSiteCount++;
    var callRef = callSites[csi];
    if (!callRef || !callRef.node) continue;
    var callerEnc = callRef.getFunctionParent && callRef.getFunctionParent();
    var callerNode = callerEnc && callerEnc.node ? callerEnc.node : programPath.node;
    if (!globalThis._specSliceFns.has(callerNode) && globalThis._t.isFunction(callerNode)) {
      nonSliceCallerCount++;
      fnNonSliceCallers++;
    }
  }
  if (fnNonSliceCallers > 0) {
    sliceCallSiteFnsWithNonSliceCallers++;
    if (fnNonSliceCallers > maxNonSliceCallers) maxNonSliceCallers = fnNonSliceCallers;
  }
}
console.log("Total call sites in _specCallSitesByFn for slice fns: " + sliceCallSiteCount);
console.log("Of those, non-slice callers: " + nonSliceCallerCount);
console.log("Slice fns with at least one non-slice caller: " + sliceCallSiteFnsWithNonSliceCallers);
console.log("Max non-slice callers per slice fn: " + maxNonSliceCallers);
