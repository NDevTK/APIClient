// cont.²² Fchain: chained `return this` prototype setters mutating
// nested `this.opts.<k>`, terminal method does XHR `.open(this.opts.*)`.
// Where does it bottom out (post cont.²¹ edge + this-instance fix +
// fetch/XHR backward fallback)? Instrument-first, no assumptions.
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;" +
  "globalThis._DR=function(n,c){try{return _demandResolve(n,c);}catch(e){return 'ERR:'+e.message;}};" +
  "globalThis._FCS=function(fp){try{return _specFindCallSites(fp);}catch(e){return 'ERR:'+e.message;}};" +
  "globalThis._FPBN=function(){return _specFuncPathByNode;};" +
  "globalThis._BTIA=function(fn,fp){try{return _specBuildThisInstanceAv(fn,fp);}catch(e){return 'ERR:'+e.message;}};" +
  "globalThis._CRA=function(fn){try{return _specContextRecvAv.get(fn);}catch(e){return 'ERR:'+e.message;}};" +
  "globalThis._DCF2=function(c,s){try{return _demandCalleeFns(c,s);}catch(e){return 'ERR';}};" +
  "globalThis._NP2=function(){return _demandNodePath;};" +
  "globalThis._SF2=function(){return _specSliceFns;};" +
  "globalThis._ISL2=function(p){try{return _isPathInSlice(p);}catch(e){return 'ERR:'+e.message;}};" +
  "globalThis._ENC2=function(){return _specEnclFnByNode;};" +
  "globalThis._RAV2=function(node){try{var p=_demandNodePath.get(node);return p?_resolveAllValues(p,0):'no-path';}catch(e){return 'ERR:'+e.message;}};" +
  "globalThis._PVM3=function(){return _specPathValMemo;};" +
  "globalThis._THISEFF=function(fn){try{return _specThisEffectsMemo.get(fn);}catch(e){return 'ERR:'+e.message;}};" +
  "globalThis._EFF=function(fn){try{return _specEffectsMemo.get(fn);}catch(e){return 'ERR:'+e.message;}};" +
  "globalThis._SF=function(){return _specSliceFns;};" +
  "globalThis._FCS2=function(fp){try{return _specFindCallSites(fp);}catch(e){return 'ERR';}};" +
  "globalThis._BT=function(){return BabelBundle.traverse;};" +
  "globalThis._TT=function(){return _t;};").call(globalThis);

function avs(a){ if(!a) return "<null>"; if(typeof a!=="object") return String(a);
  if(a.kind==="const") return "const:"+JSON.stringify(a.value);
  if(a.kind==="param") return "param#"+a.idx;
  if(a.kind==="member") return "member("+avs(a.obj)+"."+(a.key&&(a.key.value||a.key.kind))+")";
  if(a.kind==="obj-lit") return "obj-lit{"+Object.keys(a.props||{}).join(",")+"}";
  return a.kind; }

var code = [
  'function Request(){ this.opts = {}; }',
  'Request.prototype.method = function(m){ this.opts.method = m; return this; };',
  'Request.prototype.url = function(u){ this.opts.url = u; return this; };',
  'Request.prototype.end = function(){ var x = new XMLHttpRequest(); x.open(this.opts.method, this.opts.url); return x; };',
  'new Request().method("POST").url("/api/Fchain").end();'
].join("\n");

var r = globalThis.analyzeJSBundle(code, "https://ex.com/app", "https://ex.com", null);
console.log("sites=" + JSON.stringify((r.fetchCallSites || []).map(function (s) { return s.method + " " + s.url; })));

var tt = globalThis._TT(), bt = globalThis._BT(), FPBN = globalThis._FPBN();
var endFE = null, openCall = null, ctorReq = null;
bt(r._ast, {
  AssignmentExpression: function (p) {
    var L = p.node.left;
    if (tt.isMemberExpression(L) && tt.isMemberExpression(L.object) &&
        tt.isIdentifier(L.object.property) && L.object.property.name === "prototype" &&
        tt.isIdentifier(L.property) && L.property.name === "end") endFE = p.node.right;
  },
  FunctionDeclaration: function (p) { if (p.node.id && p.node.id.name === "Request") ctorReq = p.node; },
  CallExpression: function (p) {
    var c = p.node.callee;
    if (tt.isMemberExpression(c) && tt.isIdentifier(c.property) && c.property.name === "open") openCall = p;
  }
});

