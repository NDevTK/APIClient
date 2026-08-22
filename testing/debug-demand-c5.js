// C5 fail-first: atomic reproducer of jQuery's extend-merge + factory xhr.
// `s = extend({}, ajaxSettings, opts); xhr = s.xhr(); xhr.open(...)`.
// Probe how far the demand engine resolves the xhr.open url/method.
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
globalThis.__DEMAND_PROBE = true;
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;").call(globalThis);

var code = `
function extend() {
  var t = arguments[0];
  for (var i = 1; i < arguments.length; i++) {
    var s = arguments[i];
    for (var k in s) t[k] = s[k];
  }
  return t;
}
var ajaxSettings = { xhr: function(){ return new XMLHttpRequest(); } };
function ajax(opts) {
  var s = extend({}, ajaxSettings, opts);
  var xhr = s.xhr();
  xhr.open(opts.type, opts.url);
}
ajax({ type: "GET", url: "/api/c5" });
`;
globalThis.analyzeJSBundle(code, "t://c5", true, null);
console.log("---");
// Also a simpler shape: direct ajaxSettings, no extend (isolate factory hop).
var code2 = `
var ajaxSettings = { xhr: function(){ return new XMLHttpRequest(); } };
function ajax(opts) {
  var xhr = ajaxSettings.xhr();
  xhr.open(opts.type, opts.url);
}
ajax({ type: "POST", url: "/api/c5b" });
`;
globalThis.analyzeJSBundle(code2, "t://c5b", true, null);
