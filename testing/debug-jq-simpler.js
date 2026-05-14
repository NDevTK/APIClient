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

// V1: array → push fn → call fn from outer fn (already works)
run("v1-flat",
  'var arr = [];\n' +
  'function register(fn) { arr.push(fn); }\n' +
  'function dispatch(opts) { return arr[0](opts); }\n' +
  'register(function(opts){ fetch(opts.url); });\n' +
  'dispatch({url: "/api/v1"});');

// V2: same but the fn returns an object with a method that does fetch
run("v2-returns-obj",
  'var arr = [];\n' +
  'function register(fn) { arr.push(fn); }\n' +
  'function dispatch(opts) { return arr[0](opts); }\n' +
  'register(function(opts){ return { send: function(){ fetch(opts.url); } }; });\n' +
  'var r = dispatch({url: "/api/v2"});\n' +
  'r.send();');

// V3: obj-lit prop instead of direct array
run("v3-objprop",
  'var transports = {};\n' +
  'function register(name, fn) { (transports[name] = transports[name] || []).push(fn); }\n' +
  'function dispatch(opts) { return transports["*"][0](opts); }\n' +
  'register("*", function(opts){ fetch(opts.url); });\n' +
  'dispatch({url: "/api/v3"});');
