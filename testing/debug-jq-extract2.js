// Extract jQuery 3.7.1's real inspectPrefiltersOrTransports + how the
// transport `c` is obtained + the jQuery.get→ajax entry. The transport
// factory's `i` param ← whatever `inspect(...)` passes; trace that.
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var B = globalThis.BabelBundle;
var src = fs.readFileSync(path.join(rd, "testing/harness-dumps/jquery-3.7.1.min.js"), "utf8");
var ast = B.parse(src, { sourceType: "script", errorRecovery: true });
function snip(n, len) { return n && n.start != null ? src.slice(n.start, Math.min(n.end, n.start + (len || 300))).replace(/\s+/g, " ") : "<?>"; }

// Find `c.send(a,l)` — c is the transport; locate its assignment + the
// inspect fn that produced it.
B.traverse(ast, {
  CallExpression: function(p) {
    var c = p.node.callee;
    if (c && c.type === "MemberExpression" && c.property && c.property.name === "send" &&
        c.object && c.object.type === "Identifier" && p.node.arguments.length === 2) {
      var tName = c.object.name;
      var fnP = p.getFunctionParent();
      // Find where `tName` is assigned in this fn body.
      var binding = p.scope.getBinding(tName);
      var assignSrc = "<none>";
      if (binding) {
        if (binding.path && binding.path.node) assignSrc = snip(binding.path.node, 220);
        if (binding.constantViolations && binding.constantViolations.length)
          assignSrc += " ||VIOL: " + binding.constantViolations.map(function(v){ return snip(v.node, 160); }).join(" ; ");
      }
      console.log("=== transport.send site ===");
      console.log("send: " + snip(p.node, 80));
      console.log(tName + " binding: " + assignSrc);
      // The ajax fn around it
      console.log("ajaxFn head: " + snip(fnP && fnP.node, 200));
      p.stop();
    }
  }
});

// Find the inspect fn: a fn taking (structure, options/Options...) that
// loops a list and invokes elements returning a transport. Heuristic-free:
// find fns whose body has `X[i](Y,...)` where X derives from a param and
// the result is returned. Just print candidates referencing ".dataTypes".
var seen = 0;
B.traverse(ast, {
  Function: function(p) {
    if (seen >= 3) return;
    var s = snip(p.node, 360);
    if (/dataTypes|crossDomain|hasContent/.test(s) && /for\(|while\(/.test(s) && s.indexOf("send") < 0) {
      console.log("=== inspect-candidate ===");
      console.log(s);
      seen++;
    }
  }
});
