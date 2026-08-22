// Real 87KB jQuery 3.7.1 backward-resolution diagnostic. The synthetic
// F1–F4b reproducers all resolve; this pins WHICH hop bottoms out in
// the FULL minified library for `jQuery.ajax({url:"/api/simpler"})`.
// The single xhr.open in jQuery is the transport-factory send():
//   ce.ajaxTransport(function(i){ if(...)return{send:function(e,t){
//     var n,r=i.xhr(); r.open(i.type,i.url,i.async,...) }}; });
// We demand-resolve, from that sink: the callee r.open (receiver type),
// the receiver `r` (= i.xhr() factory call), and args i.type / i.url —
// reporting where each collapses to top/member/call so the missing
// general native-JS rule is identified (NOT guessed).
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;" +
  "globalThis._specBuildSlice=_specBuildSlice;" +
  "globalThis._demandSinkSeeds=_demandSinkSeeds;" +
  "globalThis._demandResolve=_demandResolve;" +
  "globalThis._demandResolveCalleeSink=_demandResolveCalleeSink;" +
  "globalThis._specPathValMemo=function(){return _specPathValMemo;};" +
  "globalThis._djc=function(){_demandJump=new Map();_demandProvisional=new Set();};").call(globalThis);

var jq = fs.readFileSync(path.join(rd, "testing/harness-dumps/jquery-3.7.1.min.js"), "utf8");
var code = jq + "\njQuery.ajax({url:\"/api/simpler\"});\n";
var r = globalThis.analyzeJSBundle(code, "https://ex.com/app", "https://ex.com", null);
console.log("sites=" + JSON.stringify((r.fetchCallSites || []).map(function (s) { return s.method + " " + s.url; })));

function dump(av, d) {
  d = d || 0; if (!av || d > 5) return av ? av.kind : "<none>";
  if (av.kind === "or" || av.kind === "logical") return av.kind + "(" + dump(av.left, d + 1) + "|" + dump(av.right, d + 1) + ")";
  if (av.kind === "obj-lit") return "{" + Object.keys(av.props || {}).join(",") + "}";
  if (av.kind === "call") return "call(" + (av.callee && (av.callee.kind === "function-ref" ? "fn:" + (av.callee.funcNode && (av.callee.funcNode.id ? av.callee.funcNode.id.name : "anon")) : av.callee.kind)) + ")";
  if (av.kind === "member") return "member(" + dump(av.obj, d + 1) + "." + (av.key && (av.key.kind === "str" ? av.key.value : av.key.kind)) + ")";
  if (av.kind === "function-ref") return "fn-ref";
  if (av.kind === "str") return JSON.stringify(av.value);
  if (av.kind === "param") return "param#" + av.idx;
  if (av.kind === "coerce") return "coerce(" + dump(av.arg, d + 1) + ")";
  return av.kind;
}

globalThis.BabelBundle.traverse(r._ast, {
  Program: function (p) {
    globalThis._specBuildSlice(p);
    var seeds = globalThis._demandSinkSeeds(p);
    var openCS = seeds.filter(function (s) {
      var c = s.callNode && s.callNode.callee;
      return s.kind === "callee" && c && c.type === "MemberExpression" &&
        c.property && c.property.name === "open";
    });
    console.log("open sink seeds: " + openCS.length);
    if (openCS.length) {
      var callNode = openCS[0].callNode;
      var callee = callNode.callee;          // r.open
      var recvNode = callee.object;          // r
      var argType = callNode.arguments[0];   // i.type
      var argUrl = callNode.arguments[1];    // i.url
      globalThis._djc();
      var sink = globalThis._demandResolveCalleeSink(callee);
      console.log("_demandResolveCalleeSink(r.open) = " + (sink ? sink.id : "<null>"));
      globalThis._djc();
      console.log("recv r (=i.xhr())   → " + dump(globalThis._demandResolve(recvNode, null), 0));
      globalThis._djc();
      console.log("arg0 i.type          → " + dump(globalThis._demandResolve(argType, null), 0));
      globalThis._djc();
      console.log("arg1 i.url           → " + dump(globalThis._demandResolve(argUrl, null), 0));
    }
    p.stop();
  }
});
