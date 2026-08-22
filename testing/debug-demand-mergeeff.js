// Pin WHY the proper effect-based merger fix doesn't engage for M1:
// does _specEffectsMemo.get(ext) carry the loop-key merge effect in the
// canonical form _specDetectPropagationFromEffects expects? Is ext
// refined (concrete args) so its effects/return are computed? This is
// ECMA-completion diagnosis (effect-record ⇄ fold-path consistency),
// not shape-matching.
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;" +
  "globalThis._specBuildSlice=_specBuildSlice;" +
  "globalThis._specEffectsMemo=function(){return _specEffectsMemo;};" +
  "globalThis._specDetectPropagationFromEffects=_specDetectPropagationFromEffects;" +
  "globalThis._specReturnValueMemo=function(){return _specReturnValueMemo;};" +
  "globalThis._SPEC_STANDALONE_CTX=_SPEC_STANDALONE_CTX;").call(globalThis);

var code = [
  "function ext(a, b){ for (var k in b) a[k] = b[k]; return a; }",
  "var o = ext({}, { path: '/api/m1' });",
  "fetch(o.path);"
].join("\n");
var r = globalThis.analyzeJSBundle(code, "t://m1eff", true, null);

globalThis.BabelBundle.traverse(r._ast, {
  Program: function (p) {
    globalThis._specBuildSlice(p);
    var extFn = null;
    p.traverse({
      FunctionDeclaration: function (q) { if (q.node.id && q.node.id.name === "ext") extFn = q.node; }
    });
    console.log("extFn found:", !!extFn);
    if (extFn) {
      var eff = globalThis._specEffectsMemo().get(extFn);
      console.log("ext effects:", eff ? eff.length : "<none>");
      if (eff) eff.forEach(function (e, i) {
        console.log("  eff[" + i + "] target=" + (e.target && e.target.kind) +
          " key=" + (e.key && e.key.kind) + (e.key && e.key.kind === "loop-key" ? "(src=" + (e.key.src && e.key.src.kind) + ")" : "") +
          " value=" + (e.value && e.value.kind) + (e.value && e.value.kind === "member" ? "(obj=" + (e.value.obj && e.value.obj.kind) + ",key=" + (e.value.key && e.value.key.kind) + ")" : ""));
      });
      var det = globalThis._specDetectPropagationFromEffects(eff);
      console.log("_specDetectPropagationFromEffects ->", det ? JSON.stringify({ tgt: det.target && det.target.kind, slot: det.targetSlot }) : "null");
      var rv = globalThis._specReturnValueMemo().get(extFn);
      if (rv) rv.forEach(function (av, ctx) {
        console.log("ext return @ctx=" + (ctx === globalThis._SPEC_STANDALONE_CTX ? "STANDALONE" : "callsite") +
          " -> " + (av && av.kind) + (av && av.kind === "param" ? "(idx" + av.idx + ")" : "") +
          (av && av.kind === "obj-lit" ? " props=" + JSON.stringify(Object.keys(av.props || {})) : ""));
      });
      else console.log("ext return memo: <none>");
    }
    p.stop();
  }
});
