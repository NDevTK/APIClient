var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode +
  "globalThis.analyzeJSBundle = analyzeJSBundle;\n" +
  "globalThis._pathValMemo = function(){ return _specPathValMemo; };\n" +
  "globalThis._ptObjProps = function(){ return _specPointsToObjProps; };\n" +
  "globalThis._ptObjPropsReverse = function(){ return _specPointsToObjPropsReverse; };\n" +
  "globalThis._WILD = _SPEC_PT_WILDCARD_KEY;\n").call(globalThis);

var jq = fs.readFileSync(path.join(rootDir, "testing/harness-dumps/jquery-3.7.1.min.js"), "utf8");
var userCode = "\njQuery.get('/api/profile');\n";
var result = globalThis.analyzeJSBundle(jq + userCode, "test://jq", true, null);

console.log("fetchCallSites:", (result.fetchCallSites || []).length);
console.log("astLearnedMethods:", (result.astLearnedMethods || []).length);

var memo = globalThis._pathValMemo();
console.log("Searching for jQuery binding's _extraProps for 'get'/'post' keys...");
var jqBindings = [];
memo.forEach(function (av, node) {
  if (av && av.kind === "function-ref" && av._extraProps &&
      (Object.prototype.hasOwnProperty.call(av._extraProps, "get") ||
       Object.prototype.hasOwnProperty.call(av._extraProps, "post"))) {
    var keys = Object.keys(av._extraProps);
    jqBindings.push({ av: av, keys: keys, hasGet: !!av._extraProps.get, hasPost: !!av._extraProps.post });
  }
});
console.log("function-ref AVs containing get/post in _extraProps:", jqBindings.length);
jqBindings.slice(0, 3).forEach(function (b, i) {
  console.log("  [" + i + "] keys count:", b.keys.length, "hasGet:", b.hasGet, "hasPost:", b.hasPost);
  if (b.hasGet) console.log("    get AV kind:", b.av._extraProps.get.kind, b.av._extraProps.get.funcNode ? "(fn)" : "");
});

var ptObjProps = globalThis._ptObjProps();
console.log("\n_specPointsToObjProps WeakMap (cannot enumerate keys), checking via reverse for any 'get'/'post' key entries...");
var ptRev = globalThis._ptObjPropsReverse();
var declsWithGet = new Set();
var declsWithPost = new Set();
var declsWithWild = new Set();
ptRev.forEach && console.log("_specPointsToObjPropsReverse is", typeof ptRev, "(WeakMap not enumerable)");
// WeakMap not enumerable; instead walk memo for function-ref AVs and check reverse for the funcNode
memo.forEach(function (av, node) {
  if (av && av.kind === "function-ref" && av.funcNode) {
    var rev = ptRev.get(av.funcNode);
    if (rev) {
      rev.forEach(function (entry) {
        if (entry && entry.key === "get") declsWithGet.add(entry.decl);
        if (entry && entry.key === "post") declsWithPost.add(entry.decl);
        if (entry && entry.key === globalThis._WILD) declsWithWild.add(entry.decl);
      });
    }
  }
});
console.log("Decls with 'get' prop pointing to a fn:", declsWithGet.size);
console.log("Decls with 'post' prop pointing to a fn:", declsWithPost.size);
console.log("Decls with WILDCARD entries:", declsWithWild.size);
