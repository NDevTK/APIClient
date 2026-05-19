// STEP-5+ instrument (durable). Real 87KB jQuery `$.get` → `xhr.open`
// bottoms out at `_demandCalleeFns(i.xhr)=0` (cont.³⁹/DIAG). Walk the
// transport-dispatch chain hop-by-hop with `_demandResolve` to pin the
// FIRST silent bottom-out:
//   xhr.open recv = xhr  ← `xhr = i.xhr()`  [i = transport-fn fn1's param 0]
//     ← fn1 call site = `t(i,o,a)` inside Vt's `ce.each(t[e]||[],cb)` cb
//       [cb=function(e,t){…}; `t` = cb's param idx1 = the array element]
//       ← arr = t[e]  (t=Vt param0=_t, e="*")  ← `(o[n]=o[n]||[]).push(fn1)`
//         inside Ut(o)'s returned registrar  ← ce.ajaxTransport(fn1)
//         [ajaxTransport = Ut(_t); dataType defaults "*"]
var fs = require("fs"), path = require("path");
var rd = path.join(__dirname, "..");
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;" +
  "globalThis._DR=function(n){try{return _demandResolve(n,null);}catch(e){return {kind:'ERR',m:e.message};}};" +
  "globalThis._DRC=function(n,c){try{return _demandResolve(n,c);}catch(e){return {kind:'ERR',m:e.message};}};" +
  "globalThis._DCF=function(n,sc){try{return _demandCalleeFns(n,sc);}catch(e){return 'ERR:'+e.message;}};" +
  "globalThis._CSBF=function(){return _specCallSitesByFn;};" +
  "globalThis._FCS=function(fp){try{return _specFindCallSites(fp);}catch(e){return 'ERR:'+e.message;}};" +
  "globalThis._SLICE=function(){return _specSliceFns;};" +
  "globalThis._PVM=function(){return _specPathValMemo;};" +
  "globalThis._FPBN=function(){return _specFuncPathByNode;};" +
  "globalThis._DNP=function(){return _demandNodePath;};" +
  "globalThis._CONE=function(n){try{return !!(_specDemandConeFns&&_specDemandConeFns.has(n));}catch(e){return 'ERR';}};" +
  "globalThis._CONEON=function(){return typeof _specDemandConeFns!=='undefined'&&!!_specDemandConeFns;};" +
  "globalThis._t=_t;").call(globalThis);

function avs(a, d) {
  d = d || 0; if (d > 6) return "…";
  if (!a) return "<null>";
  if (typeof a !== "object") return String(a);
  if (a.kind === "const") return "const:" + JSON.stringify(a.value);
  if (a.kind === "param") return "param#" + a.idx + "@fn" + (a.fn && a.fn.start);
  if (a.kind === "member") return "member(" + avs(a.obj, d + 1) + "." +
    (a.key && (a.key.value != null ? JSON.stringify(a.key.value) : a.key.kind)) + ")";
  if (a.kind === "call") return "call(" + avs(a.callee, d + 1) + ")";
  if (a.kind === "function-ref") return "fn-ref#" + (a.funcNode && a.funcNode.start);
  if (a.kind === "or" || a.kind === "logical") return a.kind + "(" + avs(a.left, d + 1) + "|" + avs(a.right, d + 1) + ")";
  if (a.kind === "obj-lit") return "obj-lit{" + Object.keys(a.props || {}).slice(0, 10).join(",") + "}";
  if (a.kind === "array-lit") return "array-lit[" + (a.elements || []).length + "]";
  if (a.kind === "ERR") return "ERR:" + a.m;
  return a.kind;
}
function fnsList(g) {
  if (!Array.isArray(g)) return String(g);
  return g.length + "fn(s)" + (g.length ? "@" + g.map(function (f) { return f && f.start; }).join(",") : "");
}
function sitesLen(s) { return Array.isArray(s) ? s.length + " site(s)" : String(s); }

