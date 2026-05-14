var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode + "globalThis.analyzeJSBundle = analyzeJSBundle;\n").call(globalThis);

// Test jQuery-like transport-table shape
var code =
  'var transports = {};\n' +
  'function ajaxTransport(name, fn) {\n' +
  '  if (typeof name !== "string") { fn = name; name = "*"; }\n' +
  '  (transports[name] = transports[name] || []).push(fn);\n' +
  '}\n' +
  'function dispatchTransport(opts) {\n' +
  '  var t = transports["*"];\n' +
  '  if (t && t[0]) return t[0](opts);\n' +
  '}\n' +
  'ajaxTransport(function(opts){ var r = { send: function(){ fetch(opts.url); } }; return r; });\n' +
  'var tres = dispatchTransport({url: "/api/jq-shape"});\n' +
  'tres.send();\n';

var r = globalThis.analyzeJSBundle(code, "test://jqs", true, null);
console.log("fetchCallSites:", JSON.stringify(r.fetchCallSites));
console.log("resolverErrors:", JSON.stringify((r.resolverErrors||[]).map(function(e){return e.message})));
