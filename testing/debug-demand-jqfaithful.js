// Faithful atomic reproducer of jQuery 3.x ajax internals: curried
// addToPrefiltersOrTransports registrar + transports registry +
// inspectPrefiltersOrTransports dispatch + deep-extend options +
// transport.send() + options.xhr() factory + options.type/url. If the
// demand engine resolves this end-to-end, the real-bundle gap is pure
// scale/minification, not a missing composition edge.
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
globalThis.__DEMAND_PROBE = true; globalThis.__DEMAND_PROBE_DIAG = true;
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;").call(globalThis);

var code = `
var transports = {};
function addTo(structure){
  return function(dataTypeExpr, func){
    if (typeof dataTypeExpr !== "string") { func = dataTypeExpr; dataTypeExpr = "*"; }
    (structure[dataTypeExpr] = structure[dataTypeExpr] || []).push(func);
  };
}
var ajaxTransport = addTo(transports);
function inspect(structure, options){
  var list = structure["*"] || [];
  for (var i = 0; i < list.length; i++) {
    var r = list[i](options);
    if (r && r.send) return r;
  }
}
function jqExtend(){
  var i = 0, t = arguments[0];
  if (typeof t === "boolean") { t = arguments[1]; i = 2; } else { i = 1; }
  for (; i < arguments.length; i++) { var src = arguments[i]; for (var k in src) t[k] = src[k]; }
  return t;
}
var ajaxSettings = { xhr: function(){ return new XMLHttpRequest(); } };
ajaxTransport("*", function(options){
  return { send: function(){ var xhr = options.xhr(); xhr.open(options.type, options.url); } };
});
function ajax(s){
  var opts = jqExtend(true, {}, ajaxSettings, s);
  var transport = inspect(transports, opts);
  transport.send();
}
ajax({ type: "GET", url: "/api/jqfaithful" });
`;
globalThis.analyzeJSBundle(code, "t://jqfaithful", true, null);