console.log("Q1 endFE? " + !!endFE + "  ctorReq? " + !!ctorReq + "  openCall? " + !!openCall);
if (endFE) {
  var SF2 = globalThis._SF2();
  console.log("Q15 _specSliceFns.has(endFE)=" + SF2.has(endFE) +
    "  openCall path in slice=" + (openCall ? globalThis._ISL2(openCall) : "n/a") +
    "  _specEnclFnByNode.get(openCall.node)===endFE? " + (openCall ? (globalThis._ENC2().get(openCall.node) === endFE) : "n/a"));
  if (openCall) {
    console.log("Q16 FORWARD _resolveAllValues(x.open arg0)=" + JSON.stringify(globalThis._RAV2(openCall.node.arguments[0])) +
      " arg1=" + JSON.stringify(globalThis._RAV2(openCall.node.arguments[1])) +
      "  forward _specPathValMemo(this in x.open arg0)=" + (function(){var t=openCall.node.arguments[0]; while(t&&t.object)t=t.object; var a=globalThis._PVM3().get(t); return a?a.kind:"<none>";})());
  }
  console.log("Q14 _specContextRecvAv.get(endFE) = " + deepOpts(globalThis._CRA(endFE)));
  // What does _demandResolve(<chain>.end) give (edge-completion's ecFns source)?
  var endCallE = null;
  bt(r._ast, { CallExpression: function (p) {
    var c = p.node.callee;
    if (tt.isMemberExpression(c) && tt.isIdentifier(c.property) && c.property.name === "end") endCallE = p.node;
  }});
  if (endCallE) {
    var cAv = globalThis._DR(endCallE.callee, null);
    console.log("Q14b _demandResolve(<chain>.end callee) = " + (cAv ? (cAv.kind + (cAv.kind === "function-ref" ? "(funcNode==endFE? " + (cAv.funcNode === endFE) + ")" : "")) : "<null>"));
    var ep = globalThis._NP2().get(endCallE.callee);
    var dcf = ep && ep.scope ? globalThis._DCF2(endCallE.callee, ep.scope) : "no-scope";
    console.log("Q14c _demandCalleeFns(<chain>.end) = " + (Array.isArray(dcf) ? dcf.length + " fn(s)" + (dcf.indexOf(endFE) >= 0 ? " [incl endFE]" : "") : String(dcf)));
  }
}
if (endFE) {
  var ep = FPBN.get(endFE);
  console.log("Q2 _specFindCallSites(endFE) = " + (ep ? (function(){var s=globalThis._FCS(ep);return Array.isArray(s)?s.length+" site(s)":s;})() : "no-path"));
  if (ep) {
    var tia = globalThis._BTIA(endFE, ep);
    console.log("Q3 _specBuildThisInstanceAv(end) = " + (tia&&typeof tia==="object"?(tia.kind+(tia.kind==="obj-lit"?" props={"+Object.keys(tia.props||{}).map(function(k){return k+":"+avs(tia.props[k]);}).join(",")+"}":"")):tia));
  }
}
if (openCall) {
  console.log("Q4 _demandResolve(x.open arg0 'this.opts.method') = " + avs(globalThis._DR(openCall.node.arguments[0], null)));
  console.log("Q5 _demandResolve(x.open arg1 'this.opts.url') = " + avs(globalThis._DR(openCall.node.arguments[1], null)));
  console.log("Q6 _demandResolve('this' in end) = " + avs(globalThis._DR(openCall.node.arguments[1].object.object, null)));
}
// The receiver chain `new Request().method("POST").url("/api/Fchain")`
// is `.end`'s callee.object — what does it evaluate to?
var endCallChain = null, methodFE = null, urlFE = null;
bt(r._ast, {
  CallExpression: function (p) {
    var c = p.node.callee;
    if (tt.isMemberExpression(c) && tt.isIdentifier(c.property) && c.property.name === "end")
      endCallChain = c.object;
  },
  AssignmentExpression: function (p) {
    var L = p.node.left;
    if (tt.isMemberExpression(L) && tt.isMemberExpression(L.object) &&
        tt.isIdentifier(L.object.property) && L.object.property.name === "prototype" &&
        tt.isIdentifier(L.property)) {
      if (L.property.name === "method") methodFE = p.node.right;
      if (L.property.name === "url") urlFE = p.node.right;
    }
  }
});
if (endCallChain) console.log("Q7 _demandResolve(receiver chain '…method(\"POST\").url(\"/api/Fchain\")') = " + avs(globalThis._DR(endCallChain, null)));
// Q12: resolve each chain hop bottom-up.
var newReq = null, methodCall = null, urlCall = null;
bt(r._ast, {
  NewExpression: function (p) { if (tt.isIdentifier(p.node.callee) && p.node.callee.name === "Request") newReq = p.node; },
  CallExpression: function (p) {
    var c = p.node.callee;
    if (tt.isMemberExpression(c) && tt.isIdentifier(c.property)) {
      if (c.property.name === "method") methodCall = p.node;
      if (c.property.name === "url") urlCall = p.node;
    }
  }
});
function deepOpts(a){ if(!a||a.kind!=="obj-lit"||!a.props) return avs(a); var o=a.props.opts;
  return "{opts="+(o&&o.kind==="obj-lit"?("{"+Object.keys(o.props||{}).map(function(k){return k+":"+avs(o.props[k]);}).join(",")+"}"):avs(o))+", "+Object.keys(a.props).filter(function(k){return k!=="opts";}).join(",")+"}"; }
