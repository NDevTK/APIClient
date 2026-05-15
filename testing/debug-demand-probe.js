var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;" +
  "globalThis._specBuildSlice=_specBuildSlice;" +
  "globalThis._demandSinkSeeds=_demandSinkSeeds;" +
  "globalThis._pvm=function(){return _specPathValMemo;};" +
  "globalThis._isPathInSlice=_isPathInSlice;").call(globalThis);

var r = globalThis.analyzeJSBundle('function f(u){ fetch(u); } f("/api/b1");', "t://b", true, null);
globalThis.BabelBundle.traverse(r._ast, {
  Program: function(p) {
    globalThis._specBuildSlice(p);
    p.traverse({
      CallExpression: function(cp) {
        if (cp.node.callee && cp.node.callee.name === "fetch") {
          var av = globalThis._pvm().get(cp.node.callee);
          console.log("fetch callee AV:", av ? av.kind + (av.id ? "/" + av.id : "") : "<none>");
          console.log("inSlice:", globalThis._isPathInSlice(cp));
          console.log("arg0 AV:", (globalThis._pvm().get(cp.node.arguments[0]) || {}).kind);
        }
      }
    });
    var seeds = globalThis._demandSinkSeeds(p);
    console.log("seeds:", seeds.length, JSON.stringify(seeds.map(function(s){ return s.kind + "/" + s.role; })));
    p.stop();
  }
});
