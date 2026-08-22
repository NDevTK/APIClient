// Extract REAL jQuery 3.7.1's ajax driver: how transport `c` is obtained
// (inspectPrefiltersOrTransports), how the factory is invoked + what its
// `i`/options param is bound to, and the full inspect fn body — to pin
// why real `i.xhr` won't resolve where the faithful reproducers do.
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var B = globalThis.BabelBundle;
var src = fs.readFileSync(path.join(rd, "testing/harness-dumps/jquery-3.7.1.min.js"), "utf8");
var ast = B.parse(src, { sourceType: "script", errorRecovery: true });
function snip(n, L) { return n && n.start != null ? src.slice(n.start, Math.min(n.end, n.start + (L || 300))).replace(/\s+/g, " ") : "<?>"; }

B.traverse(ast, {
  CallExpression: function(p) {
    var c = p.node.callee;
    if (c && c.type === "MemberExpression" && c.property && c.property.name === "send" &&
        c.object && c.object.type === "Identifier" && p.node.arguments.length === 2) {
      var tName = c.object.name;
      var bnd = p.scope.getBinding(tName);
      console.log("transport var = " + tName);
      if (bnd && bnd.path) {
        console.log("  decl: " + snip(bnd.path.node, 240));
        (bnd.constantViolations || []).forEach(function(v, i) {
          console.log("  reassign[" + i + "]: " + snip(v.node, 240));
        });
        // Trace the RHS that produced the transport: find the call
        // (inspectPrefiltersOrTransports) and resolve its callee fn.
        var rhs = null;
        if (bnd.path.node && bnd.path.node.type === "VariableDeclarator") rhs = bnd.path.node.init;
        (bnd.constantViolations || []).forEach(function(v) {
          if (v.node && v.node.type === "AssignmentExpression" && v.node.left &&
              v.node.left.name === tName) rhs = v.node.right;
        });
        console.log("  transport RHS: " + snip(rhs, 200));
        if (rhs && rhs.type === "CallExpression") {
          var ce = rhs.callee;
          console.log("  inspect callee: " + snip(ce, 60) +
            " args=" + (rhs.arguments || []).map(function(a){ return a.name || a.type; }).join(","));
          // Resolve the inspect fn body.
          if (ce.type === "Identifier") {
            var ib = p.scope.getBinding(ce.name);
            if (ib && ib.path) console.log("  inspect FN: " + snip(
              ib.path.node.init || ib.path.node, 520));
          }
        }
      }
      p.stop();
    }
  }
});
