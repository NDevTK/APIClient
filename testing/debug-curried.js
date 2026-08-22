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

// V11: Curried helper - mirrors jQuery's Ut(_t) pattern
run("v11-curried-helper",
  'var transports = {};\n' +
  'function makeRegister(o) {\n' +
  '  return function(name, fn) {\n' +
  '    if (typeof name !== "string") { fn = name; name = "*"; }\n' +
  '    (o[name] = o[name] || []).push(fn);\n' +
  '  };\n' +
  '}\n' +
  'var register = makeRegister(transports);\n' +
  'function dispatch(opts) {\n' +
  '  var t = transports["*"];\n' +
  '  if (t && t[0]) return t[0](opts);\n' +
  '}\n' +
  'register(function(opts){ return { send: function(){ fetch(opts.url); } }; });\n' +
  'var tres = dispatch({url: "/api/v11"});\n' +
  'tres.send();');
