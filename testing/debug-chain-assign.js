// Sub-problem isolation with deep logging.
var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");

// Patch the Branch-A code to log subTarget === gtWin3 checks
astCode = astCode.replace(
  "if (subTarget === gtWin3) {",
  "console.log('[DBG branchA] eff.key=' + (eff.key && JSON.stringify(eff.key.value)) + ' subTarget.kind=' + (subTarget && subTarget.kind) + ' === gtWin3? ' + (subTarget === gtWin3) + ' subTargetIsObjLit=' + (subTarget && subTarget.kind === 'obj-lit') + ' subTarget=' + (subTarget === gtWin3 ? 'gtWin3' : (subTarget && subTarget._isWindow ? 'isWindow' : 'other')));\nif (subTarget === gtWin3) {"
);

new Function(astCode +
  "globalThis.analyzeJSBundle = analyzeJSBundle;\n" +
  "globalThis._effectsMemo = function(){ return _specEffectsMemo; };\n" +
  "globalThis._globalOverrides = function(){ return _specGlobalPropOverrides; };\n").call(globalThis);

var r1 = globalThis.analyzeJSBundle(
  "!function(e, t){ t(e); }(typeof window !== 'undefined' ? window : this, function(ie){\n" +
  "  var ce = function(){};\n" +
  "  ie.jQuery = ie.$ = ce;\n" +
  "});\n",
  "test://chain", true, null);

var go1 = globalThis._globalOverrides();
console.log("\nglobalOverride keys:", Object.keys(go1).join(","));