var jq = fs.readFileSync(path.join(rd, "testing/harness-dumps/jquery-3.7.1.min.js"), "utf8");
var t0 = Date.now();
var r = globalThis.analyzeJSBundle(jq + '\njQuery.get("/api/profile");', "https://ex.com/app", "https://ex.com", null);
console.log("(" + (Date.now() - t0) + "ms) sites=" + JSON.stringify((r.fetchCallSites || []).map(function (s) { return s.method + " " + s.url; })));

var tt = globalThis._t, bt = globalThis.BabelBundle.traverse;
var CSBF = globalThis._CSBF(), SLICE = globalThis._SLICE(), PVM = globalThis._PVM(), FPBN = globalThis._FPBN(), DNP = globalThis._DNP();

// ── Locate nodes. fn1 = transport-fn (from receiver AV param#0@fnX).
var xhrOpen = null, fn1 = null;
bt(r._ast, {
  CallExpression: function (p) {
    var c = p.node.callee;
    if (tt.isMemberExpression(c) && !c.computed && tt.isIdentifier(c.property) &&
        c.property.name === "open" && p.node.arguments.length >= 2 && !xhrOpen) xhrOpen = p;
  }
});
var recvVarBindInit = null, iNode = null, iXhr = null;
if (xhrOpen) {
  var recvNode = xhrOpen.node.callee.object;       // Identifier `xhr` (a local)
  var rav = PVM.get(recvNode);
  if (rav && rav.kind === "call" && rav.callee && rav.callee.kind === "member" &&
      rav.callee.obj && rav.callee.obj.kind === "param" && rav.callee.obj.fn) fn1 = rav.callee.obj.fn;
  // Resolve the `xhr` local to its declarator/assignment init `i.xhr()`.
  var rp = xhrOpen.get("callee.object");
  if (tt.isIdentifier(recvNode) && rp.scope) {
    var b = rp.scope.getBinding(recvNode.name);
    if (b) {
      if (b.path && tt.isVariableDeclarator(b.path.node) && b.path.node.init) recvVarBindInit = b.path.node.init;
      if (!recvVarBindInit && b.constantViolations) for (var k = b.constantViolations.length - 1; k >= 0; k--) {
        var cv = b.constantViolations[k];
        if (cv && tt.isAssignmentExpression(cv.node) && cv.node.right) { recvVarBindInit = cv.node.right; break; }
      }
    }
  }
  if (recvVarBindInit && tt.isCallExpression(recvVarBindInit) && tt.isMemberExpression(recvVarBindInit.callee)) {
    iXhr = recvVarBindInit.callee;   // i.xhr
    iNode = iXhr.object;             // i  (fn1 param 0)
  }
}
console.log("\n── xhrOpen?" + !!xhrOpen + " fn1@" + (fn1 && fn1.start) +
  " recvBindInit=" + (recvVarBindInit ? recvVarBindInit.type + "@" + recvVarBindInit.start : "n/a"));

// ── H1/H2/H3: receiver → i.xhr → i.
if (xhrOpen) {
  var recvNode2 = xhrOpen.node.callee.object;
  console.log("\nH1 fwd PVM(recv 'xhr') = " + avs(PVM.get(recvNode2)) +
    "  _demandResolve = " + avs(globalThis._DR(recvNode2)));
  if (iXhr) {
    var sc = DNP.get(iXhr) && DNP.get(iXhr).scope;
    console.log("H2 _demandCalleeFns(i.xhr) = " + fnsList(globalThis._DCF(iXhr, sc)) +
      "  fwd PVM(i.xhr) = " + avs(PVM.get(iXhr)) + "  _demandResolve(i.xhr) = " + avs(globalThis._DR(iXhr)));
    console.log("H3 fwd PVM(i) = " + avs(PVM.get(iNode)) + "  _demandResolve(i) = " + avs(globalThis._DR(iNode)));
  }
}

// ── H4: fn1 call sites / slice.
if (fn1) {
  var byFn = CSBF.get(fn1), fp1 = FPBN.get(fn1);
  console.log("\nH4 _specCallSitesByFn(fn1) = " + (byFn ? byFn.length + " site(s)" : "0/none") +
    "  _specFindCallSites(fn1) = " + (fp1 ? sitesLen(globalThis._FCS(fp1)) : "no-path") +
    "  inSlice=" + SLICE.has(fn1));
}

