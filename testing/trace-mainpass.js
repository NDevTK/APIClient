// Trace main-pass phase progress.
var cp = require("child_process");
var fs = require("fs");
var path = require("path");
var os = require("os");
var rootDir = path.join(__dirname, "..");
var bundleName = process.argv[2] || "index.js";
var deadlineMs = parseInt(process.argv[3] || "300000", 10);

var babelPath = path.join(rootDir, "extension/lib/babel-bundle.js").replace(/\\/g, "/");
var astPath = path.join(rootDir, "extension/lib/ast.js").replace(/\\/g, "/");
var bundleAbs = path.join(rootDir, "testing/harness-dumps", bundleName).replace(/\\/g, "/");

var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");

// Wrap _specAnalyzePropertyFlow + _resolveAllValues
astCode = astCode.replace(
  "function _specAnalyzePropertyFlow(funcPath, force) {",
  "globalThis._sapfCount = 0; globalThis._sapfTime = 0;\nfunction _specAnalyzePropertyFlow_orig(funcPath, force) {"
).replace(
  "function _resolveAllValues(initialPath, initialDepth) {",
  "globalThis._ravCount = 0; globalThis._ravTime = 0;\nfunction _resolveAllValues_orig(initialPath, initialDepth) {"
);
astCode += `
function _specAnalyzePropertyFlow(funcPath, force) {
  globalThis._sapfCount++;
  var _t0 = Date.now();
  try { return _specAnalyzePropertyFlow_orig(funcPath, force); }
  finally {
    globalThis._sapfTime += (Date.now() - _t0);
    if (globalThis._sapfCount % 500 === 0) console.log('[sapf] ' + globalThis._sapfCount + ' calls, ' + globalThis._sapfTime + 'ms');
  }
}
function _resolveAllValues(initialPath, initialDepth) {
  globalThis._ravCount++;
  var _t0 = Date.now();
  try { return _resolveAllValues_orig(initialPath, initialDepth); }
  finally {
    globalThis._ravTime += (Date.now() - _t0);
    if (globalThis._ravCount % 100 === 0) console.log('[rav] ' + globalThis._ravCount + ' calls, ' + globalThis._ravTime + 'ms');
  }
}
`;
// Also wrap _isPathInSlice + _avAtPath
astCode = astCode.replace(
  "function _isPathInSlice(path) {",
  "globalThis._ipsCount = 0; globalThis._ipsTime = 0;\nfunction _isPathInSlice_orig(path) {"
).replace(
  "function _avAtPath(path) {",
  "globalThis._avapCount = 0; globalThis._avapTime = 0;\nfunction _avAtPath_orig(path) {"
);
astCode += `
function _isPathInSlice(path) {
  globalThis._ipsCount++;
  var _t0 = Date.now();
  try { return _isPathInSlice_orig(path); }
  finally {
    globalThis._ipsTime += (Date.now() - _t0);
    if (globalThis._ipsCount % 10000 === 0) console.log('[ips] ' + globalThis._ipsCount + ' calls, ' + globalThis._ipsTime + 'ms');
  }
}
function _avAtPath(path) {
  globalThis._avapCount++;
  var _t0 = Date.now();
  try { return _avAtPath_orig(path); }
  finally {
    globalThis._avapTime += (Date.now() - _t0);
    if (globalThis._avapCount % 500 === 0) console.log('[avap] ' + globalThis._avapCount + ' calls, ' + globalThis._avapTime + 'ms');
  }
}
`;
// Wrap _resolveToObject + _traceValueSource + _isGlobalFetchCall
astCode = astCode.replace(
  "function _resolveToObject(initialPath, initialDepth) {",
  "globalThis._rtoCount = 0; globalThis._rtoTime = 0;\nfunction _resolveToObject_orig(initialPath, initialDepth) {"
).replace(
  "function _traceValueSource(path, _unused) {",
  "globalThis._tvsCount = 0; globalThis._tvsTime = 0;\nfunction _traceValueSource_orig(path, _unused) {"
).replace(
  "function _isGlobalFetchCall(callee, scope, path) {",
  "globalThis._gfcCount = 0; globalThis._gfcTime = 0;\nfunction _isGlobalFetchCall_orig(callee, scope, path) {"
);
astCode += `
function _resolveToObject(initialPath, initialDepth) {
  globalThis._rtoCount++;
  var _t0 = Date.now();
  try { return _resolveToObject_orig(initialPath, initialDepth); }
  finally {
    globalThis._rtoTime += (Date.now() - _t0);
    if (globalThis._rtoCount % 500 === 0) console.log('[rto] ' + globalThis._rtoCount + ' calls, ' + globalThis._rtoTime + 'ms');
  }
}
function _traceValueSource(path, _unused) {
  globalThis._tvsCount++;
  var _t0 = Date.now();
  try { return _traceValueSource_orig(path, _unused); }
  finally {
    globalThis._tvsTime += (Date.now() - _t0);
    if (globalThis._tvsCount % 500 === 0) console.log('[tvs] ' + globalThis._tvsCount + ' calls, ' + globalThis._tvsTime + 'ms');
  }
}
function _isGlobalFetchCall(callee, scope, path) {
  globalThis._gfcCount++;
  var _t0 = Date.now();
  try { return _isGlobalFetchCall_orig(callee, scope, path); }
  finally {
    globalThis._gfcTime += (Date.now() - _t0);
    if (globalThis._gfcCount % 5000 === 0) console.log('[gfc] ' + globalThis._gfcCount + ' calls, ' + globalThis._gfcTime + 'ms');
  }
}
`;
// Wrap _processNetworkSink, _processSecurityCallSink, _processDangerousPattern, _processIIFE
astCode = astCode.replace(
  "function _processNetworkSink(path, result) {",
  "globalThis._pnsCount = 0; globalThis._pnsTime = 0;\nfunction _processNetworkSink_orig(path, result) {"
).replace(
  "function _processSecurityCallSink(path, result) {",
  "globalThis._pscsCount = 0; globalThis._pscsTime = 0;\nfunction _processSecurityCallSink_orig(path, result) {"
).replace(
  "function _processDangerousPattern(path, result) {",
  "globalThis._pdpCount = 0; globalThis._pdpTime = 0;\nfunction _processDangerousPattern_orig(path, result) {"
).replace(
  "function _processIIFE(path) {",
  "globalThis._piifeCount = 0; globalThis._piifeTime = 0;\nfunction _processIIFE_orig(path) {"
);
astCode += `
function _processNetworkSink(path, result) {
  globalThis._pnsCount++;
  var _t0 = Date.now();
  try { return _processNetworkSink_orig(path, result); }
  finally {
    globalThis._pnsTime += (Date.now() - _t0);
    if (globalThis._pnsCount % 2000 === 0) console.log('[pns] ' + globalThis._pnsCount + ' calls, ' + globalThis._pnsTime + 'ms');
  }
}
function _processSecurityCallSink(path, result) {
  globalThis._pscsCount++;
  var _t0 = Date.now();
  try { return _processSecurityCallSink_orig(path, result); }
  finally {
    globalThis._pscsTime += (Date.now() - _t0);
    if (globalThis._pscsCount % 2000 === 0) console.log('[pscs] ' + globalThis._pscsCount + ' calls, ' + globalThis._pscsTime + 'ms');
  }
}
function _processDangerousPattern(path, result) {
  globalThis._pdpCount++;
  var _t0 = Date.now();
  try { return _processDangerousPattern_orig(path, result); }
  finally {
    globalThis._pdpTime += (Date.now() - _t0);
    if (globalThis._pdpCount % 2000 === 0) console.log('[pdp] ' + globalThis._pdpCount + ' calls, ' + globalThis._pdpTime + 'ms');
  }
}
function _processIIFE(path) {
  globalThis._piifeCount++;
  var _t0 = Date.now();
  try { return _processIIFE_orig(path); }
  finally {
    globalThis._piifeTime += (Date.now() - _t0);
    if (globalThis._piifeCount % 2000 === 0) console.log('[piife] ' + globalThis._piifeCount + ' calls, ' + globalThis._piifeTime + 'ms');
  }
}
`;
// Wrap AssignmentExpression visitors
astCode = astCode.replace(
  "function _trackGlobalAssignment(path) {",
  "globalThis._tgaCount = 0; globalThis._tgaTime = 0;\nfunction _trackGlobalAssignment_orig(path) {"
).replace(
  "function _detectProtoFieldAssignment(path, result) {",
  "globalThis._dpfaCount = 0; globalThis._dpfaTime = 0;\nfunction _detectProtoFieldAssignment_orig(path, result) {"
).replace(
  "function _processSecurityAssignSink(path, result) {",
  "globalThis._psasCount = 0; globalThis._psasTime = 0;\nfunction _processSecurityAssignSink_orig(path, result) {"
).replace(
  "function _processDangerousAssignment(path, result) {",
  "globalThis._pdaCount = 0; globalThis._pdaTime = 0;\nfunction _processDangerousAssignment_orig(path, result) {"
);
astCode += `
function _trackGlobalAssignment(path) {
  globalThis._tgaCount++; var _t0 = Date.now();
  try { return _trackGlobalAssignment_orig(path); }
  finally { globalThis._tgaTime += (Date.now() - _t0); if (globalThis._tgaCount % 500 === 0) console.log('[tga] ' + globalThis._tgaCount + ' calls, ' + globalThis._tgaTime + 'ms'); }
}
function _detectProtoFieldAssignment(path, result) {
  globalThis._dpfaCount++; var _t0 = Date.now();
  try { return _detectProtoFieldAssignment_orig(path, result); }
  finally { globalThis._dpfaTime += (Date.now() - _t0); if (globalThis._dpfaCount % 500 === 0) console.log('[dpfa] ' + globalThis._dpfaCount + ' calls, ' + globalThis._dpfaTime + 'ms'); }
}
function _processSecurityAssignSink(path, result) {
  globalThis._psasCount++; var _t0 = Date.now();
  try { return _processSecurityAssignSink_orig(path, result); }
  finally { globalThis._psasTime += (Date.now() - _t0); if (globalThis._psasCount % 500 === 0) console.log('[psas] ' + globalThis._psasCount + ' calls, ' + globalThis._psasTime + 'ms'); }
}
function _processDangerousAssignment(path, result) {
  globalThis._pdaCount++; var _t0 = Date.now();
  try { return _processDangerousAssignment_orig(path, result); }
  finally { globalThis._pdaTime += (Date.now() - _t0); if (globalThis._pdaCount % 500 === 0) console.log('[pda] ' + globalThis._pdaCount + ' calls, ' + globalThis._pdaTime + 'ms'); }
}
`;
// Trigger trace on _specAnalyzeProgramWithFixpoint
astCode = astCode.replace(
  "function _specAnalyzeProgramWithFixpoint(programPath) {",
  "function _specAnalyzeProgramWithFixpoint(programPath) { console.log('[fp] enter t=' + Date.now());"
);

