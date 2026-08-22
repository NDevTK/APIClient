// Diagnostic: trace user-site jQuery Identifier resolution + walk
// through the chain that should populate jQuery's _extraProps.
// Helps isolate WHICH ECMA spec step in the prefilter/transport/extend
// composition fails to propagate.
var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode +
  "globalThis.analyzeJSBundle = analyzeJSBundle;\n" +
  "globalThis._pathValMemo = function(){ return _specPathValMemo; };\n" +
  "globalThis._ctxBySite = function(){ return _specCtxEffectsBySite; };\n" +
  "globalThis._globalOverrides = function(){ return _specGlobalPropOverrides; };\n" +
  "globalThis._funcRefClosure = function(){ return _specFuncRefClosureState; };\n").call(globalThis);

var jq = fs.readFileSync(path.join(rootDir, "testing/harness-dumps/jquery-3.7.1.min.js"), "utf8");
var userCode = "\njQuery.ajax({url: '/api/widgets', method: 'POST', data: {name: 'alpha', count: 3}});\n";
var result = globalThis.analyzeJSBundle(jq + userCode, "test://jq", true, null);

var memo = globalThis._pathValMemo();
function dumpAv(av, d) {
  d = d || 0;
  if (d > 3) return "...";
  if (!av) return "null";
  if (av.kind === "const") return JSON.stringify(av.value).slice(0, 40);
  if (av.kind === "param") return "param(idx=" + av.idx + ",fn#" + (av.fn && av.fn.start) + ")";
  if (av.kind === "or") return "or(" + dumpAv(av.left, d+1) + "," + dumpAv(av.right, d+1) + ")";
  if (av.kind === "obj-lit") return "obj-lit{" + Object.keys(av.props || {}).slice(0, 8).join(",") + (Object.keys(av.props||{}).length > 8 ? ",...(" + Object.keys(av.props).length + ")" : "") + "}";
  if (av.kind === "function-ref") return "function-ref#" + (av.funcNode && av.funcNode.start) + (av._extraProps ? "{extra:" + Object.keys(av._extraProps).length + "}" : "");
  if (av.kind === "member") return "member(" + dumpAv(av.obj, d+1) + "." + (av.key && av.key.kind === "const" ? JSON.stringify(av.key.value) : av.key && av.key.kind) + ")";
  if (av.kind === "top") return "TOP";
  if (av.kind === "builtin-ctor") return "builtin-ctor:" + av.id;
  if (av.kind === "builtin-method") return "builtin-method:" + av.id;
  if (av.kind === "this") return "this";
  return av.kind;
}

// Find the user-code call jQuery.ajax(...)
globalThis.BabelBundle.traverse(result._ast, {
  CallExpression: function(p) {
    var c = p.node.callee;
    if (c && c.type === "MemberExpression" && !c.computed &&
        c.object && c.object.type === "Identifier" && c.object.name === "jQuery" &&
        c.property && c.property.name === "ajax") {
      console.log("=== user-site jQuery.ajax call ===");
      console.log("  callee AV:", dumpAv(memo.get(c)));
      console.log("  jQuery AV:", dumpAv(memo.get(c.object)));
      var jqAv = memo.get(c.object);
      if (jqAv && jqAv._extraProps) {
        console.log("  jQuery._extraProps keys (" + Object.keys(jqAv._extraProps).length + "):", Object.keys(jqAv._extraProps).slice(0, 20).join(","));
        console.log("  has get?", Object.prototype.hasOwnProperty.call(jqAv._extraProps, "get"),
          "has post?", Object.prototype.hasOwnProperty.call(jqAv._extraProps, "post"),
          "has ajax?", Object.prototype.hasOwnProperty.call(jqAv._extraProps, "ajax"));
        console.log("  _hasUnknownExtraProps:", !!jqAv._hasUnknownExtraProps);
        var allKeysRaw = Object.keys(jqAv._extraProps);
        console.log("  has WILDCARD?", allKeysRaw.some(function(k){return k.length > 10;}));
        console.log("  ALL keys:", allKeysRaw.sort().map(function(k){return k.length > 10 ? "<WILDCARD>" : k;}).join(","));
      }
    }
  }
});

// Check globalOverrides: what was set for window.jQuery / window.$
var go = globalThis._globalOverrides();
if (go) {
  console.log("\n=== _specGlobalPropOverrides ===");
  var keys = Object.keys(go);
  console.log("keys count:", keys.length);
  console.log("first 20 keys:", keys.slice(0, 20).join(","));
  if (Object.prototype.hasOwnProperty.call(go, "jQuery")) {
    console.log("jQuery override AV:", dumpAv(go.jQuery));
  }
  if (Object.prototype.hasOwnProperty.call(go, "$")) {
    console.log("$ override AV:", dumpAv(go.$));
  }
}

console.log("\nfetchCallSites:", (result.fetchCallSites || []).length);
console.log("xhrCallSites:", (result.fetchCallSites || []).filter(function(s){return s.type === "xhr"}).length);
console.log("winAliases:", (result.winAliases || []).length);
