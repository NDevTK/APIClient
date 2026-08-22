// Pin the faithful-jQuery gap: does _demandDispatchSites(transportFactory)
// find the `list[i](options)` registry dispatch? And is param-backthrough
// recursive enough (transportFactory.options ← inspect.options ← ajax.opts)?
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;" +
  "globalThis._specBuildSlice=_specBuildSlice;" +
  "globalThis._demandIndexPaths=_demandIndexPaths;" +
  "globalThis._demandDispatchSites=_demandDispatchSites;" +
  "globalThis._demandResolve=_demandResolve;" +
  "globalThis._demandMergeMember=_demandMergeMember;" +
  "globalThis._specFuncPathByNode=function(){return _specFuncPathByNode;};").call(globalThis);

var code = fs.readFileSync(path.join(rd, "testing/debug-demand-jqfaithful.js"), "utf8")
  .match(/var code = `([\s\S]*?)`;/)[1];
var r = globalThis.analyzeJSBundle(code, "t://jqf", true, null);
globalThis.BabelBundle.traverse(r._ast, {
  Program: function(p) {
    globalThis._specBuildSlice(p);
    globalThis._demandIndexPaths(p);
    var transportFE = null;
    p.traverse({
      FunctionExpression: function(fp) {
        if (fp.node.params[0] && fp.node.params[0].name === "options" &&
            fp.node.body && fp.node.body.body && fp.node.body.body.length === 1 &&
            fp.node.body.body[0].type === "ReturnStatement") transportFE = fp.node;
      }
    });
    console.log("transportFE found?", !!transportFE);
    if (transportFE) {
      var ds = globalThis._demandDispatchSites(transportFE);
      console.log("_demandDispatchSites(transportFE) →", ds.length,
        ds.map(function(c){ var ce=c.callee; return c.type+"/"+(ce&&(ce.name||(ce.type==="MemberExpression"?((ce.object&&ce.object.name)+(ce.computed?"[..]":"."+(ce.property&&ce.property.name))):ce.type))); }));
    }
    // options.xhr inside transportFE — does _demandMergeMember resolve it?
    var optsXhr = null;
    p.traverse({
      MemberExpression: function(mp) {
        if (mp.node.object && mp.node.object.name === "options" &&
            mp.node.property && mp.node.property.name === "xhr") optsXhr = mp.node;
      }
    });
    if (optsXhr) {
      var mm = globalThis._demandMergeMember(optsXhr);
      console.log("_demandMergeMember(options.xhr) =", mm ? "{key="+mm.key+",args="+mm.argNodes.length+"}" : "null");
      var dr = globalThis._demandResolve(optsXhr, null);
      console.log("_demandResolve(options.xhr) =", dr ? dr.kind : "<none>");
    }
    p.stop();
  }
});
