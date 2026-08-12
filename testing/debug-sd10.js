var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode +
  "globalThis.analyzeJSBundle = analyzeJSBundle;\n" +
  "globalThis._pathValMemo = function(){ return _specPathValMemo; };\n" +
  "globalThis._effectsMemo = function(){ return _specEffectsMemo; };\n" +
  "globalThis._ctxBySite = function(){ return _specCtxEffectsBySite; };\n" +
  "globalThis._t = _t;\n").call(globalThis);

var code = "\nfunction extend(target, src) { for (var k in src) target[k] = src[k]; return target; }\nfunction ajax(opts) { var s = {}; extend(s, opts); var xhr = new XMLHttpRequest(); xhr.open('GET', s.url); xhr.send(); }\najax({url: '/api/sd10'});\n";
var result = globalThis.analyzeJSBundle(code, "test://sd10", true, null);
console.log("fetchCallSites:", (result.fetchCallSites || []).length);

var memo = globalThis._pathValMemo();
var openCalls = [];
globalThis.BabelBundle.traverse(result._ast, {
  CallExpression: function (p) {
    var c = p.node.callee;
    if (c && c.type === "MemberExpression" && c.property && c.property.name === "open") {
      var args = p.node.arguments;
      var arg1 = args[1];
      openCalls.push({ arg1: arg1, av: memo.get(arg1), loc: p.node.loc });
    }
  }
});
console.log("xhr.open sites:", openCalls.length);
openCalls.forEach(function (oc) {
  console.log("  loc:", oc.loc.start.line + ":" + oc.loc.start.column);
  console.log("  arg1 type:", oc.arg1.type);
  console.log("  arg1 AV kind:", oc.av && oc.av.kind);
  if (oc.av && oc.av.kind === "member") {
    console.log("  arg1 AV.obj.kind:", oc.av.obj && oc.av.obj.kind);
    console.log("  arg1 AV.obj._hasUnknownExtraProps:", oc.av.obj && oc.av.obj._hasUnknownExtraProps);
    console.log("  arg1 AV.key:", oc.av.key && JSON.stringify(oc.av.key));
  }
});

var effs = globalThis._effectsMemo();
console.log("\nEffects per slice fn:");
effs.forEach && console.log("  WeakMap not enumerable");
