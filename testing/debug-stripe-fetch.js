// Find Stripe's fetch call and verify the analyzer's AV chain for it.
var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode + "\nglobalThis.analyzeJSBundle = analyzeJSBundle; globalThis._specPathValMemo = _specPathValMemo; globalThis._specSliceFns = _specSliceFns; globalThis._specFnContainsSlice = _specFnContainsSlice; globalThis._babelTraverse = _babelTraverse; globalThis._t = _t; globalThis._specEnsureProgramGlobalsPrepass = _specEnsureProgramGlobalsPrepass; globalThis._specBuildSlice = _specBuildSlice; globalThis._specAnalyzeProgramWithFixpoint = _specAnalyzeProgramWithFixpoint;").call(globalThis);

var code = fs.readFileSync(path.join(rootDir, "testing/harness-dumps/index.js"), "utf8");
var ast = globalThis.BabelBundle.parse(code, { sourceType: "unambiguous", errorRecovery: true });

// Find program path
var programPath = null;
globalThis._babelTraverse(ast, { Program: function(p) { programPath = p; p.stop(); } });
console.log("[1] parse + getProgramPath done");

// Build slice
globalThis._specEnsureProgramGlobalsPrepass(programPath);
globalThis._specBuildSlice(programPath);
console.log("[2] slice built");

// Find the fetch CallExpression
var fetchCallPath = null;
programPath.traverse({
  CallExpression: function(p) {
    var c = p.node.callee;
    if (c && c.type === "MemberExpression" &&
        c.object && c.object.type === "Identifier" && c.object.name === "window" &&
        c.property && c.property.name === "fetch") {
      fetchCallPath = p;
      p.stop();
    }
  }
});
console.log("[3] fetch call found:", !!fetchCallPath, "at line", fetchCallPath && fetchCallPath.node.loc && fetchCallPath.node.loc.start.line);

if (!fetchCallPath) { process.exit(1); }

// Check enclosing fn slice membership
var encFn = fetchCallPath.getFunctionParent();
console.log("[4] encFn type:", encFn && encFn.node.type);
console.log("[5] encFn in _specSliceFns:", encFn && globalThis._specSliceFns.has(encFn.node));
console.log("[6] encFn in _specFnContainsSlice:", encFn && globalThis._specFnContainsSlice.has(encFn.node));

// Walk up ancestors
var anc = encFn ? encFn.parentPath : null;
while (anc && anc.node) {
  if (globalThis._t.isFunction(anc.node)) {
    var inSlice = globalThis._specSliceFns.has(anc.node);
    var hasSlice = globalThis._specFnContainsSlice.has(anc.node);
    console.log("[anc] " + anc.node.type + " inSlice=" + inSlice + " hasSlice=" + hasSlice);
  }
  anc = anc.parentPath;
}

// Count fns in/out of slice
var totalFns = 0, inSlice = 0, hasSlice = 0, skipped = 0;
programPath.traverse({
  "FunctionDeclaration|FunctionExpression|ArrowFunctionExpression|ObjectMethod|ClassMethod|ClassPrivateMethod": function(p) {
    totalFns++;
    if (globalThis._specSliceFns.has(p.node)) inSlice++;
    if (globalThis._specFnContainsSlice.has(p.node)) hasSlice++;
    if (!globalThis._specSliceFns.has(p.node) && !globalThis._specFnContainsSlice.has(p.node)) skipped++;
  }
});
console.log("[stats] totalFns=" + totalFns + " inSlice=" + inSlice + " hasSlice=" + hasSlice + " skipped=" + skipped);

// Skip the long analyzeJSBundle call. Instead manually simulate the
// main pass to see what's slow.
console.log("[before main pass] " + new Date().toISOString());

// First time the main pass traversal alone (program already analyzed via fixpoint above)
var mainPassStart = Date.now();
var visitCount = 0;
var skipCount = 0;
programPath.traverse({
  "FunctionDeclaration|FunctionExpression|ArrowFunctionExpression|ObjectMethod|ClassMethod|ClassPrivateMethod": {
    enter: function (path) {
      if (!globalThis._specSliceFns.has(path.node) && !globalThis._specFnContainsSlice.has(path.node)) {
        skipCount++;
        path.skip();
      }
    }
  },
  CallExpression: function(p) { visitCount++; }
});
console.log("[main-pass-traversal-only] " + (Date.now() - mainPassStart) + "ms, " + visitCount + " CallExpression visits, " + skipCount + " skipped fns");

// Just for verification
var result = { _phaseTimings: null };
if (result._phaseTimings) console.log("[phases]", JSON.stringify(result._phaseTimings));
console.log("[8] fetchCallSites:", (result.fetchCallSites || []).length);
console.log("[9] securitySinks:", (result.securitySinks || []).length);
console.log("[10] resolverErrors:", (result.resolverErrors || []).length);
