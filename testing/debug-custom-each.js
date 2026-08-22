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
  "globalThis._callSitesByFn = function(){ return _specCallSitesByFn; };\n" +
  "globalThis._readersOf = function(){ return _specReadersOf; };\n").call(globalThis);

function dumpAv(av, d) {
  d = d || 0;
  if (d > 6) return "...";
  if (!av) return "null";
  if (av.kind === "const") return JSON.stringify(av.value);
  if (av.kind === "or") return "or(" + dumpAv(av.left, d+1) + "," + dumpAv(av.right, d+1) + ")";
  if (av.kind === "obj-lit") return "obj-lit{" + Object.keys(av.props||{}).join(",") + "}";
  if (av.kind === "array-lit") return "array-lit[" + (av.elements||[]).map(function(e){return dumpAv(e, d+1)}).join(",") + "]";
  if (av.kind === "function-ref") return "function-ref#" + (av.funcNode && av.funcNode.start);
  if (av.kind === "member") return "member(" + dumpAv(av.obj, d+1) + "." + (av.key && av.key.kind === "const" ? JSON.stringify(av.key.value) : av.key && av.key.kind) + ")";
  if (av.kind === "param") return "param(idx=" + av.idx + ",fn#" + (av.fn && av.fn.start) + ")";
  if (av.kind === "call") return "call(" + dumpAv(av.callee, d+1) + ", [" + (av.args||[]).map(function(a){return dumpAv(a, d+1)}).join(",") + "])";
  if (av.kind === "builtin-ctor") return "builtin-ctor:" + av.id;
  if (av.kind === "args-elt") return "args-elt(idx=" + av.idx + ",fn#" + (av.fn && av.fn.start) + ")";
  if (av.kind === "top") return "TOP";
  return av.kind;
}

var code =
  'function each(arr, callback){\n' +
  '  for (var i = 0; i < arr.length; i++) { callback(i, arr[i]); }\n' +
  '}\n' +
  'each([function(){ return "/api/each0"; }], function(idx, factory){ var u = factory(); fetch(u); });\n';

var r = globalThis.analyzeJSBundle(code, "test://each", true, null);
console.log("fetchCallSites:", JSON.stringify(r.fetchCallSites));
console.log("resolverErrors:", JSON.stringify(r.resolverErrors));

var memo = globalThis._pathValMemo();
var slice = globalThis._sliceFns();
var callSitesByFn = globalThis._callSitesByFn();

var eachFn = null, cbFn = null, factoryFn = null;
globalThis.BabelBundle.traverse(r._ast, {
  FunctionDeclaration: function(p) {
    if (p.node.id.name === "each") eachFn = p.node;
  },
  FunctionExpression: function(p) {
    if (p.node.params.length === 2) cbFn = p.node;
    if (p.node.params.length === 0) factoryFn = p.node;
  }
});

console.log("\neach inSlice:", slice.has(eachFn), "callSites:", (callSitesByFn.get(eachFn)||[]).length);
console.log("cb inSlice:", slice.has(cbFn), "callSites:", (callSitesByFn.get(cbFn)||[]).length);
console.log("factory inSlice:", slice.has(factoryFn), "callSites:", (callSitesByFn.get(factoryFn)||[]).length);

globalThis.BabelBundle.traverse(r._ast, {
  CallExpression: function(p) {
    var c = p.node.callee;
    if (c && c.type === "Identifier" && c.name === "fetch") {
      console.log("\nfetch call:");
      console.log("  arg0 (u) AV:", dumpAv(memo.get(p.node.arguments[0])));
    }
    if (c && c.type === "Identifier" && c.name === "callback") {
      console.log("\ncallback call in each body:");
      console.log("  arg0 (i) AV:", dumpAv(memo.get(p.node.arguments[0])));
      console.log("  arg1 (arr[i]) AV:", dumpAv(memo.get(p.node.arguments[1])));
    }
    if (c && c.type === "Identifier" && c.name === "factory") {
      console.log("\nfactory() call:");
      console.log("  callee AV:", dumpAv(memo.get(c)));
      console.log("  result AV:", dumpAv(memo.get(p.node)));
    }
  }
});