// ── Locate the TRUE transport dispatch: a CallExpression `t(A,B,C)` whose
//   Identifier callee binds to param idx1 of an enclosing FunctionExpression
//   that is the 2nd arg of a `ce.each(arrArg, cb)` call. (Vt's
//   `ce.each(t[e]||[],function(e,t){var n=t(i,o,a)…})`.)
var dispatchCall = null, dispatchEach = null, dispatchCb = null;
bt(r._ast, {
  CallExpression: function (p) {
    if (dispatchCall) return;
    if (!tt.isIdentifier(p.node.callee) || p.node.arguments.length < 1) return;
    var encFp = p.getFunctionParent && p.getFunctionParent();
    if (!encFp || !(tt.isFunctionExpression(encFp.node) || tt.isArrowFunctionExpression(encFp.node))) return;
    var ps = encFp.node.params;
    if (ps.length < 2 || !tt.isIdentifier(ps[1]) || ps[1].name !== p.node.callee.name) return;
    // resolve callee binding === param idx1 of encFp
    var bnd = p.scope.getBinding(p.node.callee.name);
    if (!bnd || !bnd.path || bnd.path.node !== ps[1]) return;
    // encFp must be the 2nd arg of a `.each(arr, encFp)` call
    var gp = encFp.parentPath;
    if (!gp || !tt.isCallExpression(gp.node) || gp.node.arguments[1] !== encFp.node) return;
    var gc = gp.node.callee;
    if (tt.isMemberExpression(gc) && tt.isIdentifier(gc.property) && gc.property.name === "each") {
      dispatchCall = p; dispatchEach = gp; dispatchCb = encFp.node;
    }
  }
});
console.log("\nH6 dispatch found? " + !!dispatchCall +
  (dispatchCall ? " `" + dispatchCall.node.callee.name + "(…" + dispatchCall.node.arguments.length +
    "args)` @L" + dispatchCall.node.loc.start.line + ":" + dispatchCall.node.callee.start : ""));
if (dispatchCall) {
  var dcCallee = dispatchCall.node.callee;
  var dsc = DNP.get(dcCallee) && DNP.get(dcCallee).scope;
  console.log("H6 fwd PVM(dispatch callee 't') = " + avs(PVM.get(dcCallee)) +
    "  _demandResolve(t) = " + avs(globalThis._DR(dcCallee)) +
    "  _demandCalleeFns(t) = " + fnsList(globalThis._DCF(dcCallee, dsc)));
  console.log("H6 cb@" + dispatchCb.start + " inSlice=" + SLICE.has(dispatchCb) +
    " _specCallSitesByFn(cb)=" + (CSBF.get(dispatchCb) ? CSBF.get(dispatchCb).length : "0/none") +
    " _specFindCallSites(cb)=" + (FPBN.get(dispatchCb) ? sitesLen(globalThis._FCS(FPBN.get(dispatchCb))) : "no-path"));
  var arrArg = dispatchEach.node.arguments[0];
  console.log("H7 ce.each arrArg.type=" + arrArg.type + " @" + arrArg.start +
    "  fwd PVM = " + avs(PVM.get(arrArg)) + "  _demandResolve = " + avs(globalThis._DR(arrArg)));
  // The inner `t[e]` member (arrArg.left when arrArg = `t[e]||[]`).
  var innerMem = (tt.isLogicalExpression(arrArg) && tt.isMemberExpression(arrArg.left)) ? arrArg.left : (tt.isMemberExpression(arrArg) ? arrArg : null);
  if (innerMem) {
    console.log("H7 inner `t[e]` member @" + innerMem.start + "  fwd PVM = " + avs(PVM.get(innerMem)) +
      "  _demandResolve = " + avs(globalThis._DR(innerMem)));
    console.log("H7   .object 't'  _demandResolve = " + avs(globalThis._DR(innerMem.object)) +
      "  .property 'e'  _demandResolve = " + avs(globalThis._DR(innerMem.property)));
  }
  // ce.each's own param idx0 (the array) — needs ce.each's call sites.
  var eachCalleeAv = PVM.get(dispatchEach.node.callee);
  var eachFn = (eachCalleeAv && eachCalleeAv.kind === "function-ref") ? eachCalleeAv.funcNode : null;
  if (!eachFn) { var ef = globalThis._DCF(dispatchEach.node.callee, DNP.get(dispatchEach.node.callee) && DNP.get(dispatchEach.node.callee).scope); if (Array.isArray(ef) && ef[0]) eachFn = ef[0]; }
  console.log("H8 ce.each fn@" + (eachFn && eachFn.start) +
    (eachFn ? "  param0 '" + (eachFn.params[0] && eachFn.params[0].name) + "' _demandResolve = " +
      avs(globalThis._DR(eachFn.params[0])) +
      "  _specFindCallSites=" + (FPBN.get(eachFn) ? sitesLen(globalThis._FCS(FPBN.get(eachFn))) : "no-path") +
      "  _specCallSitesByFn=" + (CSBF.get(eachFn) ? CSBF.get(eachFn).length : "0/none") : ""));
  // cb param idx1 (the array element bound by ce.each's internal call)
  var cbP1 = dispatchCb.params[1];
  console.log("H7 cb param idx1 '" + (cbP1 && cbP1.name) + "'  fwd PVM = " + avs(PVM.get(cbP1)) +
    "  _demandResolve = " + avs(globalThis._DR(cbP1)));
}

