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

run("computed-ctor-cond",
  'class A { send(u){ fetch(u); } }\n' +
  'class B { send(u){ fetch(u); } }\n' +
  'var Cls = (Math.random() > 0.5) ? A : B;\n' +
  'new Cls().send("/api/computed-ctor");');

run("computed-ctor-inline",
  'class A { send(u){ fetch(u); } }\n' +
  'class B { send(u){ fetch(u); } }\n' +
  'new ((Math.random() > 0.5) ? A : B)().send("/api/computed-inline");');
