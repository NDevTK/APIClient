var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode +
  "globalThis.analyzeJSBundle = analyzeJSBundle;\n" +
  "globalThis._pathValMemo = function(){ return _specPathValMemo; };\n" +
  "globalThis._globalOverrides = function(){ return _specGlobalPropOverrides; };\n").call(globalThis);

var jq = fs.readFileSync(path.join(rootDir, "testing/harness-dumps/jquery-3.7.1.min.js"), "utf8");
var userCode = "\njQuery.ajax({url: '/api/simpler'});\n";
var result = globalThis.analyzeJSBundle(jq + userCode, "test://jq-xhr", true, null);

console.log("fetchCallSites:", (result.fetchCallSites || []).length);

var go = globalThis._globalOverrides();
console.log("jQuery in overrides:", !!go.jQuery);
if (go.jQuery) {
  console.log("jQuery kind:", go.jQuery.kind);
  if (go.jQuery._extraProps) {
    var ajax = go.jQuery._extraProps.ajax;
    console.log("jQuery._extraProps.ajax kind:", ajax && ajax.kind);
    if (ajax && ajax._extraProps) {
      console.log("ajax._extraProps keys:", Object.keys(ajax._extraProps).slice(0, 10).join(","));
    }
    var ajaxSettings = go.jQuery._extraProps.ajaxSettings;
    console.log("jQuery._extraProps.ajaxSettings kind:", ajaxSettings && ajaxSettings.kind);
    if (ajaxSettings) {
      var src = ajaxSettings.kind === "obj-lit" ? ajaxSettings.props :
                ajaxSettings.kind === "function-ref" ? ajaxSettings._extraProps : null;
      if (src) {
        console.log("ajaxSettings keys:", Object.keys(src).slice(0, 15).join(","));
        console.log("ajaxSettings.xhr kind:", src.xhr && src.xhr.kind);
      }
    }
  }
}
