var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode +
  "globalThis.analyzeJSBundle = analyzeJSBundle;\n" +
  "globalThis._sliceFns = function(){ return _specSliceFns; };\n" +
  "globalThis._returnMemo = function(){ return _specReturnValueMemo; };\n").call(globalThis);

var r = globalThis.analyzeJSBundle(
  "var make = function(matches){ return matches ? { send: function(u){ fetch(u); } } : undefined; };\n" +
  "var transports = [function(){ return make(false); }, function(){ return make(true); }];\n" +
  "var selected;\n" +
  "transports.forEach(function(fn){ var r = fn(); if (r) selected = r; });\n" +
  "if (selected) selected.send('/api/transport-pick');\n",
  "test://full", true, null);

var slice = globalThis._sliceFns();
var memo = globalThis._returnMemo();
globalThis.BabelBundle.traverse(r._ast, {
  FunctionExpression: function(p) {
    var paramNames = (p.node.params||[]).map(function(p){return p.name;}).join(",");
    console.log("FE#" + p.node.start + " params=[" + paramNames + "] inSlice=" + slice.has(p.node) + " hasReturnMemo=" + memo.has(p.node));
  }
});
console.log("\nfetchCallSites:", JSON.stringify((r.fetchCallSites||[]).map(function(s){return s.url})));
