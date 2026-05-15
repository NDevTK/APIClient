// R3a evidence: with the carrier walk OFF (__DEMAND_NO_EDGES), trace
// every demand frame for JQ6's xhr.open url seed to see exactly which
// backward query bottoms out — that node is the general gap the proper
// dispatcher-refinement (not a bespoke def→use carrier walk) must close.
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;" +
  "globalThis._specBuildSlice=_specBuildSlice;" +
  "globalThis._demandSinkSeeds=_demandSinkSeeds;" +
  "globalThis._demandResolve=_demandResolve;" +
  "globalThis._avFlattenStringLeaves=_avFlattenStringLeaves;").call(globalThis);

var code = `
function Ut(o){return function(e,t){if(typeof e!=="string"){t=e;e="*";}(o[e]=o[e]||[]).push(t);};}
function Vt(t,opts){var arr=t["*"]||[];for(var i=0;i<arr.length;i++){var r=arr[i](opts);if(r&&r.send){r.send();return;}}}
var _t={};var at=Ut(_t);
at(function(opts){return{send:function(){var x=new XMLHttpRequest();x.open(opts.method,opts.url);}};});
Vt(_t,{url:"/api/jq6",method:"POST"});
`;
var r = globalThis.analyzeJSBundle(code, "t://jq6uc", true, null);
globalThis.BabelBundle.traverse(r._ast, {
  Program: function (p) {
    globalThis._specBuildSlice(p);
    var seeds = globalThis._demandSinkSeeds(p).filter(function (s) {
      return s.kind === "arg" && s.role === "url";
    });
    console.log("url seeds:", seeds.length);
    seeds.forEach(function (s) {
      console.log("\n--- carrier walk OFF (pure uniform rules) ---");
      globalThis.__DEMAND_NO_EDGES = true;
      globalThis.__DEMAND_UC_TRACE = true;
      var off = globalThis._demandResolve(s.argNode, null);
      globalThis.__DEMAND_UC_TRACE = false;
      console.log("OFF result:", off ? off.kind : "<none>",
        "leaves=" + JSON.stringify(globalThis._avFlattenStringLeaves(off)));
      console.log("\n--- carrier walk ON (baseline) ---");
      globalThis.__DEMAND_NO_EDGES = false;
      var on = globalThis._demandResolve(s.argNode, null);
      console.log("ON result:", on ? on.kind : "<none>",
        "leaves=" + JSON.stringify(globalThis._avFlattenStringLeaves(on)));
    });
    p.stop();
  }
});
