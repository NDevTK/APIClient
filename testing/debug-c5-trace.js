// C5: pin where extend-merge backward resolution bottoms out.
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;" +
  "globalThis._specBuildSlice=_specBuildSlice;" +
  "globalThis._demandResolve=_demandResolve;" +
  "globalThis._demandResolveCalleeSink=_demandResolveCalleeSink;" +
  "globalThis._avFlattenStringLeaves=_avFlattenStringLeaves;" +
  "globalThis._pvm=function(){return _specPathValMemo;};").call(globalThis);

var code = `
function extend() {
  var t = arguments[0];
  for (var i = 1; i < arguments.length; i++) {
    var s = arguments[i];
    for (var k in s) t[k] = s[k];
  }
  return t;
}
var ajaxSettings = { xhr: function(){ return new XMLHttpRequest(); } };
function ajax(opts) {
  var s = extend({}, ajaxSettings, opts);
  var xhr = s.xhr();
  xhr.open(opts.type, opts.url);
}
ajax({ type: "GET", url: "/api/c5" });
`;
var r = globalThis.analyzeJSBundle(code, "t://c5", true, null);
globalThis.BabelBundle.traverse(r._ast, {
  Program: function(p) {
    globalThis._specBuildSlice(p);
    var openCall = null, sDeclInit = null, xhrDeclInit = null;
    p.traverse({
      CallExpression: function(cp) {
        var c = cp.node.callee;
        if (c && c.type === "MemberExpression" && c.property && c.property.name === "open") openCall = cp.node;
      },
      VariableDeclarator: function(vp) {
        if (vp.node.id && vp.node.id.name === "s" && vp.node.init) sDeclInit = vp.node.init;
        if (vp.node.id && vp.node.id.name === "xhr" && vp.node.init) xhrDeclInit = vp.node.init;
      }
    });
    function show(label, node) {
      if (!node) { console.log(label + " <no node>"); return; }
      var base = globalThis._pvm().get(node);
      var dr = globalThis._demandResolve(node, null);
      console.log(label + " base=" + (base ? base.kind + (base._ctorId ? "/" + base._ctorId : "") : "<none>") +
        " demand=" + (dr ? dr.kind + (dr._ctorId ? "/" + dr._ctorId : "") +
          (dr.kind === "obj-lit" && dr.props ? " props=[" + Object.keys(dr.props).join(",") + "]" : "") : "<none>") +
        " leaves=" + JSON.stringify(globalThis._avFlattenStringLeaves(dr)));
    }
    show("s.init (extend(...))", sDeclInit);
    show("xhr.init (s.xhr())", xhrDeclInit);
    if (openCall) {
      show("xhr (open receiver)", openCall.callee.object);
      var sink = globalThis._demandResolveCalleeSink(openCall.callee);
      console.log("calleeSink(xhr.open) = " + (sink ? sink.sink + " id=" + sink.id : "null"));
      show("opts.url arg", openCall.arguments[1]);
    }
    p.stop();
  }
});
