var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode +
  "globalThis.analyzeJSBundle = analyzeJSBundle;\n" +
  "globalThis._pathValMemo = function(){ return _specPathValMemo; };\n" +
  "globalThis._returnMemo = function(){ return _specReturnValueMemo; };\n").call(globalThis);

function dumpAv(av, d) {
  d = d || 0;
  if (d > 4) return "...";
  if (!av) return "null";
  if (av.kind === "const") return JSON.stringify(av.value);
  if (av.kind === "or") return "or(" + dumpAv(av.left, d+1) + "," + dumpAv(av.right, d+1) + ")";
  if (av.kind === "obj-lit") return "obj-lit{" + Object.keys(av.props||{}).join(",") + "}";
  if (av.kind === "array-lit") return "array-lit[" + (av.elements||[]).map(function(e){return dumpAv(e, d+1)}).join(",") + "]";
  if (av.kind === "function-ref") return "function-ref#" + (av.funcNode && av.funcNode.start);
  if (av.kind === "member") return "member(" + dumpAv(av.obj, d+1) + "." + (av.key && av.key.kind === "const" ? JSON.stringify(av.key.value) : av.key && av.key.kind) + ")";
  if (av.kind === "param") return "param(idx=" + av.idx + ",fn#" + (av.fn && av.fn.start) + ")";
  if (av.kind === "call") return "call(...)";
  if (av.kind === "top") return "TOP";
  return av.kind;
}

var code =
  'var registry = [];\n' +
  'function register(fn) { registry.push(fn); }\n' +
  'function dispatch(opts) { return registry[0](opts); }\n' +
  'register(function(opts){ fetch(opts.url); });\n' +
  'dispatch({url: "/api/dispatch"});\n';

var r = globalThis.analyzeJSBundle(code, "test://push", true, null);
console.log("fetchCallSites:", JSON.stringify(r.fetchCallSites));
console.log("resolverErrors:", JSON.stringify((r.resolverErrors||[]).map(function(e){return e.message})));

var memo = globalThis._pathValMemo();

globalThis.BabelBundle.traverse(r._ast, {
  Identifier: function(p) {
    if (p.node.name === "registry" && p.parent.type === "MemberExpression" &&
        p.parent.property.name === "push") {
      console.log("\nregistry @ push (L"+p.node.loc.start.line+"): AV =", dumpAv(memo.get(p.node)));
    }
    if (p.node.name === "registry" && p.parent.type === "MemberExpression" &&
        p.parent.computed && p.parent.property.value === 0) {
      console.log("\nregistry @ [0] (L"+p.node.loc.start.line+"): AV =", dumpAv(memo.get(p.node)));
      console.log("  registry[0] AV =", dumpAv(memo.get(p.parent)));
    }
  }
});
