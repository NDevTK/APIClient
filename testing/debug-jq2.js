var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
globalThis._DBG_JQ2 = true;
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode +
  "globalThis.analyzeJSBundle = analyzeJSBundle;\n" +
  "globalThis._pathValMemo = function(){ return _specPathValMemo; };\n" +
  "globalThis._ctxBySite = function(){ return _specCtxEffectsBySite; };\n" +
  "globalThis._refByFn = function(){ return _specLatestRefinedMemoByFn; };\n" +
  "globalThis._retMemo = function(){ return _specReturnValueMemo; };\n").call(globalThis);

var code = `
function extend(t, src) { for (var k in src) t[k] = src[k]; return t; }
function go() {
  var s = {};
  extend(s, { xhr: function() { return new XMLHttpRequest(); } });
  var r = s.xhr();
  r.open("GET", "/api/jq2");
  r.send();
}
go();
`;
var result = globalThis.analyzeJSBundle(code, "test://jq2", true, null);
console.log("fetchCallSites:", (result.fetchCallSites || []).length);

var memo = globalThis._pathValMemo();
var ctxBy = globalThis._ctxBySite();
var ctxCount = 0;
globalThis.BabelBundle.traverse(result._ast, {
  CallExpression: function(p) {
    if (ctxBy.has(p.node)) {
      ctxCount++;
      var entry = ctxBy.get(p.node);
      var calleeName = p.node.callee && p.node.callee.type === "Identifier" ?
        p.node.callee.name :
        (p.node.callee && p.node.callee.type === "MemberExpression" && p.node.callee.property ? "." + p.node.callee.property.name : "?");
      console.log("ctx-refined:", calleeName, "fn.id:", (entry.fn && entry.fn.id ? entry.fn.id.name : "(anon)"),
        "effects.len:", entry.effects ? entry.effects.length : "?",
        "refinedMemo.size:", entry.refinedNodeMemo ? "(WeakMap)" : "none");
    }
    // Identify s.xhr() call to check its callee AV
    if (p.node.callee && p.node.callee.type === "MemberExpression" &&
        p.node.callee.object && p.node.callee.object.type === "Identifier" &&
        p.node.callee.object.name === "s") {
      console.log("s.X() callee.property:", p.node.callee.property && p.node.callee.property.name);
      console.log("  s AV:", JSON.stringify(memo.get(p.node.callee.object)).slice(0, 200));
      console.log("  s.x callee AV:", JSON.stringify(memo.get(p.node.callee)).slice(0, 200));
    }
    // r.open detection
    if (p.node.callee && p.node.callee.type === "MemberExpression" &&
        p.node.callee.object && p.node.callee.object.type === "Identifier" &&
        p.node.callee.object.name === "r") {
      console.log("r.open call:");
      console.log("  r AV:", JSON.stringify(memo.get(p.node.callee.object)).slice(0, 200));
      console.log("  r.open callee AV:", JSON.stringify(memo.get(p.node.callee)).slice(0, 200));
    }
  }
});
console.log("Total ctx-refined sites:", ctxCount);

// Find the factory FE and check its return memo
var retMemo = globalThis._retMemo();
globalThis.BabelBundle.traverse(result._ast, {
  FunctionExpression: function(p) {
    var body = p.node.body && p.node.body.body;
    if (!body) return;
    for (var i = 0; i < body.length; i++) {
      var stmt = body[i];
      if (stmt.type === "ReturnStatement" && stmt.argument &&
          stmt.argument.type === "NewExpression" &&
          stmt.argument.callee && stmt.argument.callee.name === "XMLHttpRequest") {
        var retAv = retMemo.get(p.node);
        console.log("factory FE @ line " + p.node.loc.start.line + " return memo:",
          retAv ? JSON.stringify(retAv).slice(0, 200) : "(none)");
      }
    }
  }
});
