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
  "globalThis._funcRefClosureState = function(){ return _specFuncRefClosureState; };\n" +
  "globalThis._returnMemo = function(){ return _specReturnValueMemo; };\n" +
  "globalThis._effectsMemo = function(){ return _specEffectsMemo; };\n").call(globalThis);

var code = `
function Ut(o) {
  return function(e, t) {
    if (typeof e !== "string") { t = e; e = "*"; }
    (o[e] = o[e] || []).push(t);
  };
}
function Vt(t, opts) {
  var arr = t["*"] || [];
  for (var i = 0; i < arr.length; i++) {
    var r = arr[i](opts);
    if (r && r.send) { r.send(); return; }
  }
}
var _t = {};
var ajaxTransport = Ut(_t);
ajaxTransport(function(opts) {
  return {
    send: function() {
      var x = new XMLHttpRequest();
      x.open(opts.method, opts.url);
    }
  };
});
Vt(_t, {url: "/api/jq6", method: "POST"});
`;
var result = globalThis.analyzeJSBundle(code, "test://jq6", true, null);
console.log("fetchCallSites:", (result.fetchCallSites || []).length);

var memo = globalThis._pathValMemo();
globalThis.BabelBundle.traverse(result._ast, {
  Identifier: function(p) {
    if (p.node.name === "_t" && p.parent.type === "CallExpression" && p.parent.callee &&
        p.parent.callee.type === "Identifier" && p.parent.callee.name === "Vt" &&
        p.parent.arguments[0] === p.node) {
      var av = memo.get(p.node);
      console.log("Vt(_t, ...) — _t AV at call site:");
      if (av) {
        console.log("  kind:", av.kind);
        if (av.props) {
          console.log("  props keys:", Object.keys(av.props));
          console.log("  has '*'?", Object.prototype.hasOwnProperty.call(av.props, "*"));
          if (av.props["*"]) console.log("  '*' kind:", av.props["*"].kind, av.props["*"].elements ? "elements:" + av.props["*"].elements.length : "");
        }
      }
    }
  }
});
