// Manually walk the C3 carrier chain using exported primitives (no ast.js
// instrumentation) to find exactly where factory→arr[i](opts) breaks.
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;" +
  "globalThis._specBuildSlice=_specBuildSlice;" +
  "globalThis._t=_t;" +
  "globalThis._specFuncPathByNode=function(){return _specFuncPathByNode;};" +
  "globalThis._specWalkAvForFnRefs=_specWalkAvForFnRefs;" +
  "globalThis._specFindCallSites=_specFindCallSites;" +
  "globalThis._demandResolve=_demandResolve;" +
  "globalThis._demandCalleeFns=_demandCalleeFns;" +
  "globalThis._demandDispatchSites=_demandDispatchSites;" +
  "globalThis._pvm=function(){return _specPathValMemo;};").call(globalThis);

var code = `
function Ut(o){return function(e,t){if(typeof e!=="string"){t=e;e="*";}(o[e]=o[e]||[]).push(t);};}
function Vt(t,opts){var arr=t["*"]||[];for(var i=0;i<arr.length;i++){var r=arr[i](opts);if(r&&r.send){r.send();return;}}}
var _t={};var at=Ut(_t);
at(function(opts){return{send:function(){var x=new XMLHttpRequest();x.open(opts.method,opts.url);}};});
Vt(_t,{url:"/api/jq6",method:"POST"});
`;
var T = globalThis._t, pvm = globalThis._pvm();
var r = globalThis.analyzeJSBundle(code, "t://jq6", true, null);
globalThis.BabelBundle.traverse(r._ast, {
  Program: function(p) {
    globalThis._specBuildSlice(p);
    var fpByNode = globalThis._specFuncPathByNode();
    var factory = null;
    p.traverse({ FunctionExpression: function(fp){ if(fp.node.params[0]&&fp.node.params[0].name==="opts"&&fp.parentPath&&fp.parentPath.isCallExpression()) factory=fp.node; } });
    var fpath = fpByNode.get(factory);
    console.log("1. factory funcPath?", !!fpath, "parent=", fpath && fpath.parent && fpath.parent.type);
    // Direct: does _demandCalleeFns resolve `at` from factory-path scope?
    var atCall = fpath && fpath.parentPath && fpath.parentPath.node;
    var atCalleeNode = atCall && atCall.callee;
    var cf = atCalleeNode ? globalThis._demandCalleeFns(atCalleeNode, fpath.scope) : [];
    console.log("0. _demandCalleeFns(at, fpath.scope) →",
      cf.map(function(g){ return "fn@L" + (g.loc && g.loc.start.line); }));
    var atBind = fpath && fpath.scope && fpath.scope.getBinding("at");
    console.log("0b. fpath.scope.getBinding('at')?", !!atBind,
      atBind && atBind.path && atBind.path.node ? atBind.path.node.type : "",
      atBind && atBind.path && atBind.path.node && atBind.path.node.init ? "init=" + atBind.path.node.init.type : "");
    if (atBind && atBind.path && atBind.path.node && atBind.path.node.init) {
      var initAv2 = pvm.get(atBind.path.node.init);
      console.log("0c. pvm.get(at.init=Ut(_t))=", initAv2 ? initAv2.kind +
        (initAv2.kind === "function-ref" ? "@L" + (initAv2.funcNode && initAv2.funcNode.loc.start.line) : "") : "<none>");
    }
    console.log("0d. _demandDispatchSites(factory) =", globalThis._demandDispatchSites(factory).length);
    // seedName?
    var seedName = null;
    if (T.isFunctionDeclaration(factory) && factory.id) seedName = factory.id.name;
    else if (fpath.parentPath && T.isVariableDeclarator(fpath.parent) &&
             T.isIdentifier(fpath.parent.id) && fpath.parent.init === factory) seedName = fpath.parent.id.name;
    console.log("2. seedName=", seedName, "(null ⇒ seed = pushVal(fnPath))");
    // val(fpath): parent = at(factory) call
    var par = fpath.parentPath, pn = par && par.node;
    console.log("3. val parent isCall?", pn && T.isCallExpression(pn),
      "argIdx=", pn && pn.arguments ? pn.arguments.indexOf(factory) : "?",
      "callee=", pn && pn.callee && (pn.callee.name || pn.callee.type));
    var ceAv = pn && pvm.get(pn.callee);
    console.log("4. at-callee base AV=", ceAv ? ceAv.kind : "<none>");
    var ceDemand = pn && globalThis._demandResolve(pn.callee, null);
    console.log("4b. _demandResolve(at-callee)=", ceDemand ? ceDemand.kind +
      (ceDemand.kind === "function-ref" ? "@L" + (ceDemand.funcNode && ceDemand.funcNode.loc.start.line) : "") : "<none>");
    var gFns = [];
    if (ceDemand) globalThis._specWalkAvForFnRefs(ceDemand, function(g){ gFns.push(g); });
    if (!gFns.length && ceAv) globalThis._specWalkAvForFnRefs(ceAv, function(g){ gFns.push(g); });
    console.log("   → walkFnRefs:", gFns.map(function(g){ return "fn@L"+(g.loc&&g.loc.start.line)+" params="+(g.params||[]).map(function(x){return x.name;}); }));
    // param(innerFE, 0) → e binding refs
    if (gFns.length) {
      var inner = gFns[0];
      var ipath = fpByNode.get(inner);
      console.log("5. innerFE funcPath?", !!ipath);
      if (ipath) {
        var e0 = inner.params[0];
        var eName = T.isAssignmentPattern(e0) ? e0.left.name : e0.name;
        var eb = ipath.scope.getBinding(eName);
        console.log("   param0 name=", eName, "binding refs=", eb && eb.referencePaths ? eb.referencePaths.length : "noBinding");
        if (eb && eb.referencePaths) eb.referencePaths.forEach(function(rp, i){
          console.log("     eRef[" + i + "] parent=" + (rp.parent && rp.parent.type) +
            (rp.parent && T.isAssignmentExpression(rp.parent) ? " (assign left=" + (rp.parent.left && rp.parent.left.name) + " right=" + (rp.parent.right===rp.node) + ")" : "") +
            (rp.parent && T.isCallExpression(rp.parent) ? " (callArg)" : "") +
            (rp.parent && T.isMemberExpression(rp.parent) ? " (member obj=" + (rp.parent.object===rp.node) + ")" : ""));
        });
      }
    }
    p.stop();
  }
});
