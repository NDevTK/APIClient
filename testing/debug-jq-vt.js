// Real jQuery 3.7.1: c=Vt(_t,v,t,T) — Vt IS inspectPrefiltersOrTransports.
// Dump Vt's full body + the curried registrar that fills _t, to see how
// the factory is invoked + what its `i` param binds to.
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var B = globalThis.BabelBundle;
var src = fs.readFileSync(path.join(rd, "testing/harness-dumps/jquery-3.7.1.min.js"), "utf8");
var ast = B.parse(src, { sourceType: "script", errorRecovery: true });
function full(n) { return n && n.start != null ? src.slice(n.start, n.end) : "<?>"; }

var prog = null;
B.traverse(ast, { Program: function(p){ prog = p; p.stop(); } });
// Find `function Vt(...)` declaration / `var Vt = function`.
B.traverse(ast, {
  "FunctionDeclaration|FunctionExpression": function(p) {
    var nm = (p.node.id && p.node.id.name) ||
      (p.parentPath && p.parent.type === "VariableDeclarator" && p.parent.id && p.parent.id.name) ||
      (p.parentPath && p.parent.type === "AssignmentExpression" && p.parent.left && p.parent.left.name);
    if (nm === "Vt") {
      console.log("=== Vt (inspectPrefiltersOrTransports) — params: " +
        p.node.params.map(function(x){return x.name||x.type;}).join(",") + " ===");
      console.log(full(p.node).slice(0, 900));
      console.log("");
    }
    if (nm === "Ut") {
      console.log("=== Ut (curried registrar) — params: " +
        p.node.params.map(function(x){return x.name||x.type;}).join(",") + " ===");
      console.log(full(p.node).slice(0, 500));
      console.log("");
    }
  }
});
// Also: how is the xhr transport registered? find `ajaxTransport(` call.
B.traverse(ast, {
  CallExpression: function(p) {
    var c = p.node.callee;
    if (c && ((c.type === "MemberExpression" && c.property && c.property.name === "ajaxTransport") ||
              (c.type === "Identifier" && c.name === "ajaxTransport"))) {
      console.log("=== ajaxTransport registration call ===");
      console.log(full(p.node).slice(0, 160) + " ...");
      p.stop();
    }
  }
});
