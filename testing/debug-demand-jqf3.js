// Step-E divergence pinned by reverse-engineering real jQuery 3.7.1
// (debug-jq-extract.js): options = `ce.ajaxSetup({}, t)` — a fn that
// RETURNS the deep-extend merge (`return jqExtend(true,e,ajaxSettings,t)`),
// NOT a direct merge call. Faithful reproducer of merge-via-return-
// delegation. If c4Hits=0 here, this is the real-bundle blocker.
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
globalThis.__DEMAND_PROBE = true; globalThis.__DEMAND_PROBE_DIAG = true;
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;").call(globalThis);

console.log("=== F3-ajaxSetup-return-delegates-merge ===");
globalThis.analyzeJSBundle(`
var support = { cors: true };
var transports = {};
function addTo(structure){ return function(d, f){ if (typeof d !== "string") { f = d; d = "*"; } (structure[d] = structure[d] || []).push(f); }; }
var ajaxTransport = addTo(transports);
function inspect(structure, options){ var list = structure["*"] || []; for (var i = 0; i < list.length; i++) { var r = list[i](options); if (r && r.send) return r; } }
function jqExtend(){ var i = 0, t = arguments[0]; if (typeof t === "boolean") { t = arguments[1]; i = 2; } else { i = 1; } for (; i < arguments.length; i++) { var src = arguments[i]; for (var k in src) t[k] = src[k]; } return t; }
var ajaxSettings = { xhr: function(){ return new XMLHttpRequest(); } };
function ajaxSetup(e, t){ return jqExtend(true, e || {}, ajaxSettings, t); }
ajaxTransport(function(i){
  if (support.cors || !i.crossDomain)
    return { send: function(e, t){ var n, r = i.xhr(); if (r.open(i.type, i.url, i.async), i.xhrFields) for (n in i.xhrFields) r[n] = i.xhrFields[n]; } };
});
function ajax(t){
  var v = ajaxSetup({}, t);
  var c = inspect(transports, v);
  c.send({}, function(){});
}
ajax({ type: "GET", url: "/api/f3" });
`, "t://f3", true, null);
