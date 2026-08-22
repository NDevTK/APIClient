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
  if (d > 5) return "...";
  if (!av) return "null";
  if (av.kind === "const") return JSON.stringify(av.value);
  if (av.kind === "or") return "or(" + dumpAv(av.left, d+1) + "," + dumpAv(av.right, d+1) + ")";
  if (av.kind === "obj-lit") return "obj-lit{" + Object.keys(av.props||{}).join(",") + "}";
  if (av.kind === "array-lit") return "array-lit[" + (av.elements||[]).map(function(e){return dumpAv(e, d+1)}).join(",") + "]";
  if (av.kind === "function-ref") return "function-ref#" + (av.funcNode && av.funcNode.start);
  if (av.kind === "param") return "param(idx=" + av.idx + ")";
  if (av.kind === "top") return "TOP";
  return av.kind;
}

var code =
  'var arr = [];\n' +
  'function register(name, fn) {\n' +
  '  if (typeof name !== "string") { fn = name; name = "*"; }\n' +
  '  arr.push(fn);\n' +
  '}\n' +
  'function dispatch(opts) { return arr[0](opts); }\n' +
  'register(function(opts){ fetch(opts.url); });\n' +
  'dispatch({url: "/api/typeof-rebind"});\n';

var r = globalThis.analyzeJSBundle(code, "test://x", true, null);
var memo = globalThis._pathValMemo();
globalThis.BabelBundle.traverse(r._ast, {
  Identifier: function(p) {
    if (p.node.name === "fn" && p.parent.type === "CallExpression" &&
        p.parent.arguments.indexOf(p.node) === 0 && p.parent.callee.type === "MemberExpression" &&
        p.parent.callee.property.name === "push") {
      console.log("fn at push (L"+p.node.loc.start.line+"): AV =", dumpAv(memo.get(p.node)));
    }
    if (p.node.name === "arr") {
      console.log("arr at L"+p.node.loc.start.line+":C"+p.node.loc.start.column+" parent="+p.parent.type+": AV =", dumpAv(memo.get(p.node)));
    }
  }
});
