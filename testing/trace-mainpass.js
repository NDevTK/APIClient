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

// Wrap _extractFetchCall + _processSecurityCallSink to count + time.
astCode = astCode.replace(
  "function _extractFetchCall(fetchCallExprPath, result, sinkName) {",
  "globalThis._efcCount = 0; globalThis._efcTime = 0;\nfunction _extractFetchCall_orig(fetchCallExprPath, result, sinkName) {"
);
astCode += `
function _extractFetchCall(fetchCallExprPath, result, sinkName) {
  globalThis._efcCount++;
  var _t0 = Date.now();
  try { return _extractFetchCall_orig(fetchCallExprPath, result, sinkName); }
  finally {
    globalThis._efcTime += (Date.now() - _t0);
    if (globalThis._efcCount % 10 === 0) console.log('[efc] ' + globalThis._efcCount + ' calls, ' + globalThis._efcTime + 'ms');
  }
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
