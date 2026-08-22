// Sub-problem isolation: § 14.15 TryStatement state propagation.
// try-block writes to closure-captured outer should survive past the
// try/catch/finally. Currently the per-block frames are pushed without
// merge or _reportTo, so outer state stays at pre-try.
var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode +
  "globalThis.analyzeJSBundle = analyzeJSBundle;\n").call(globalThis);

function runTest(name, code) {
  console.log("\n=== " + name + " ===");
  var r = globalThis.analyzeJSBundle(code, "test://" + name, true, null);
  console.log("fetchCallSites:", JSON.stringify((r.fetchCallSites||[]).map(function(s){return s.url})));
}

runTest("T1: try-block write visible post-try",
  "var u;\n" +
  "try { u = '/api/try-write'; } catch (e) { }\n" +
  "fetch(u);\n");

runTest("T2: try without catch, finalizer",
  "var u;\n" +
  "try { u = '/api/try-finally'; } finally { }\n" +
  "fetch(u);\n");

runTest("T3: catch-block write visible post-try",
  "var u = '/api/default';\n" +
  "try { throw new Error(); } catch (e) { u = '/api/catch-write'; }\n" +
  "fetch(u);\n");
