// Profile _specAnalyzePropertyFlow for the outer IIFE of index.js.
// Compares per-fn analysis time vs body size.
var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode + "\nglobalThis.analyzeJSBundle = analyzeJSBundle;\nglobalThis._babelTraverse = _babelTraverse;\nglobalThis._t = _t;\nglobalThis._specAnalyzePropertyFlow = _specAnalyzePropertyFlow;\nglobalThis._specBuildSlice = _specBuildSlice;\nglobalThis._specEnsureProgramGlobalsPrepass = _specEnsureProgramGlobalsPrepass;").call(globalThis);

var code = fs.readFileSync(path.join(rootDir, "testing/harness-dumps/index.js"), "utf8");
var ast = globalThis.BabelBundle.parse(code, { sourceType: "unambiguous", errorRecovery: true });

// Run the slice build first
var programPath = null;
globalThis._babelTraverse(ast, {
  Program: function(p) {
    programPath = p;
    globalThis._specEnsureProgramGlobalsPrepass(p);
    globalThis._specBuildSlice(p);
    p.stop();
  }
});

// Now collect all function paths and time each analysis
var fnPaths = [];
programPath.traverse({
  "FunctionDeclaration|FunctionExpression|ArrowFunctionExpression|ObjectMethod|ClassMethod|ClassPrivateMethod": function(p) {
    fnPaths.push(p);
  }
});
console.log("Total fns:", fnPaths.length);

// Time each fn analysis
var timings = [];
var totalMs = 0;
var allFnTime = Date.now();
for (var i = 0; i < fnPaths.length; i++) {
  var fp = fnPaths[i];
  var bodySize = 0;
  if (fp.node.body && fp.node.body.body) bodySize = fp.node.body.body.length;
  var t0 = Date.now();
  try {
    globalThis._specAnalyzePropertyFlow(fp);
  } catch (e) { console.error("analyze failed:", e.message); }
  var dt = Date.now() - t0;
  totalMs += dt;
  if (dt > 5) timings.push({ idx: i, dt: dt, bodyStmts: bodySize, fp: fp, loc: fp.node.loc && fp.node.loc.start });
}
console.log("Total time for all " + fnPaths.length + " fn analyses: " + totalMs + "ms");
console.log("Wall-clock time: " + (Date.now() - allFnTime) + "ms");
timings.sort(function (a, b) { return b.dt - a.dt; });
console.log("\nTop 5 slowest fn analyses:");
for (var ti = 0; ti < Math.min(5, timings.length); ti++) {
  var t = timings[ti];
  var fnType = t.fp.node.type;
  var firstParam = (t.fp.node.params && t.fp.node.params[0] && t.fp.node.params[0].name) || "?";
  var fnStart = t.fp.node.start || 0;
  var src = code.slice(fnStart, Math.min(fnStart + 200, code.length)).replace(/\n/g, " ");
  console.log("  " + t.dt + "ms, " + t.bodyStmts + " stmts, type=" + fnType + ", param0=" + firstParam);
  console.log("    src: " + src);
}
console.log("\nTotal >5ms analyses:", timings.length);
var totalMs = timings.reduce(function (s, t) { return s + t.dt; }, 0);
console.log("Cumulative ms in slow analyses:", totalMs);
