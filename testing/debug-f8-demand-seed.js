// cont.¹⁸ instrument-first: CONFIRM the F8 chicken-and-egg precisely
// before touching _demandSinkSeeds. Questions (answer from the LIVE
// pipeline, do not assume):
//  Q1 how many demand seeds does F8 produce? (cont.¹⁷ said 0)
//  Q2 is `R.prototype.go` (the sink's host fn) in _specSliceFns?
//  Q3 what does the forward engine resolve `new R()` to?
//  Q4 is the `x.open` call's enclosing fn the go-FunctionExpression,
//     and does _isPathInSlice gate it out?
//  Q5 over-approx seed-set size IF we seeded sinks in any fn assigned
//     as `*.prototype.*` / ctor-obj method whose ctor is ever `new`'d.
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;" +
  "globalThis._SF=function(){return _specSliceFns;};" +
  "globalThis._PVM=function(){return _specPathValMemo;};" +
  "globalThis._ENC=function(){return _specEnclFnByNode;};" +
  "globalThis._ISL=function(p){try{return _isPathInSlice(p);}catch(e){return 'ERR:'+e.message;}};" +
  "globalThis._BT=function(){return BabelBundle.traverse;};" +
  "globalThis._FCS=function(fp){try{return _specFindCallSites(fp);}catch(e){return 'ERR:'+e.message;}};" +
  "globalThis._FPBN=function(){return _specFuncPathByNode;};" +
  "globalThis._DCF=function(c,s){try{return _demandCalleeFns(c,s);}catch(e){return 'ERR:'+e.message;}};" +
  "globalThis._DR=function(n,c){try{return _demandResolve(n,c);}catch(e){return 'ERR:'+e.message;}};" +
  "globalThis._NP=function(){return _demandNodePath;};" +
  "globalThis._TT=function(){return _t;};").call(globalThis);

globalThis.__DEMAND_PROBE = 1;
var code = [
  'function R(){ this.opts = {}; }',
  'R.prototype.go = function(u){ var o = {}; o.url = u; var x = new XMLHttpRequest(); x.open("GET", o.url); };',
  'var inst = new R();',
  'inst.go("/api/F8");'
].join("\n");

var r = globalThis.analyzeJSBundle(code, "https://ex.com/app", "https://ex.com", null);
console.log("sites=" + JSON.stringify((r.fetchCallSites || []).map(function (s) { return s.method + " " + s.url; })));

var SF = globalThis._SF(), PVM = globalThis._PVM(), tt = globalThis._TT(), bt = globalThis._BT();
var goFn = null, newR = null, openCall = null, instGoCall = null, sliceCount = 0;
bt(r._ast, {
  Function: function (p) { if (SF.has(p.node)) sliceCount++; },
  AssignmentExpression: function (p) {
    var L = p.node.left;
    if (tt.isMemberExpression(L) && tt.isMemberExpression(L.object) &&
        tt.isIdentifier(L.object.property) && L.object.property.name === "prototype" &&
        tt.isIdentifier(L.property) && L.property.name === "go") goFn = p.node.right;
  },
  NewExpression: function (p) { if (tt.isIdentifier(p.node.callee) && p.node.callee.name === "R") newR = p; },
  CallExpression: function (p) {
    var c = p.node.callee;
    if (tt.isMemberExpression(c) && tt.isIdentifier(c.property) && c.property.name === "open") openCall = p;
    if (tt.isMemberExpression(c) && tt.isIdentifier(c.property) && c.property.name === "go") instGoCall = p;
  }
});

console.log("Q2 goFn in _specSliceFns? " + (goFn ? SF.has(goFn) : "goFn-not-found") +
            "  (total slice fns=" + sliceCount + ")");
console.log("Q3 new R() forward AV = " + (newR ? (function () {
  var a = PVM.get(newR.node.callee) || PVM.get(newR.node); return a ? a.kind : "<no-memo>"; })() : "newR-not-found"));
