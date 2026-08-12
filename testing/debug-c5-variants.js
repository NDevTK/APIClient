// Isolate which extend shape the resolver handles: named-2arg vs
// named-3arg vs arguments-variadic. Probe demand resolution of the
// merged `.xhr` factory → xhr.open url.
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
globalThis.__DEMAND_PROBE = true;
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;").call(globalThis);

function run(label, code) {
  console.log("=== " + label + " ===");
  globalThis.analyzeJSBundle(code, "t://" + label, true, null);
}

// V1: named 2-arg merge, single extend.
run("V1-named2arg", `
function extend(t, s){ for (var k in s) t[k] = s[k]; return t; }
var as = { xhr: function(){ return new XMLHttpRequest(); } };
function ajax(o){ var m = extend({}, as); var x = m.xhr(); x.open(o.type, o.url); }
ajax({ type: "GET", url: "/api/v1" });
`);

// V2: named 2-arg, nested extend(extend({},as),o) — jQuery-ish layering.
run("V2-nested", `
function extend(t, s){ for (var k in s) t[k] = s[k]; return t; }
var as = { xhr: function(){ return new XMLHttpRequest(); } };
function ajax(o){ var m = extend(extend({}, as), o); var x = m.xhr(); x.open(o.type, o.url); }
ajax({ type: "POST", url: "/api/v2" });
`);

// V3: arguments-variadic 3-arg (real jQuery shape).
run("V3-variadic", `
function extend(){ var t = arguments[0]; for (var i=1;i<arguments.length;i++){ var s=arguments[i]; for (var k in s) t[k]=s[k]; } return t; }
var as = { xhr: function(){ return new XMLHttpRequest(); } };
function ajax(o){ var m = extend({}, as, o); var x = m.xhr(); x.open(o.type, o.url); }
ajax({ type: "GET", url: "/api/v3" });
`);
