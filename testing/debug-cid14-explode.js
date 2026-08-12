// Diagnostic: measure ctx-refinement volume on real jQuery with CID-14 on.
// analyzeJSBundle is fully synchronous so setInterval can't fire — patch
// the refine + commit sites to append a synchronous progress line to a
// file (appendFileSync is durable across an abrupt FATAL) and hard-abort
// once a volume threshold is crossed, revealing the growth rate.
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
var LOG = path.join(rd, "testing/.cid14-explode.log");
try { fs.unlinkSync(LOG); } catch (e) {}
function logln(s) { fs.appendFileSync(LOG, s + "\n"); }
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
globalThis.__logln = logln;
var ast = fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8");
var needle = "_specAnalyzePropertyFlow(candPath, true);";
var inject = "globalThis.__r=(globalThis.__r||0)+1;" +
  "if(globalThis.__r%200===0){globalThis.__logln('refines='+globalThis.__r+' commits='+(globalThis.__c||0)+' rss='+Math.round(process.memoryUsage().rss/1048576)+'MB');}" +
  "if(globalThis.__r>30000){globalThis.__logln('ABORT runaway refines='+globalThis.__r);process.exit(7);} ";
ast = ast.replace(needle, inject + needle);
ast = ast.replace("_specCtxEffectsBySite.set(cand.callSite.node, {",
  "globalThis.__c=(globalThis.__c||0)+1; _specCtxEffectsBySite.set(cand.callSite.node, {");
// Localize OOM: log cone entry/exit + RSS, and a heartbeat inside the
// cone worklist so a runaway cone is visible.
ast = ast.replace("function _specComputeDemandCone(programPath) {",
  "function _specComputeDemandCone(programPath) { globalThis.__logln('CONE-START rss='+Math.round(process.memoryUsage().rss/1048576)+'MB');");
ast = ast.replace("    var nd = work.pop();",
  "globalThis.__cw=(globalThis.__cw||0)+1; if(globalThis.__cw%5000===0)globalThis.__logln('cone seen='+globalThis.__cw+' work='+work.length+' rss='+Math.round(process.memoryUsage().rss/1048576)+'MB'); if(globalThis.__cw>3000000){globalThis.__logln('CONE-RUNAWAY');process.exit(8);} var nd = work.pop();");
ast = ast.replace("  var seeds = _demandSinkSeeds(programPath);",
  "  var seeds = _demandSinkSeeds(programPath); globalThis.__logln('CONE seeds='+seeds.length+' arg='+seeds.filter(function(s){return s.kind===String.fromCharCode(97,114,103);}).length+' callee='+seeds.filter(function(s){return s.kind===String.fromCharCode(99,97,108,108,101,101);}).length);");
ast = ast.replace("  _specComputeDemandCone(programPath); globalThis",
  "  globalThis"); // de-dup if prior patch applied
ast = ast.replace("  _specComputeDemandCone(programPath);",
  "  _specComputeDemandCone(programPath); globalThis.__logln('CONE-DONE cw='+(globalThis.__cw||0)+' coneFns='+(globalThis.__coneN||0)+' rss='+Math.round(process.memoryUsage().rss/1048576)+'MB');");
// Count cone fns: every _specDemandConeFns.add increments a counter.
ast = ast.replace("  _specDemandConeFns.add(fnNode);",
  "  globalThis.__coneN=(globalThis.__coneN||0)+1; _specDemandConeFns.add(fnNode);");
ast = ast.replace("        _specDemandConeFns.add(cf);",
  "        globalThis.__coneN=(globalThis.__coneN||0)+1; _specDemandConeFns.add(cf);");
new Function(ast + "globalThis.analyzeJSBundle=analyzeJSBundle;").call(globalThis);
var jq = fs.readFileSync(path.join(rd, "testing/harness-dumps/jquery-3.7.1.min.js"), "utf8");
var t0 = Date.now();
try {
  var r = globalThis.analyzeJSBundle(jq + '\njQuery.get("/api/profile");', "t://j", true, null);
  logln("DONE " + (Date.now() - t0) + "ms refines=" + globalThis.__r +
    " commits=" + globalThis.__c + " sites=" + (r.fetchCallSites || []).length);
} catch (e) {
  logln("THREW " + (Date.now() - t0) + "ms refines=" + globalThis.__r +
    " commits=" + globalThis.__c + " :" + (e.message || e));
}
