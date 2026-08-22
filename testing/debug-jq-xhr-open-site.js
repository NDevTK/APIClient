// Locate xhr.open call site in jQuery and walk back through encFn chain
// to identify which functions are reached and what their params bind to.
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
  "globalThis._sliceFns = function(){ return _specSliceFns; };\n" +
  "globalThis._callSitesByFn = function(){ return _specCallSitesByFn; };\n" +
  "globalThis._specFuncPathByNode = _specFuncPathByNode;\n" +
  "globalThis._t = _t;\n").call(globalThis);

function dumpAv(av, d) {
  d = d || 0;
  if (d > 5) return "...";
  if (!av) return "null";
  if (av.kind === "const") return JSON.stringify(av.value);
  if (av.kind === "or") return "or(" + dumpAv(av.left, d+1) + "," + dumpAv(av.right, d+1) + ")";
  if (av.kind === "obj-lit") return "obj-lit{" + Object.keys(av.props||{}).join(",") + "}";
  if (av.kind === "array-lit") return "array-lit[" + (av.elements||[]).length + "elts]";
  if (av.kind === "function-ref") return "function-ref#" + (av.funcNode && av.funcNode.start);
  if (av.kind === "member") return "member(" + dumpAv(av.obj, d+1) + "." + (av.key && av.key.kind === "const" ? JSON.stringify(av.key.value) : av.key && av.key.kind) + ")";
  if (av.kind === "param") return "param(idx=" + av.idx + ",fn#" + (av.fn && av.fn.start) + ")";
  if (av.kind === "call") return "call(" + dumpAv(av.callee, d+1) + ",[...])";
  if (av.kind === "this") return "this";
  if (av.kind === "top") return "TOP";
  return av.kind;
}

var jq = fs.readFileSync(path.join(rootDir, "testing/harness-dumps/jquery-3.7.1.min.js"), "utf8");
var userCode = "\njQuery.ajax({url: '/api/widgets', method: 'POST'});\n";
var result = globalThis.analyzeJSBundle(jq + userCode, "test://jq", true, null);
var memo = globalThis._pathValMemo();
var slice = globalThis._sliceFns();

var xhrOpenSite = null;
globalThis.BabelBundle.traverse(result._ast, {
  CallExpression: function(p) {
    var c = p.node.callee;
    if (c && c.type === "MemberExpression" && !c.computed &&
        c.property && c.property.name === "open" && p.node.arguments.length >= 2) {
      xhrOpenSite = p;
    }
  }
});

console.log("xhr.open site at L" + xhrOpenSite.node.loc.start.line + ":C" + xhrOpenSite.node.loc.start.column);
console.log("  recv AV:", dumpAv(memo.get(xhrOpenSite.node.callee.object)));
console.log("  method AV:", dumpAv(memo.get(xhrOpenSite.node.arguments[0])));
console.log("  url AV:", dumpAv(memo.get(xhrOpenSite.node.arguments[1])));

// Walk up the enclosing function chain
var encFn = xhrOpenSite.getFunctionParent();
var depth = 0;
while (encFn && depth < 8) {
  var n = encFn.node;
  var label = n.id && n.id.name ? n.id.name : "anon@L"+n.loc.start.line+":"+n.start;
  console.log("\nEnclosing fn " + depth + ": " + label + " (start=" + n.start + ")");
  console.log("  params:", n.params.map(function(p) { return p.name || p.type; }).join(","));
  console.log("  inSlice:", slice.has(n));
  var sites = globalThis._callSitesByFn().get(n);
  console.log("  callSites:", sites ? sites.length : 0);
  if (sites && sites.length > 0) {
    sites.slice(0, 3).forEach(function(s, i) {
      var calleeExpr = s.node.callee;
      var calleeText = calleeExpr.type === "Identifier" ? calleeExpr.name :
                        calleeExpr.type === "MemberExpression" ? (calleeExpr.property && calleeExpr.property.name) : calleeExpr.type;
      console.log("    site" + i + ": ." + calleeText + "(...) at L" + s.node.loc.start.line + ":" + s.node.loc.start.column);
    });
  }
  encFn = encFn.parentPath && encFn.parentPath.getFunctionParent && encFn.parentPath.getFunctionParent();
  depth++;
}
