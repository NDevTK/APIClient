// Pin C8b: for chained `ns.extend = ns.fn.extend = function(){…}`,
// does _demandCalleeFns(ns.extend) return the extend FE, is it in
// _specFuncPathByNode, and does on-demand effects analysis make
// _specDetectPropagationFromEffects confirm it?
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;" +
  "globalThis._specBuildSlice=_specBuildSlice;" +
  "globalThis._demandIndexPaths=_demandIndexPaths;" +
  "globalThis._demandCalleeFns=_demandCalleeFns;" +
  "globalThis._specAnalyzePropertyFlow=_specAnalyzePropertyFlow;" +
  "globalThis._specDetectPropagationFromEffects=_specDetectPropagationFromEffects;" +
  "globalThis._specEffectsMemo=function(){return _specEffectsMemo;};" +
  "globalThis._specFuncPathByNode=function(){return _specFuncPathByNode;};").call(globalThis);

var code = `
var ns = { fn: {} };
ns.extend = ns.fn.extend = function(){ var i=0,t=arguments[0]; if(typeof t==="boolean"){t=arguments[1];i=2;}else{i=1;} for(;i<arguments.length;i++){var s=arguments[i];for(var k in s)t[k]=s[k];} return t; };
ns.ajaxSettings = { xhr: function(){ return new XMLHttpRequest(); } };
ns.ajax = function(o){ var s = ns.extend(true, {}, ns.ajaxSettings, o); var x = s.xhr(); x.open(o.type, o.url); };
ns.ajax({ type: "POST", url: "/api/c8b" });
`;
var r = globalThis.analyzeJSBundle(code, "t://c8b", true, null);
globalThis.BabelBundle.traverse(r._ast, {
  Program: function(p) {
    globalThis._specBuildSlice(p);
    globalThis._demandIndexPaths(p);
    var extendCalleeNode = null, extendFE = null;
    p.traverse({
      CallExpression: function(cp) {
        var c = cp.node.callee;
        if (c && c.type === "MemberExpression" && c.object && c.object.name === "ns" &&
            c.property && c.property.name === "extend") extendCalleeNode = c;
      },
      FunctionExpression: function(fp) {
        if (fp.node.params.length === 0 && fp.parentPath &&
            fp.parentPath.isAssignmentExpression && fp.parentPath.isAssignmentExpression()) extendFE = fp.node;
      }
    });
    console.log("extend callee node?", !!extendCalleeNode, "extendFE?", !!extendFE);
    // resolve scope for the callee via a fresh traversal
    var probeScope = null;
    p.traverse({ MemberExpression: function(mp){ if (mp.node === extendCalleeNode) probeScope = mp.scope; } });
    var cf = extendCalleeNode ? globalThis._demandCalleeFns(extendCalleeNode, probeScope) : [];
    console.log("_demandCalleeFns(ns.extend) →", cf.length,
      cf.map(function(g){ return g.type + "@L" + (g.loc && g.loc.start.line); }));
    if (cf.length) {
      var F = cf[0];
      var fpn = globalThis._specFuncPathByNode().get(F);
      console.log("extendFE in _specFuncPathByNode?", !!fpn);
      var ef0 = globalThis._specEffectsMemo().get(F);
      console.log("effects before on-demand:", ef0 ? ef0.length : "<none>");
      if (!ef0 && fpn) { try { globalThis._specAnalyzePropertyFlow(fpn, true); } catch(e){ console.log("APF threw:", e.message); } }
      var ef1 = globalThis._specEffectsMemo().get(F);
      console.log("effects after on-demand:", ef1 ? ef1.length : "<none>");
      console.log("detectPropagation?", ef1 ? !!globalThis._specDetectPropagationFromEffects(ef1) : "n/a");
    }
    p.stop();
  }
});
