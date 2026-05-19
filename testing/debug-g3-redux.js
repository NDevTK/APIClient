// G3 = redux applyMiddleware: `while(m=mws[i++]) dispatch=m(dispatch);
// return dispatch` — an ITERATIVE FIXPOINT over function-composition
// (Cousot loop-fixpoint+widening, Bourdoncle WTO). The XHR sink is in
// api's returned fn; `store(action)` must flow action through the
// composed chain. Where does it bottom out? Instrument-first.
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;" +
  "globalThis._DR=function(n,c){try{return _demandResolve(n,c);}catch(e){return 'ERR:'+e.message;}};" +
  "globalThis._PVM=function(){return _specPathValMemo;};" +
  "globalThis._RVG=function(fn){try{return _specReturnValueGet(_specActiveContextKey(),fn);}catch(e){return 'ERR';}};" +
  "globalThis._STD2=function(fn){try{return _specReturnValueGet(_SPEC_STANDALONE_CTX,fn);}catch(e){return 'ERR:'+e.message;}};" +
  "globalThis._CSBF=function(fn){try{return _specCallSitesByFn.get(fn);}catch(e){return 'ERR';}};" +
  "globalThis._FCS=function(fp){try{return _specFindCallSites(fp);}catch(e){return 'ERR:'+e.message;}};" +
  "globalThis._INST=function(av,args,fn){try{return _specInstantiateAv(av,args,undefined,fn);}catch(e){return 'ERR:'+e.message;}};" +
  "globalThis._MK=function(k,v){return {kind:k,value:v};};" +
  "globalThis._FPBN=function(){return _specFuncPathByNode;};" +
  "globalThis._BT=function(){return BabelBundle.traverse;};" +
  "globalThis._TT=function(){return _t;};").call(globalThis);

function avs(a){ if(!a) return "<null>"; if(typeof a!=="object") return String(a);
  if(a.kind==="const") return "const:"+JSON.stringify(a.value);
  if(a.kind==="param") return "param#"+a.idx;
  if(a.kind==="member") return "member("+avs(a.obj)+"."+(a.key&&(a.key.value||a.key.kind))+")";
  if(a.kind==="call") return "call("+avs(a.callee)+")";
  if(a.kind==="function-ref") return "fn-ref";
  if(a.kind==="or"||a.kind==="logical") return a.kind+"("+avs(a.left)+"|"+avs(a.right)+")";
  if(a.kind==="obj-lit") return "obj-lit{"+Object.keys(a.props||{}).join(",")+"}";
  return a.kind; }

var code = [
  'function thunk(next){ return function(action){ if (typeof action === "function") return action(); return next(action); }; }',
  'function api(next){ return function(action){ if (action.type === "CALL") { var x = new XMLHttpRequest(); x.open(action.method, action.url); } return next(action); }; }',
  'function applyMiddleware(mws){',
  '  var dispatch = function(a){ return a; };',
  '  var i = 0, m;',
  '  while (m = mws[i++]) dispatch = m(dispatch);',
  '  return dispatch;',
  '}',
  'var store = applyMiddleware([thunk, api]);',
  'store({ type: "CALL", method: "GET", url: "/api/g3-data" });'
].join("\n");

var r = globalThis.analyzeJSBundle(code, "https://ex.com/app", "https://ex.com", null);
console.log("sites=" + JSON.stringify((r.fetchCallSites || []).map(function (s) { return s.method + " " + s.url; })));

var tt = globalThis._TT(), bt = globalThis._BT(), PVM = globalThis._PVM();
var openCall = null, storeCall = null, amFn = null, dispatchInit = null, applyRet = null;
bt(r._ast, {
  FunctionDeclaration: function (p) { if (p.node.id && p.node.id.name === "applyMiddleware") amFn = p.node; },
  CallExpression: function (p) {
    var c = p.node.callee;
    if (tt.isMemberExpression(c) && tt.isIdentifier(c.property) && c.property.name === "open") openCall = p;
    if (tt.isIdentifier(c) && c.name === "store") storeCall = p;
  },
  ReturnStatement: function (p) {
    if (p.getFunctionParent() && p.getFunctionParent().node === amFn && p.node.argument) applyRet = p.node.argument;
  }
});
console.log("Q1 amFn? " + !!amFn + "  openCall? " + !!openCall + "  storeCall? " + !!storeCall);
if (openCall) {
  console.log("Q2 _demandResolve(x.open arg1 'action.url') = " + avs(globalThis._DR(openCall.node.arguments[1], null)));
  console.log("Q2b _demandResolve(x.open arg0 'action.method') = " + avs(globalThis._DR(openCall.node.arguments[0], null)));
}
if (applyRet) console.log("Q3 _demandResolve(applyMiddleware return 'dispatch') = " + avs(globalThis._DR(applyRet, null)));
if (storeCall) {
  console.log("Q4 _demandResolve('store' = applyMiddleware([thunk,api])) = " + avs(globalThis._DR(storeCall.node.callee, null)));
  console.log("Q5 FORWARD _specPathValMemo(store callee) = " + avs(PVM.get(storeCall.node.callee)));
}
if (applyRet) console.log("Q6 FORWARD _specPathValMemo(applyMiddleware return node) = " + avs(PVM.get(applyRet)) +
  "  RVG(applyMiddleware)=" + avs(globalThis._RVG(amFn)));
