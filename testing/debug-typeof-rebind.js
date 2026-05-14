var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode + "globalThis.analyzeJSBundle = analyzeJSBundle;\n").call(globalThis);

function run(label, code) {
  console.log("=== " + label + " ===");
  var r = globalThis.analyzeJSBundle(code, "test://" + label, true, null);
  console.log("  fetchCallSites:", JSON.stringify(r.fetchCallSites));
  if (r.resolverErrors && r.resolverErrors.length) {
    console.log("  resolverErrors:", JSON.stringify(r.resolverErrors.map(function(e){return e.message})));
  }
  console.log("");
}

run("typeof-rebind-direct",
  'var arr = [];\n' +
  'function register(name, fn) {\n' +
  '  if (typeof name !== "string") { fn = name; name = "*"; }\n' +
  '  arr.push(fn);\n' +
  '}\n' +
  'function dispatch(opts) { return arr[0](opts); }\n' +
  'register(function(opts){ fetch(opts.url); });\n' +
  'dispatch({url: "/api/typeof-rebind"});');
