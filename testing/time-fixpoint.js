// Time just the fixpoint phase of index.js.
var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode + "\nglobalThis._babelTraverse = _babelTraverse;\nglobalThis._t = _t;\nglobalThis._specBuildSlice = _specBuildSlice;\nglobalThis._specEnsureProgramGlobalsPrepass = _specEnsureProgramGlobalsPrepass;\nglobalThis._specAnalyzeProgramWithFixpoint = _specAnalyzeProgramWithFixpoint;").call(globalThis);

var code = fs.readFileSync(path.join(rootDir, "testing/harness-dumps/index.js"), "utf8");
console.log("Bundle: " + (code.length / 1024).toFixed(1) + " KB");
var t0 = Date.now();
var ast = globalThis.BabelBundle.parse(code, { sourceType: "unambiguous", errorRecovery: true });
console.log("parse: " + (Date.now() - t0) + "ms");

var programPath = null;
var t1 = Date.now();
globalThis._babelTraverse(ast, {
  Program: function (p) {
    programPath = p;
    p.stop();
  }
});
console.log("getProgramPath: " + (Date.now() - t1) + "ms");

var t2 = Date.now();
globalThis._specEnsureProgramGlobalsPrepass(programPath);
console.log("globalsPrepass: " + (Date.now() - t2) + "ms");

var t3 = Date.now();
globalThis._specBuildSlice(programPath);
console.log("buildSlice: " + (Date.now() - t3) + "ms");

var t4 = Date.now();
try {
  globalThis._specAnalyzeProgramWithFixpoint(programPath);
} catch (e) { console.error("threw:", e.message); }
console.log("fixpoint: " + (Date.now() - t4) + "ms");
if (globalThis._lastFixpointTimings) {
  console.log("fixpoint stats:", JSON.stringify(globalThis._lastFixpointTimings));
}