// Q13: .end's call site + its receiver (the chain) resolution.
var endCall2 = null;
bt(r._ast, { CallExpression: function (p) {
  var c = p.node.callee;
  if (tt.isMemberExpression(c) && tt.isIdentifier(c.property) && c.property.name === "end") endCall2 = p.node;
}});
if (endCall2 && endFE) {
  var cs = globalThis._CSBF ? globalThis._CSBF(endFE) : "n/a";
  console.log("Q13 _specCallSitesByFn(endFE)=" + (Array.isArray(cs)?cs.length+" site(s)":String(cs)) +
    "  endCall.callee.object kind=" + endCall2.callee.object.type +
    "  _demandResolve(.end recv chain)=" + deepOpts(globalThis._DR(endCall2.callee.object, null)));
}
console.log("Q12 _demandResolve(new Request()) = " + (newReq ? deepOpts(globalThis._DR(newReq, null)) : "n/a"));
console.log("Q12b _demandResolve(new Request().method(\"POST\")) = " + (methodCall ? deepOpts(globalThis._DR(methodCall, null)) : "n/a"));
console.log("Q12c _demandResolve(....url(\"/api/Fchain\")) = " + (urlCall ? deepOpts(globalThis._DR(urlCall, null)) : "n/a"));
function effs(fn){ var e = fn ? globalThis._THISEFF(fn) : null;
  return e && e.length ? "[" + e.length + "] " + e.map(function(x){
    return "{tgt=" + avs(x.target) + " key=" + (x.key?(x.key.kind+(x.key.kind==="const"?":"+JSON.stringify(x.key.value):"")):"?") + " val=" + avs(x.value) + "}"; }).join(" ") : (e ? "[0]" : "<none>"); }
console.log("Q8 _specThisEffectsMemo(method) = " + effs(methodFE));
console.log("Q8b _specThisEffectsMemo(url) = " + effs(urlFE));
// Q11: is the setter-effect target obj-lit the SAME identity as the
// method/end instance-shape `opts` prop, and as the receiver's opts?
var FPBN3 = globalThis._FPBN();
function instOpts(fe){ if(!fe) return null; var fp=FPBN3.get(fe); if(!fp) return "<no-path>";
  var tia=globalThis._BTIA(fe,fp); return (tia&&tia.props)?tia.props.opts:null; }
var mOpts = instOpts(methodFE), eOpts = instOpts(endFE);
var rawM = methodFE ? globalThis._EFF(methodFE) : null;
var mEffTgt = (Array.isArray(rawM)&&rawM[0])?rawM[0].target:null;
console.log("Q11 method-inst-shape.opts===end-inst-shape.opts? " + (mOpts===eOpts) +
  "  method-eff-target===method-inst-shape.opts? " + (mEffTgt===mOpts) +
  "  (mOpts kind=" + (mOpts&&mOpts.kind) + " eOpts kind=" + (eOpts&&eOpts.kind) + " effTgt kind=" + (mEffTgt&&mEffTgt.kind) + ")");
function raweff(fn){ var e = fn ? globalThis._EFF(fn) : null;
  return Array.isArray(e) ? "["+e.length+"] "+e.map(function(x){ return "{tgt="+avs(x.target)+" key="+(x.key?(x.key.kind+(x.key.kind==="const"?":"+JSON.stringify(x.key.value):"")):"?")+" val="+avs(x.value)+"}"; }).join(" ") : String(e); }
var SF = globalThis._SF(), FPBN2 = globalThis._FPBN();
console.log("Q9 methodFE in slice? " + (methodFE ? SF.has(methodFE) : "n/a") +
  "  _specFindCallSites(method)=" + (methodFE && FPBN2.get(methodFE) ? (function(){var s=globalThis._FCS2(FPBN2.get(methodFE));return Array.isArray(s)?s.length:s;})() : "no-path") +
  "  raw _specEffectsMemo(method)=" + raweff(methodFE));
console.log("Q9b urlFE in slice? " + (urlFE ? SF.has(urlFE) : "n/a") +
  "  raw _specEffectsMemo(url)=" + raweff(urlFE));