// apiInner = api's returned FE; is store({...}) registered as its call site?
var apiInner = null;
bt(r._ast, {
  FunctionDeclaration: function (p) {
    if (p.node.id && p.node.id.name === "api") {
      var b = p.node.body;
      if (b && tt.isBlockStatement(b)) for (var i = 0; i < b.body.length; i++)
        if (tt.isReturnStatement(b.body[i]) && b.body[i].argument &&
            (tt.isFunctionExpression(b.body[i].argument) || tt.isArrowFunctionExpression(b.body[i].argument)))
          apiInner = b.body[i].argument;
    }
  }
});
if (apiInner) {
  var cs = globalThis._CSBF(apiInner);
  console.log("Q7 _specCallSitesByFn(apiInner) = " + (Array.isArray(cs) ? cs.length + " site(s)" : String(cs)));
  var ap = globalThis._FPBN().get(apiInner);
  console.log("Q7b _specFindCallSites(apiInner) = " + (ap ? (function(){var s=globalThis._FCS(ap);return Array.isArray(s)?s.length+" site(s)":s;})() : "no-path"));
}
// Q8: instantiate applyMiddleware's summary with the concrete [thunk,api] array.
var thunkFn = null, apiFn = null;
bt(r._ast, { FunctionDeclaration: function (p) {
  if (p.node.id && p.node.id.name === "thunk") thunkFn = p.node;
  if (p.node.id && p.node.id.name === "api") apiFn = p.node;
}});
var amCallNode = null, storeDeclInit = null;
bt(r._ast, {
  CallExpression: function (p) {
    if (tt.isIdentifier(p.node.callee) && p.node.callee.name === "applyMiddleware") amCallNode = p.node;
  },
  VariableDeclarator: function (p) {
    if (tt.isIdentifier(p.node.id) && p.node.id.name === "store" && p.node.init) storeDeclInit = p.node.init;
  }
});
console.log("Q9 FORWARD _specPathValMemo(applyMiddleware([thunk,api]) call node) = " + avs(PVM.get(amCallNode)));
console.log("Q9b FORWARD _specPathValMemo(store decl init) = " + (storeDeclInit ? avs(PVM.get(storeDeclInit)) : "n/a") +
  "  _demandResolve(store decl init) = " + (storeDeclInit ? avs(globalThis._DR(storeDeclInit, null)) : "n/a"));
if (thunkFn && apiFn) {
  console.log("Q10 _specReturnValueGet(STANDALONE, thunk) = " + avs(globalThis._STD2 ? globalThis._STD2(thunkFn) : globalThis._RVG(thunkFn)) +
    "  (api) = " + avs(globalThis._RVG(apiFn)));
  console.log("Q10b _demandResolve(store callee node) [post-pipeline] = " + avs(globalThis._DR(storeCall.node.callee, null)) +
    "  forward _specPathValMemo = " + avs(globalThis._PVM().get(storeCall.node.callee)));
}
if (amFn && thunkFn && apiFn) {
  var amSummary = globalThis._RVG(amFn);
  var arrArg = { kind: "array-lit", elements: [
    { kind: "function-ref", funcNode: thunkFn }, { kind: "function-ref", funcNode: apiFn } ] };
  var inst = globalThis._INST(amSummary, [arrArg], amFn);
  console.log("Q8 _specInstantiateAv(amSummary, [[thunk,api]]) = " + avs(inst) +
    (inst && inst.kind === "call" ? "  callee=" + avs(inst.callee) : ""));
  // Sub-step: member([thunk,api], opaque) — what does the array-opaque [[Get]] give?
  var memAv = { kind: "member", obj: arrArg, key: { kind: "top" } };
  console.log("Q8b _specInstantiateAv(member([thunk,api], top)) = " + avs(globalThis._INST(memAv, [], null)));
}
