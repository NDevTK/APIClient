// F3a DEEP empirical diagnostic (no modelling — measure the actual
// break). Questions: (a) by what path is inspectFn refined? (b) is
// `inspect` in _specRefinedCallSiteArgs / _specContextCallerArgs when
// inspectFn refines? (c) what is the AV of `structure`, `structure["*"]`,
// `list`, `list[i]` in inspectFn's refined memo — which sub-term is
// opaque? (d) does _runDynamicEdgeIndex visit `list[i]`?
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;" +
  "globalThis._specBuildSlice=_specBuildSlice;" +
  "globalThis._lrmbf=function(){return _specLatestRefinedMemoByFn;};" +
  "globalThis._rcsa=function(){return _specRefinedCallSiteArgs;};" +
  "globalThis._scsbf=function(){return _specCallSitesByFn;};" +
  "globalThis._pvm=function(){return _specPathValMemo;};").call(globalThis);

var code = `
var transports = {};
function addTo(structure){
  return function(dataTypeExpr, func){
    if (typeof dataTypeExpr !== "string") { func = dataTypeExpr; dataTypeExpr = "*"; }
    (structure[dataTypeExpr] = structure[dataTypeExpr] || []).push(func);
  };
}
var ajaxTransport = addTo(transports);
function jqExtend(){
  var i = 0, t = arguments[0];
  if (typeof t === "boolean") { t = arguments[1]; i = 2; } else { i = 1; }
  for (; i < arguments.length; i++) { var src = arguments[i]; for (var k in src) t[k] = src[k]; }
  return t;
}
var ajaxSettings = { xhr: function(){ return new XMLHttpRequest(); } };
var cors = window.someFlag;
ajaxTransport("*", function(options){
  if (cors || !options.crossDomain) {
    return { send: function(){ var xhr = options.xhr(); xhr.open(options.type, options.url); } };
  }
});
function inspect(structure, options){
  function inspectFn(){
    var list = structure["*"] || [];
    for (var i = 0; i < list.length; i++) { var r = list[i](options); if (r && r.send) return r; }
  }
  return inspectFn();
}
function ajax(s){ var opts = jqExtend(true, {}, ajaxSettings, s); var t = inspect(transports, opts); t.send(); }
ajax({ type: "GET", url: "/api/f3a" });
`;
var r = globalThis.analyzeJSBundle(code, "https://ex.com/app", "https://ex.com", null);
console.log("sites=" + JSON.stringify((r.fetchCallSites || []).map(function (s) { return s.method + " " + s.url; })));

globalThis.BabelBundle.traverse(r._ast, {
  Program: function (p) {
    globalThis._specBuildSlice(p);
    var fns = {};
    p.traverse({ FunctionDeclaration: function (fp) { fns[fp.node.id.name] = fp.node; } });
    function dump(av) {
      if (!av) return "<none>";
      var s = av.kind;
      if (av.kind === "member") s += "(obj=" + (av.obj && av.obj.kind) + ",key=" + (av.key && (av.key.kind === "const" ? JSON.stringify(av.key.value) : av.key.kind)) + ")";
      else if (av.kind === "logical" || av.kind === "or") s += "(" + (av.left && av.left.kind) + "," + (av.right && av.right.kind) + ")";
      else if (av.kind === "array-lit") s += "[" + (av.elements ? av.elements.length : 0) + "]";
      else if (av.kind === "obj-lit") s += "{" + Object.keys(av.props || {}).join(",") + "}";
      else if (av.kind === "function-ref") s += "(fn)";
      else if (av.kind === "param") s += "(idx=" + av.idx + ",fn=" + (av.fn && av.fn.type) + ")";
      else if (av.kind === "const") s += "(" + JSON.stringify(av.value) + ")";
      return s;
    }
    // Deep recursive prop dump — does any leaf actually carry *:[fn]?
    function deep(av, d) {
      d = d || 0; if (!av || d > 4) return dump(av);
      if (av.kind === "or" || av.kind === "logical") return av.kind + "(" + deep(av.left, d + 1) + " | " + deep(av.right, d + 1) + ")";
      if (av.kind === "obj-lit") {
        var ks = Object.keys(av.props || {});
        return "{" + ks.map(function (k) { return k + ":" + deep(av.props[k], d + 1); }).join(",") + "}";
      }
      if (av.kind === "array-lit") return "[" + (av.elements || []).map(function (e) { return deep(e, d + 1); }).join(",") + "]";
      return dump(av);
    }
    var rcsa = globalThis._rcsa(), lrmbf = globalThis._lrmbf(), scsbf = globalThis._scsbf();
    // (1) does inspect's refined `structure` arg actually carry *:[factory]?
    if (rcsa.has(fns["inspect"])) {
      var insArgs = rcsa.get(fns["inspect"]) || [];
      console.log("(1) inspect.structure refined-arg DEEP = " + deep(insArgs[0], 0));
    }
    // and the `transports` identifier AV at the inspect(transports,opts) call site
    var insCall = null;
    p.traverse({ CallExpression: function (cp) {
      if (cp.node.callee && cp.node.callee.name === "inspect") insCall = cp.node;
    }});
    if (insCall && insCall.arguments[0]) {
      console.log("(1) transports@inspect-callsite base = " + deep(globalThis._pvm().get(insCall.arguments[0]), 0));
    }
    // (b) is inspect/inspectFn in the refined-args / call-sites indices?
    ["inspect", "inspectFn", "ajax"].forEach(function (k) {
      console.log("(b) " + k + ": _specRefinedCallSiteArgs=" +
        (rcsa.has(fns[k]) ? "[" + (rcsa.get(fns[k]) || []).map(dump).join(", ") + "]" : "NO") +
        " refinedMemo=" + (lrmbf.get(fns[k]) ? "yes" : "no"));
    });
    // factory fn
    var factory = null;
    p.traverse({ FunctionExpression: function (fp) { if (fp.node.params[0] && fp.node.params[0].name === "options") factory = fp.node; } });
    console.log("(d) factory in _specCallSitesByFn=" + (factory && scsbf.get(factory) ? scsbf.get(factory).length + " sites" : "NO"));
    // (c) AVs inside inspectFn's refined memo for structure / structure["*"] / list / list[i]
    var nodes = { structureRef: null, structureStar: null, listDecl: null, listI: null };
    p.traverse({
      MemberExpression: function (mp) {
        var n = mp.node;
        if (n.object && n.object.name === "structure" && n.property && (n.property.value === "*" || n.property.name === "*")) nodes.structureStar = n;
      },
      Identifier: function (ip) {
        if (ip.node.name === "structure" && !nodes.structureRef &&
            ip.parentPath && ip.parentPath.node.type === "MemberExpression") nodes.structureRef = ip.node;
      },
      VariableDeclarator: function (vp) { if (vp.node.id && vp.node.id.name === "list") nodes.listDecl = vp.node.init; },
      CallExpression: function (cp) {
        var c = cp.node.callee;
        if (c && c.type === "MemberExpression" && c.computed && c.object && c.object.name === "list") nodes.listI = c;
      }
    });
    var rm = lrmbf.get(fns["inspectFn"]);
    var base = globalThis._pvm();
    ["structureRef", "structureStar", "listDecl", "listI"].forEach(function (key) {
      var nd = nodes[key];
      if (!nd) { console.log("(c) " + key + " NODE NOT FOUND"); return; }
      console.log("(c) " + key + " base=" + dump(base.get(nd)) +
        " refined(inspectFn)=" + dump(rm ? rm.get(nd) : null));
    });
    p.stop();
  }
});
