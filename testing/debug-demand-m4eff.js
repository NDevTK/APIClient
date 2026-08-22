// M4 chain diagnostic: build(s){return ext({},s)}; build({path}).
// Pin where the deferred-Object.assign-call-AV chain breaks: is ext's
// return the deferred call-AV? is build's return memo it (param(s)
// preserved)? does build({path}) fold substitute param(s)→{path} then
// fold Object.assign?
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;" +
  "globalThis._specBuildSlice=_specBuildSlice;" +
  "globalThis._specReturnValueMemo=function(){return _specReturnValueMemo;};" +
  "globalThis._specEffectsMemo=function(){return _specEffectsMemo;};" +
  "globalThis._specMergerReturnInfo=_specMergerReturnInfo;" +
  "globalThis._SPEC_STANDALONE_CTX=_SPEC_STANDALONE_CTX;").call(globalThis);

function dump(av, d) {
  d = d || 0; if (!av || d > 4) return String(av && av.kind);
  if (av.kind === "call") return "call(" + dump(av.callee, d + 1) + ",[" +
    (av.args || []).map(function (a) { return dump(a, d + 1); }).join(",") + "])";
  if (av.kind === "builtin-method") return "bm:" + av.id;
  if (av.kind === "obj-lit") return "obj{" + Object.keys(av.props || {}).map(function (k) {
    return k + ":" + dump(av.props[k], d + 1);
  }).join(",") + "}";
  if (av.kind === "member") return "member(" + dump(av.obj, d + 1) + "." + dump(av.key, d + 1) + ")";
  if (av.kind === "param") return "param(" + av.idx + (av.fn && av.fn.id ? ",fn=" + av.fn.id.name : "") + ")";
  if (av.kind === "args-elt") return "args-elt(" + dump(av.idx, d + 1) + ")";
  if (av.kind === "const") return "const=" + JSON.stringify(av.value);
  if (av.kind === "or") return "or(" + dump(av.left, d + 1) + "|" + dump(av.right, d + 1) + ")";
  return av.kind;
}

var code = [
  "function ext(a, b){ for (var k in b) a[k] = b[k]; return a; }",
  "function build(s){ return ext({}, s); }",
  "var o = build({ path: '/api/m4' });",
  "fetch(o.path);"
].join("\n");
var r = globalThis.analyzeJSBundle(code, "t://m4eff", true, null);

globalThis.BabelBundle.traverse(r._ast, {
  Program: function (p) {
    globalThis._specBuildSlice(p);
    var extFn = null, buildFn = null;
    p.traverse({ FunctionDeclaration: function (q) {
      if (q.node.id && q.node.id.name === "ext") extFn = q.node;
      if (q.node.id && q.node.id.name === "build") buildFn = q.node;
    }});
    [["ext", extFn], ["build", buildFn]].forEach(function (pair) {
      var nm = pair[0], fn = pair[1];
      if (!fn) { console.log(nm + ": <not found>"); return; }
      console.log(nm + " mergerInfo=" + (globalThis._specMergerReturnInfo(fn) ? "merger" : "null") +
        " effects=" + ((globalThis._specEffectsMemo().get(fn) || []).length));
      var rv = globalThis._specReturnValueMemo().get(fn);
      if (rv) rv.forEach(function (av, ctx) {
        console.log("  " + nm + " return @" +
          (ctx === globalThis._SPEC_STANDALONE_CTX ? "STANDALONE" : "callsite") + " -> " + dump(av));
      }); else console.log("  " + nm + " return memo: <none>");
    });
    p.stop();
  }
});
