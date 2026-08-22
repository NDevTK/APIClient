var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode +
  "globalThis.analyzeJSBundle = analyzeJSBundle;\n" +
  "globalThis._pathValMemo = function(){ return _specPathValMemo; };\n").call(globalThis);

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
  if (av.kind === "param") return "param(idx=" + av.idx + ")";
  if (av.kind === "call") return "call(" + dumpAv(av.callee, d+1) + ")";
  if (av.kind === "top") return "TOP";
  return av.kind;
}

var code =
  'var arr = [];\n' +
  'function register(fn) { arr.push(fn); }\n' +
  'function dispatch(opts) { return arr[0](opts); }\n' +
  'register(function(opts){ fetch(opts.url); });\n' +
  'dispatch({url: "/api/v1"});\n';

var r = globalThis.analyzeJSBundle(code, "test://v1", true, null);
var memo = globalThis._pathValMemo();
console.log("fetchCallSites:", JSON.stringify(r.fetchCallSites));

// Find the fetch call site
globalThis.BabelBundle.traverse(r._ast, {
  CallExpression: function(p) {
    if (p.node.callee.type === "Identifier" && p.node.callee.name === "fetch") {
      console.log("\nfetch arg AV:", dumpAv(memo.get(p.node.arguments[0])));
      // Find cb (the enclosing fn)
      var encFn = p.getFunctionParent();
      console.log("encFn.id:", encFn.node.id && encFn.node.id.name, "type:", encFn.node.type, "params:", encFn.node.params.map(function(pn){return pn.name}).join(","));
    }
    if (p.node.callee.type === "MemberExpression" && p.node.callee.property.name === "push") {
      console.log("\npush call args[0] type:", p.node.arguments[0].type);
    }
    if (p.node.callee.type === "MemberExpression" && p.node.callee.object.type === "Identifier" &&
        p.node.callee.object.name === "arr" && p.node.callee.computed && p.node.callee.property.value === 0) {
      console.log("\narr[0](...) callee AV:", dumpAv(memo.get(p.node.callee)));
    }
  }
});
