// Trace what happens INSIDE jQuery.ajax when called by user code.
// Goal: identify which AST nodes from inside ajax body are reached
// during analysis, and where TOP/null infiltrates the chain.
var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode +
  "globalThis.analyzeJSBundle = analyzeJSBundle;\n" +
  "globalThis._pathValMemo = function(){ return _specPathValMemo; };\n" +
  "globalThis._returnMemo = function(){ return _specReturnValueMemo; };\n" +
  "globalThis._sliceFns = function(){ return _specSliceFns; };\n").call(globalThis);

var jq = fs.readFileSync(path.join(rootDir, "testing/harness-dumps/jquery-3.7.1.min.js"), "utf8");
var userCode = "\njQuery.ajax({url: '/api/widgets', method: 'POST'});\n";
var result = globalThis.analyzeJSBundle(jq + userCode, "test://jq", true, null);

console.log("fetchCallSites:", (result.fetchCallSites || []).length);
console.log("xhrCallSites:", (result.fetchCallSites || []).filter(function(s) { return s.type === "xhr"; }).length);
console.log("resolverErrors:", (result.resolverErrors || []).length);
if (result.resolverErrors && result.resolverErrors.length > 0) {
  console.log("\nFirst 5 resolver errors:");
  result.resolverErrors.slice(0, 5).forEach(function(e, i) {
    console.log("  " + i + ":", e.message);
  });
}

// Find xhr.open call sites in jQuery — is the AST reaching them?
var memo = globalThis._pathValMemo();
var xhrOpenCount = 0;
var xhrOpenWithMethodAv = [];
globalThis.BabelBundle.traverse(result._ast, {
  CallExpression: function(p) {
    var c = p.node.callee;
    if (c && c.type === "MemberExpression" && !c.computed &&
        c.property && c.property.name === "open" && p.node.arguments.length >= 2) {
      xhrOpenCount++;
      // Check if the receiver's AV looks like XHR
      var recvAv = memo.get(c.object);
      if (xhrOpenCount <= 5) {
        xhrOpenWithMethodAv.push({
          line: p.node.loc.start.line + ":" + p.node.loc.start.column,
          recvAvKind: recvAv && recvAv.kind,
          methodArgKind: p.node.arguments[0] && (memo.get(p.node.arguments[0]) || {}).kind,
          urlArgKind: p.node.arguments[1] && (memo.get(p.node.arguments[1]) || {}).kind
        });
      }
    }
  }
});
console.log("\nxhr.open call sites in jQuery source:", xhrOpenCount);
console.log("First 5 with AV info:");
xhrOpenWithMethodAv.forEach(function(x) {
  console.log("  " + x.line, "recv:", x.recvAvKind, "method:", x.methodArgKind, "url:", x.urlArgKind);
});
