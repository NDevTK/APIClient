// Real jQuery 3.7.1 + user call jQuery.get('/api/profile').
// Verifies _specHandleUserHofCb: the jQuery.each(["get","post",...],
// function(i,method){ jQuery[method]=function(url,...){...} }) HOF must
// now propagate `method` (= SetUnion of the literal-array elements) so
// jQuery.get/.post land in jQuery's _extraProps. Then reports the next
// composition break (if any) honestly.
var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
globalThis.__DEMAND_PROBE = true;
globalThis.__DEMAND_PROBE_DIAG = true;
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode +
  "globalThis.analyzeJSBundle = analyzeJSBundle;\n" +
  "globalThis._globalOverrides = function(){ return _specGlobalPropOverrides; };\n").call(globalThis);

var jq = fs.readFileSync(path.join(rootDir, "testing/harness-dumps/jquery-3.7.1.min.js"), "utf8");
var userCode = "\njQuery.get('/api/profile');\n";
var result = globalThis.analyzeJSBundle(jq + userCode, "test://jq", true, null);

console.log("=== fetchCallSites:", (result.fetchCallSites || []).length, "===");
(result.fetchCallSites || []).forEach(function (s) {
  console.log("  " + s.method + " " + s.url);
});

var go = globalThis._globalOverrides();
var jqAv = go && go.jQuery;
console.log("\njQuery override:", jqAv && jqAv.kind);
if (jqAv && jqAv._extraProps) {
  var ep = jqAv._extraProps;
  var keys = Object.keys(ep);
  console.log("jQuery._extraProps key count:", keys.length);
  console.log("  has get?", Object.prototype.hasOwnProperty.call(ep, "get"),
    ep.get ? "(kind=" + ep.get.kind + (ep.get.funcNode ? " fn#" + ep.get.funcNode.start : "") + ")" : "");
  console.log("  has post?", Object.prototype.hasOwnProperty.call(ep, "post"),
    ep.post ? "(kind=" + ep.post.kind + (ep.post.funcNode ? " fn#" + ep.post.funcNode.start : "") + ")" : "");
  console.log("  has getJSON?", Object.prototype.hasOwnProperty.call(ep, "getJSON"),
    ep.getJSON ? "(kind=" + ep.getJSON.kind + ")" : "");
  console.log("  has ajax?", Object.prototype.hasOwnProperty.call(ep, "ajax"),
    ep.ajax ? "(kind=" + ep.ajax.kind + ")" : "");
}
