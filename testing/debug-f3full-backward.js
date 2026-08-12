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
  "globalThis._demandJumpClear=function(){_demandJump=new Map();};" +
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
  var dataTypes = options.dataTypes;
  function inspectFn(dataTypeExpr){
    var list = structure[dataTypeExpr] || [];
    for (var i = 0; i < list.length; i++) { var r = list[i](options); if (r && r.send) return r; }
  }
  return inspectFn(dataTypes[0]) || inspectFn("*");
}
function ajax(s){ var opts = jqExtend(true, { dataTypes: ["*"] }, ajaxSettings, s); var t = inspect(transports, opts); t.send(); }
ajax({ type: "GET", url: "/api/f3full" });
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
      console.log("--- backward _demandResolve(xhr) [UC trace, fresh jump] ---");
      globalThis._demandJumpClear();
      globalThis.__DEMAND_UC_TRACE = true;
      var recv = globalThis._demandResolve(c.object, null);
      globalThis.__DEMAND_UC_TRACE = false;
      console.log("xhr → " + (recv ? recv.kind : "<none>"));
      // Decisive: does the resolved `options` (= merged jqExtend obj-lit)
      // actually carry the `xhr` prop? Resolve the factory's param via
      // its dispatch site, then project `.xhr`.
      var fq2 = null;
      var fac2 = null;
      p.traverse({ FunctionExpression: function (fp) {
        if (fp.node.params[0] && fp.node.params[0].name === "options" && !fac2) fac2 = fp.node;
      }});
      if (fac2) {
        globalThis._demandJumpClear();
        var q = globalThis._demandArgQueries(fac2, 0, null);
        if (q && q.length) {
          var oav = globalThis._demandResolve(q[0].node, q[0].ctx || null);
          function dProps(av, d) {
            d = d || 0; if (!av || d > 3) return av ? av.kind : "<none>";
            if (av.kind === "or" || av.kind === "logical") return av.kind + "(" + dProps(av.left, d + 1) + "|" + dProps(av.right, d + 1) + ")";
            if (av.kind === "obj-lit") return "{" + Object.keys(av.props || {}).join(",") + (av._extraProps ? " +ep:" + Object.keys(av._extraProps).join(",") : "") + "}";
            return av.kind;
          }
          console.log("options(@dispatch) → " + dProps(oav, 0));
          // project .xhr
          var st = oav ? [oav] : []; var sn = new Set(); var xhrAv = null;
          while (st.length) { var x = st.pop(); if (!x || typeof x !== "object" || sn.has(x)) continue; sn.add(x);
            if (x.kind === "or" || x.kind === "logical") { st.push(x.left); st.push(x.right); continue; }
            if (x.kind === "obj-lit" && x.props && x.props.xhr) { xhrAv = x.props.xhr; break; } }
          console.log("options.xhr → " + (xhrAv ? xhrAv.kind + (xhrAv.kind === "function-ref" ? "(fn)" : "") : "<NOT IN obj-lit props>"));
          // C-isolation: resolve `opts` (ajax's `var opts=jqExtend(...)`)
          // and jqExtend's 4th arg `s` standalone — is the gap the
          // chain itself or only the (U-call) sub-query context?
          globalThis._demandJumpClear();
          var optsNode = null, sArg = null;
          p.traverse({
            VariableDeclarator: function (vp) { if (vp.node.id && vp.node.id.name === "opts" && vp.node.init) optsNode = vp.node.init; },
            CallExpression: function (cp) {
              var cc = cp.node.callee;
              if (cc && cc.name === "jqExtend" && cp.node.arguments.length >= 4) sArg = cp.node.arguments[3];
            }
          });
          if (optsNode) { var oR = globalThis._demandResolve(optsNode, null); console.log("opts=jqExtend(...) → " + dProps(oR, 0)); }
          if (sArg) { globalThis._demandJumpClear(); var sR = globalThis._demandResolve(sArg, null); console.log("jqExtend arg3 `s` → " + dProps(sR, 0)); }
        } else console.log("options(@dispatch): _demandArgQueries(factory,0)=0");
      }
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
