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

// V13: jQuery-shape — helper Ut wraps registry, result is obj-lit method prop
run("v13-jq-objlit-method",
  'var _t = {};\n' +
  'function Ut(o) {\n' +
  '  return function(name, fn) {\n' +
  '    if (typeof name !== "string") { fn = name; name = "*"; }\n' +
  '    (o[name] = o[name] || []).push(fn);\n' +
  '  };\n' +
  '}\n' +
  'var ce = { ajaxTransport: Ut(_t) };\n' +
  'function dispatch(opts) {\n' +
  '  var t = _t["*"];\n' +
  '  if (t && t[0]) return t[0](opts);\n' +
  '}\n' +
  'ce.ajaxTransport(function(opts){ return { send: function(){ fetch(opts.url); } }; });\n' +
  'var tres = dispatch({url: "/api/v13"});\n' +
  'tres.send();');