// ── H5: ce.ajaxTransport(fn1) registration (single fn arg; dataType "*").
var atReg = null;
if (fn1) bt(r._ast, {
  CallExpression: function (p) {
    if (atReg) return;
    var c = p.node.callee;
    if (tt.isMemberExpression(c) && tt.isIdentifier(c.property) && c.property.name === "ajaxTransport" &&
        p.node.arguments.some(function (a) { return a === fn1; })) atReg = p;
  }
});
console.log("\nH5 ce.ajaxTransport(fn1) reg found? " + !!atReg +
  (atReg ? " nargs=" + atReg.node.arguments.length + " @L" + atReg.node.loc.start.line : ""));
if (atReg) {
  var atCallee = atReg.node.callee;
  var asc = DNP.get(atCallee) && DNP.get(atCallee).scope;
  console.log("H5 fwd PVM(ajaxTransport callee) = " + avs(PVM.get(atCallee)) +
    "  _demandResolve = " + avs(globalThis._DR(atCallee)) +
    "  _demandCalleeFns = " + fnsList(globalThis._DCF(atCallee, asc)));
}

// ── H9: Vt (inspectPrefiltersOrTransports) — the fn whose param0 is `_t`
//   and that contains the `ce.each(t[e]||[],cb)` dispatch. Find it as the
//   function enclosing dispatchEach. Probe its call sites + param0/param1.
var VtFn = null;
if (typeof dispatchEach !== "undefined" && dispatchEach) {
  var de = dispatchEach.getFunctionParent && dispatchEach.getFunctionParent();
  // ascend to the nearest NAMED FunctionDeclaration with >=2 params (Vt)
  var seenV = new Set();
  while (de && de.node && !seenV.has(de.node)) {
    seenV.add(de.node);
    if (tt.isFunctionDeclaration(de.node) && de.node.params.length >= 3) { VtFn = de.node; break; }
    de = de.parentPath && de.parentPath.getFunctionParent ? de.parentPath.getFunctionParent() : null;
  }
}
console.log("\nH9 Vt fn@" + (VtFn && VtFn.start) +
  (VtFn ? " params=" + VtFn.params.map(function (p) { return p.name || p.type; }).join(",") +
    "  _specFindCallSites=" + (FPBN.get(VtFn) ? sitesLen(globalThis._FCS(FPBN.get(VtFn))) : "no-path") +
    "  _specCallSitesByFn=" + (CSBF.get(VtFn) ? CSBF.get(VtFn).length : "0/none") +
    "  inSlice=" + SLICE.has(VtFn) : ""));
