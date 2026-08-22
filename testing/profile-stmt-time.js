// Time individual statements inside the slowest fn to find the slow operation.
var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
// Instrument _specInitialFunctionBodyState + _specEvalLeaf
astCode = astCode.replace(
  "function _specInitialFunctionBodyState(funcNode, funcPath) {",
  "globalThis._sIfbsCount = 0; globalThis._sIfbsTime = 0; function _specInitialFunctionBodyState_orig(funcNode, funcPath) {"
).replace(
  "function _specEvalLeaf(path, state, vals, effects) {",
  "globalThis._sElCount = 0; function _specEvalLeaf_orig(path, state, vals, effects) {"
);
astCode += `
function _specInitialFunctionBodyState(funcNode, funcPath) {
  globalThis._sIfbsCount++;
  var _t0 = Date.now();
  try { return _specInitialFunctionBodyState_orig(funcNode, funcPath); }
  finally { globalThis._sIfbsTime += (Date.now() - _t0); }
}
function _specEvalLeaf(path, state, vals, effects) {
  globalThis._sElCount++;
  return _specEvalLeaf_orig(path, state, vals, effects);
}
`;
new Function(astCode + "\nglobalThis._babelTraverse = _babelTraverse;\nglobalThis._t = _t;\nglobalThis._specEnsureProgramGlobalsPrepass = _specEnsureProgramGlobalsPrepass;\nglobalThis._specBuildSlice = _specBuildSlice;\nglobalThis._specInitialFunctionBodyState = _specInitialFunctionBodyState;\nglobalThis._specApplyStatement = _specApplyStatement;").call(globalThis);

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

// Find a fn with body matching the slow pattern (large VariableDeclaration).
var slowFnPath = null;
var slowFnSize = 0;
programPath.traverse({
  "FunctionExpression": function (p) {
    if (!p.node.body || !p.node.body.body) return;
    for (var i = 0; i < p.node.body.body.length; i++) {
      var stmt = p.node.body.body[i];
      if (stmt.type === "VariableDeclaration" && (stmt.end - stmt.start) > 100000) {
        var srcSnip = code.slice(p.node.start, p.node.start + 50);
        if (srcSnip.indexOf("function(e,t,n)") === 0 && !slowFnPath) {
          slowFnPath = p;
          slowFnSize = stmt.end - stmt.start;
          break;
        }
      }
    }
  }
});

if (!slowFnPath) {
  console.log("No slow fn found");
  process.exit(0);
}

console.log("Slow fn body has " + slowFnPath.node.body.body.length + " statements");
var bodyPath = slowFnPath.get("body");
var state = globalThis._specInitialFunctionBodyState(slowFnPath.node, slowFnPath);

// Time each statement.
var slowStmtIdx = -1;
for (var si = 0; si < slowFnPath.node.body.body.length; si++) {
  var stmtPath = bodyPath.get("body." + si);
  var stmt = stmtPath.node;
  var declCount = stmt.type === "VariableDeclaration" ? stmt.declarations.length : 0;
  var effects = [];
  var stack = [];
  var t0 = Date.now();
  try {
    globalThis._specApplyStatement(stmtPath, state, effects, stack);
  } catch (e) { console.error("stmt " + si + " threw: " + e.message); }
  var dt = Date.now() - t0;
  console.log("stmt " + si + " (" + stmt.type + ", " + (stmt.end - stmt.start) + " chars, declCount=" + declCount + "): " + dt + "ms, ifbsCalls=" + globalThis._sIfbsCount + ", ifbsTime=" + globalThis._sIfbsTime + "ms, leafEvals=" + globalThis._sElCount);
  globalThis._sIfbsCount = 0; globalThis._sIfbsTime = 0; globalThis._sElCount = 0;
  if (dt > 1000 && slowStmtIdx < 0) slowStmtIdx = si;
}

// Find longest declarators in the slow stmt
if (slowStmtIdx >= 0) {
  var slowStmt = slowFnPath.node.body.body[slowStmtIdx];
  var sizedDecls = [];
  for (var di = 0; di < slowStmt.declarations.length; di++) {
    var decl = slowStmt.declarations[di];
    if (!decl.init) continue;
    sizedDecls.push({ idx: di, size: decl.end - decl.start, type: decl.init.type, decl: decl });
  }
  sizedDecls.sort(function (a, b) { return b.size - a.size; });
  console.log("\n=== Top 10 LARGEST declarators in slow stmt ===");
  for (var sdi = 0; sdi < 10; sdi++) {
    var sd = sizedDecls[sdi];
    var src = code.slice(sd.decl.start, Math.min(sd.decl.start + 120, code.length)).replace(/\n/g, " ");
    console.log("  decl " + sd.idx + " (" + sd.type + ", " + sd.size + " chars): " + src);
  }
}
if (false && slowStmtIdx >= 0) {
  console.log("\n=== Slow stmt " + slowStmtIdx + " — first 20 declarator init types ===");
  var slowStmt = slowFnPath.node.body.body[slowStmtIdx];
  var initTypeCounts = {};
  for (var di = 0; di < slowStmt.declarations.length; di++) {
    var decl = slowStmt.declarations[di];
    if (!decl.init) continue;
    var t = decl.init.type;
    initTypeCounts[t] = (initTypeCounts[t] || 0) + 1;
    if (di < 20) {
      var srcD = code.slice(decl.start, Math.min(decl.start + 80, code.length)).replace(/\n/g, " ");
      console.log("  decl " + di + " (init=" + t + "): " + srcD);
    }
  }
  console.log("\nInit type counts:");
  Object.keys(initTypeCounts).sort(function(a,b){return initTypeCounts[b]-initTypeCounts[a];}).forEach(function(t) {
    console.log("  " + t + ": " + initTypeCounts[t]);
  });
}
