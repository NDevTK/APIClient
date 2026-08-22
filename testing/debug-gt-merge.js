// Pin the real-jQuery `Gt` (ajaxExtend) resolver gap. F9 resolved the
// user's `ajax({url:"/api/f9"})` to "/default" (the ajaxSettings.url
// default) — the nested `Gt(Gt({},settings), s)` with conditional
// write-target `(fo[n]?e:r||(r={}))[n]=t[n]` (§13.14 ConditionalExpr as
// AssignmentTarget object + §14.7.5.9 for-in + lazy `r||(r={})` +
// `r&&extend(!0,e,r)` deep merge) isn't applying the outer Gt's
// user-`s` url overwrite. Resolve the merged object's .url/.type
// directly to see which sub-step degrades.
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;" +
  "globalThis._specPathValMemo=function(){return _specPathValMemo;};").call(globalThis);

function dump(av, d) {
  d = d || 0; if (!av || d > 6) return av ? av.kind : "<none>";
  if (av.kind === "or" || av.kind === "logical") return av.kind + "(" + dump(av.left, d + 1) + "|" + dump(av.right, d + 1) + ")";
  if (av.kind === "obj-lit") return "{" + Object.keys(av.props || {}).map(function (k) { return k + ":" + dump(av.props[k], d + 1); }).join(",") + "}";
  if (av.kind === "array-lit") return "[" + (av.elements || []).length + "]";
  if (av.kind === "call") return "call(" + (av.callee ? av.callee.kind : "?") + ")";
  if (av.kind === "member") return "member(" + dump(av.obj, d + 1) + ")";
  if (av.kind === "str" || av.kind === "const") return JSON.stringify(av.value);
  if (av.kind === "coerce") return "coerce(" + dump(av.arg, d + 1) + ")";
  return av.kind;
}

// Faithful real Gt, isolated. `opts` should resolve url:"/api/f9".
var code = [
  'var jQuery={};',
  'jQuery.ajaxSettings={url:"/default",type:"GET",flatOptions:{url:true},xhr:function(){return new XMLHttpRequest();}};',
  'function jqExtend(){var i=0,t=arguments[0];if(typeof t==="boolean"){t=arguments[1];i=2;}else{i=1;}for(;i<arguments.length;i++){var src=arguments[i];for(var k in src)t[k]=src[k];}return t;}',
  'function Gt(e,t){var n,r,fo=jQuery.ajaxSettings.flatOptions||{};for(n in t)void 0!==t[n]&&((fo[n]?e:r||(r={}))[n]=t[n]);return r&&jqExtend(true,e,r),e;}',
  'jQuery.ajaxSetup=function(e,t){return t?Gt(Gt(e,jQuery.ajaxSettings),t):Gt(jQuery.ajaxSettings,e);};',
  'var inner=Gt({},jQuery.ajaxSettings);',                       // expect {url:/default,type:GET,...}
  'var opts=jQuery.ajaxSetup({},{type:"GET",url:"/api/f9"});',   // expect url:/api/f9
  // bisect A: Gt with e = a plain obj-lit var (NOT call-result), t=user
  'var eA={url:"/default",type:"GET"};var bA=Gt(eA,{type:"GET",url:"/api/A"});',  // expect url:/api/A
  // bisect B: Gt with e = a CALL-RESULT (Gt(...)), t=user — the F9 shape
  'var bB=Gt(Gt({},jQuery.ajaxSettings),{type:"GET",url:"/api/B"});',             // expect url:/api/B
  // bisect C: same write but NON-conditional target (always e)
  'function GtC(e,t){var n;for(n in t)void 0!==t[n]&&(e[n]=t[n]);return e;}',
  'var bC=GtC(GtC({},jQuery.ajaxSettings),{type:"GET",url:"/api/C"});',           // expect url:/api/C
  // bisect D: fn-call indirection with nested Gt but NO ternary
  'var bD=(function(e,t){return Gt(Gt(e,jQuery.ajaxSettings),t);})({},{type:"GET",url:"/api/D"});', // expect url:/api/D
  // bisect E: ternary-return only, trivial branches (no nested Gt)
  'function asE(e,t){return t?Gt(e,t):Gt(jQuery.ajaxSettings,e);}',
  'var bE=asE({url:"/seed"},{type:"GET",url:"/api/E"});',                         // expect url:/api/E
  'fetch(opts.url);fetch(bA.url);fetch(bB.url);fetch(bC.url);fetch(bD.url);fetch(bE.url);'
].join("\n");

var r = globalThis.analyzeJSBundle(code, "https://ex.com/app", "https://ex.com", null);
console.log("sites=" + JSON.stringify((r.fetchCallSites || []).map(function (s) { return s.url; })));
var memo = globalThis._specPathValMemo();
globalThis.BabelBundle.traverse(r._ast, {
  VariableDeclarator: function (p) {
    var nm = p.node.id && p.node.id.name;
    if ((nm === "inner" || nm === "opts" || nm === "bA" || nm === "bB" || nm === "bC" || nm === "bD" || nm === "bE") && p.node.init) {
      console.log("  " + nm + " = " + dump(memo.get(p.node.init), 0));
    }
  }
});
