// M2 variadic-merger effects/detection probe: does the engine record
// `t[k]=s[k]` (t=arguments[0] alias, s=arguments[i] counter-loop) as the
// canonical loop-key/member effect, and does _specDetectPropagation-
// FromEffects resolve targetSlot? Pin the exact ECMA modeling gap for
// the variadic $.extend/jqExtend shape (general, not jQuery bytes).
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
  "function ext(){ var t = arguments[0]; for (var i = 1; i < arguments.length; i++){ var s = arguments[i]; for (var k in s) t[k] = s[k]; } return t; }",
  "var o = ext({}, { a: 1 }, { path: '/api/m2' });",
  "fetch(o.path);"
].join("\n");
var r = globalThis.analyzeJSBundle(code, "t://m2eff", true, null);

globalThis.BabelBundle.traverse(r._ast, {
  Program: function (p) {
    globalThis._specBuildSlice(p);
    var extFn = null;
    p.traverse({ FunctionDeclaration: function (q) { if (q.node.id && q.node.id.name === "ext") extFn = q.node; } });
    console.log("extFn:", !!extFn);
    if (extFn) {
      var eff = globalThis._specEffectsMemo().get(extFn);
      console.log("ext effects:", eff ? eff.length : "<none>");
      if (eff) eff.forEach(function (e, i) {
        console.log("  eff[" + i + "] target=" + (e.target && e.target.kind) + (e.target && typeof e.target.idx === "number" ? "(idx" + e.target.idx + ")" : "") +
          " key=" + (e.key && e.key.kind) + (e.key && e.key.kind === "loop-key" ? "(src=" + (e.key.src && e.key.src.kind) + (e.key.src && typeof e.key.src.idx === "number" ? "idx" + e.key.src.idx : "") + ")" : "") +
          " value=" + (e.value && e.value.kind) + (e.value && e.value.kind === "member" ? "(obj=" + (e.value.obj && e.value.obj.kind) + ",key=" + (e.value.key && e.value.key.kind) + ")" : ""));
      });
      var det = globalThis._specDetectPropagationFromEffects(eff);
      console.log("_specDetectPropagationFromEffects ->", det ? JSON.stringify({ tgt: det.target && det.target.kind, slot: det.targetSlot }) : "null");
      var rv = globalThis._specReturnValueMemo().get(extFn);
      if (rv) rv.forEach(function (av, ctx) {
        console.log("ext return @" + (ctx === globalThis._SPEC_STANDALONE_CTX ? "STANDALONE" : "callsite") +
          " -> " + (av && av.kind) + (av && typeof av.idx === "number" ? "(idx" + av.idx + ")" : "") +
          (av && av.kind === "obj-lit" ? " props=" + JSON.stringify(Object.keys(av.props || {})) : ""));
      }); else console.log("ext return memo: <none>");
    }
    p.stop();
  }
});
