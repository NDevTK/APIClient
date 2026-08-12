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

// V4: host fn as method on obj (jQuery-like)
run("v4-host-method",
  'var jq = {};\n' +
  'var transports = [];\n' +
  'jq.ajaxTransport = function(fn) { transports.push(fn); };\n' +
  'jq.dispatch = function(opts) { return transports[0](opts); };\n' +
  'jq.ajaxTransport(function(opts){ fetch(opts.url); });\n' +
  'jq.dispatch({url: "/api/v4"});');
