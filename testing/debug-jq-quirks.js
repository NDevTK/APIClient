// Incremental real-jQuery-quirk reproducer. Faithful jQuery ajax
// internals RESOLVE post-grounding (C4 url+method). Real 87KB jQuery
// does NOT (sites=[]). Memory pins 3 structural divergences vs
// faithful; quirk (3) extend is now closed (E3). This adds the
// remaining quirks ONE at a time to find the exact general-rule break:
//   F1 = faithful baseline (must resolve — sanity).
//   F2 = + CONDITIONAL factory return `if(cond)return{send}` (real:
//        `if(le.cors||Qt&&!i.crossDomain)return{send}`) ⇒ factory
//        return-memo = or({send},undefined).
//   F3 = F2 + prefilter/dataType-loop inspect (real
//        inspectPrefiltersOrTransports walks options.dataTypes with a
//        seekingTransport flag + prefilter recursion) instead of the
//        simple `list[i](options)`.
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;").call(globalThis);

function probe(label, code) {
  console.log("\n=== " + label + " ===");
  var r;
  try { r = globalThis.analyzeJSBundle(code, "https://ex.com/app", "https://ex.com", null); }
  catch (e) { console.log("  threw: " + e.message); return; }
  var sites = (r.fetchCallSites || []).map(function (s) { return s.method + " " + s.url; });
  console.log("  sites=" + JSON.stringify(sites));
}

var REG = `
var transports = {};
function addTo(structure){
  return function(dataTypeExpr, func){
    if (typeof dataTypeExpr !== "string") { func = dataTypeExpr; dataTypeExpr = "*"; }
    (structure[dataTypeExpr] = structure[dataTypeExpr] || []).push(func);
  };
}
var ajaxTransport = addTo(transports);
function jqExtend(){
  var i = 0, t = arguments[0];
  if (typeof t === "boolean") { t = arguments[1]; i = 2; } else { i = 1; }
  for (; i < arguments.length; i++) { var src = arguments[i]; for (var k in src) t[k] = src[k]; }
  return t;
}
var ajaxSettings = { xhr: function(){ return new XMLHttpRequest(); } };
`;

// F1 — faithful baseline (simple inspect, unconditional factory return).
probe("F1-faithful-baseline", REG + `
function inspect(structure, options){
  var list = structure["*"] || [];
  for (var i = 0; i < list.length; i++) { var r = list[i](options); if (r && r.send) return r; }
}
ajaxTransport("*", function(options){
  return { send: function(){ var xhr = options.xhr(); xhr.open(options.type, options.url); } };
});
function ajax(s){ var opts = jqExtend(true, {}, ajaxSettings, s); var transport = inspect(transports, opts); transport.send(); }
ajax({ type: "GET", url: "/api/f1" });
`);

// F2 — + CONDITIONAL factory return (real jQuery's
// `if(le.cors||Qt&&!i.crossDomain)return{send}`). cond opaque ⇒
// factory return AV = or({send:{…}}, undefined).
probe("F2-conditional-factory-return", REG + `
function inspect(structure, options){
  var list = structure["*"] || [];
  for (var i = 0; i < list.length; i++) { var r = list[i](options); if (r && r.send) return r; }
}
var cors = window.someFlag;
ajaxTransport("*", function(options){
  var o, a;
  if (cors || !options.crossDomain) {
    return { send: function(){ var xhr = options.xhr(); xhr.open(options.type, options.url); } };
  }
});
function ajax(s){ var opts = jqExtend(true, {}, ajaxSettings, s); var transport = inspect(transports, opts); transport.send(); }
ajax({ type: "GET", url: "/api/f2" });
`);

// F3 — F2 + real inspectPrefiltersOrTransports dataType-loop with
// seekingTransport flag (jQuery: walks options.dataTypes, supports a
// "*" wildcard + a recursive seekingTransport re-dispatch).
probe("F3-prefilter-datatype-loop", REG + `
function inspect(structure, options, jqXHR, dataType){
  var dataTypes = options.dataTypes;
  var seekingTransport, inspected = {};
  function inspectFn(dataTypeExpr){
    var selected;
    var list = structure[dataTypeExpr] || [];
    for (var i = 0; i < list.length; i++) {
      selected = list[i](options, jqXHR);
      if (typeof selected === "string") {
        if (!seekingTransport && !inspected[selected]) { options.dataTypes.unshift(selected); inspectFn(selected); return false; }
        else if (seekingTransport) { return !(selected = seekingTransport); }
      }
      if (selected) return selected;
    }
  }
  return inspectFn(dataTypes[0]) || (!inspected["*"] && inspectFn("*"));
}
var cors = window.someFlag;
ajaxTransport("*", function(options){
  if (cors || !options.crossDomain) {
    return { send: function(){ var xhr = options.xhr(); xhr.open(options.type, options.url); } };
  }
});
function ajax(s){ var opts = jqExtend(true, { dataTypes: ["*"] }, ajaxSettings, s); var transport = inspect(transports, opts); transport.send(); }
ajax({ type: "GET", url: "/api/f3" });
`);
