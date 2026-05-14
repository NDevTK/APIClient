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

// V14: ce.each-like iterator using Function.prototype.call
run("v14-each-call",
  'function each(arr, cb) {\n' +
  '  for (var i = 0; i < arr.length; i++) cb.call(arr[i], i, arr[i]);\n' +
  '}\n' +
  'each(["/api/v14"], function(idx, val){ fetch(val); });\n');

// V15: ce.each-like + transport pattern
run("v15-each-call-transport",
  'var _t = {};\n' +
  'function each(arr, cb) {\n' +
  '  for (var i = 0; i < arr.length; i++) cb.call(arr[i], i, arr[i]);\n' +
  '}\n' +
  'function Ut(o) {\n' +
  '  return function(name, fn) {\n' +
  '    if (typeof name !== "string") { fn = name; name = "*"; }\n' +
  '    (o[name] = o[name] || []).push(fn);\n' +
  '  };\n' +
  '}\n' +
  'var register = Ut(_t);\n' +
  'function dispatch(opts) {\n' +
  '  var n;\n' +
  '  each(_t["*"], function(idx, t) {\n' +
  '    n = t(opts);\n' +
  '  });\n' +
  '  return n;\n' +
  '}\n' +
  'register(function(opts){ return { send: function(){ fetch(opts.url); } }; });\n' +
  'var tres = dispatch({url: "/api/v15"});\n' +
  'tres.send();');
