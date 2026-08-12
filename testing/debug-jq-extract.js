// Programmatically extract jQuery 3.7.1's real ajax driver structure
// (parse with Babel, not grep): the transport factory (contains xhr.open),
// its registration, the dispatcher that invokes it, and the ajax fn that
// builds options + calls transport.send. Print minimal source snippets.
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var B = globalThis.BabelBundle;
var src = fs.readFileSync(path.join(rd, "testing/harness-dumps/jquery-3.7.1.min.js"), "utf8");
var ast = B.parse(src, { sourceType: "script", errorRecovery: true });
function snip(node) {
  if (!node || node.start == null) return "<no-loc>";
  return src.slice(node.start, Math.min(node.end, node.start + 320));
}
var found = [];
B.traverse(ast, {
  CallExpression: function(p) {
    var c = p.node.callee;
    // xhr.open(...) sink site
    if (c && c.type === "MemberExpression" && c.property && c.property.name === "open" &&
        p.node.arguments.length >= 2) {
      var fnP = p.getFunctionParent();           // send: function(){…}
      var fnP2 = fnP && fnP.getFunctionParent();  // factory
      var fnP3 = fnP2 && fnP2.getFunctionParent();
      found.push({
        openCall: snip(p.node),
        sendFn: fnP && snip(fnP.node).slice(0, 160),
        factoryFn: fnP2 && snip(fnP2.node).slice(0, 220),
        factoryParent: fnP2 && fnP2.parentPath && fnP2.parent && fnP2.parent.type,
        factoryRegCall: fnP2 && fnP2.parentPath && fnP2.parent &&
          fnP2.parent.type === "CallExpression" ? snip(fnP2.parent.callee).slice(0, 80) : "?",
        encFn3: fnP3 && snip(fnP3.node).slice(0, 120)
      });
    }
  }
});
found.forEach(function(f, i) {
  console.log("=== xhr.open site #" + i + " ===");
  console.log("openCall:   " + f.openCall.replace(/\s+/g, " "));
  console.log("sendFn:     " + f.sendFn.replace(/\s+/g, " "));
  console.log("factoryFn:  " + f.factoryFn.replace(/\s+/g, " "));
  console.log("factoryReg: parent=" + f.factoryParent + " regCallee=" + f.factoryRegCall);
});
// Find the dispatcher: a fn whose body invokes a list element with the
// options, returning a transport, AND the ajax fn calling transport.send.
var disp = [];
B.traverse(ast, {
  CallExpression: function(p) {
    var c = p.node.callee;
    if (c && c.type === "MemberExpression" && c.property && c.property.name === "send" &&
        p.node.arguments.length >= 1 && c.object && c.object.type === "Identifier") {
      var fnP = p.getFunctionParent();
      disp.push({ sendCall: snip(p.node).slice(0, 100),
                  encFn: fnP && snip(fnP.node).slice(0, 260) });
    }
  }
});
console.log("\n=== transport.send() invocations (ajax driver) ===");
disp.slice(0, 4).forEach(function(d, i) {
  console.log("[" + i + "] " + d.sendCall.replace(/\s+/g, " "));
  console.log("    encFn: " + (d.encFn || "").replace(/\s+/g, " "));
});
