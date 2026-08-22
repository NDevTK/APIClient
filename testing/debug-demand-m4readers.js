// M4 L5-followup: is the Program node a recorded reader of `build` in
// _specReadersOf? Localizes whether the read-edge exists (then break is
// fixpoint bookkeeping sigByFn/nodeToPath) or is missing (then
// _specRecordCalleeRead isn't capturing the top-level→build read).
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;" +
  "globalThis._specReadersOf=function(){return _specReadersOf;};" +
  "globalThis._specReadEdges=function(){return _specReadEdges;};").call(globalThis);

var code = [
  "function ext(a, b){ for (var k in b) a[k] = b[k]; return a; }",
  "function build(s){ return ext({}, s); }",
  "var o = build({ path: '/api/m4' });",
  "fetch(o.path);"
].join("\n");
var r = globalThis.analyzeJSBundle(code, "t://m4r", true, null);

var progNode = r._ast.program || r._ast;
var buildFn = null, extFn = null;
globalThis.BabelBundle.traverse(r._ast, {
  Program: function (p) { progNode = p.node; },
  FunctionDeclaration: function (q) {
    if (q.node.id && q.node.id.name === "build") buildFn = q.node;
    if (q.node.id && q.node.id.name === "ext") extFn = q.node;
  }
});
var ro = globalThis._specReadersOf();
function readers(fn, nm) {
  var s = fn && ro.get(fn);
  if (!s) { console.log(nm + " readers: <none>"); return; }
  var kinds = [];
  s.forEach(function (c) {
    kinds.push(c === progNode ? "PROGRAM" : (c && c.type) + (c && c.id ? ":" + c.id.name : ""));
  });
  console.log(nm + " readers(" + s.size + "): " + kinds.join(", ") + "  hasProgram=" + s.has(progNode));
}
console.log("progNode.type=" + (progNode && progNode.type) + " buildFn=" + !!buildFn + " extFn=" + !!extFn);
readers(buildFn, "build");
readers(extFn, "ext");
