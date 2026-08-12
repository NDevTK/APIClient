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

runTest("Class method calls fetch",
  "class C { send(url) { fetch(url); } }\n" +
  "new C().send('/api/class-method');\n");

runTest("Class method calls fetch — via var",
  "class C { send(url) { fetch(url); } }\n" +
  "var c = new C();\n" +
  "c.send('/api/class-via-var');\n");

runTest("Class method chained",
  "class C { send(url) { fetch(url); return this; } }\n" +
  "new C().send('/api/chained');\n");

runTest("Class extends, super",
  "class B { send(url) { fetch(url); } }\n" +
  "class D extends B { call(u) { this.send(u); } }\n" +
  "new D().call('/api/extends');\n");
