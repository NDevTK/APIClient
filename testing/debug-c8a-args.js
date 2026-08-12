// Pin C8a arg-backward: for the member-assigned `ns.ajax = function(o){…}`,
// what do _specFindCallSites / _demandDispatchSites return for ajaxFn?
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;" +
  "globalThis._specBuildSlice=_specBuildSlice;" +
  "globalThis._demandIndexPaths=_demandIndexPaths;" +
  "globalThis._specFindCallSites=_specFindCallSites;" +
  "globalThis._demandDispatchSites=_demandDispatchSites;" +
  "globalThis._demandArgQueries=_demandArgQueries;" +
  "globalThis._specFuncPathByNode=function(){return _specFuncPathByNode;};").call(globalThis);

var code = `
var ns = {};
ns.extend = function(){ var i=0,t=arguments[0]; if(typeof t==="boolean"){t=arguments[1];i=2;}else{i=1;} for(;i<arguments.length;i++){var s=arguments[i];for(var k in s)t[k]=s[k];} return t; };
ns.ajaxSettings = { xhr: function(){ return new XMLHttpRequest(); } };
ns.ajax = function(o){ var s = ns.extend(true, {}, ns.ajaxSettings, o); var x = s.xhr(); x.open(o.type, o.url); };
ns.ajax({ type: "GET", url: "/api/c8a" });
`;
var r = globalThis.analyzeJSBundle(code, "t://c8a", true, null);
globalThis.BabelBundle.traverse(r._ast, {
  Program: function(p) {
    globalThis._specBuildSlice(p);
    globalThis._demandIndexPaths(p);
    var ajaxFn = null;
    p.traverse({
      AssignmentExpression: function(ap) {
        if (ap.node.left && ap.node.left.type === "MemberExpression" &&
            ap.node.left.property && ap.node.left.property.name === "ajax" &&
            ap.node.right && ap.node.right.type === "FunctionExpression") ajaxFn = ap.node.right;
      }
    });
    console.log("ajaxFn found?", !!ajaxFn, ajaxFn ? "params=" + ajaxFn.params.map(function(x){return x.name;}) : "");
    var fpByNode = globalThis._specFuncPathByNode();
    var ajaxPath = fpByNode.get(ajaxFn);
    console.log("ajaxFn funcPath?", !!ajaxPath);
    var fcs = ajaxPath ? globalThis._specFindCallSites(ajaxPath) : [];
    console.log("_specFindCallSites(ajaxFn) →", fcs.length,
      fcs.map(function(s){ return s && s.node ? s.node.type + "/" +
        (s.node.callee && (s.node.callee.name || (s.node.callee.type === "MemberExpression" ?
          ((s.node.callee.object && s.node.callee.object.name) + "." + (s.node.callee.property && s.node.callee.property.name)) : s.node.callee.type))) : "?"; }));
    var dds = globalThis._demandDispatchSites(ajaxFn);
    console.log("_demandDispatchSites(ajaxFn) →", dds.length,
      dds.map(function(c){ return c.type + "@L" + (c.loc && c.loc.start.line); }));
    var aq = globalThis._demandArgQueries(ajaxFn, 0, null);
    console.log("_demandArgQueries(ajaxFn,0) →", aq.length,
      aq.map(function(q){ return (q.node && q.node.type) + (q.node && q.node.type === "ObjectExpression" ? "{" + q.node.properties.map(function(pp){return pp.key && pp.key.name;}).join(",") + "}" : ""); }));
    p.stop();
  }
});
