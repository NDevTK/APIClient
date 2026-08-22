// cont.²² instrument-first: with cont.²¹ making the prototype method
// REACHED (call-graph edge registered), where does the backward query
// for `this.<prop>` inside the method bottom out? Fthis = ctor stores
// param on `this`, proto method reads it. No assumptions.
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;" +
  "globalThis._DR=function(n,c){try{return _demandResolve(n,c);}catch(e){return 'ERR:'+e.message;}};" +
  "globalThis._FCS=function(fp){try{return _specFindCallSites(fp);}catch(e){return 'ERR:'+e.message;}};" +
  "globalThis._FPBN=function(){return _specFuncPathByNode;};" +
  "globalThis._BT=function(){return BabelBundle.traverse;};" +
  "globalThis._SF=function(){return _specSliceFns;};" +
  "globalThis._CRTI=function(fn,fp){try{return _specCanResolveThisInstance(fn,fp);}catch(e){return 'ERR:'+e.message;}};" +
  "globalThis._BTIA=function(fn,fp){try{return _specBuildThisInstanceAv(fn,fp);}catch(e){return 'ERR:'+e.message;}};" +
  "globalThis._TT=function(){return _t;};").call(globalThis);

globalThis.__DEMAND_PROBE = 1;
function avs(a){ return a ? (a.kind + (a.kind==="const"?(":"+JSON.stringify(a.value)):a.kind==="param"?("#"+a.idx):a.kind==="member"?("."+(a.key&&(a.key.value||a.key.kind))):a.kind==="this"?"":"")) : "<null>"; }

var code = [
  'function C(u){ this.endpoint = u; }',
  'C.prototype.fire = function(){ fetch(this.endpoint); };',
  'new C("/api/Fthis").fire();'
].join("\n");

var r = globalThis.analyzeJSBundle(code, "https://ex.com/app", "https://ex.com", null);
console.log("sites=" + JSON.stringify((r.fetchCallSites || []).map(function (s) { return s.method + " " + s.url; })));

var tt = globalThis._TT(), bt = globalThis._BT(), FPBN = globalThis._FPBN();
var fireFE = null, ctorC = null, fetchArg = null, ctorThisAssign = null;
bt(r._ast, {
  AssignmentExpression: function (p) {
    var L = p.node.left;
    if (tt.isMemberExpression(L) && tt.isMemberExpression(L.object) &&
        tt.isIdentifier(L.object.property) && L.object.property.name === "prototype" &&
        tt.isIdentifier(L.property) && L.property.name === "fire") fireFE = p.node.right;
    if (tt.isMemberExpression(L) && tt.isThisExpression(L.object) &&
        tt.isIdentifier(L.property) && L.property.name === "endpoint") ctorThisAssign = p.node;
  },
  FunctionDeclaration: function (p) { if (p.node.id && p.node.id.name === "C") ctorC = p.node; },
  CallExpression: function (p) {
    if (tt.isIdentifier(p.node.callee) && p.node.callee.name === "fetch") fetchArg = p.node.arguments[0];
  }
});

console.log("Q1 fireFE found? " + !!fireFE + "  ctorC found? " + !!ctorC);
if (fireFE) {
  var fp = FPBN.get(fireFE);
  console.log("Q2 _specFindCallSites(fireFE) = " +
    (fp ? (function(){ var s = globalThis._FCS(fp); return Array.isArray(s) ? s.length + " site(s)" : s; })() : "no-path"));
}
console.log("Q3 _demandResolve(fetch arg 'this.endpoint') = " +
  (fetchArg ? avs(globalThis._DR(fetchArg, null)) : "n/a"));
if (fetchArg && tt.isMemberExpression(fetchArg)) {
  console.log("Q3b _demandResolve(receiver 'this') = " + avs(globalThis._DR(fetchArg.object, null)));
}
console.log("Q4 ctor `this.endpoint=u` assign found? " + !!ctorThisAssign +
  "  RHS _demandResolve = " + (ctorThisAssign ? avs(globalThis._DR(ctorThisAssign.right, null)) : "n/a"));
if (fireFE) {
  var fp2 = globalThis._FPBN().get(fireFE);
  console.log("Q5 fireFE in _specSliceFns? " + globalThis._SF().has(fireFE) +
    "  _specCanResolveThisInstance(fire)=" + (fp2 ? globalThis._CRTI(fireFE, fp2) : "no-path"));
  if (fp2) {
    var tia = globalThis._BTIA(fireFE, fp2);
    console.log("Q6 _specBuildThisInstanceAv(fire) = " +
      (tia && typeof tia === "object" ? (tia.kind + (tia.kind === "obj-lit" ?
        " props={" + Object.keys(tia.props || {}).map(function (k) { return k + ":" + avs(tia.props[k]); }).join(",") + "}" : "")) : tia));
  }
}
