var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode +
  "globalThis.analyzeJSBundle = analyzeJSBundle;\n" +
  "globalThis._findCallSites = _specFindCallSites;\n" +
  "globalThis._specFuncPathByNode = _specFuncPathByNode;\n" +
  "globalThis._t = _t;\n").call(globalThis);

var code =
  'function each(arr, callback){\n' +
  '  for (var i = 0; i < arr.length; i++) { callback(i, arr[i]); }\n' +
  '}\n' +
  'each([function(){ return "/api/each0"; }], function(idx, factory){ var u = factory(); fetch(u); });\n';

var r = globalThis.analyzeJSBundle(code, "test://each", true, null);

var cbFn = null, eachFn = null;
globalThis.BabelBundle.traverse(r._ast, {
  FunctionDeclaration: function(p) {
    if (p.node.id.name === "each") eachFn = p.node;
  },
  FunctionExpression: function(p) {
    if (p.node.params.length === 2) cbFn = p.node;
  }
});

var cbPath = globalThis._specFuncPathByNode.get(cbFn);
console.log("cbPath:", !!cbPath, "parent type:", cbPath && cbPath.parent && cbPath.parent.type);

var cbCallSites = globalThis._findCallSites(cbPath);
console.log("cb call sites found:", cbCallSites.length);
for (var i = 0; i < cbCallSites.length; i++) {
  var cs = cbCallSites[i];
  console.log("  callsite", i, ":", cs.node.callee && (cs.node.callee.name || cs.node.callee.type), "at L"+cs.node.loc.start.line+":C"+cs.node.loc.start.column);
}

var eachPath = globalThis._specFuncPathByNode.get(eachFn);
var eachCallSites = globalThis._findCallSites(eachPath);
console.log("each call sites:", eachCallSites.length);
