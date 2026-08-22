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
  if (av.kind === "const") return JSON.stringify(av.value).slice(0, 30);
  if (av.kind === "or") return "or(" + dumpAv(av.left, d+1) + "," + dumpAv(av.right, d+1) + ")";
  if (av.kind === "obj-lit") return "obj-lit{" + Object.keys(av.props || {}).join(",") + "}";
  if (av.kind === "function-ref") return "function-ref";
  if (av.kind === "top") return "TOP";
  return av.kind;
}

var r = globalThis.analyzeJSBundle(
  "var make = function(matches){ return matches ? { send: function(u){ fetch(u); } } : undefined; };\n" +
  "var transports = [function(){ return make(false); }, function(){ return make(true); }];\n" +
  "var selected;\n" +
  "transports.forEach(function(fn){ var r = fn(); if (r) selected = r; });\n" +
  "if (selected) selected.send('/api/F3');\n",
  "test://F3", true, null);
console.log("fetchCallSites:", JSON.stringify((r.fetchCallSites||[]).map(function(s){return s.url})));

var memo = globalThis._pathValMemo();
globalThis.BabelBundle.traverse(r._ast, {
  Identifier: function(p) {
    if (p.node.name === "selected") {
      var ctx = "";
      if (p.parent && p.parent.type === "MemberExpression" && p.parent.object === p.node) ctx = ".M";
      if (p.parent && p.parent.type === "IfStatement" && p.parent.test === p.node) ctx = ".If";
      if (p.parent && p.parent.type === "AssignmentExpression" && p.parent.left === p.node) ctx = ".=L";
      if (p.parent && p.parent.type === "AssignmentExpression" && p.parent.right === p.node) ctx = ".=R";
      if (ctx) console.log("  selected@L" + p.node.loc.start.line + ctx + " AV:", dumpAv(memo.get(p.node)));
    }
    if (p.node.name === "r") {
      var ctxR = "";
      if (p.parent && p.parent.type === "IfStatement" && p.parent.test === p.node) ctxR = ".If";
      if (p.parent && p.parent.type === "AssignmentExpression" && p.parent.right === p.node) ctxR = ".=R";
      if (ctxR) console.log("  r@L" + p.node.loc.start.line + ctxR + " AV:", dumpAv(memo.get(p.node)));
    }
    if (p.node.name === "fn" && p.parent && p.parent.type === "CallExpression" && p.parent.callee === p.node) {
      console.log("  fn@.()L" + p.node.loc.start.line + " AV:", dumpAv(memo.get(p.node)));
    }
  },
  CallExpression: function(p) {
    if (p.node.callee && p.node.callee.type === "Identifier" && p.node.callee.name === "fn") {
      console.log("  fn() callResult@L" + p.node.loc.start.line + " AV:", dumpAv(memo.get(p.node)));
    }
  }
});
