// F4b-deeper: anonymous jQuery.ajaxSetup=function(){return jqExtend(...)}
// + outer 3-arg jqExtend(true,{dataTypes},ajaxSetup({},s)). Pin: does
// the ajaxSetup-call sub-query fold (recursive IDE composition)? does
// the outer jqExtend (U-call) enter + fold with arg2=concrete?
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;" +
  "globalThis._specBuildSlice=_specBuildSlice;" +
  "globalThis._demandSinkSeeds=_demandSinkSeeds;" +
  "globalThis._demandResolve=_demandResolve;" +
  "globalThis._demandResolveCalleeSink=_demandResolveCalleeSink;" +
  "globalThis._djc=function(){_demandJump=new Map();_demandProvisional=new Set();};").call(globalThis);

var code = `
var transports = {};
function addTo(structure){
  return function(dataTypeExpr, func){
    if (typeof dataTypeExpr !== "string") { func = dataTypeExpr; dataTypeExpr = "*"; }
    (structure[dataTypeExpr] = structure[dataTypeExpr] || []).push(func);
  };
}
var ajaxTransport = addTo(transports);
function jqExtend(){
  var i = 0, t = arguments[0];
  if (typeof t === "boolean") { t = arguments[1]; i = 2; } else { i = 1; }
  for (; i < arguments.length; i++) { var src = arguments[i]; for (var k in src) t[k] = src[k]; }
  return t;
}
var jQuery = {};
jQuery.ajaxSettings = { xhr: function(){ return new XMLHttpRequest(); } };
jQuery.ajaxSetup = function(target, settings){ return jqExtend(target, jQuery.ajaxSettings, settings); };
function inspect(structure, options){
  function inspectFn(dataTypeExpr){
    var list = structure[dataTypeExpr] || [];
    for (var i = 0; i < list.length; i++) { var r = list[i](options); if (r && r.send) return r; }
  }
  return inspectFn(options.dataTypes[0]) || inspectFn("*");
}
var cors = window.someFlag;
ajaxTransport("*", function(options){
  if (cors || !options.crossDomain) {
    return { send: function(){ var xhr = options.xhr(); xhr.open(options.type, options.url); } };
  }
});
function ajax(s){ var opts = jqExtend(true, { dataTypes: ["*"] }, jQuery.ajaxSetup({}, s)); var transport = inspect(transports, opts); transport.send(); }
ajax({ type: "GET", url: "/api/f4b" });
`;
var r = globalThis.analyzeJSBundle(code, "https://ex.com/app", "https://ex.com", null);
console.log("sites=" + JSON.stringify((r.fetchCallSites || []).map(function (s) { return s.method + " " + s.url; })));

function dump(av, d) {
  d = d || 0; if (!av || d > 4) return av ? av.kind : "<none>";
  if (av.kind === "or" || av.kind === "logical") return av.kind + "(" + dump(av.left, d + 1) + "|" + dump(av.right, d + 1) + ")";
  if (av.kind === "obj-lit") return "{" + Object.keys(av.props || {}).join(",") + "}";
  if (av.kind === "call") return "call(" + (av.callee && (av.callee.kind === "function-ref" ? "fn:" + (av.callee.funcNode && (av.callee.funcNode.id ? av.callee.funcNode.id.name : "anon")) : av.callee.kind)) + ")";
  if (av.kind === "function-ref") return "fn-ref";
  return av.kind;
}
globalThis.BabelBundle.traverse(r._ast, {
  Program: function (p) {
    globalThis._specBuildSlice(p);
    var nodes = { ajaxSetupCall: null, optsInit: null };
    p.traverse({
      CallExpression: function (cp) {
        var c = cp.node.callee;
        if (c && c.type === "MemberExpression" && c.object && c.object.name === "jQuery" &&
            c.property && c.property.name === "ajaxSetup") nodes.ajaxSetupCall = cp.node;
      },
      VariableDeclarator: function (vp) { if (vp.node.id && vp.node.id.name === "opts" && vp.node.init) nodes.optsInit = vp.node.init; }
    });
    if (nodes.ajaxSetupCall) {
      globalThis._djc();
      var asR = globalThis._demandResolve(nodes.ajaxSetupCall, null);
      console.log("jQuery.ajaxSetup({},s) → " + dump(asR, 0));
    } else console.log("ajaxSetup call node NOT FOUND");
    if (nodes.optsInit) {
      globalThis._djc();
      var oR = globalThis._demandResolve(nodes.optsInit, null);
      console.log("opts=jqExtend(true,{dataTypes},ajaxSetup({},s)) → " + dump(oR, 0));
    }
    // xhr.open backward
    var seeds = globalThis._demandSinkSeeds(p);
    var openCS = seeds.filter(function (s) { var c = s.callNode && s.callNode.callee; return s.kind === "callee" && c && c.type === "MemberExpression" && c.property && c.property.name === "open"; });
    if (openCS.length) {
      globalThis._djc();
      var sink = globalThis._demandResolveCalleeSink(openCS[0].callNode.callee);
      console.log("_demandResolveCalleeSink(xhr.open) = " + (sink ? sink.id : "<null>"));
    }
    p.stop();
  }
});
