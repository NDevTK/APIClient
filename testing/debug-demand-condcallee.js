// Probe: for `var pick=(1>0)?a:b; pick("/api/cond")`, what does
// _specFindCallSites(a) return? (Step C: does existing points-to already
// discover the indirect `pick(...)` site, or must the demand engine add a
// virtual-call-site edge?)
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;" +
  "globalThis._specBuildSlice=_specBuildSlice;" +
  "globalThis._specFindCallSites=_specFindCallSites;" +
  "globalThis._specFuncPathByNode=function(){return _specFuncPathByNode;};" +
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
    var aNode = null, pickCall = null;
    p.traverse({
      FunctionDeclaration: function(fp) { if (fp.node.id && fp.node.id.name === "a") aNode = fp.node; },
      CallExpression: function(cp) { if (cp.node.callee && cp.node.callee.name === "pick") pickCall = cp.node; }
    });
    var fpByNode = globalThis._specFuncPathByNode();
    var aPath = fpByNode.get(aNode);
    console.log("a funcPath?", !!aPath);
    var sites = aPath ? globalThis._specFindCallSites(aPath) : [];
    console.log("_specFindCallSites(a) →", sites.length, "sites");
    sites.forEach(function(sp, i) {
      var sn = sp && sp.node;
      console.log("  [" + i + "]", sn ? (sn.type + " callee=" +
        (sn.callee && sn.callee.name ? sn.callee.name : (sn.callee && sn.callee.type))) : "?");
    });
    if (pickCall) {
      var cAv = globalThis._pvm().get(pickCall.callee);
      console.log("pick callee base AV:", cAv ? cAv.kind : "<none>",
        cAv && cAv.kind === "or" ? "(or)" : "");
    }
    p.stop();
  }
});
