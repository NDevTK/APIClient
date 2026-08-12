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
  var a = "*".match(/[a-z*]/g);
  var b = a || [""];
  var c = b[0];
  fetch("/api/m/" + c);
`, "test://m", true, null);
console.log("fetchCallSites:", (result.fetchCallSites || []).length);
(result.fetchCallSites || []).forEach(function(s){console.log("  ", s.method, s.url);});
var memo = globalThis._pathValMemo();
function dumpAv(av) {
  if (!av) return "null";
  if (av.kind === "const") return JSON.stringify(av.value);
  if (av.kind === "array-lit") return "array-lit[" + (av.elements || []).map(dumpAv).join(",") + "]";
  if (av.kind === "or") return "or(" + dumpAv(av.left) + "," + dumpAv(av.right) + ")";
  return av.kind;
}
globalThis.BabelBundle.traverse(result._ast, {
  VariableDeclarator: function(p) {
    if (p.node.id && p.node.id.name) {
      var av = memo.get(p.node.init);
      console.log("var " + p.node.id.name + " AV:", dumpAv(av));
    }
  }
});
