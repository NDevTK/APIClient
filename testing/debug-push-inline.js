var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode +
  "globalThis.analyzeJSBundle = analyzeJSBundle;\n" +
  "globalThis._pathValMemo = function(){ return _specPathValMemo; };\n").call(globalThis);

// Inline push (no register helper)
var code =
  'var registry = [];\n' +
  'registry.push(function(opts){ fetch(opts.url); });\n' +
  'registry[0]({url: "/api/inline-push"});\n';

var r = globalThis.analyzeJSBundle(code, "test://push", true, null);
console.log("fetchCallSites:", JSON.stringify(r.fetchCallSites));
console.log("resolverErrors:", JSON.stringify((r.resolverErrors||[]).map(function(e){return e.message})));

var memo = globalThis._pathValMemo();
globalThis.BabelBundle.traverse(r._ast, {
  Identifier: function(p) {
    if (p.node.name === "registry" && p.parent.type === "MemberExpression") {
      var av = memo.get(p.node);
      console.log("registry @ L"+p.node.loc.start.line+":C"+p.node.loc.start.column+" parent."+p.parent.property.name+":",
        av ? av.kind + (av.kind === "array-lit" ? "["+(av.elements||[]).length+"]" : "") : "null");
    }
  }
});
