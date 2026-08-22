// Phase-timing measurement: prints _phaseTimings then exits.
// Designed to be killed via setTimeout if analysis hangs past N seconds.
var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode + "\nglobalThis.analyzeJSBundle = analyzeJSBundle;").call(globalThis);

var bundleName = process.argv[2];
var bundlePath = path.join(rootDir, "testing/harness-dumps", bundleName);
var code = fs.readFileSync(bundlePath, "utf8");
console.log("Analyzing " + bundleName + " (" + (code.length / 1024).toFixed(1) + " KB)");

var deadlineMs = parseInt(process.argv[3] || "60000", 10);
var timer = setTimeout(function () {
  console.log("TIMEOUT after " + deadlineMs + "ms");
  process.exit(99);
}, deadlineMs);
timer.unref && timer.unref();

var t0 = Date.now();
var result = analyzeJSBundle(code, "https://test.example.com/" + bundleName, true, null);
var t1 = Date.now();
clearTimeout(timer);

console.log("Analysis " + (t1 - t0) + "ms");
console.log("Phase timings:", JSON.stringify(result._phaseTimings));
console.log("fetchCallSites: " + (result.fetchCallSites || []).length);
console.log("securitySinks: " + (result.securitySinks || []).length);
