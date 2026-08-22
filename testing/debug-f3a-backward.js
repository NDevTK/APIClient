// F3a BACKWARD trace (post strategy-redirect): the fix belongs in the
// bounded demand query, not the forward fixpoint. Drive the production
// backward path — _demandResolveCalleeSink(xhr.open callee) — with
// __DEMAND_UC_TRACE so we see exactly where _demandResolve(options)
// bottoms out for the nested-closure registry dispatch.
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;" +
  "globalThis._specBuildSlice=_specBuildSlice;" +
  "globalThis._demandSinkSeeds=_demandSinkSeeds;" +
  "globalThis._demandResolve=_demandResolve;" +
  "globalThis._demandResolveCalleeSink=_demandResolveCalleeSink;" +
  "globalThis._demandArgQueries=_demandArgQueries;" +
  "globalThis._ptRev=function(fn){return _specPointsToObjPropsReverseGet(fn);};" +
  "globalThis._ptGet=function(d,k){return _specPointsToObjPropsGet?_specPointsToObjPropsGet(d,k):null;};" +
  "globalThis._avFlattenStringLeaves=_avFlattenStringLeaves;").call(globalThis);

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
var ajaxSettings = { xhr: function(){ return new XMLHttpRequest(); } };
var cors = window.someFlag;
ajaxTransport("*", function(options){
  if (cors || !options.crossDomain) {
    return { send: function(){ var xhr = options.xhr(); xhr.open(options.type, options.url); } };
  }
});
function inspect(structure, options){
  function inspectFn(){
    var list = structure["*"] || [];
    for (var i = 0; i < list.length; i++) { var r = list[i](options); if (r && r.send) return r; }
  }
  return inspectFn();
}
function ajax(s){ var opts = jqExtend(true, {}, ajaxSettings, s); var t = inspect(transports, opts); t.send(); }
ajax({ type: "GET", url: "/api/f3a" });
`;
var r = globalThis.analyzeJSBundle(code, "https://ex.com/app", "https://ex.com", null);
console.log("sites=" + JSON.stringify((r.fetchCallSites || []).map(function (s) { return s.method + " " + s.url; })));

globalThis.BabelBundle.traverse(r._ast, {
  Program: function (p) {
    globalThis._specBuildSlice(p);
    var seeds = globalThis._demandSinkSeeds(p);
    var openCS = seeds.filter(function (s) {
      var c = s.callNode && s.callNode.callee;
      return s.kind === "callee" && c && c.type === "MemberExpression" && c.property && c.property.name === "open";
    });
    console.log("x.open callee-seeds=" + openCS.length);
    if (openCS.length) {
      var cs = openCS[0];
      var c = cs.callNode.callee; // xhr.open
      console.log("--- _demandResolveCalleeSink(xhr.open) ---");
      var sink = globalThis._demandResolveCalleeSink(c);
      console.log("sink=" + (sink ? sink.id : "<null>"));
      console.log("--- backward _demandResolve(xhr) [UC trace] ---");
      globalThis.__DEMAND_UC_TRACE = true;
      var recv = globalThis._demandResolve(c.object, null);
      globalThis.__DEMAND_UC_TRACE = false;
      console.log("xhr → " + (recv ? recv.kind : "<none>"));
    }
    // Grounded bounded substrate: is the factory recorded in the
    // points-to reverse index (where it was stored via the registrar
    // `.push`)? That + the slot's readers = the bounded backward way
    // to find `list[i](options)` without the forward fixpoint.
    var factory = null;
    p.traverse({ FunctionExpression: function (fp) {
      if (fp.node.params[0] && fp.node.params[0].name === "options" &&
          !factory) factory = fp.node;
    }});
    if (factory) {
      var rev = globalThis._ptRev(factory);
      console.log("--- _specPointsToObjPropsReverseGet(factory) ---");
      if (!rev) console.log("  <null> — factory NOT in points-to reverse index");
      else {
        var arr = [];
        rev.forEach(function (e) {
          arr.push("{decl:" + (e.decl && (e.decl.type + (e.decl.id ? "#" + e.decl.id.name : "@L" + (e.decl.loc && e.decl.loc.start.line)))) +
            ", key:" + JSON.stringify(e.key) + "}");
        });
        console.log("  " + arr.length + " entries: " + arr.join("  "));
      }
      var fq = globalThis._demandArgQueries(factory, 0, null);
      console.log("_demandArgQueries(factory,0) → " + (fq ? fq.length + " queries" : "<none>"));
    } else console.log("factory fn NOT FOUND");
    p.stop();
  }
});
