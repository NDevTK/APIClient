// R3b fail-first: prove the PROPER mechanism for replacing the carrier
// walk — refining JQ6's static-concrete dispatcher chain (Ut(_t) →
// at(factory) registrar → Vt(_t,{...})) via the general
// _specRefineCallUnderContext primitive makes the ENGINE'S OWN value
// resolution populate `arr[i]→factory`, so the sink arg resolves and
// _specCallSitesByFn[factory] gets the `arr[i](opts)` site — NO bespoke
// def→use carrier walk. Carrier walk forced OFF (__DEMAND_NO_EDGES) so
// only the engine path is measured.
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;" +
  "globalThis._specBuildSlice=_specBuildSlice;" +
  "globalThis._demandSinkSeeds=_demandSinkSeeds;" +
  "globalThis._demandResolve=_demandResolve;" +
  "globalThis._specRefineCallUnderContext=_specRefineCallUnderContext;" +
  "globalThis._specAnalyzePropertyFlow=_specAnalyzePropertyFlow;" +
  "globalThis._specCallSitesByFn=function(){return _specCallSitesByFn;};" +
  "globalThis._specFindCallSites=_specFindCallSites;" +
  "globalThis._specFuncPathByNode=function(){return _specFuncPathByNode;};" +
  "globalThis._specPVM=function(){return _specPathValMemo;};" +
  "globalThis._avFlattenStringLeaves=_avFlattenStringLeaves;" +
  "globalThis._tt=_t;").call(globalThis);

var B = globalThis.BabelBundle, _t = globalThis._tt;
var code = `
function Ut(o){return function(e,t){if(typeof e!=="string"){t=e;e="*";}(o[e]=o[e]||[]).push(t);};}
function Vt(t,opts){var arr=t["*"]||[];for(var i=0;i<arr.length;i++){var r=arr[i](opts);if(r&&r.send){r.send();return;}}}
var _t={};var at=Ut(_t);
at(function(opts){return{send:function(){var x=new XMLHttpRequest();x.open(opts.method,opts.url);}};});
Vt(_t,{url:"/api/jq6",method:"POST"});
`;
var r = globalThis.analyzeJSBundle(code, "t://jq6dr", true, null);

B.traverse(r._ast, {
  Program: function (pp) {
    globalThis._specBuildSlice(pp);
    globalThis.__DEMAND_NO_EDGES = true; // carrier walk OFF — engine path only

    // Locate the transport factory FE (the FE arg to the registrar call)
    // and the program's top-level concrete Identifier-callee calls.
    var factoryFE = null, topCalls = [];
    pp.traverse({
      FunctionExpression: function (q) {
        if (_t.isCallExpression(q.parent) && q.parent.arguments.indexOf(q.node) >= 0 &&
            q.parent.callee !== q.node) factoryFE = q.node;
      },
      CallExpression: function (q) {
        // top-level statement-expression / declarator-init calls
        if (_t.isIdentifier(q.node.callee)) topCalls.push(q);
      }
    });
    console.log("factoryFE found:", !!factoryFE, " topCalls:",
      topCalls.map(function (c) { return c.node.callee.name; }));

    var seeds = globalThis._demandSinkSeeds(pp).filter(function (s) {
      return s.kind === "arg" && s.role === "url";
    });
    var sinkArg = seeds.length ? seeds[0].argNode : null;
    console.log("sink url seed:", !!sinkArg);

    function rd0() {
      var av = globalThis._demandResolve(sinkArg, null);
      return av ? av.kind + " leaves=" + JSON.stringify(globalThis._avFlattenStringLeaves(av)) : "<none>";
    }
    function csCount() {
      var m = globalThis._specCallSitesByFn();
      var arr = factoryFE && m.get(factoryFE);
      return arr ? arr.length : 0;
    }
    console.log("BEFORE refine: sink=" + rd0() + " callSitesByFn[factory]=" + csCount());

    // Refine each top-level concrete call in source order via the general
    // primitive: resolve callee→fn (scope binding / base AV), args via
    // _demandResolve, then _specRefineCallUnderContext. Then re-measure.
    topCalls.sort(function (a, b) { return a.node.start - b.node.start; });
    topCalls.forEach(function (cp) {
      var ce = cp.node;
      var fnNode = null, fnAv = null;
      var b = cp.scope.getBinding(ce.callee.name);
      if (b && b.path && b.path.node) {
        if (_t.isFunctionDeclaration(b.path.node)) fnNode = b.path.node;
        else if (_t.isVariableDeclarator(b.path.node) && b.path.node.init) {
          var iv = b.path.node.init;
          if (_t.isFunctionExpression(iv) || _t.isArrowFunctionExpression(iv)) fnNode = iv;
          else {
            var av = globalThis._specPVM().get(iv) || globalThis._demandResolve(iv, null);
            if (av && av.kind === "function-ref") { fnNode = av.funcNode; fnAv = av; }
          }
        }
      }
      if (!fnNode) { console.log("  refine " + ce.callee.name + " → callee fn UNRESOLVED"); return; }
      var argAvs = ce.arguments.map(function (a) { return globalThis._demandResolve(a, null); });
      var out = globalThis._specRefineCallUnderContext(fnNode, argAvs, ce, null, [sinkArg], fnAv);
      console.log("  refined " + ce.callee.name + "(args=" +
        argAvs.map(function (x) { return x ? x.kind : "?"; }).join(",") + ") → ret=" +
        (out && out.retAv ? out.retAv.kind : "null") +
        "  sink=" + rd0() + "  callSitesByFn[factory]=" + csCount());
    });

    console.log("AFTER chain refine: sink=" + rd0() + " callSitesByFn[factory]=" + csCount());

    // SP4 hypothesis: with _specCtxEffectsBySite[Ut(_t)] now recorded,
    // re-evaluating the enclosing scope (Program body) should replay the
    // registrar's closure write onto `_t` (the forward path's write-back),
    // so `_t["*"]=[factory]` ⇒ engine resolves arr[i]→factory.
    try { globalThis._specAnalyzePropertyFlow(pp, true); } catch (e) { console.log("reEval threw " + e.message); }
    console.log("AFTER program re-eval: sink=" + rd0() + " callSitesByFn[factory]=" + csCount());
    pp.stop();
  }
});
