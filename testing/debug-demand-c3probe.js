// C3 probe: for JQ-6, did points-to record factory ∈ _t["*"] ? i.e. does
// _specPointsToObjPropsReverseGet(factoryFnNode) yield {decl,key}? And what
// does _specFindCallSites(factory) return? Determines whether C3 can reuse
// the points-to-reverse index or must first establish the curried-push
// storage fact.
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;" +
  "globalThis._specBuildSlice=_specBuildSlice;" +
  "globalThis._specFindCallSites=_specFindCallSites;" +
  "globalThis._specFuncPathByNode=function(){return _specFuncPathByNode;};" +
  "globalThis._specPointsToObjPropsReverseGet=_specPointsToObjPropsReverseGet;" +
  "globalThis._specPointsToReturnReverse=function(){return _specPointsToReturnReverse;};" +
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
    var factory = null, optsParamFn = null;
    p.traverse({
      FunctionExpression: function(fp) {
        // factory = the FE passed as arg to `at(...)` (has param `opts`)
        if (fp.node.params && fp.node.params[0] && fp.node.params[0].name === "opts" &&
            fp.parentPath && fp.parentPath.isCallExpression &&
            fp.parentPath.isCallExpression()) {
          factory = fp.node;
        }
      }
    });
    console.log("factory FE found?", !!factory,
      factory ? "@L" + (factory.loc && factory.loc.start.line) : "");
    if (factory) {
      var fpByNode = globalThis._specFuncPathByNode();
      var fPath = fpByNode.get(factory);
      console.log("factory funcPath?", !!fPath);
      var sites = fPath ? globalThis._specFindCallSites(fPath) : [];
      console.log("_specFindCallSites(factory) →", sites.length, "sites",
        sites.map(function(s){ return s && s.node ? s.node.type + "/" +
          (s.node.callee && (s.node.callee.name ||
           (s.node.callee.type === "MemberExpression" ? "member" : s.node.callee.type))) : "?"; }));
      var rev = globalThis._specPointsToObjPropsReverseGet(factory);
      console.log("pointsToObjPropsReverse(factory) →",
        rev ? "[" + Array.from(rev).map(function(e){
          return "{decl@L" + (e.decl && e.decl.loc && e.decl.loc.start.line) + ",key=" + JSON.stringify(e.key) + "}";
        }).join(",") + "]" : "null");
      var rret = globalThis._specPointsToReturnReverse().get(factory);
      console.log("pointsToReturnReverse(factory) →", rret ? rret.size + " entries" : "null");
    }
    p.stop();
  }
});
