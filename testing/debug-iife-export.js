var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode +
  "globalThis.analyzeJSBundle = analyzeJSBundle;\n" +
  "globalThis._globalOverrides = function(){ return _specGlobalPropOverrides; };\n" +
  "globalThis._pathValMemo = function(){ return _specPathValMemo; };\n").call(globalThis);

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
console.log("globalOverrides keys:", Object.keys(go).join(","));
if (go.myLib3) {
  console.log("myLib3 AV kind:", go.myLib3.kind);
  if (go.myLib3._extraProps) {
    var keys = Object.keys(go.myLib3._extraProps);
    console.log("myLib3._extraProps keys (" + keys.length + "):", keys.join(","));
    console.log("  has each?", Object.prototype.hasOwnProperty.call(go.myLib3._extraProps, "each"),
      "has aaa?", Object.prototype.hasOwnProperty.call(go.myLib3._extraProps, "aaa"),
      "has bbb?", Object.prototype.hasOwnProperty.call(go.myLib3._extraProps, "bbb"));
  console.log("  _hasUnknownExtraProps?", !!go.myLib3._hasUnknownExtraProps);
  console.log("  all _extraProps keys (raw):", Object.keys(go.myLib3._extraProps).map(function(k){return k.length>20?"WILD-"+k.length:k;}).join(","));
  } else {
    console.log("  no _extraProps");
  }
}
