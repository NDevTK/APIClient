var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode +
  "globalThis.analyzeJSBundle = analyzeJSBundle;\n" +
  "globalThis._pathValMemo = function(){ return _specPathValMemo; };\n" +
  "globalThis._returnMemo = function(){ return _specReturnValueMemo; };\n" +
  "globalThis._sliceFns = function(){ return _specSliceFns; };\n" +
  "globalThis._callSitesByFn = function(){ return _specCallSitesByFn; };\n").call(globalThis);

function dumpAv(av, d) {
  d = d || 0;
  if (d > 5) return "...";
  if (!av) return "null";
  if (av.kind === "const") return JSON.stringify(av.value);
  if (av.kind === "or") return "or(" + dumpAv(av.left, d+1) + "," + dumpAv(av.right, d+1) + ")";
  if (av.kind === "obj-lit") return "obj-lit{" + Object.keys(av.props||{}).join(",") + "}";
  if (av.kind === "function-ref") return "function-ref#" + (av.funcNode && av.funcNode.start);
  if (av.kind === "member") return "member(" + dumpAv(av.obj, d+1) + "." + (av.key && av.key.kind === "const" ? JSON.stringify(av.key.value) : av.key && av.key.kind) + ")";
  if (av.kind === "param") return "param(idx=" + av.idx + ",fn#" + (av.fn && av.fn.start) + ")";
  if (av.kind === "call") return "call(" + dumpAv(av.callee, d+1) + ", [" + (av.args||[]).map(function(a){return dumpAv(a, d+1)}).join(",") + "])";
  if (av.kind === "builtin-ctor") return "builtin-ctor:" + av.id;
  if (av.kind === "top") return "TOP";
  return av.kind;
}

var code =
  'function trans(opts){ var xhr = opts.xhr(); xhr.open(opts.method, opts.url); }\n' +
  'function dispatch(opts){ trans(opts); }\n' +
  'dispatch({xhr: function(){ return new XMLHttpRequest(); }, method: "POST", url: "/api/jq2"});\n';
var r = globalThis.analyzeJSBundle(code, "test://jq", true, null);
console.log("fetchCallSites:", JSON.stringify(r.fetchCallSites));

var memo = globalThis._pathValMemo();
var slice = globalThis._sliceFns();
var callSitesByFn = globalThis._callSitesByFn();

// Find trans, dispatch, factory funcs
var transNode = null, dispatchNode = null, factoryNode = null;
globalThis.BabelBundle.traverse(r._ast, {
  FunctionDeclaration: function(p) {
    if (p.node.id.name === "trans") transNode = p.node;
    if (p.node.id.name === "dispatch") dispatchNode = p.node;
  },
  FunctionExpression: function(p) {
    if (!p.node.params.length) factoryNode = p.node;
  }
});

console.log("\ntrans inSlice:", slice.has(transNode), "callSites:", (callSitesByFn.get(transNode)||[]).length);
console.log("dispatch inSlice:", slice.has(dispatchNode), "callSites:", (callSitesByFn.get(dispatchNode)||[]).length);
console.log("factory inSlice:", slice.has(factoryNode), "callSites:", (callSitesByFn.get(factoryNode)||[]).length);

// Find xhr.open
globalThis.BabelBundle.traverse(r._ast, {
  CallExpression: function(p) {
    var c = p.node.callee;
    if (c && c.type === "MemberExpression" && c.property && c.property.name === "open") {
      console.log("\nxhr.open call:");
      console.log("  callee.object (xhr) AV:", dumpAv(memo.get(c.object)));
      console.log("  arg0 (method) AV:", dumpAv(memo.get(p.node.arguments[0])));
      console.log("  arg1 (url) AV:", dumpAv(memo.get(p.node.arguments[1])));
    }
  }
});
