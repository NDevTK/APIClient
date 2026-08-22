// Step-E divergence #1: jQuery's transport factory return is CONDITIONAL
// (`if(le.cors||Qt&&!i.crossDomain) return {send:…}`), unlike the faithful
// reproducer's unconditional return. Isolate: does a guarded factory
// return break the engine's end-to-end resolution?
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
globalThis.__DEMAND_PROBE = true; globalThis.__DEMAND_PROBE_DIAG = true;
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;").call(globalThis);

// G1: conditional factory return (jQuery's exact shape).
console.log("=== G1-conditional-factory-return ===");
globalThis.analyzeJSBundle(`
var support = { cors: true };
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
  for (var i = 0; i < list.length; i++) { var r = list[i](options); if (r && r.send) return r; }
}
function jqExtend(){
  var i = 0, t = arguments[0];
  if (typeof t === "boolean") { t = arguments[1]; i = 2; } else { i = 1; }
  for (; i < arguments.length; i++) { var src = arguments[i]; for (var k in src) t[k] = src[k]; }
  return t;
}
var ajaxSettings = { xhr: function(){ return new XMLHttpRequest(); } };
ajaxTransport(function(options){
  if (support.cors || !options.crossDomain)
    return { send: function(){ var xhr = options.xhr(); xhr.open(options.type, options.url); } };
});
function ajax(s){
  var opts = jqExtend(true, {}, ajaxSettings, s);
  var transport = inspect(transports, opts);
  transport.send();
}
ajax({ type: "GET", url: "/api/g1" });
`, "t://g1", true, null);

// G2: + r.open inside a comma/SequenceExpression in an if-test (jQuery's
// `if(r.open(i.type,i.url,...), i.xhrFields)for(...)`).
console.log("=== G2-open-in-seqexpr ===");
globalThis.analyzeJSBundle(`
var support = { cors: true };
var transports = {};
function addTo(structure){ return function(d, f){ if (typeof d !== "string") { f = d; d = "*"; } (structure[d] = structure[d] || []).push(f); }; }
var ajaxTransport = addTo(transports);
function inspect(structure, options){ var list = structure["*"] || []; for (var i = 0; i < list.length; i++) { var r = list[i](options); if (r && r.send) return r; } }
function jqExtend(){ var i = 0, t = arguments[0]; if (typeof t === "boolean") { t = arguments[1]; i = 2; } else { i = 1; } for (; i < arguments.length; i++) { var src = arguments[i]; for (var k in src) t[k] = src[k]; } return t; }
var ajaxSettings = { xhr: function(){ return new XMLHttpRequest(); } };
ajaxTransport(function(i){
  if (support.cors || !i.crossDomain)
    return { send: function(e, t){ var n, r = i.xhr(); if (r.open(i.type, i.url, i.async), i.xhrFields) for (n in i.xhrFields) r[n] = i.xhrFields[n]; } };
});
function ajax(s){ var opts = jqExtend(true, {}, ajaxSettings, s); var transport = inspect(transports, opts); transport.send(); }
ajax({ type: "POST", url: "/api/g2" });
`, "t://g2", true, null);
