var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode +
  "globalThis.analyzeJSBundle = analyzeJSBundle;\n" +
  "globalThis._ctxBySite = function(){ return _specCtxEffectsBySite; };\n").call(globalThis);
var jq = fs.readFileSync(path.join(rootDir, "testing/harness-dumps/jquery-3.7.1.min.js"), "utf8");
var result = globalThis.analyzeJSBundle(jq + "jQuery.ajax({url: '/api/simpler'});", "test://jq", true, null);
console.log("fetchCallSites:", (result.fetchCallSites || []).length);
var ctxBy = globalThis._ctxBySite();
var ctxCount = 0;
var userCallSiteRefined = false;
globalThis.BabelBundle.traverse(result._ast, {
  CallExpression: function(p) {
    if (ctxBy.has(p.node)) ctxCount++;
    var c = p.node.callee;
    if (c && c.type === "MemberExpression" && c.object && c.object.type === "Identifier" &&
        c.object.name === "jQuery" && c.property && c.property.name === "ajax") {
      console.log("user-code jQuery.ajax call AT loc:" + (p.node.loc ? p.node.loc.start.line : "?"));
      console.log("  in ctxBy:", ctxBy.has(p.node));
      if (ctxBy.has(p.node)) {
        var entry = ctxBy.get(p.node);
        console.log("  refined fn.id:", entry.fn && entry.fn.id ? entry.fn.id.name : "(anon)",
          "fn.start:", entry.fn && entry.fn.start);
        console.log("  effects.length:", entry.effects ? entry.effects.length : "null");
        userCallSiteRefined = true;
      }
    }
  }
});
console.log("ctx-refined sites in jQuery analysis:", ctxCount);
console.log("user-call ctx-refined:", userCallSiteRefined);
