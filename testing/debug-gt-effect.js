// Step-4: does real `Gt` (ajaxExtend) get a recorded §14.7.5.9 merge
// EFFECT, and is _specJumpFnNonTrivial(Gt) true, when Gt is ACTUALLY
// CALLED through the F9 chain? (An uncalled top-level fn gets no
// effects — the fixpoint only analyses reached fns; the earlier
// <none> probe was void.) Faithful string-concat source (no heredoc).
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;" +
  "globalThis._eff=function(){return _specEffectsMemo;};" +
  "globalThis._jnt=function(f){try{return _specJumpFnNonTrivial(f);}catch(e){return 'ERR:'+e.message;}};").call(globalThis);

function avk(a){ return a ? (a.kind + (a.kind === "const" || a.kind === "str" ? ":" + JSON.stringify(a.value) : (a.kind === "or" || a.kind === "logical" ? "(" + avk(a.left) + "|" + avk(a.right) + ")" : ""))) : "<none>"; }

var code = [
  'var jQuery={};',
  'jQuery.ajaxSettings={url:"/default",type:"GET",flatOptions:{url:true},xhr:function(){return new XMLHttpRequest();}};',
  'function jqExtend(){var i=0,t=arguments[0];if(typeof t==="boolean"){t=arguments[1];i=2;}else{i=1;}for(;i<arguments.length;i++){var s=arguments[i];for(var k in s)t[k]=s[k];}return t;}',
  'function Gt(e,t){var n,r,fo=jQuery.ajaxSettings.flatOptions||{};for(n in t)void 0!==t[n]&&((fo[n]?e:r||(r={}))[n]=t[n]);return r&&jqExtend(true,e,r),e;}',
  'jQuery.ajaxSetup=function(e,t){return t?Gt(Gt(e,jQuery.ajaxSettings),t):Gt(jQuery.ajaxSettings,e);};',
  'function ajax(s){var o=jQuery.ajaxSetup({},s);fetch(o.url);}',   // CALLS Gt via ajaxSetup
  'ajax({type:"GET",url:"/api/f9"});'
].join("\n");

var r = globalThis.analyzeJSBundle(code, "https://ex.com/app", "https://ex.com", null);
console.log("sites=" + JSON.stringify((r.fetchCallSites || []).map(function (s) { return s.url; })));
var eff = globalThis._eff();
globalThis.BabelBundle.traverse(r._ast, {
  FunctionDeclaration: function (p) {
    var nm = p.node.id && p.node.id.name;
    if (nm === "Gt" || nm === "jqExtend") {
      var e = eff.get(p.node);
      var summary = e ? ("[" + e.length + "] " + e.map(function (x) {
        return "{tgt=" + avk(x.target) + " key=" + (x.key ? x.key.kind : "?") + " val=" + avk(x.value) + "}";
      }).join(" ")) : "<none>";
      console.log(nm + ": jumpFnNonTrivial=" + globalThis._jnt(p.node) + " effects=" + summary);
    }
  }
});
