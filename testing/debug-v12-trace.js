var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode +
  "globalThis.analyzeJSBundle = analyzeJSBundle;\n" +
  "globalThis._pathValMemo = function(){ return _specPathValMemo; };\n" +
  "globalThis._callSitesByFn = function(){ return _specCallSitesByFn; };\n").call(globalThis);

function dumpAv(av, d) {
  d = d || 0;
  if (d > 5) return "...";
  if (!av) return "null";
  if (av.kind === "const") return JSON.stringify(av.value);
  if (av.kind === "or") return "or(" + dumpAv(av.left, d+1) + "," + dumpAv(av.right, d+1) + ")";
  if (av.kind === "obj-lit") return "obj-lit{" + Object.keys(av.props||{}).map(function(k){return k+":"+dumpAv(av.props[k], d+1)}).join(",") + "}";
  if (av.kind === "array-lit") return "array-lit[" + (av.elements||[]).map(function(e){return dumpAv(e, d+1)}).join(",") + "]";
  if (av.kind === "function-ref") return "function-ref#" + (av.funcNode && av.funcNode.start);
  if (av.kind === "member") return "member(" + dumpAv(av.obj, d+1) + "." + (av.key && av.key.kind === "const" ? JSON.stringify(av.key.value) : av.key && av.key.kind) + ")";
  if (av.kind === "param") return "param(idx=" + av.idx + ")";
  if (av.kind === "call") return "call(" + dumpAv(av.callee, d+1) + ")";
  if (av.kind === "top") return "TOP";
  return av.kind;
}

var code =
  'var transports = {};\n' +
  'function makeRegister(o) {\n' +
  '  return function(name, fn) {\n' +
  '    if (typeof name !== "string") { fn = name; name = "*"; }\n' +
  '    (o[name] = o[name] || []).push(fn);\n' +
  '  };\n' +
  '}\n' +
  'var register = makeRegister(transports);\n' +
  'function dispatch(opts) {\n' +
  '  var t = transports["*"];\n' +
  '  if (t && t[0]) return t[0](opts);\n' +
  '}\n' +
  'register(function(opts){ return { send: function(){ fetch(opts.url); } }; });\n' +
  'var tres = dispatch({url: "/api/v12"});\n' +
  'tres.send();\n';

var r = globalThis.analyzeJSBundle(code, "test://v12", true, null);
var memo = globalThis._pathValMemo();

var innerFn = null, makeReg = null;
globalThis.BabelBundle.traverse(r._ast, {
  FunctionDeclaration: function(p) {
    if (p.node.id && p.node.id.name === "makeRegister") makeReg = p.node;
  },
  FunctionExpression: function(p) {
    if (p.node.params.length === 2 && p.node.params[0].name === "name") innerFn = p.node;
  }
});
console.log("makeReg start:", makeReg && makeReg.start);
console.log("innerFn start:", innerFn && innerFn.start);
console.log("makeReg callSites:", (globalThis._callSitesByFn().get(makeReg)||[]).length);
console.log("innerFn callSites:", (globalThis._callSitesByFn().get(innerFn)||[]).length);

globalThis.BabelBundle.traverse(r._ast, {
  Identifier: function(p) {
    if (p.node.name === "transports") {
      var av = memo.get(p.node);
      console.log("transports@L"+p.node.loc.start.line+":C"+p.node.loc.start.column+" parent="+p.parent.type+":", dumpAv(av));
    }
  }
});
