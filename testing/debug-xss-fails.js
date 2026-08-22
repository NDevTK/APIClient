// Debug the 2 specific XSS patterns that failed with upfront fixpoint.
var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode + "\nglobalThis.analyzeJSBundle = analyzeJSBundle;").call(globalThis);

function run(label, code) {
  console.log("\n=== " + label + " ===");
  var result = globalThis.analyzeJSBundle(code, "test://" + label, true, null);
  var sinks = result.securitySinks || [];
  console.log("sinks: " + sinks.length);
  sinks.forEach(function (s) {
    console.log("  " + s.type + " " + s.sink + " <- " + s.source + " (sev=" + s.severity + ")");
  });
  // Inspect the AssignmentExpression in cb body for test 1.
  // Re-parse to find v's AV via spec-eval directly.
  var ast = globalThis.BabelBundle.parse(code, { sourceType: "unambiguous", errorRecovery: true });
  globalThis.BabelBundle.traverse(ast, {
    AssignmentExpression: function(p) {
      var rhs = p.node.right;
      if (rhs && rhs.type === "Identifier" && rhs.name === "v") {
        // After analyzeJSBundle, _specPathValMemo would have been reset.
        // We'd need to inspect memo before analyzeJSBundle's reset. Skip
        // for now — this just confirms which AssignmentExpression matches.
        console.log("  found AssignmentExpression with v RHS");
      }
    }
  });
}

// Probe canonical AV identity for `loc` node in test 2
function probe() {
  console.log("\n=== Probe test 2 ===");
  var code = `
    var loc = window.location;
    loc.href = document.cookie;
  `;
  var ast = globalThis.BabelBundle.parse(code, { sourceType: "unambiguous", errorRecovery: true });
  // Find the second `loc` Identifier (in loc.href)
  var locUseNode = null;
  globalThis.BabelBundle.traverse(ast, {
    AssignmentExpression: function(p) {
      if (p.node.left && p.node.left.object && p.node.left.object.name === "loc") {
        locUseNode = p.node.left.object;
      }
    }
  });
  if (!locUseNode) { console.log("locUseNode not found"); return; }
  globalThis.analyzeJSBundle(code, "test://probe", true, null);
  console.log("after analyze, query _specPathValMemo for locUseNode...");
  // Test analyzeJSBundle ran. Memos are reset per analyze call, so the
  // memo for this AST is cleared by the next call. Skip direct memo query;
  // re-analyze with bare minimum to inspect.
}
probe();

run("array.push tainted, then forEach -> innerHTML", `
  var items = [];
  items.push(location.hash.slice(1));
  items.forEach(function(v) { document.body.innerHTML = v; });
`);

run("var loc = window.location; loc.href = taint", `
  var loc = window.location;
  loc.href = document.cookie;
`);
