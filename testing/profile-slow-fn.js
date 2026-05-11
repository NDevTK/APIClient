// Profile the specific slow fns in index.js, breaking down per-statement.
var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
// Count _specEvalExpression and _specEvalLeaf
astCode = astCode.replace(
  "function _specEvalExpression(path, state, effects, noWriteMemo) {",
  "globalThis._sEeCount = 0; function _specEvalExpression_orig(path, state, effects, noWriteMemo) {"
).replace(
  "function _specEvalLeaf(path, state, vals, effects) {",
  "globalThis._sElCount = 0; function _specEvalLeaf_orig(path, state, vals, effects) {"
);
astCode += `
function _specEvalExpression(path, state, effects, noWriteMemo) {
  globalThis._sEeCount++;
  return _specEvalExpression_orig(path, state, effects, noWriteMemo);
}
function _specEvalLeaf(path, state, vals, effects) {
  globalThis._sElCount++;
  return _specEvalLeaf_orig(path, state, vals, effects);
}
`;
new Function(astCode + "\nglobalThis.analyzeJSBundle = analyzeJSBundle;\nglobalThis._babelTraverse = _babelTraverse;\nglobalThis._t = _t;\nglobalThis._specAnalyzePropertyFlow = _specAnalyzePropertyFlow;\nglobalThis._specBuildSlice = _specBuildSlice;\nglobalThis._specEnsureProgramGlobalsPrepass = _specEnsureProgramGlobalsPrepass;\nglobalThis._specApplyStatement = _specApplyStatement;\nglobalThis._specInitialFunctionBodyState = _specInitialFunctionBodyState;").call(globalThis);

var code = fs.readFileSync(path.join(rootDir, "testing/harness-dumps/index.js"), "utf8");
var ast = globalThis.BabelBundle.parse(code, { sourceType: "unambiguous", errorRecovery: true });
var programPath = null;
globalThis._babelTraverse(ast, {
  Program: function (p) {
    programPath = p;
    globalThis._specEnsureProgramGlobalsPrepass(p);
    globalThis._specBuildSlice(p);
    p.stop();
  }
});

// Find slow fns by timing each function path
var fnPaths = [];
programPath.traverse({
  "FunctionDeclaration|FunctionExpression|ArrowFunctionExpression|ObjectMethod|ClassMethod|ClassPrivateMethod": function (p) {
    fnPaths.push(p);
  }
});

// First pass: find the 2 slowest with per-phase timing
var t1 = Date.now();
var slowFns = [];
for (var i = 0; i < fnPaths.length; i++) {
  var fp = fnPaths[i];
  // Time _specInitialFunctionBodyState separately
  var tInit = Date.now();
  try {
    globalThis._specInitialFunctionBodyState(fp.node, fp);
  } catch (_) {}
  var dtInit = Date.now() - tInit;
  var t0 = Date.now();
  try { globalThis._specAnalyzePropertyFlow(fp); } catch (_) {}
  var dt = Date.now() - t0;
  if (dt > 1000) slowFns.push({ fp: fp, dt: dt, dtInit: dtInit });
}
console.log("Pass 1: total " + (Date.now() - t1) + "ms");
console.log("_specEvalExpression calls: " + globalThis._sEeCount);
console.log("_specEvalLeaf calls: " + globalThis._sElCount);
console.log("Slow fns (>1000ms):", slowFns.length);
slowFns.forEach(function (sf, idx) {
  var src = code.slice(sf.fp.node.start, Math.min(sf.fp.node.start + 100, code.length));
  console.log("  " + idx + ": apf=" + sf.dt + "ms, initState=" + sf.dtInit + "ms — " + src.replace(/\n/g, " "));
});

if (slowFns.length === 0) {
  console.log("No slow fns found — all already memoised from prior pass.");
  process.exit(0);
}

// Pick the slowest and re-analyze it with statement-level timing
// (We need a FRESH analyzer state — but we already cached via _specEffectsMemo.
//  Skip and just print structure of slow fn instead.)
var sf = slowFns[0];
console.log("\nStructure of slowest fn (" + sf.dt + "ms):");
var fnNode = sf.fp.node;
var body = fnNode.body && fnNode.body.body;
if (body) {
  for (var bi = 0; bi < body.length; bi++) {
    var stmt = body[bi];
    var src = code.slice(stmt.start, Math.min(stmt.start + 150, code.length)).replace(/\n/g, " ");
    console.log("  stmt " + bi + " (" + stmt.type + ", " + (stmt.end - stmt.start) + " chars): " + src);
  }
}
