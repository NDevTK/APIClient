// C8 fail-first: namespace-member extend + namespace-member settings arg
// (jQuery's `jQuery.extend(true,{},jQuery.ajaxSettings,opts)` shape).
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
globalThis.__DEMAND_PROBE = true; globalThis.__DEMAND_PROBE_DIAG = true;
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;").call(globalThis);

function run(label, code) { console.log("=== " + label + " ==="); globalThis.analyzeJSBundle(code, "t://" + label, true, null); }

// C8a: namespace-member extend + namespace-member settings arg.
run("C8a-ns-member-extend", `
var ns = {};
ns.extend = function(){ var i=0,t=arguments[0]; if(typeof t==="boolean"){t=arguments[1];i=2;}else{i=1;} for(;i<arguments.length;i++){var s=arguments[i];for(var k in s)t[k]=s[k];} return t; };
ns.ajaxSettings = { xhr: function(){ return new XMLHttpRequest(); } };
ns.ajax = function(o){ var s = ns.extend(true, {}, ns.ajaxSettings, o); var x = s.xhr(); x.open(o.type, o.url); };
ns.ajax({ type: "GET", url: "/api/c8a" });
`);

// C8b: same but extend assigned via chained `ns.extend = ns.fn.extend = fn`
// (jQuery's actual `jQuery.extend = jQuery.fn.extend = function(){…}`).
run("C8b-chained-extend-assign", `
var ns = { fn: {} };
ns.extend = ns.fn.extend = function(){ var i=0,t=arguments[0]; if(typeof t==="boolean"){t=arguments[1];i=2;}else{i=1;} for(;i<arguments.length;i++){var s=arguments[i];for(var k in s)t[k]=s[k];} return t; };
ns.ajaxSettings = { xhr: function(){ return new XMLHttpRequest(); } };
ns.ajax = function(o){ var s = ns.extend(true, {}, ns.ajaxSettings, o); var x = s.xhr(); x.open(o.type, o.url); };
ns.ajax({ type: "POST", url: "/api/c8b" });
`);
