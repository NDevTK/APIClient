// Trace _demandResolve internals for the cond-callee case.
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
  "globalThis._avFlattenStringLeaves=_avFlattenStringLeaves;" +
  "globalThis._pvm=function(){return _specPathValMemo;};").call(globalThis);

var code = `
function a(u){ fetch(u); }
function b(u){ fetch(u); }
var pick = (1>0) ? a : b;
pick("/api/cond");
`;
var r = globalThis.analyzeJSBundle(code, "t://cc", true, null);
globalThis.BabelBundle.traverse(r._ast, {
  Program: function(p) {
    globalThis._specBuildSlice(p);
    var seeds = globalThis._demandSinkSeeds(p);
    var argS = seeds.filter(function(s){ return s.kind === "arg" && s.role === "url"; });
    console.log("arg url seeds:", argS.length);
    argS.forEach(function(s, i) {
      var bAv = globalThis._pvm().get(s.argNode);
      console.log("seed[" + i + "] argNode.type=" + s.argNode.type +
        " base AV=" + (bAv ? bAv.kind + (bAv.kind === "param" ? "(idx" + bAv.idx + ",fn@" + (bAv.fn && bAv.fn.loc && bAv.fn.loc.start.line) + ")" : "") : "<none>"));
      if (bAv && bAv.kind === "param") {
        var leaves = globalThis._demandParamLeaves(bAv);
        console.log("  paramLeaves:", leaves.map(function(L){ return "idx" + L.idx + "@fnL" + (L.fn && L.fn.loc && L.fn.loc.start.line); }));
        leaves.forEach(function(L) {
          var qs = globalThis._demandArgQueries(L.fn, L.idx, null);
          console.log("  argQueries(fnL" + (L.fn.loc && L.fn.loc.start.line) + ",idx" + L.idx + ") → " + qs.length);
          qs.forEach(function(q, qi) {
            var qAv = globalThis._pvm().get(q.node);
            console.log("    q[" + qi + "] node.type=" + q.node.type +
              " base=" + (qAv ? qAv.kind + (qAv.kind === "const" ? "=" + JSON.stringify(qAv.value) : "") : "<none>"));
          });
        });
      }
      var res = globalThis._demandResolve(s.argNode, null);
      console.log("  _demandResolve →", res ? res.kind + (res.kind === "const" ? "=" + JSON.stringify(res.value) : "") : "<none>",
        "leaves=" + JSON.stringify(globalThis._avFlattenStringLeaves(res)));
    });
    p.stop();
  }
});
