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
  if (av.kind === "member") return "member(" + dumpAv(av.obj, d+1) + "." + (av.key && av.key.kind === "const" ? JSON.stringify(av.key.value) : av.key && av.key.kind) + ")";
  if (av.kind === "param") return "param(idx=" + av.idx + ")";
  if (av.kind === "call") return "call(" + dumpAv(av.callee, d+1) + ")";
  if (av.kind === "top") return "TOP";
  return av.kind;
}

var code =
  'var jq = {};\n' +
  'var transports = [];\n' +
  'jq.ajaxTransport = function(fn) { transports.push(fn); };\n' +
  'jq.dispatch = function(opts) { return transports[0](opts); };\n' +
  'jq.ajaxTransport(function(opts){ fetch(opts.url); });\n' +
  'jq.dispatch({url: "/api/v4"});\n';

var r = globalThis.analyzeJSBundle(code, "test://v4", true, null);
var memo = globalThis._pathValMemo();

globalThis.BabelBundle.traverse(r._ast, {
  Identifier: function(p) {
    if (p.node.name === "transports") {
      var av = memo.get(p.node);
      console.log("transports@L"+p.node.loc.start.line+":C"+p.node.loc.start.column+" parent="+p.parent.type+":", dumpAv(av));
    }
  }
});
