// Step C target: trace what _demandResolve needs for JQ-6's xhr.open url.
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;" +
  "globalThis._specBuildSlice=_specBuildSlice;" +
  "globalThis._demandSinkSeeds=_demandSinkSeeds;" +
  "globalThis._demandResolve=_demandResolve;" +
  "globalThis._demandParamLeaves=_demandParamLeaves;" +
  "globalThis._demandArgQueries=_demandArgQueries;" +
  "globalThis._specFindCallSites=_specFindCallSites;" +
  "globalThis._specFuncPathByNode=function(){return _specFuncPathByNode;};" +
  "globalThis._avFlattenStringLeaves=_avFlattenStringLeaves;" +
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
    var seeds = globalThis._demandSinkSeeds(p);
    var argS = seeds.filter(function(s){ return s.kind === "arg"; });
    var disp = seeds.filter(function(s){ return s.kind === "callee"; });
    console.log("arg seeds:", argS.length, argS.map(function(s){ return s.sink + ":" + s.role; }));
    console.log("dispatch seeds:", disp.length);
    argS.forEach(function(s) {
      var bAv = globalThis._pvm().get(s.argNode);
      console.log(s.role + " argNode=" + s.argNode.type +
        " base=" + (bAv ? bAv.kind + (bAv.kind === "member" ? "(obj=" + (bAv.obj && bAv.obj.kind) + ",key=" + (bAv.key && bAv.key.kind) + ")" : "") : "<none>"));
      var res = globalThis._demandResolve(s.argNode, null);
      console.log("  _demandResolve → " + (res ? res.kind : "<none>") +
        " leaves=" + JSON.stringify(globalThis._avFlattenStringLeaves(res)));
    });
    p.stop();
  }
});
