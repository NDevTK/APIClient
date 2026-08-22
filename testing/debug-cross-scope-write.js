var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode +
  "globalThis.analyzeJSBundle = analyzeJSBundle;\n").call(globalThis);

// Test 1: cross-scope assignment
console.log("=== Test 1: outer assignment from inner fn ===");
var code1 =
  'var u = "/init";\n' +
  'function f() { u = "/api/cross-assign"; }\n' +
  'f();\n' +
  'fetch(u);\n';
var r1 = globalThis.analyzeJSBundle(code1, "test://x1", true, null);
console.log("  fetchCallSites:", JSON.stringify(r1.fetchCallSites));

// Test 2: cross-scope array push
console.log("\n=== Test 2: outer array push from inner fn ===");
var code2 =
  'var arr = [];\n' +
  'function register(x) { arr.push(x); }\n' +
  'register("/api/cross-push");\n' +
  'fetch(arr[0]);\n';
var r2 = globalThis.analyzeJSBundle(code2, "test://x2", true, null);
console.log("  fetchCallSites:", JSON.stringify(r2.fetchCallSites));
console.log("  resolverErrors:", JSON.stringify((r2.resolverErrors||[]).map(function(e){return e.message})));

// Test 3: inline (sanity)
console.log("\n=== Test 3: inline assign + read ===");
var code3 =
  'var arr = [];\n' +
  'arr.push("/api/inline-push2");\n' +
  'fetch(arr[0]);\n';
var r3 = globalThis.analyzeJSBundle(code3, "test://x3", true, null);
console.log("  fetchCallSites:", JSON.stringify(r3.fetchCallSites));
