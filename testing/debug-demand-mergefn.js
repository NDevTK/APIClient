// Minimal GENERAL merge-function ECMA capability isolation (NOT jQuery).
// Pin exactly which spec-eval modeling step is missing so the fix is in
// the engine, general to all manual-merge/builder fns.
//   M1 fixed-arity merge: function ext(a,b){for(k in b)a[k]=b[k];return a}
//   M2 variadic merge:    function ext(){t=arguments[0];for(i=1;...)for(k in arguments[i])t[k]=arguments[i][k];return t}
//   M3 builder return:    function mk(){var o={};o.x=v;return o}
// Each feeds a fetch URL from a merged/built prop; [RESOLVED]/[GAP].
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;").call(globalThis);

function run(label, code, expect) {
  var r = globalThis.analyzeJSBundle(code, "t://" + label, true, null);
  var sites = (r && r.fetchCallSites) || [];
  var urls = sites.map(function (s) { return s.url || "?"; });
  console.log(label + ": fetchSites=" + sites.length + " " + JSON.stringify(urls) +
    (urls.some(function (u) { return u.indexOf(expect) >= 0; }) ? "  [RESOLVED]" : "  [GAP]"));
}

// M1 — fixed-arity for-in copy into arg, return that arg.
run("M1-forin-copy-arg", `
function ext(a, b){ for (var k in b) a[k] = b[k]; return a; }
var o = ext({}, { path: "/api/m1" });
fetch(o.path);
`, "/api/m1");

// M2 — variadic arguments[] merge loop (the jqExtend shape, general).
run("M2-variadic-merge", `
function ext(){ var t = arguments[0]; for (var i = 1; i < arguments.length; i++){ var s = arguments[i]; for (var k in s) t[k] = s[k]; } return t; }
var o = ext({}, { a: 1 }, { path: "/api/m2" });
fetch(o.path);
`, "/api/m2");

// M3 — local builder obj, prop-assign, return (baseline; should work).
run("M3-builder-return", `
function mk(p){ var o = {}; o.path = p; return o; }
var o = mk("/api/m3");
fetch(o.path);
`, "/api/m3");

// M4 — fixed-arity merge but through an extra call hop (param-backward).
run("M4-merge-paramhop", `
function ext(a, b){ for (var k in b) a[k] = b[k]; return a; }
function build(s){ return ext({}, s); }
var o = build({ path: "/api/m4" });
fetch(o.path);
`, "/api/m4");
