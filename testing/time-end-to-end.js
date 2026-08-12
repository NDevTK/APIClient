// Time end-to-end analysis with phase breakdown.
var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode + "\nglobalThis.analyzeJSBundle = analyzeJSBundle;").call(globalThis);

var bundleName = process.argv[2] || "index.js";
var code = fs.readFileSync(path.join(rootDir, "testing/harness-dumps", bundleName), "utf8");
console.log("Bundle: " + bundleName + " (" + (code.length / 1024).toFixed(1) + " KB)");
var t0 = Date.now();
var result = globalThis.analyzeJSBundle(code, "https://test.example.com/" + bundleName, true, null);
var t1 = Date.now();
console.log("Total: " + (t1 - t0) + "ms");
if (result._phaseTimings) console.log("Phases: " + JSON.stringify(result._phaseTimings));
if (globalThis._lastFixpointTimings) console.log("Fixpoint: " + JSON.stringify(globalThis._lastFixpointTimings));
console.log("fetchSites: " + (result.fetchCallSites || []).length);
console.log("sinks: " + (result.securitySinks || []).length);
