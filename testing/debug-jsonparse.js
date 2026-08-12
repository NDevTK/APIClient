var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode + "globalThis.analyzeJSBundle = analyzeJSBundle;").call(globalThis);
var code =
  'var cfg = JSON.parse(\'{"url":"/api/parse","method":"POST"}\');\n' +
  'fetch(cfg.url, {method: cfg.method});\n';
var r = globalThis.analyzeJSBundle(code, "test://js", true, null);
console.log("fetchCallSites:", JSON.stringify((r.fetchCallSites||[]).map(function(s){return {url:s.url,method:s.method}})));
