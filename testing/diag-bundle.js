// Diagnostic runner: spawns analyzer in a child process with a hard
// deadline. Captures phase timings + fixpoint stats. Kills the child
// when deadline hits to surface hang without blocking the parent.
var cp = require("child_process");
var fs = require("fs");
var path = require("path");
var os = require("os");
var rootDir = path.join(__dirname, "..");
var bundleName = process.argv[2];
var deadlineMs = parseInt(process.argv[3] || "60000", 10);

// Read + instrument ast.js, write to temp file.
var astPath = path.join(rootDir, "extension/lib/ast.js");
var astCode = fs.readFileSync(astPath, "utf8");

// Phase markers
astCode = astCode.replace(
  "var _t_parse_end = (typeof performance",
  "console.log('[phase] parse done'); var _t_parse_end = (typeof performance"
).replace(
  "var _t_prepass_end = (typeof performance",
  "console.log('[phase] prepass done'); var _t_prepass_end = (typeof performance"
).replace(
  "var _t_slice_end = (typeof performance",
  "console.log('[phase] slice done'); var _t_slice_end = (typeof performance"
).replace(
  "var _t_end = (typeof performance",
  "console.log('[phase] mainpass done'); var _t_end = (typeof performance"
);

// Fixpoint entry/exit log
astCode = astCode.replace(
  "function _specAnalyzeProgramWithFixpoint(programPath) {",
  "function _specAnalyzeProgramWithFixpoint(programPath) { console.log('[fp] enter');"
);

// Call counter on _resolveAvBySubstitutingCallerArgs (insert at function body start)
astCode = astCode.replace(
  "function _resolveAvBySubstitutingCallerArgs(funcPath, avWithParamRefs) {\n  if (!funcPath || !funcPath.node || !avWithParamRefs) return null;",
  "var _rasCount = 0;\nfunction _resolveAvBySubstitutingCallerArgs(funcPath, avWithParamRefs) {\n  _rasCount++; if (_rasCount % 500 === 0) console.log('[ras] ' + _rasCount + ' calls');\n  if (!funcPath || !funcPath.node || !avWithParamRefs) return null;"
);

