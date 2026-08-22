// STEP-5+ fix-5 instrument (durable). Pin the demand-tabulation CYCLE:
// resolve ONLY the dispatch callee `t` (cb.param1) under hofCtx with
// __DEMAND_UC_TRACE, dump the frame trace, find the inFlight re-entry
// (the key that cycles → returns the unresolved provisional base).
var fs = require("fs"), path = require("path");
var rd = path.join(__dirname, "..");
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;" +
  "globalThis._DRC=function(n,c){try{return _demandResolve(n,c);}catch(e){return {kind:'ERR',m:e.message};}};" +
  "globalThis._t=_t;").call(globalThis);

var jq = fs.readFileSync(path.join(rd, "testing/harness-dumps/jquery-3.7.1.min.js"), "utf8");
var r = globalThis.analyzeJSBundle(jq + '\njQuery.get("/api/profile");', "https://ex.com/app", "https://ex.com", null);
var tt = globalThis._t, bt = globalThis.BabelBundle.traverse;

// Locate the transport dispatch `t(i,o,a)` inside the `ce.each(arrArg,cb)`
// callback (same precise structural locator as debug-jq-transport-dispatch).
var dispatchCall = null, dispatchEach = null;
bt(r._ast, {
  CallExpression: function (p) {
    if (dispatchCall) return;
    if (!tt.isIdentifier(p.node.callee) || p.node.arguments.length < 1) return;
    var encFp = p.getFunctionParent && p.getFunctionParent();
    if (!encFp || !(tt.isFunctionExpression(encFp.node) || tt.isArrowFunctionExpression(encFp.node))) return;
    var ps = encFp.node.params;
    if (ps.length < 2 || !tt.isIdentifier(ps[1]) || ps[1].name !== p.node.callee.name) return;
    var bnd = p.scope.getBinding(p.node.callee.name);
    if (!bnd || !bnd.path || bnd.path.node !== ps[1]) return;
    var gp = encFp.parentPath;
    if (!gp || !tt.isCallExpression(gp.node) || gp.node.arguments[1] !== encFp.node) return;
    var gc = gp.node.callee;
    if (tt.isMemberExpression(gc) && tt.isIdentifier(gc.property) && gc.property.name === "each") {
      dispatchCall = p; dispatchEach = gp;
    }
  }
});
if (!dispatchCall) { console.log("dispatch NOT FOUND"); process.exit(1); }

var outFile = path.join(rd, "testing/.cid14-cycle.log");
var lines = [];
var origLog = console.log;
globalThis.__DEMAND_UC_TRACE = 1;
console.log = function () { lines.push(Array.prototype.join.call(arguments, " ")); };
var res;
try { res = globalThis._DRC(dispatchCall.node.callee, dispatchEach.node); }
finally { console.log = origLog; globalThis.__DEMAND_UC_TRACE = 0; }
fs.writeFileSync(outFile, lines.join("\n"));

origLog("dispatch `t`@" + dispatchCall.node.callee.start +
  " under hofCtx ce.each@" + dispatchEach.node.start);
origLog("result.kind=" + (res && res.kind) + "  trace lines=" + lines.length + " → " + outFile);

// Cycle analysis: a node#start that appears with " IF" (inFlight re-entry)
// is the cycle pivot; tally per node#start, show those that recur AND ever
// hit IF, with their base kinds.
var byNode = {};
for (var i = 0; i < lines.length; i++) {
  var m = /^\[UC\] (\w+)#(\d+) ph(\d+)( IF)?( PROV)?/.exec(lines[i]);
  if (!m) continue;
  var k = m[1] + "#" + m[2];
  if (!byNode[k]) byNode[k] = { n: 0, if: 0, prov: 0, bases: {} };
  byNode[k].n++; if (m[4]) byNode[k].if++; if (m[5]) byNode[k].prov++;
  var bm = / base=(\S+)/.exec(lines[i]); if (bm) byNode[k].bases[bm[1]] = (byNode[k].bases[bm[1]] || 0) + 1;
}
var cyc = Object.keys(byNode).filter(function (k) { return byNode[k].if > 0; })
  .sort(function (a, b) { return byNode[b].n - byNode[a].n; });
origLog("\n── inFlight-cycle pivots (node#start: visits/IF/PROV bases) ──");
cyc.slice(0, 25).forEach(function (k) {
  var b = byNode[k];
  origLog("  " + k + ": " + b.n + "v/" + b.if + "IF/" + b.prov + "PROV  bases=" +
    JSON.stringify(b.bases));
});
// Most-revisited nodes overall (cycle hot-spots even if IF not flagged).
origLog("\n── most-revisited nodes ──");
Object.keys(byNode).sort(function (a, b) { return byNode[b].n - byNode[a].n; })
  .slice(0, 12).forEach(function (k) { origLog("  " + k + ": " + byNode[k].n + "v  bases=" + JSON.stringify(byNode[k].bases)); });