if (VtFn) {
  for (var vpi = 0; vpi < Math.min(VtFn.params.length, 4); vpi++) {
    var vp = VtFn.params[vpi];
    console.log("H9   Vt.param" + vpi + " '" + (vp && vp.name) + "' _demandResolve = " + avs(globalThis._DR(vp)));
  }
}

// ── H10: fn1 (the transport factory `function(i){…}`) — its param0 `i`
//   (the options object). fn1.param0 ← Vt's `t(i,o,a)` dispatch arg0.
if (fn1) {
  console.log("\nH10 fn1 fn@" + fn1.start + " params=" + fn1.params.map(function (p) { return p.name || p.type; }).join(",") +
    "  _specFindCallSites=" + (FPBN.get(fn1) ? sitesLen(globalThis._FCS(FPBN.get(fn1))) : "no-path") +
    "  _specCallSitesByFn=" + (CSBF.get(fn1) ? CSBF.get(fn1).length : "0/none"));
  if (fn1.params[0]) console.log("H10  fn1.param0 '" + fn1.params[0].name + "' _demandResolve = " + avs(globalThis._DR(fn1.params[0])));
  // The dispatch `t(i,o,a)` inside cb — its arg0 (=fn1.param0 source).
  if (typeof dispatchCall !== "undefined" && dispatchCall && dispatchCall.node.arguments[0])
    console.log("H10  dispatch arg0 '" + (dispatchCall.node.arguments[0].name || dispatchCall.node.arguments[0].type) +
      "' _demandResolve = " + avs(globalThis._DR(dispatchCall.node.arguments[0])));
}

// ── H11: resolve the inner `_t[dt]` member + its `.object` (Vt.param0)
//   UNDER the hofCall ctx (`ce.each(arrArg,cb)`=dispatchEach.node) — the
//   exact context fix-4b threads. If `_t` resolves here but not in the
//   full cb.param1 chain ⇒ inFlight/cycle artifact, not a ctx-direct gap.
if (typeof dispatchEach !== "undefined" && dispatchEach && typeof innerMem !== "undefined" && innerMem) {
  var hofC = dispatchEach.node; // ce.each(arrArg, cb)
  console.log("\nH11 hofCall = ." + (hofC.callee.property && hofC.callee.property.name) +
    "(" + hofC.arguments.length + "args) @" + hofC.start);
  console.log("H11 _demandResolve(innerMem `_t[dt]`, hofCtx) = " + avs(globalThis._DRC(innerMem, hofC)));
  console.log("H11 _demandResolve(innerMem.object `_t`, hofCtx) = " + avs(globalThis._DRC(innerMem.object, hofC)));
  console.log("H11 _demandResolve(arrArg, hofCtx) = " + avs(globalThis._DRC(arrArg, hofC)));
  // and the dispatch callee `t` under cb's own param-binding context
  if (typeof dispatchCall !== "undefined" && dispatchCall)
    console.log("H11 _demandResolve(dispatch callee 't', hofCtx) = " + avs(globalThis._DRC(dispatchCall.node.callee, hofC)));
}

// ── H12: demand-cone membership of the key fns. The forward ctx-refine
//   composition (and _ctxResolveAv refined-memo read) is gated by
//   _specDemandConeFns. If Vt/ce.each/cb/fn1 are NOT in the cone, the
//   entry-seeded cone + cone-growth never reached them ⇒ composition
//   never starts. This is the decisive architectural fact.
console.log("\nH12 _specDemandConeFns live? " + globalThis._CONEON());
var ceEachFn2 = (typeof dispatchEach !== "undefined" && dispatchEach)
  ? (function () { var a = PVM.get(dispatchEach.node.callee); return a && a.kind === "function-ref" ? a.funcNode : null; })() : null;
[["Vt", typeof VtFn !== "undefined" ? VtFn : null],
 ["ce.each", ceEachFn2],
 ["cb", typeof dispatchCb !== "undefined" ? dispatchCb : null],
 ["fn1", fn1]].forEach(function (e) {
  console.log("H12 cone.has(" + e[0] + "@" + (e[1] && e[1].start) + ") = " +
    (e[1] ? globalThis._CONE(e[1]) : "n/a"));
});
