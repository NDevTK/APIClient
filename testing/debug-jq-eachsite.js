// Why does real jQuery jQuery.each(["get","post"],fn) NOT populate
// jQuery.get/.post when synthetic B (same minified each shape) does?
// Inspect the real .each(["get","post"]) call site's callee resolution
// + whether `each` is a resolvable 2-arg fn on jQuery.
var fs = require("fs"), path = require("path"), rd = path.join(__dirname, "..");
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle = analyzeJSBundle;\n" +
  "globalThis._pvm = function(n){ return _specPathValMemo.get(n); };\n" +
  "globalThis._go = function(){ return _specGlobalPropOverrides; };\n").call(globalThis);

var jq = fs.readFileSync(path.join(rd, "testing/harness-dumps/jquery-3.7.1.min.js"), "utf8");
var result = globalThis.analyzeJSBundle(jq + "\njQuery.get('/api/profile');\n", "test://jq", true, null);
console.log("fetchCallSites:", (result.fetchCallSites || []).length);

function avDesc(av) {
  if (!av) return "null";
  if (av.kind === "function-ref") return "function-ref#" + (av.funcNode && av.funcNode.start) +
    " params=" + (av.funcNode && av.funcNode.params ? av.funcNode.params.length : "?");
  if (av.kind === "or") return "or(" + avDesc(av.left) + "," + avDesc(av.right) + ")";
  if (av.kind === "const") return "const:" + JSON.stringify(av.value).slice(0, 30);
  if (av.kind === "obj-lit") return "obj-lit{" + Object.keys(av.props || {}).length + "}";
  return av.kind;
}

// jQuery._extraProps: is `each` there? what is it?
var go = globalThis._go();
var jqAv = go && go.jQuery;
var ep = jqAv && jqAv._extraProps;
if (ep) {
  console.log("jQuery._extraProps has 'each'?", Object.prototype.hasOwnProperty.call(ep, "each"),
    ep.each ? "→ " + avDesc(ep.each) : "");
  console.log("  get?", !!ep.get, " post?", !!ep.post, " ajax?", !!ep.ajax, " extend?", !!ep.extend);
}

// Find every CallExpression `X.each([...string lits...], function(){})`
// and report its callee AV + arg shapes.
var found = 0;
globalThis.BabelBundle.traverse(result._ast, {
  CallExpression: function (p) {
    var n = p.node;
    if (!n.callee || n.callee.type !== "MemberExpression") return;
    if (n.callee.computed || !n.callee.property || n.callee.property.name !== "each") return;
    if (!n.arguments[0] || n.arguments[0].type !== "ArrayExpression") return;
    var elems = n.arguments[0].elements || [];
    var strs = elems.filter(function (e) { return e && e.type === "StringLiteral"; })
      .map(function (e) { return e.value; });
    if (strs.indexOf("get") < 0 && strs.indexOf("post") < 0) return;
    found++;
    console.log("\n.each([" + strs.join(",") + "], fn) @L" +
      (n.loc && n.loc.start.line) + ":C" + (n.loc && n.loc.start.column));
    console.log("  callee AV:", avDesc(globalThis._pvm(n.callee)));
    console.log("  callee.object AV:", avDesc(globalThis._pvm(n.callee.object)));
    console.log("  arg0 (array) AV:", avDesc(globalThis._pvm(n.arguments[0])));
    var cb = n.arguments[1];
    console.log("  arg1 (cb):", cb && cb.type, "params=" + (cb && cb.params ? cb.params.map(function (x) { return x.name; }).join(",") : "?"));
  }
});
console.log("\n.each get/post call sites found:", found);