// Call counter + cumulative timing on _specAnalyzePropertyFlow
astCode = astCode.replace(
  "function _specAnalyzePropertyFlow(funcPath, isFixpoint) {",
  "var _sapfCount = 0, _sapfTime = 0;\nfunction _specAnalyzePropertyFlow(funcPath, isFixpoint) { _sapfCount++; var _sapfT0 = (typeof performance!=='undefined'?performance.now():Date.now()); if (_sapfCount % 100 === 0) console.log('[sapf] ' + _sapfCount + ' analyses, ' + Math.round(_sapfTime) + 'ms');"
);
// Wrap _resolveAllValues to track time. Real signature is
// (initialPath, initialDepth). Match precisely.
astCode = astCode.replace(
  "function _resolveAllValues(initialPath, initialDepth) {",
  "var _ravCount = 0, _ravTime = 0;\nfunction _resolveAllValues_orig(initialPath, initialDepth) {"
);
astCode += `
function _resolveAllValues(initialPath, initialDepth) {
  _ravCount++;
  var _t0 = (typeof performance!=='undefined'?performance.now():Date.now());
  try { return _resolveAllValues_orig(initialPath, initialDepth); }
  finally {
    _ravTime += ((typeof performance!=='undefined'?performance.now():Date.now()) - _t0);
    if (_ravCount % 100 === 0) console.log('[rav] ' + _ravCount + ' calls, ' + Math.round(_ravTime) + 'ms');
  }
}
`;
// Call counter + cumulative time on _avAtPath + _resolveByContextSensitiveReanalysis
astCode = astCode.replace(
  "function _avAtPath(path) {",
  "var _avapCount = 0, _avapTime = 0;\nfunction _avAtPath_orig(path) {"
).replace(
  "function _resolveByContextSensitiveReanalysis(initialPath, encFn) {",
  "var _rcsrCount = 0, _rcsrTime = 0;\nfunction _resolveByContextSensitiveReanalysis_orig(initialPath, encFn) {"
);
astCode += `
function _avAtPath(path) {
  _avapCount++;
  var _t0 = (typeof performance!=='undefined'?performance.now():Date.now());
  try { return _avAtPath_orig(path); }
  finally {
    _avapTime += ((typeof performance!=='undefined'?performance.now():Date.now()) - _t0);
    if (_avapCount % 100 === 0) console.log('[avap] ' + _avapCount + ' calls, ' + Math.round(_avapTime) + 'ms');
  }
}
function _resolveByContextSensitiveReanalysis(initialPath, encFn) {
  _rcsrCount++;
  var _t0 = (typeof performance!=='undefined'?performance.now():Date.now());
  try { return _resolveByContextSensitiveReanalysis_orig(initialPath, encFn); }
  finally {
    _rcsrTime += ((typeof performance!=='undefined'?performance.now():Date.now()) - _t0);
    if (_rcsrCount % 10 === 0) console.log('[rcsr] ' + _rcsrCount + ' calls, ' + Math.round(_rcsrTime) + 'ms');
  }
}
`;
// Call counter on _isPathInSlice + cache-hit/miss
astCode = astCode.replace(
  "function _isPathInSlice(path) {",
  "var _ipsCount = 0, _ipsHit = 0;\nfunction _isPathInSlice(path) { _ipsCount++; if (_ipsCount % 5000 === 0) console.log('[ips] ' + _ipsCount + ' calls, ' + _ipsHit + ' cache hits');"
).replace(
  "var encNode = path.node ? _specEnclFnByNode.get(path.node) : null;",
  "var encNode = path.node ? _specEnclFnByNode.get(path.node) : null; if (encNode) _ipsHit++;"
);
// Call counter + cumulative timing on visitors
astCode = astCode.replace(
  "function _processNetworkSink(path, result) {",
  "var _pnsCount = 0, _pnsTime = 0;\nfunction _processNetworkSink_orig(path, result) {"
).replace(
  "function _processSecurityCallSink(path, result) {",
  "var _pscsCount = 0, _pscsTime = 0;\nfunction _processSecurityCallSink_orig(path, result) {"
).replace(
  "function _processDangerousPattern(path, result) {",
  "var _pdpCount = 0, _pdpTime = 0;\nfunction _processDangerousPattern_orig(path, result) {"
);
// Add wrappers at end
astCode += `
function _processNetworkSink(path, result) {
  _pnsCount++;
  var _t0 = (typeof performance!=='undefined'?performance.now():Date.now());
  try { return _processNetworkSink_orig(path, result); }
  finally {
    _pnsTime += ((typeof performance!=='undefined'?performance.now():Date.now()) - _t0);
    if (_pnsCount % 500 === 0) console.log('[pns t=' + Math.round(Date.now()/1) + 'ms] ' + _pnsCount + ' calls, in-vis ' + Math.round(_pnsTime) + 'ms');
  }
}
function _processSecurityCallSink(path, result) {
  _pscsCount++;
  var _t0 = (typeof performance!=='undefined'?performance.now():Date.now());
  try { return _processSecurityCallSink_orig(path, result); }
  finally {
    _pscsTime += ((typeof performance!=='undefined'?performance.now():Date.now()) - _t0);
    if (_pscsCount % 500 === 0) console.log('[pscs] ' + _pscsCount + ' calls, ' + Math.round(_pscsTime) + 'ms');
  }
}
function _processDangerousPattern(path, result) {
  _pdpCount++;
  var _t0 = (typeof performance!=='undefined'?performance.now():Date.now());
  try { return _processDangerousPattern_orig(path, result); }
  finally {
    _pdpTime += ((typeof performance!=='undefined'?performance.now():Date.now()) - _t0);
    if (_pdpCount % 500 === 0) console.log('[pdp] ' + _pdpCount + ' calls, ' + Math.round(_pdpTime) + 'ms');
  }
}
`;

var tmpAst = path.join(os.tmpdir(), "ast-instrumented-" + Date.now() + ".js");
fs.writeFileSync(tmpAst, astCode);

var babelPath = path.join(rootDir, "extension/lib/babel-bundle.js");
var bundleAbs = path.join(rootDir, "testing/harness-dumps", bundleName);
var runnerScript = `
var fs = require("fs");
var babelCode = fs.readFileSync(${JSON.stringify(babelPath)}, "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(${JSON.stringify(tmpAst)}, "utf8");
new Function(astCode + "\\nglobalThis.analyzeJSBundle = analyzeJSBundle;").call(globalThis);
var code = fs.readFileSync(${JSON.stringify(bundleAbs)}, "utf8");
console.log("[bundle] " + (code.length / 1024).toFixed(1) + " KB");
var t0 = Date.now();
var result = analyzeJSBundle(code, "https://test.example.com/" + ${JSON.stringify(bundleName)}, true, null);
console.log("[total] " + (Date.now() - t0) + "ms");
console.log("[phases]", JSON.stringify(result._phaseTimings));
if (globalThis._lastFixpointTimings) console.log("[fixpoint]", JSON.stringify(globalThis._lastFixpointTimings));
console.log("[fetchSites] " + (result.fetchCallSites || []).length);
console.log("[sinks] " + (result.securitySinks || []).length);
`;

var child = cp.spawn(process.execPath, ["-e", runnerScript], { stdio: "inherit" });
var killTimer = setTimeout(function () {
  console.log("[deadline] " + deadlineMs + "ms exceeded — killing child");
  child.kill("SIGKILL");
  try { fs.unlinkSync(tmpAst); } catch (_) {}
}, deadlineMs);
child.on("exit", function (code) {
  clearTimeout(killTimer);
  try { fs.unlinkSync(tmpAst); } catch (_) {}
  process.exit(code || 0);
});