var tmpAst = path.join(os.tmpdir(), "ast-trace-" + Date.now() + ".js");
fs.writeFileSync(tmpAst, astCode);

var runnerScript = `
var fs = require("fs");
var babelCode = fs.readFileSync(${JSON.stringify(babelPath)}, "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(${JSON.stringify(tmpAst)}, "utf8");
new Function(astCode + "\\nglobalThis.analyzeJSBundle = analyzeJSBundle;").call(globalThis);
var code = fs.readFileSync(${JSON.stringify(bundleAbs)}, "utf8");
console.log("[start t=" + Date.now() + "] " + (code.length / 1024).toFixed(1) + " KB");
var t0 = Date.now();
var result = analyzeJSBundle(code, "https://test.example.com/" + ${JSON.stringify(bundleName)}, true, null);
console.log("[done t=" + Date.now() + "] " + (Date.now() - t0) + "ms");
console.log("[fetchSites] " + (result.fetchCallSites || []).length);
console.log("[sinks] " + (result.securitySinks || []).length);
console.log("[efc final] " + globalThis._efcCount + " calls, " + globalThis._efcTime + "ms total");
`;

var child = cp.spawn(process.execPath, ["-e", runnerScript], { stdio: "inherit" });
var killTimer = setTimeout(function () {
  console.log("[deadline] " + deadlineMs + "ms exceeded — killing");
  child.kill("SIGKILL");
  try { fs.unlinkSync(tmpAst); } catch (_) {}
}, deadlineMs);
child.on("exit", function (code) {
  clearTimeout(killTimer);
  try { fs.unlinkSync(tmpAst); } catch (_) {}
  process.exit(code || 0);
});
