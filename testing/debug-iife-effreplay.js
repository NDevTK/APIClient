var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode +
  "globalThis.analyzeJSBundle = analyzeJSBundle;\n" +
  "globalThis._globalOverrides = function(){ return _specGlobalPropOverrides; };\n" +
  "globalThis._funcPathByNode = function(){ return _specFuncPathByNode; };\n" +
  "globalThis._ptObjProps = function(){ return _specPointsToObjProps; };\n" +
  "globalThis._WILD = _SPEC_PT_WILDCARD_KEY;\n").call(globalThis);

var code = `
(function() {
  function extend(target, src) {
    for (var k in src) target[k] = src[k];
    return target;
  }
  var ce = function() {};
  extend(ce, {
    each: function(e, t) {
      for (var r = 0; r < e.length; r++) t.call(e[r], r, e[r]);
    }
  });
  ce.each(["aaa", "bbb"], function(idx, m) {
    ce[m] = function(url) { fetch("/api/iife3/" + url); };
  });
  window.myLib3 = ce;
})();
myLib3.aaa("xx");
`;
var result = globalThis.analyzeJSBundle(code, "test://iife", true, null);
console.log("fetchCallSites:", (result.fetchCallSites || []).length);

var go = globalThis._globalOverrides();
console.log("\n=== myLib3 override ===");
if (go.myLib3) {
  console.log("kind:", go.myLib3.kind, "funcNode?", !!go.myLib3.funcNode);
  console.log("_extraProps keys:", Object.keys(go.myLib3._extraProps || {}).join(","));
  console.log("_hasUnknownExtraProps:", !!go.myLib3._hasUnknownExtraProps);

  // For the funcNode, look up via _specFuncPathByNode
  var fpbn = globalThis._funcPathByNode();
  var fp = go.myLib3.funcNode ? fpbn.get(go.myLib3.funcNode) : null;
  console.log("\nfuncNode-to-path lookup:", !!fp);
  if (fp) {
    console.log("  fp.parent.type:", fp.parent && fp.parent.type);
    if (fp.parent && fp.parent.type === "VariableDeclarator") {
      console.log("  parent decl id name:", fp.parent.id && fp.parent.id.name);
      var pt = globalThis._ptObjProps().get(fp.parent);
      console.log("  pt for parent decl:", !!pt, pt ? "size=" + pt.size : "");
      if (pt) {
        var ptKeys = [];
        pt.forEach(function (_, k) {
          ptKeys.push(k === globalThis._WILD ? "<WILD>" : k);
        });
        console.log("  pt keys:", ptKeys.join(","));
      }
    }
  }
}
