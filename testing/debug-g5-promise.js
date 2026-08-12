// G5 = §27.2 promise-then-chain thenable value-flow. The XHR sink is
// inside get(url); get is called from a .then callback whose param is
// the chain's resolved value. Where does it bottom out? Instrument-
// first, no assumptions. Promise is a builtin (§27.2) — value-flow, not
// framework special-casing.
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;" +
  "globalThis._DR=function(n,c){try{return _demandResolve(n,c);}catch(e){return 'ERR:'+e.message;}};" +
  "globalThis._RAV=function(p){try{return _resolveAllValues(p,0);}catch(e){return 'ERR:'+e.message;}};" +
  "globalThis._FCS=function(fp){try{return _specFindCallSites(fp);}catch(e){return 'ERR:'+e.message;}};" +
  "globalThis._FPBN=function(){return _specFuncPathByNode;};" +
  "globalThis._BT=function(){return BabelBundle.traverse;};" +
  "globalThis._PVM=function(){return _specPathValMemo;};" +
  "globalThis._CES=function(){return typeof _specCtxEffectsBySite!=='undefined'?_specCtxEffectsBySite:null;};" +
  "globalThis._RVGK=function(fn){try{return _specReturnValueGet(_specActiveContextKey(),fn);}catch(e){return 'ERR:'+e.message;}};" +
  "globalThis._STD2=function(fn){try{return _specReturnValueGet(_SPEC_STANDALONE_CTX,fn);}catch(e){return 'ERR:'+e.message;}};" +
  "globalThis._TT=function(){return _t;};").call(globalThis);

function avs(a){ if(!a) return "<null>"; if(typeof a!=="object") return String(a);
  if(a.kind==="const") return "const:"+JSON.stringify(a.value);
  if(a.kind==="param") return "param#"+a.idx;
  if(a.kind==="binop") return "binop("+avs(a.left)+a.op+avs(a.right)+")";
  if(a.kind==="member") return "member("+avs(a.obj)+"."+(a.key&&(a.key.value||a.key.kind))+")";
  if(a.kind==="call") return "call("+avs(a.callee)+")";
  if(a.kind==="or"||a.kind==="logical") return a.kind+"("+avs(a.left)+"|"+avs(a.right)+")";
  return a.kind; }

var code = [
  'function get(url){ var x = new XMLHttpRequest(); x.open("GET", url); return Promise.resolve(x); }',
  'function withBase(base){ return function(p){ return base + p; }; }',
  'Promise.resolve("/api").then(withBase("/api")).then(function(full){ return get(full + "/g5-data"); });'
].join("\n");

var r = globalThis.analyzeJSBundle(code, "https://ex.com/app", "https://ex.com", null);
console.log("sites=" + JSON.stringify((r.fetchCallSites || []).map(function (s) { return s.method + " " + s.url; })));

var tt = globalThis._TT(), bt = globalThis._BT(), FPBN = globalThis._FPBN();
var getFn = null, openCall = null, getCall = null, thenCbFull = null;
bt(r._ast, {
  FunctionDeclaration: function (p) { if (p.node.id && p.node.id.name === "get") getFn = p.node; },
  CallExpression: function (p) {
    var c = p.node.callee;
    if (tt.isMemberExpression(c) && tt.isIdentifier(c.property) && c.property.name === "open") openCall = p;
    if (tt.isIdentifier(c) && c.name === "get") getCall = p;
  },
  FunctionExpression: function (p) {
    if (p.node.params.length === 1 && tt.isIdentifier(p.node.params[0]) && p.node.params[0].name === "full") thenCbFull = p.node;
  }
});

