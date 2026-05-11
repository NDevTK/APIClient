// Count slice size vs total function count in a bundle.
var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();

// Instrument _specBuildSlice to log slice size before main pass runs.
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
// Inject a log after the slice is built.
astCode = astCode.replace(
  "_specEntryPathsByProgram.set(programPath.node, entryPaths);",
  "console.log('[slice] total fns: ' + fnPaths.length + ', slice size: ' + slicePaths.length + ', entries: ' + entryPaths.length); _specEntryPathsByProgram.set(programPath.node, entryPaths);"
);
new Function(astCode + "\nglobalThis.analyzeJSBundle = analyzeJSBundle;").call(globalThis);

var bundleName = process.argv[2];
var bundlePath = path.join(rootDir, "testing/harness-dumps", bundleName);
var code = fs.readFileSync(bundlePath, "utf8");
console.log("Bundle: " + bundleName + " (" + (code.length / 1024).toFixed(1) + " KB)");

// Build slice only — don't run full analysis. We'll let it start but kill via process timer.
try {
  var deadlineMs = 30000;
  // Spawn child? No, just analyze and exit on slice log via process.exit hack: nope, slice log is sync.
  // Just parse and call _specBuildSlice directly.
  var ast = globalThis.BabelBundle.parse(code, {
    sourceType: "unambiguous", errorRecovery: true,
    plugins: ["jsx", "asyncDoExpressions", "decoratorAutoAccessors", "decorators", "destructuringPrivate", "doExpressions", "exportDefaultFrom", "explicitResourceManagement", "functionBind", "functionSent", "importAssertions", "importAttributes", "moduleBlocks", "partialApplication", ["pipelineOperator", { proposal: "hack", topicToken: "#" }], "privateIn", "regexpUnicodeSets", "throwExpressions", "asyncGenerators", "classPrivateMethods", "classPrivateProperties", "classProperties", "classStaticBlock", "deferredImportEvaluation", "dynamicImport", "exponentiationOperator", "logicalAssignment", "nullishCoalescingOperator", "numericSeparator", "objectRestSpread", "optionalCatchBinding", "optionalChaining", "topLevelAwait"]
  });
  globalThis.BabelBundle.traverse(ast, {
    Program: function (p) {
      // Inline what _specBuildSlice does (it's a function defined inside ast.js)
      // by invoking analyzeJSBundle and catching the slice log. Easier: just
      // run analyzeJSBundle and rely on the inserted console.log to fire
      // BEFORE the slow mainPass.
    }
  });
} catch (e) { console.error(e.message); }

// Just run analyze - the inserted console.log will print slice size before mainPass.
console.log("[start] analyzing...");
var t0 = Date.now();
try {
  var result = globalThis.analyzeJSBundle(code, "https://test.example.com/" + bundleName, true, null);
  console.log("[done] " + (Date.now() - t0) + "ms");
} catch (e) {
  console.error("threw: " + e.message);
}
