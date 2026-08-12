// Decisive: the exact post-analysis AVs the URL resolver consumes for
// M4 — _specPathValMemo[build-call-node] and [o.path-node]. Localizes
// the fix to call-node-AV vs o.path-node-AV vs resolver-not-reading-AV.
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;" +
  "globalThis._pvm=function(){return _specPathValMemo;};").call(globalThis);

function dump(av, d) {
  d = d || 0; if (!av) return String(av); if (d > 5) return av.kind || "?";
  if (av.kind === "call") return "call(" + dump(av.callee, d + 1) + ",[" + (av.args || []).map(function (a) { return dump(a, d + 1); }).join(",") + "])";
  if (av.kind === "builtin-method") return "bm:" + av.id;
  if (av.kind === "obj-lit") return "obj{" + Object.keys(av.props || {}).map(function (k) { return k + ":" + dump(av.props[k], d + 1); }).join(",") + "}";
  if (av.kind === "param") return "param(" + av.idx + ")";
  if (av.kind === "args-elt") return "args-elt";
  if (av.kind === "member") return "member(" + dump(av.obj, d + 1) + "." + (av.key && (av.key.value || av.key.kind)) + ")";
  if (av.kind === "const") return "const=" + JSON.stringify(av.value);
  if (av.kind === "or") return "or(" + dump(av.left, d + 1) + "|" + dump(av.right, d + 1) + ")";
  return av.kind;
}

var code = [
  "function ext(a, b){ for (var k in b) a[k] = b[k]; return a; }",
  "function build(s){ return ext({}, s); }",
  "var o = build({ path: '/api/m4' });",
  "fetch(o.path);"
].join("\n");
var r = globalThis.analyzeJSBundle(code, "t://m4pvm", true, null);
var pvm = globalThis._pvm();
globalThis.BabelBundle.traverse(r._ast, {
  CallExpression: function (p) {
    if (p.node.callee && p.node.callee.name === "build") console.log("build() call-node AV = " + dump(pvm.get(p.node)));
    if (p.node.callee && p.node.callee.name === "fetch") {
      var arg = p.node.arguments[0];
      console.log("fetch arg (" + (arg && arg.type) + ") AV = " + dump(pvm.get(arg)));
      if (arg && arg.type === "MemberExpression") console.log("  o.path .object('o') AV = " + dump(pvm.get(arg.object)));
    }
  },
  VariableDeclarator: function (p) {
    if (p.node.id && p.node.id.name === "o") console.log("var o init AV = " + dump(pvm.get(p.node.init)) + " ; id 'o' AV = " + dump(pvm.get(p.node.id)));
  }
});