console.log("Q1 getFn? " + !!getFn + "  openCall? " + !!openCall + "  getCall? " + !!getCall + "  thenCbFull? " + !!thenCbFull);
if (openCall) {
  console.log("Q2 _demandResolve(x.open arg1 'url') = " + avs(globalThis._DR(openCall.node.arguments[1], null)));
  console.log("Q2b _resolveAllValues(x.open arg1) = " + JSON.stringify(globalThis._RAV(FPBN.get(getFn) ? null : null) || []));
}
if (getFn) {
  var gp = FPBN.get(getFn);
  console.log("Q3 _specFindCallSites(getFn) = " + (gp ? (function(){var s=globalThis._FCS(gp);return Array.isArray(s)?s.length+" site(s)":s;})() : "no-path"));
}
if (getCall) {
  console.log("Q4 _demandResolve(get-call arg 'full + \"/g5-data\"') = " + avs(globalThis._DR(getCall.node.arguments[0], null)));
  console.log("Q4b _demandResolve('full' the .then cb param) = " + avs(globalThis._DR(getCall.node.arguments[0].left, null)));
}
// Q5: the first .then's callback arg is `withBase("/api")` — what AV?
var firstThenCb = null, withBaseInner = null;
bt(r._ast, {
  CallExpression: function (p) {
    var c = p.node.callee;
    if (tt.isMemberExpression(c) && tt.isIdentifier(c.property) && c.property.name === "then" &&
        p.node.arguments[0] && tt.isCallExpression(p.node.arguments[0]) &&
        tt.isIdentifier(p.node.arguments[0].callee) && p.node.arguments[0].callee.name === "withBase")
      firstThenCb = p.node.arguments[0];
  },
  ReturnStatement: function (p) {
    if (p.node.argument && (tt.isFunctionExpression(p.node.argument) || tt.isArrowFunctionExpression(p.node.argument)) &&
        p.node.argument.params.length === 1 && tt.isIdentifier(p.node.argument.params[0]) &&
        p.node.argument.params[0].name === "p") withBaseInner = p.node.argument;
  }
});
if (firstThenCb) {
  var cbAv = globalThis._DR(firstThenCb, null);
  console.log("Q5 withBase(\"/api\") AV = " + avs(cbAv) + " kind=" + (cbAv && cbAv.kind) +
    " funcNode=" + (cbAv && cbAv.funcNode ? "yes" : "no") +
    " _producingCall=" + (cbAv && cbAv._producingCall ? "yes" : "no"));
  // Does the forward .then dispatch fire? recv = Promise.resolve("/api")
  var firstThenParent = null;
  bt(r._ast, { CallExpression: function (p) {
    if (p.node.arguments[0] === firstThenCb && tt.isMemberExpression(p.node.callee) &&
        tt.isIdentifier(p.node.callee.property) && p.node.callee.property.name === "then") firstThenParent = p;
  }});
  if (firstThenParent) {
    var recvAv = globalThis._PVM().get(firstThenParent.node.callee.object);
    console.log("Q7 forward recv 'Promise.resolve(\"/api\")' in _specPathValMemo = " +
      (recvAv ? recvAv.kind + (recvAv.kind === "promise-instance" ? " resolvedValue=" + avs(recvAv.resolvedValue) : "") : "<no-memo>"));
    var ces = globalThis._CES();
    console.log("Q8 _specCtxEffectsBySite has entry for thenCbAv._producingCall? " +
      (ces && cbAv && cbAv._producingCall ? (ces.get(cbAv._producingCall) ? "YES fn=" + (ces.get(cbAv._producingCall).fn && (ces.get(cbAv._producingCall).fn.id ? ces.get(cbAv._producingCall).fn.id.name : "anon")) + " argAvs=" + JSON.stringify((ces.get(cbAv._producingCall).argAvs||[]).map(avs)) : "NO-ENTRY") : "n/a"));
    var firstThenAv = globalThis._PVM().get(firstThenParent.node);
    console.log("Q9 forward 1st `.then(withBase(...))` call AV (_specPathValMemo) = " +
      (firstThenAv ? firstThenAv.kind + (firstThenAv.kind === "promise-instance" ? " resolvedValue=" + avs(firstThenAv.resolvedValue) : "") : "<no-memo>"));
    console.log("Q9b forward n.arguments[0] 'withBase(\"/api\")' AV (_specPathValMemo) = " +
      (function(){ var a = globalThis._PVM().get(firstThenCb); return a ? a.kind + (a.funcNode ? " funcNode=yes" : "") : "<no-memo>"; })());
  }
}
if (withBaseInner) {
  var tcr = globalThis._RVGK(withBaseInner);
  var tcrS = globalThis._STD2(withBaseInner);
  function leafKinds(av){ if(!av||typeof av!=="object") return String(av);
    if(av.kind==="binop") return "binop("+leafKinds(av.left)+av.op+leafKinds(av.right)+")";
    if(av.kind==="param") return "param#"+av.idx+(av.fn?("@"+(av.fn.id?av.fn.id.name:"anon")):"@<no-fn>");
    if(av.kind==="const") return "const:"+JSON.stringify(av.value);
    return av.kind; }
  console.log("Q10 innerFE return summary (activeCtx) = " + leafKinds(tcr));
  console.log("Q10b innerFE return summary (STANDALONE) = " + leafKinds(tcrS));
  var brn = withBaseInner.body;
  if (brn && tt.isBlockStatement(brn)) for (var bi = 0; bi < brn.body.length; bi++) {
    if (tt.isReturnStatement(brn.body[bi]) && brn.body[bi].argument) {
      console.log("Q6 innerFE return `base+p` _demandResolve = " + avs(globalThis._DR(brn.body[bi].argument, null)));
      if (tt.isBinaryExpression(brn.body[bi].argument))
        console.log("Q6b 'base' (closure) _demandResolve = " + avs(globalThis._DR(brn.body[bi].argument.left, null)));
    }
  }
}
