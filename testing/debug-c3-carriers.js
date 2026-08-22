// C3 carrier-chain probe for JQ-6: observe the exact def→use structure the
// bounded forward walk must follow (no assumptions).
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;" +
  "globalThis._specBuildSlice=_specBuildSlice;" +
  "globalThis._specFuncPathByNode=function(){return _specFuncPathByNode;};" +
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
    var pvm = globalThis._pvm();
    p.traverse({
      FunctionExpression: function(fp) {
        if (fp.node.params[0] && fp.node.params[0].name === "opts" &&
            fp.parentPath && fp.parentPath.isCallExpression()) {
          var fac = fp.node;
          console.log("factory @L" + fac.loc.start.line +
            " parent=" + fp.parent.type +
            " parentCallee=" + (fp.parent.callee && (fp.parent.callee.name || fp.parent.callee.type)) +
            " argIdx=" + fp.parent.arguments.indexOf(fac));
          var atCalleeAv = pvm.get(fp.parent.callee);
          console.log("  at(...) callee base AV=" +
            (atCalleeAv ? atCalleeAv.kind + (atCalleeAv.kind === "function-ref" ?
              "@L" + (atCalleeAv.funcNode && atCalleeAv.funcNode.loc.start.line) : "") : "<none>"));
        }
      },
      VariableDeclarator: function(vp) {
        if (vp.node.id && vp.node.id.name === "at") {
          var initAv = pvm.get(vp.node.init);
          console.log("var at = " + (vp.node.init && vp.node.init.type) +
            " → base AV " + (initAv ? initAv.kind + (initAv.kind === "function-ref" ?
              "@L" + (initAv.funcNode && initAv.funcNode.loc.start.line) : "") : "<none>"));
        }
        if (vp.node.id && vp.node.id.name === "_t") {
          console.log("var _t = " + (vp.node.init && vp.node.init.type) +
            " (declNode line " + vp.node.loc.start.line + ")");
        }
      },
      CallExpression: function(cp) {
        if (cp.node.callee && cp.node.callee.name === "Ut") {
          console.log("Ut(...) call args[0].type=" + (cp.node.arguments[0] && cp.node.arguments[0].type) +
            " name=" + (cp.node.arguments[0] && cp.node.arguments[0].name));
        }
        if (cp.node.callee && cp.node.callee.name === "Vt") {
          var a0 = pvm.get(cp.node.arguments[0]);
          var a1 = pvm.get(cp.node.arguments[1]);
          console.log("Vt(...) arg0 AV=" + (a0 && a0.kind) + " arg1 AV=" + (a1 && a1.kind) +
            (a1 && a1.kind === "obj-lit" ? " props=" + Object.keys(a1.props || {}) : ""));
        }
      }
    });
    p.stop();
  }
});
