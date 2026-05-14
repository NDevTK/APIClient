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

// V5: factory returns obj with send method (closes over opts)
run("v5-returns-obj-send",
  'var arr = [];\n' +
  'function register(fn) { arr.push(fn); }\n' +
  'function dispatch(opts) { return arr[0](opts); }\n' +
  'register(function(opts){ return { send: function(){ fetch(opts.url); } }; });\n' +
  'var r = dispatch({url: "/api/v5"});\n' +
  'r.send();');

// V6: factory + intermediate var + obj-prop registry + typeof rebind
run("v6-jq-full-shape",
  'var transports = {};\n' +
  'function register(name, fn) {\n' +
  '  if (typeof name !== "string") { fn = name; name = "*"; }\n' +
  '  (transports[name] = transports[name] || []).push(fn);\n' +
  '}\n' +
  'function dispatch(opts) {\n' +
  '  var t = transports["*"];\n' +
  '  if (t && t[0]) return t[0](opts);\n' +
  '}\n' +
  'register(function(opts){ var r = { send: function(){ fetch(opts.url); } }; return r; });\n' +
  'var tres = dispatch({url: "/api/v6"});\n' +
  'tres.send();');

// V7: simpler - obj-prop registry without typeof
run("v7-objprop-returns-obj",
  'var transports = {};\n' +
  'function register(name, fn) {\n' +
  '  (transports[name] = transports[name] || []).push(fn);\n' +
  '}\n' +
  'function dispatch(opts) {\n' +
  '  return transports["*"][0](opts);\n' +
  '}\n' +
  'register("*", function(opts){ return { send: function(){ fetch(opts.url); } }; });\n' +
  'var tres = dispatch({url: "/api/v7"});\n' +
  'tres.send();');

// V8: v7 + intermediate var t
run("v8-intermediate-t",
  'var transports = {};\n' +
  'function register(name, fn) {\n' +
  '  (transports[name] = transports[name] || []).push(fn);\n' +
  '}\n' +
  'function dispatch(opts) {\n' +
  '  var t = transports["*"];\n' +
  '  return t[0](opts);\n' +
  '}\n' +
  'register("*", function(opts){ return { send: function(){ fetch(opts.url); } }; });\n' +
  'var tres = dispatch({url: "/api/v8"});\n' +
  'tres.send();');

// V9: v8 + if guard
run("v9-if-guard",
  'var transports = {};\n' +
  'function register(name, fn) {\n' +
  '  (transports[name] = transports[name] || []).push(fn);\n' +
  '}\n' +
  'function dispatch(opts) {\n' +
  '  var t = transports["*"];\n' +
  '  if (t && t[0]) return t[0](opts);\n' +
  '}\n' +
  'register("*", function(opts){ return { send: function(){ fetch(opts.url); } }; });\n' +
  'var tres = dispatch({url: "/api/v9"});\n' +
  'tres.send();');

// V10: v9 + typeof rebind
run("v10-typeof-rebind-full",
  'var transports = {};\n' +
  'function register(name, fn) {\n' +
  '  if (typeof name !== "string") { fn = name; name = "*"; }\n' +
  '  (transports[name] = transports[name] || []).push(fn);\n' +
  '}\n' +
  'function dispatch(opts) {\n' +
  '  var t = transports["*"];\n' +
  '  if (t && t[0]) return t[0](opts);\n' +
  '}\n' +
  'register(function(opts){ return { send: function(){ fetch(opts.url); } }; });\n' +
  'var tres = dispatch({url: "/api/v10"});\n' +
  'tres.send();');
