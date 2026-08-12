// Frontier F prod-gate correctness probe: § 13.3.5 [[Construct]] on a
// FUNCTION constructor (pre-ES6/transpiled/UMD OO idiom). Each probe is
// a faithful mirror of a DIFFERENT real shape; if the URL resolves, the
// general native rule fired. No jQuery names, no shape match.
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;").call(globalThis);

function probe(label, code) {
  var r;
  try { r = globalThis.analyzeJSBundle(code, "https://ex.com/app", "https://ex.com", null); }
  catch (e) { console.log(label + ": threw " + e.message); return; }
  var sites = (r.fetchCallSites || []).map(function (s) { return s.method + " " + s.url; });
  console.log(label + ": " + JSON.stringify(sites));
}

// F8 — ctor `this.opts={}` + prototype method mutates a local then opens
// XHR from it. The classic superagent/older-axios builder shape.
probe("F8-proto-method-local-url", `
function R(){ this.opts = {}; }
R.prototype.go = function(u){ var o = {}; o.url = u; var x = new XMLHttpRequest(); x.open("GET", o.url); };
var inst = new R();
inst.go("/api/F8");
`);

// Fpo — prototype REPLACED by an object literal of methods
// (R.prototype = { go: fn }). Same instance read, different proto write.
probe("Fpo-prototype-object-literal", `
function R(){}
R.prototype = { go: function(u){ var x = new XMLHttpRequest(); x.open("GET", u); } };
new R().go("/api/Fpo");
`);

// Fchain — chained setters on `this`, terminal proto method issues the
// request (mirror of G7 class-builder but with a function constructor).
probe("Fchain-this-setters-terminal", `
function Request(){ this.opts = {}; }
Request.prototype.method = function(m){ this.opts.method = m; return this; };
Request.prototype.url = function(u){ this.opts.url = u; return this; };
Request.prototype.end = function(){ var x = new XMLHttpRequest(); x.open(this.opts.method, this.opts.url); return x; };
new Request().method("POST").url("/api/Fchain").end();
`);

// Fthis — ctor stores the URL on `this` directly; proto method reads it.
probe("Fthis-ctor-stored-url", `
function C(u){ this.endpoint = u; }
C.prototype.fire = function(){ fetch(this.endpoint); };
new C("/api/Fthis").fire();
`);
