var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode +
  "globalThis.analyzeJSBundle = analyzeJSBundle;\n" +
  "globalThis._pathValMemo = function(){ return _specPathValMemo; };\n").call(globalThis);
var result = globalThis.analyzeJSBundle(`
  function ajax(opts) {
    var i = opts;
    var x = i.dataType;
    var y = x || "*";
    var z = y.toLowerCase();
    var w = z.match(/[a-z*]+/g);
    var v = w || [""];
    var u = v[0];
    fetch("/api/dt/" + u);
  }
  ajax({});
`, "test://sd8", true, null);
console.log("fetchCallSites:", (result.fetchCallSites || []).length);
(result.fetchCallSites || []).forEach(function(s){console.log("  ", s.method, s.url);});
var memo = globalThis._pathValMemo();
function dumpAv(av) {
  if (!av) return "null";
  if (av.kind === "const") return JSON.stringify(av.value);
  if (av.kind === "array-lit") return "array-lit[" + (av.elements || []).slice(0,3).map(dumpAv).join(",") + "]";
  if (av.kind === "or") return "or(" + dumpAv(av.left) + "," + dumpAv(av.right) + ")";
  if (av.kind === "logical") return "logical(" + av.op + " " + dumpAv(av.left) + "," + dumpAv(av.right) + ")";
  if (av.kind === "member") return "member(" + dumpAv(av.obj) + "," + dumpAv(av.key) + ")";
  if (av.kind === "obj-lit") return "obj-lit{" + Object.keys(av.props||{}).join(",") + "}";
  if (av.kind === "param") return "param(" + av.idx + ")";
  return av.kind;
}
globalThis.BabelBundle.traverse(result._ast, {
  VariableDeclarator: function(p) {
    if (p.node.id && p.node.id.name) {
      var av = memo.get(p.node.init);
      console.log("var " + p.node.id.name + ":", dumpAv(av));
    }
  }
});
