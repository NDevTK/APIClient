// Probe _demandDispatchSites(factory) for JQ-6 — does the carrier walk
// reach the arr[i](opts) virtual call site? Trace each carrier transition.
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;" +
  "globalThis._specBuildSlice=_specBuildSlice;" +
  "globalThis._demandDispatchSites=_demandDispatchSites;" +
  "globalThis._specFindCallSites=_specFindCallSites;" +
  "globalThis._specFuncPathByNode=function(){return _specFuncPathByNode;};" +
  "globalThis._pvm=function(){return _specPathValMemo;};").call(globalThis);

var code = `
function Ut(o){return function(e,t){if(typeof e!=="string"){t=e;e="*";}(o[e]=o[e]||[]).push(t);};}
function Vt(t,opts){var arr=t["*"]||[];for(var i=0;i<arr.length;i++){var r=arr[i](opts);if(r&&r.send){r.send();return;}}}
var _t={};var at=Ut(_t);
at(function(opts){return{send:function(){var x=new XMLHttpRequest();x.open(opts.method,opts.url);}};});
Vt(_t,{url:"/api/jq6",method:"POST"});
`;
var r = globalThis.analyzeJSBundle(code, "t://jq6", true, null);
globalThis.BabelBundle.traverse(r._ast, {
  Program: function(p) {
    globalThis._specBuildSlice(p);
    var factory = null;
    p.traverse({
      FunctionExpression: function(fp) {
        if (fp.node.params[0] && fp.node.params[0].name === "opts" &&
            fp.parentPath && fp.parentPath.isCallExpression()) factory = fp.node;
      }
    });
    console.log("factory found?", !!factory, factory ? "@L" + factory.loc.start.line : "");
    if (factory) {
      var sites = globalThis._demandDispatchSites(factory);
      console.log("_demandDispatchSites(factory) →", sites.length, "call(s)");
      sites.forEach(function(cn, i) {
        var cs = cn.callee;
        console.log("  [" + i + "] " + cn.type + " @L" + (cn.loc && cn.loc.start.line) +
          " callee=" + (cs && (cs.name || (cs.type === "MemberExpression" ?
            ((cs.object && cs.object.name) || "?") + (cs.computed ? "[..]" : "." + (cs.property && cs.property.name)) : cs.type))) +
          " args=" + (cn.arguments || []).map(function(a){ return a.type + (a.name ? "(" + a.name + ")" : ""); }).join(","));
      });
    }
    p.stop();
  }
});
