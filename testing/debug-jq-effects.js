var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode +
  "globalThis.analyzeJSBundle = analyzeJSBundle;" +
  "globalThis.getCtx = function(){ return _specCtxEffectsBySite; };").call(globalThis);
var jq = fs.readFileSync(path.join(rootDir, "testing/harness-dumps/jquery-3.7.1.min.js"), "utf8");
var userCode = "\njQuery.ajax({url: '/api/widgets', method: 'POST'});\n";
var result = globalThis.analyzeJSBundle(jq + userCode, "test://jq", true, null);
var ctx = globalThis.getCtx();
var ajaxExtendCall = null;
globalThis.BabelBundle.traverse(result._ast, {
  CallExpression: function(p) {
    if (ajaxExtendCall) return;
    var c = p.node.callee;
    if (!c || c.type !== "MemberExpression" || !c.property || c.property.name !== "extend") return;
    var arg0 = p.node.arguments[0];
    if (!arg0 || arg0.type !== "ObjectExpression") return;
    var hasAjax = arg0.properties.some(function(prop) {
      return prop.type === "ObjectProperty" && prop.key &&
        ((prop.key.type === "Identifier" && prop.key.name === "ajax") ||
         (prop.key.type === "StringLiteral" && prop.key.value === "ajax"));
    });
    if (hasAjax) ajaxExtendCall = p;
  }
});
if (!ajaxExtendCall) { console.log("not found"); process.exit(0); }
var entry = ctx.get(ajaxExtendCall.node);
if (!entry) { console.log("not refined"); process.exit(0); }
console.log("argAvs:");
entry.argAvs.forEach(function(av, i) {
  console.log("  arg" + i + ":", av && av.kind, av && av.props ? "props=" + Object.keys(av.props).length : "");
  if (av && av.props) console.log("    first 5 keys:", Object.keys(av.props).slice(0, 5).join(","));
});
console.log("recvAv:", entry.recvAv && entry.recvAv.kind);
console.log("effects:");
entry.effects.forEach(function(e, i) {
  console.log("  eff" + i + ": target=" + (e.target && e.target.kind) +
    " key=" + (e.key && (e.key.kind === "const" ? "const(" + e.key.value + ")" : e.key.kind === "loop-key" ? "loop-key(src=" + (e.key.src && e.key.src.kind) + (e.key.src && e.key.src.props ? " props=" + Object.keys(e.key.src.props).length : "") + ")" : e.key.kind)) +
    " value=" + (e.value && e.value.kind));
});
