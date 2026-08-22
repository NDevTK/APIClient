// G6 = E∘A: factory returns a fresh-local obj with an Object.define-
// Property accessor getter closing over the factory param; consumer
// reads `cfg.url`. E (cont.¹⁵) + A (cont.²⁴) both landed — where does
// the composition bottom out now? Instrument-first, no assumptions.
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;" +
  "globalThis._DR=function(n,c){try{return _demandResolve(n,c);}catch(e){return 'ERR:'+e.message;}};" +
  "globalThis._RAV=function(p){try{return _resolveAllValues(p,0);}catch(e){return 'ERR:'+e.message;}};" +
  "globalThis._EFF=function(){return _specEffectsMemo;};" +
  "globalThis._RVG=function(ctx,fn){try{return _specReturnValueGet(ctx,fn);}catch(e){return 'ERR:'+e.message;}};" +
  "globalThis._SACK=function(){try{return _specActiveContextKey();}catch(e){return 'ERR';}};" +
  "globalThis._STDCTX=function(){return typeof _SPEC_STANDALONE_CTX!=='undefined'?_SPEC_STANDALONE_CTX:'<undef>';};" +
  "globalThis._BT=function(){return BabelBundle.traverse;};" +
  "globalThis._TT=function(){return _t;};").call(globalThis);

function avs(a){ if(!a) return "<null>"; if(typeof a!=="object") return String(a);
  if(a.kind==="const") return "const:"+JSON.stringify(a.value);
  if(a.kind==="param") return "param#"+a.idx;
  if(a.kind==="member") return "member("+avs(a.obj)+"."+(a.key&&(a.key.value||a.key.kind))+")";
  if(a.kind==="obj-lit") return "obj-lit{"+Object.keys(a.props||{}).join(",")+"}";
  if(a.kind==="or"||a.kind==="logical") return a.kind+"("+avs(a.left)+"|"+avs(a.right)+")";
  if(a.kind==="call") return "call("+avs(a.callee)+")";
  return a.kind; }

var code = [
  'function makeCfg(u){ var c = {}; Object.defineProperty(c, "url", { get: function(){ return u; }, enumerable: true }); return c; }',
  'var cfg = makeCfg("/api/g6-endpoint");',
  'var xhr = new XMLHttpRequest(); xhr.open("GET", cfg.url);'
].join("\n");

var r = globalThis.analyzeJSBundle(code, "https://ex.com/app", "https://ex.com", null);
console.log("sites=" + JSON.stringify((r.fetchCallSites || []).map(function (s) { return s.method + " " + s.url; })));

var tt = globalThis._TT(), bt = globalThis._BT();
var openCall = null, cfgDecl = null, makeCfgFn = null;
bt(r._ast, {
  FunctionDeclaration: function (p) { if (p.node.id && p.node.id.name === "makeCfg") makeCfgFn = p.node; },
  VariableDeclarator: function (p) { if (tt.isIdentifier(p.node.id) && p.node.id.name === "cfg") cfgDecl = p.node; },
  CallExpression: function (p) {
    var c = p.node.callee;
    if (tt.isMemberExpression(c) && tt.isIdentifier(c.property) && c.property.name === "open") openCall = p;
  }
});

console.log("Q1 makeCfgFn? " + !!makeCfgFn + "  cfgDecl? " + !!cfgDecl + "  openCall? " + !!openCall);
if (cfgDecl && cfgDecl.init) console.log("Q2 _demandResolve(cfg init 'makeCfg(...)') = " + avs(globalThis._DR(cfgDecl.init, null)));
if (openCall) {
  var urlArg = openCall.node.arguments[1]; // cfg.url
  console.log("Q3 _demandResolve(x.open arg1 'cfg.url') = " + avs(globalThis._DR(urlArg, null)));
  console.log("Q3b _demandResolve(receiver 'cfg') = " + avs(globalThis._DR(urlArg.object, null)));
}
if (cfgDecl && cfgDecl.init) {
  var cfgAv = globalThis._DR(cfgDecl.init, null);
  if (cfgAv && cfgAv.kind === "obj-lit" && cfgAv.props) {
    var up = cfgAv.props.url;
    console.log("Q5 cfg.props.url AV = kind=" + (up && up.kind) + " full=" + JSON.stringify(up, function(k,v){ return k==="loc"||k==="start"||k==="end"?undefined:(v&&v.type&&k!==""?("<"+v.type+">"):v); }).slice(0,300));
  }
}
var getterFn = null;
bt(r._ast, {
  CallExpression: function (p) {
    var c = p.node.callee;
    if (tt.isMemberExpression(c) && tt.isIdentifier(c.property) && c.property.name === "defineProperty") {
      var d = p.node.arguments[2];
      if (d && tt.isObjectExpression(d)) for (var i = 0; i < d.properties.length; i++) {
        var pr = d.properties[i];
        if (pr.key && tt.isIdentifier(pr.key) && pr.key.name === "get") getterFn = pr.value;
      }
    }
  }
});
console.log("Q6 getterFn found? " + !!getterFn +
  "  RVG(active)=" + (getterFn ? avs(globalThis._RVG(globalThis._SACK(), getterFn)) : "n/a") +
  "  RVG(standalone)=" + (getterFn ? avs(globalThis._RVG(globalThis._STDCTX(), getterFn)) : "n/a"));
if (getterFn) {
  console.log("Q7 getter FE node AV (_demandResolve) = " + avs(globalThis._DR(getterFn, null)) +
    "  getterFn.type=" + getterFn.type);
  // The descriptor obj-lit's `get` prop AV as the defineProperty handler sees it:
  var dpCall = null;
  bt(r._ast, { CallExpression: function (p) {
    var c = p.node.callee;
    if (tt.isMemberExpression(c) && tt.isIdentifier(c.property) && c.property.name === "defineProperty") dpCall = p;
  }});
  if (dpCall) {
    var descNode = dpCall.node.arguments[2];
    var descAv = globalThis._DR(descNode, null);
    console.log("Q8 descriptor AV = " + avs(descAv) +
      "  .props.get kind = " + (descAv && descAv.kind === "obj-lit" && descAv.props ? avs(descAv.props.get) : "n/a"));
  }
}
if (makeCfgFn) {
  var eff = globalThis._EFF().get(makeCfgFn);
  console.log("Q4 _specEffectsMemo(makeCfg) = " + (eff ? "[" + eff.length + "] " + eff.map(function (e) {
    return "{tgt=" + avs(e.target) + " key=" + (e.key ? (e.key.kind + (e.key.kind === "const" ? ":" + JSON.stringify(e.key.value) : "")) : "?") + " val=" + avs(e.value) + "}"; }).join(" ") : "<none>"));
}