console.log("Q4 x.open enclosing-fn===goFn? " +
            (openCall && goFn ? (globalThis._ENC().get(openCall.node) === goFn) : "n/a") +
            "  _isPathInSlice(x.open)=" + (openCall ? globalThis._ISL(openCall) : "n/a"));
console.log("Q4b inst.go(...) _isPathInSlice=" + (instGoCall ? globalThis._ISL(instGoCall) : "n/a") +
            "  inst.go callee AV=" + (instGoCall ? ((PVM.get(instGoCall.node.callee) || {}).kind || "<no-memo>") : "n/a"));

// Q5 — cheap syntactic over-approx: count fns that ARE a `X.prototype.M=fn`
// member or a ctor-referenced obj-lit method, AND whose ctor X is `new`'d
// somewhere, AND that syntactically contain a network/DOM sink call.
var protoFns = new Set(), ctorsNewd = new Set();
bt(r._ast, {
  NewExpression: function (p) { if (tt.isIdentifier(p.node.callee)) ctorsNewd.add(p.node.callee.name); },
  AssignmentExpression: function (p) {
    var L = p.node.left, R2 = p.node.right;
    if (tt.isMemberExpression(L) && tt.isMemberExpression(L.object) &&
        tt.isIdentifier(L.object.property) && L.object.property.name === "prototype" &&
        tt.isIdentifier(L.object.object) &&
        (tt.isFunctionExpression(R2) || tt.isArrowFunctionExpression(R2)))
      protoFns.add(JSON.stringify([L.object.object.name, R2.start]));
  }
});
console.log("Q5 ctors new'd=" + JSON.stringify([].slice.call(ctorsNewd)) +
            "  proto-method fns=" + protoFns.size +
            "  (these are the cheap over-approx demand-seed hosts)");

// Q6 — where does the BACKWARD query bottom out? Drill the precise
// failing edge: x.open url-arg → o.url → u → param(0,goFE) → callers.
function avs(a){ return a ? (a.kind + (a.kind==="const"?(":"+JSON.stringify(a.value)):a.kind==="param"?("#"+a.idx):a.kind==="member"?("."+(a.key&&a.key.value)):"")) : "<null>"; }
var FPBN = globalThis._FPBN(), NP = globalThis._NP();
var openUrlArg = openCall ? openCall.node.arguments[1] : null;   // o.url
console.log("Q6 _demandResolve(x.open arg 'o.url') = " +
            (openUrlArg ? avs(globalThis._DR(openUrlArg, null)) : "n/a"));
if (goFn) {
  var goPath = FPBN.get(goFn);
  console.log("Q6b goFE path in _specFuncPathByNode? " + !!goPath);
  if (goPath) {
    var cs = globalThis._FCS(goPath);
    console.log("Q6c _specFindCallSites(goFE) = " +
                (Array.isArray(cs) ? cs.length + " site(s): " +
                  cs.map(function(s){ return s && s.node ? globalThis.BabelBundle.types.isMemberExpression(s.node.callee) ? (s.node.callee.property&&s.node.callee.property.name)+"(...)" : "call" : "?"; }).join(",") : cs));
  }
}
if (instGoCall) {
  var igCallee = instGoCall.node.callee;          // inst.go
  var igP = NP.get(igCallee) || NP.get(instGoCall.node);
  var igScope = igP && igP.scope;
  console.log("Q6d _demandCalleeFns(inst.go) = " +
              (igScope ? (function(){ var f=globalThis._DCF(igCallee, igScope); return Array.isArray(f) ? f.length+" fn(s)"+(f.length&&f[0]===goFn?" [===goFE ✓]":f.length?" [≠goFE]":"") : f; })() : "no-scope"));
  console.log("Q6e _demandResolve(inst) = " + avs(globalThis._DR(instGoCall.node.callee.object, null)));
}
