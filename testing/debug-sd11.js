var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
globalThis._DBG_SD11 = true;
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode +
  "globalThis.analyzeJSBundle = analyzeJSBundle;\n" +
  "globalThis._ctxBySite = function(){ return _specCtxEffectsBySite; };\n").call(globalThis);

var code = `
function ajax(opts) {
  var d = (opts.x || "*").toLowerCase();
  fetch("/api/lc/" + d);
}
ajax({});
`;
var result = globalThis.analyzeJSBundle(code, "test://sd11", true, null);
console.log("fetchCallSites:", (result.fetchCallSites || []).length);
(result.fetchCallSites || []).forEach(function (s, i) {
  console.log("  [" + i + "]", s.method, s.url);
});

var ctxBy = globalThis._ctxBySite();
var ctxCount = 0;
globalThis.BabelBundle.traverse(result._ast, {
  CallExpression: function(p) {
    if (ctxBy.has(p.node)) {
      ctxCount++;
      var entry = ctxBy.get(p.node);
      console.log("ctx-refined: callee.type=" + p.node.callee.type +
        " fn.id=" + (entry.fn && entry.fn.id ? entry.fn.id.name : "(anon)") +
        " effects.len=" + (entry.effects ? entry.effects.length : "?"));
    }
  }
});
console.log("ctx-refined sites:", ctxCount);
