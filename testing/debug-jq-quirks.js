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
  // ONLY=<substr> runs just the matching probe(s) in a fresh process —
  // exposes cross-analyzeJSBundle contamination (a probe that "passes"
  // only when preceded by others is a fake pass).
  if (process.env.ONLY && label.indexOf(process.env.ONLY) === -1) return;
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

// F4 — real-jQuery faithfulness increment: ajaxSettings is a GLOBAL
// library-member (jQuery.ajaxSettings) whose `xhr` is a closure-var
// factory `function(){return new XMLHttpRequest()}`; options built via
// jQuery.extend(true,{},jQuery.ajaxSettings, jQuery.ajaxSetup-processed
// s); transport.send(headers, done) 2-arg + a jqXHR Deferred-ish obj.
probe("F4-real-global-ajaxSettings+ajaxSetup+2arg-send", REG + `
var jQuery = {};
jQuery.ajaxSettings = { xhr: function(){ return new XMLHttpRequest(); } };
jQuery.ajaxSetup = function(target, settings){ return jqExtend(target, jQuery.ajaxSettings, settings); };
function inspect(structure, options){
  function inspectFn(dataTypeExpr){
    var list = structure[dataTypeExpr] || [];
    for (var i = 0; i < list.length; i++) { var r = list[i](options); if (r && r.send) return r; }
  }
  return inspectFn(options.dataTypes[0]) || inspectFn("*");
}
var cors = window.someFlag;
ajaxTransport("*", function(i){
  if (cors || !i.crossDomain) {
    return { send: function(e, t){ var n, r = i.xhr(); r.open(i.type, i.url, i.async); } };
  }
});
jQuery.ajax = function(s){
  var v = jQuery.ajaxSetup({}, s);
  v.dataTypes = ["*"];
  var jqXHR = { readyState: 0, setRequestHeader: function(){}, done: function(){ return jqXHR; } };
  var transport = inspect(transports, v, jqXHR);
  transport.send(v.headers || {}, function(){});
  return jqXHR;
};
jQuery.ajax({ type: "GET", url: "/api/f4" });
`);

// F4a — F3 EXACTLY, only difference: ajaxSettings is a global library
// member `jQuery.ajaxSettings` (not a local var). Isolates whether the
// break is the global-member receiver of the merged xhr-factory.
probe("F4a-global-member-ajaxSettings-only", REG + `
var jQuery = {};
jQuery.ajaxSettings = { xhr: function(){ return new XMLHttpRequest(); } };
function inspect(structure, options){
  function inspectFn(dataTypeExpr){
    var list = structure[dataTypeExpr] || [];
    for (var i = 0; i < list.length; i++) { var r = list[i](options); if (r && r.send) return r; }
  }
  return inspectFn(options.dataTypes[0]) || inspectFn("*");
}
var cors = window.someFlag;
ajaxTransport("*", function(options){
  if (cors || !options.crossDomain) {
    return { send: function(){ var xhr = options.xhr(); xhr.open(options.type, options.url); } };
  }
});
function ajax(s){ var opts = jqExtend(true, { dataTypes: ["*"] }, jQuery.ajaxSettings, s); var transport = inspect(transports, opts); transport.send(); }
ajax({ type: "GET", url: "/api/f4a" });
`);

// F4b — F4a + the ajaxSetup wrapper indirection (options = return of a
// wrapper fn that ITSELF returns a jqExtend merge — nested merge-call).
probe("F4b-ajaxSetup-wrapper-returns-merge", REG + `
var jQuery = {};
jQuery.ajaxSettings = { xhr: function(){ return new XMLHttpRequest(); } };
jQuery.ajaxSetup = function(target, settings){ return jqExtend(target, jQuery.ajaxSettings, settings); };
function inspect(structure, options){
  function inspectFn(dataTypeExpr){
    var list = structure[dataTypeExpr] || [];
    for (var i = 0; i < list.length; i++) { var r = list[i](options); if (r && r.send) return r; }
  }
  return inspectFn(options.dataTypes[0]) || inspectFn("*");
}
var cors = window.someFlag;
ajaxTransport("*", function(options){
  if (cors || !options.crossDomain) {
    return { send: function(){ var xhr = options.xhr(); xhr.open(options.type, options.url); } };
  }
});
function ajax(s){ var opts = jqExtend(true, { dataTypes: ["*"] }, jQuery.ajaxSetup({}, s)); var transport = inspect(transports, opts); transport.send(); }
ajax({ type: "GET", url: "/api/f4b" });
`);

// F5 — F4b + the REAL jQuery transport-invocation indirection. jQuery
// does NOT call `list[i](options)` directly; it iterates the registered
// transports with `jQuery.each(arr, function(idx, fn){ var n =
// fn(options,jqXHR,opts); ... })`, and `jQuery.each`'s OWN body invokes
// the callback via `cb.call(arr[k], k, arr[k])` (§ 7.3.2 Call /
// § 10.2.10 FDI). So the factory's `options` param binds through TWO
// hops: jqEach's `cb.call(...,k,arr[k])` → the inspect callback's `fn`
// param ← array element ← `.push(factory)` in the curried registrar.
// Isolates whether demand connects the factory `options` param across
// the jQuery.each-mediated HOF invocation (the only transport-layer
// structural delta F4b → real 87 KB jQuery).
probe("F5-jquery-each-mediated-transport-invocation", REG + `
var jQuery = {};
jQuery.ajaxSettings = { xhr: function(){ return new XMLHttpRequest(); } };
jQuery.ajaxSetup = function(target, settings){ return jqExtend(target, jQuery.ajaxSettings, settings); };
function jqEach(arr, cb){ var k = 0, n = arr.length; for (; k < n; k++) { if (cb.call(arr[k], k, arr[k]) === false) break; } return arr; }
function inspect(structure, options){
  function inspectFn(dataTypeExpr){
    var selected;
    jqEach(structure[dataTypeExpr] || [], function(idx, fn){
      var r = fn(options);
      if (r && r.send) { selected = r; return false; }
    });
    return selected;
  }
  return inspectFn(options.dataTypes[0]) || inspectFn("*");
}
var cors = window.someFlag;
ajaxTransport("*", function(options){
  if (cors || !options.crossDomain) {
    return { send: function(){ var xhr = options.xhr(); xhr.open(options.type, options.url); } };
  }
});
function ajax(s){ var opts = jqExtend(true, { dataTypes: ["*"] }, jQuery.ajaxSetup({}, s)); var transport = inspect(transports, opts); transport.send(); }
ajax({ type: "GET", url: "/api/f5" });
`);

// F6 — F5 with the REAL jQuery curried registrar Ut (replaces REG's
// simple addTo). jQuery: `function Ut(o){return function(e,t){
// "string"!=typeof e&&(t=e,e="*");var n,r=0,i=e.toLowerCase().match(D)
// ||[];if(v(t))while(n=i[r++])"+"===n[0]?(n=n.slice(1)||"*",(o[n]=o[n]
// ||[]).unshift(t)):(o[n]=o[n]||[]).push(t)}}` with D=/[^\x20\t\r\n\f]
// +/g. The registry KEY is derived `"*".toLowerCase().match(D)[0]`
// (String.prototype.match § 22.1.3.13 → ["*"]) then bound via
// `while(n=i[r++])` (§ 13.4.3 / § 13.15) and the obj-array is grown
// `(o[n]=o[n]||[]).push(t)`. Isolates whether array-element points-to
// survives a regex-match-derived computed key + while-assignment loop —
// the only registrar-layer delta F5 → real jQuery.
probe("F6-real-Ut-regex-match-key-registrar", `
var transports = {};
var D = /[^\\x20\\t\\r\\n\\f]+/g;
function isFn(x){ return typeof x === "function"; }
function Ut(o){
  return function(e, t){
    if (typeof e !== "string") { t = e; e = "*"; }
    var n, r = 0, i = e.toLowerCase().match(D) || [];
    if (isFn(t))
      while (n = i[r++])
        "+" === n[0] ? (n = n.slice(1) || "*", (o[n] = o[n] || []).unshift(t))
                     : (o[n] = o[n] || []).push(t);
  };
}
var ajaxTransport = Ut(transports);
function jqExtend(){
  var i = 0, t = arguments[0];
  if (typeof t === "boolean") { t = arguments[1]; i = 2; } else { i = 1; }
  for (; i < arguments.length; i++) { var src = arguments[i]; for (var k in src) t[k] = src[k]; }
  return t;
}
var jQuery = {};
jQuery.ajaxSettings = { xhr: function(){ return new XMLHttpRequest(); } };
jQuery.ajaxSetup = function(target, settings){ return jqExtend(target, jQuery.ajaxSettings, settings); };
function jqEach(arr, cb){ var k = 0, n = arr.length; for (; k < n; k++) { if (cb.call(arr[k], k, arr[k]) === false) break; } return arr; }
function inspect(structure, options){
  function inspectFn(dataTypeExpr){
    var selected;
    jqEach(structure[dataTypeExpr] || [], function(idx, fn){
      var r = fn(options);
      if (r && r.send) { selected = r; return false; }
    });
    return selected;
  }
  return inspectFn(options.dataTypes[0]) || inspectFn("*");
}
var cors = window.someFlag;
ajaxTransport("*", function(options){
  if (cors || !options.crossDomain) {
    return { send: function(){ var xhr = options.xhr(); xhr.open(options.type, options.url); } };
  }
});
function ajax(s){ var opts = jqExtend(true, { dataTypes: ["*"] }, jQuery.ajaxSetup({}, s)); var transport = inspect(transports, opts); transport.send(); }
ajax({ type: "GET", url: "/api/f6" });
`);

// F6a — BISECT: F6 minus the regex-match + while-loop key derivation.
// Curried Ut + typeof-arg-swap + isFn guard kept, but the registry key
// is the swapped param `e` DIRECTLY (`(o[e]=o[e]||[]).push(t)`) like
// F5's addTo. PASS ⇒ the gap is the match()-array → while(n=i[r++])
// loop-derived computed key. FAIL ⇒ gap is the arg-swap/isFn shape.
probe("F6a-curried-Ut-direct-key-no-regex", `
var transports = {};
function isFn(x){ return typeof x === "function"; }
function Ut(o){
  return function(e, t){
    if (typeof e !== "string") { t = e; e = "*"; }
    if (isFn(t)) (o[e] = o[e] || []).push(t);
  };
}
var ajaxTransport = Ut(transports);
function jqExtend(){
  var i = 0, t = arguments[0];
  if (typeof t === "boolean") { t = arguments[1]; i = 2; } else { i = 1; }
  for (; i < arguments.length; i++) { var src = arguments[i]; for (var k in src) t[k] = src[k]; }
  return t;
}
var jQuery = {};
jQuery.ajaxSettings = { xhr: function(){ return new XMLHttpRequest(); } };
jQuery.ajaxSetup = function(target, settings){ return jqExtend(target, jQuery.ajaxSettings, settings); };
function jqEach(arr, cb){ var k = 0, n = arr.length; for (; k < n; k++) { if (cb.call(arr[k], k, arr[k]) === false) break; } return arr; }
function inspect(structure, options){
  function inspectFn(dataTypeExpr){
    var selected;
    jqEach(structure[dataTypeExpr] || [], function(idx, fn){
      var r = fn(options);
      if (r && r.send) { selected = r; return false; }
    });
    return selected;
  }
  return inspectFn(options.dataTypes[0]) || inspectFn("*");
}
var cors = window.someFlag;
ajaxTransport("*", function(options){
  if (cors || !options.crossDomain) {
    return { send: function(){ var xhr = options.xhr(); xhr.open(options.type, options.url); } };
  }
});
function ajax(s){ var opts = jqExtend(true, { dataTypes: ["*"] }, jQuery.ajaxSetup({}, s)); var transport = inspect(transports, opts); transport.send(); }
ajax({ type: "GET", url: "/api/f6a" });
`);

// F6b — BISECT: F6a + ONLY the regex-match key (no while-loop). Key =
// `e.toLowerCase().match(D)[0]` computed once. PASS ⇒ gap is purely the
// while(n=i[r++]) assignment-loop binding. FAIL ⇒ gap is match()-array
// element → computed-key points-to.
probe("F6b-regex-match-key-no-while", `
var transports = {};
var D = /[^\\x20\\t\\r\\n\\f]+/g;
function isFn(x){ return typeof x === "function"; }
function Ut(o){
  return function(e, t){
    if (typeof e !== "string") { t = e; e = "*"; }
    var i = e.toLowerCase().match(D) || [];
    var n = i[0];
    if (isFn(t)) (o[n] = o[n] || []).push(t);
  };
}
var ajaxTransport = Ut(transports);
function jqExtend(){
  var i = 0, t = arguments[0];
  if (typeof t === "boolean") { t = arguments[1]; i = 2; } else { i = 1; }
  for (; i < arguments.length; i++) { var src = arguments[i]; for (var k in src) t[k] = src[k]; }
  return t;
}
var jQuery = {};
jQuery.ajaxSettings = { xhr: function(){ return new XMLHttpRequest(); } };
jQuery.ajaxSetup = function(target, settings){ return jqExtend(target, jQuery.ajaxSettings, settings); };
function jqEach(arr, cb){ var k = 0, n = arr.length; for (; k < n; k++) { if (cb.call(arr[k], k, arr[k]) === false) break; } return arr; }
function inspect(structure, options){
  function inspectFn(dataTypeExpr){
    var selected;
    jqEach(structure[dataTypeExpr] || [], function(idx, fn){
      var r = fn(options);
      if (r && r.send) { selected = r; return false; }
    });
    return selected;
  }
  return inspectFn(options.dataTypes[0]) || inspectFn("*");
}
var cors = window.someFlag;
ajaxTransport("*", function(options){
  if (cors || !options.crossDomain) {
    return { send: function(){ var xhr = options.xhr(); xhr.open(options.type, options.url); } };
  }
});
function ajax(s){ var opts = jqExtend(true, { dataTypes: ["*"] }, jQuery.ajaxSetup({}, s)); var transport = inspect(transports, opts); transport.send(); }
ajax({ type: "GET", url: "/api/f6b" });
`);

// F6c — BISECT: F6b with the key INLINED into the member (no
// intermediate `var i`/`var n`): `(o[(e.toLowerCase().match(D)||[])[0]]
// = ...).push(t)`. PASS ⇒ gap is the multi-level local-var declarator
// chain (n←i←e) not resolved when evaluating the registry write key
// under param substitution (the registrar body statements aren't
// replayed into the key-eval state). FAIL ⇒ gap is the string-op chain
// itself not resolving when `e` is a substituted param (vs top-level).
probe("F6c-inline-regex-key-no-locals", `
var transports = {};
var D = /[^\\x20\\t\\r\\n\\f]+/g;
function isFn(x){ return typeof x === "function"; }
function Ut(o){
  return function(e, t){
    if (typeof e !== "string") { t = e; e = "*"; }
    if (isFn(t)) (o[(e.toLowerCase().match(D) || [])[0]] = o[(e.toLowerCase().match(D) || [])[0]] || []).push(t);
  };
}
var ajaxTransport = Ut(transports);
function jqExtend(){
  var i = 0, t = arguments[0];
  if (typeof t === "boolean") { t = arguments[1]; i = 2; } else { i = 1; }
  for (; i < arguments.length; i++) { var src = arguments[i]; for (var k in src) t[k] = src[k]; }
  return t;
}
var jQuery = {};
jQuery.ajaxSettings = { xhr: function(){ return new XMLHttpRequest(); } };
jQuery.ajaxSetup = function(target, settings){ return jqExtend(target, jQuery.ajaxSettings, settings); };
function jqEach(arr, cb){ var k = 0, n = arr.length; for (; k < n; k++) { if (cb.call(arr[k], k, arr[k]) === false) break; } return arr; }
function inspect(structure, options){
  function inspectFn(dataTypeExpr){
    var selected;
    jqEach(structure[dataTypeExpr] || [], function(idx, fn){
      var r = fn(options);
      if (r && r.send) { selected = r; return false; }
    });
    return selected;
  }
  return inspectFn(options.dataTypes[0]) || inspectFn("*");
}
var cors = window.someFlag;
ajaxTransport("*", function(options){
  if (cors || !options.crossDomain) {
    return { send: function(){ var xhr = options.xhr(); xhr.open(options.type, options.url); } };
  }
});
function ajax(s){ var opts = jqExtend(true, { dataTypes: ["*"] }, jQuery.ajaxSetup({}, s)); var transport = inspect(transports, opts); transport.send(); }
ajax({ type: "GET", url: "/api/f6c" });
`);

// F6d — BISECT: F6c with an INLINE regex literal (no free `var D`).
// PASS ⇒ gap is free module-scope regex-var resolution during the
// isolated registry-write key eval. FAIL ⇒ gap is the string-op chain
// (.toLowerCase().match()[0]) itself on a substituted-param receiver.
probe("F6d-inline-regex-literal-key", `
var transports = {};
function isFn(x){ return typeof x === "function"; }
function Ut(o){
  return function(e, t){
    if (typeof e !== "string") { t = e; e = "*"; }
    if (isFn(t)) (o[(e.toLowerCase().match(/[^\\x20\\t\\r\\n\\f]+/g) || [])[0]] = o[(e.toLowerCase().match(/[^\\x20\\t\\r\\n\\f]+/g) || [])[0]] || []).push(t);
  };
}
var ajaxTransport = Ut(transports);
function jqExtend(){
  var i = 0, t = arguments[0];
  if (typeof t === "boolean") { t = arguments[1]; i = 2; } else { i = 1; }
  for (; i < arguments.length; i++) { var src = arguments[i]; for (var k in src) t[k] = src[k]; }
  return t;
}
var jQuery = {};
jQuery.ajaxSettings = { xhr: function(){ return new XMLHttpRequest(); } };
jQuery.ajaxSetup = function(target, settings){ return jqExtend(target, jQuery.ajaxSettings, settings); };
function jqEach(arr, cb){ var k = 0, n = arr.length; for (; k < n; k++) { if (cb.call(arr[k], k, arr[k]) === false) break; } return arr; }
function inspect(structure, options){
  function inspectFn(dataTypeExpr){
    var selected;
    jqEach(structure[dataTypeExpr] || [], function(idx, fn){
      var r = fn(options);
      if (r && r.send) { selected = r; return false; }
    });
    return selected;
  }
  return inspectFn(options.dataTypes[0]) || inspectFn("*");
}
var cors = window.someFlag;
ajaxTransport("*", function(options){
  if (cors || !options.crossDomain) {
    return { send: function(){ var xhr = options.xhr(); xhr.open(options.type, options.url); } };
  }
});
function ajax(s){ var opts = jqExtend(true, { dataTypes: ["*"] }, jQuery.ajaxSetup({}, s)); var transport = inspect(transports, opts); transport.send(); }
ajax({ type: "GET", url: "/api/f6d" });
`);

// F6e — BISECT: F6a (direct param key, KNOWN-PASS) but key passed
// through ONE string method `e.toLowerCase()`. PASS ⇒ a single string
// op on the substituted param resolves; isolates how many chain links
// the key-eval state survives. FAIL ⇒ even one transform breaks it.
probe("F6e-single-toLowerCase-key", `
var transports = {};
function isFn(x){ return typeof x === "function"; }
function Ut(o){
  return function(e, t){
    if (typeof e !== "string") { t = e; e = "*"; }
    if (isFn(t)) (o[e.toLowerCase()] = o[e.toLowerCase()] || []).push(t);
  };
}
var ajaxTransport = Ut(transports);
function jqExtend(){
  var i = 0, t = arguments[0];
  if (typeof t === "boolean") { t = arguments[1]; i = 2; } else { i = 1; }
  for (; i < arguments.length; i++) { var src = arguments[i]; for (var k in src) t[k] = src[k]; }
  return t;
}
var jQuery = {};
jQuery.ajaxSettings = { xhr: function(){ return new XMLHttpRequest(); } };
jQuery.ajaxSetup = function(target, settings){ return jqExtend(target, jQuery.ajaxSettings, settings); };
function jqEach(arr, cb){ var k = 0, n = arr.length; for (; k < n; k++) { if (cb.call(arr[k], k, arr[k]) === false) break; } return arr; }
function inspect(structure, options){
  function inspectFn(dataTypeExpr){
    var selected;
    jqEach(structure[dataTypeExpr] || [], function(idx, fn){
      var r = fn(options);
      if (r && r.send) { selected = r; return false; }
    });
    return selected;
  }
  return inspectFn(options.dataTypes[0]) || inspectFn("*");
}
var cors = window.someFlag;
ajaxTransport("*", function(options){
  if (cors || !options.crossDomain) {
    return { send: function(){ var xhr = options.xhr(); xhr.open(options.type, options.url); } };
  }
});
function ajax(s){ var opts = jqExtend(true, { dataTypes: ["*"] }, jQuery.ajaxSetup({}, s)); var transport = inspect(transports, opts); transport.send(); }
ajax({ type: "GET", url: "/api/f6e" });
`);

// F7 — BISECT toward real Ut: F6b + ONLY the `while(n=i[r++])`
// iteration (no "+"-prefix conditional, no n.slice — those are F8/F9).
// jQuery's real registrar binds the key via `while(n=i[r++])`: the
// while TEST is an AssignmentExpression `n = i[r++]` whose value is the
// assigned value (§ 13.15.2), and `i[r++]` reads element r then post-
// increments r (§ 13.4.3). Over the loop `n` ranges the elements of
// the array `i` (= ["*"]); the write `(o[n]=o[n]||[]).push(t)` keys on
// that. Isolates the loop-bound-key rule (n ∈ element-union of i) —
// the only delta F6b → F6 at the registrar layer. PASS once § 13.15.2
// + § 13.4.3 + loop-element abstraction land; FAIL until then (no
// papering — synthetic stays red while the rule is missing).
probe("F7-while-assign-loop-bound-key", `
var transports={};
var D=/[^\\x20\\t\\r\\n\\f]+/g;
function isFn(x){return typeof x==="function";}
function Ut(o){
  return function(e,t){
    if(typeof e!=="string"){t=e;e="*";}
    var n,r=0,i=e.toLowerCase().match(D)||[];
    if(isFn(t)) while(n=i[r++]) (o[n]=o[n]||[]).push(t);
  };
}
var ajaxTransport=Ut(transports);
function jqExtend(){var i=0,t=arguments[0];if(typeof t==="boolean"){t=arguments[1];i=2;}else{i=1;}for(;i<arguments.length;i++){var src=arguments[i];for(var k in src)t[k]=src[k];}return t;}
var jQuery={};
jQuery.ajaxSettings={xhr:function(){return new XMLHttpRequest();}};
jQuery.ajaxSetup=function(target,settings){return jqExtend(target,jQuery.ajaxSettings,settings);};
function jqEach(arr,cb){var k=0,n=arr.length;for(;k<n;k++){if(cb.call(arr[k],k,arr[k])===false)break;}return arr;}
function inspect(structure,options){function inspectFn(dt){var selected;jqEach(structure[dt]||[],function(idx,fn){var r=fn(options);if(r&&r.send){selected=r;return false;}});return selected;}return inspectFn(options.dataTypes[0])||inspectFn("*");}
var cors=window.someFlag;
ajaxTransport("*",function(options){if(cors||!options.crossDomain){return{send:function(){var xhr=options.xhr();xhr.open(options.type,options.url);}};}});
function ajax(s){var opts=jqExtend(true,{dataTypes:["*"]},jQuery.ajaxSetup({},s));var transport=inspect(transports,opts);transport.send();}
ajax({type:"GET",url:"/api/f7"});
`);

// F8 — F7 (full real Ut registrar, PASSES) + the REAL `Vt`
// (inspectPrefiltersOrTransports) replacing the simple inspect/jqEach.
// jQuery: `function Vt(t,i,o,a){var s={},u=t===_t;function l(e){var r;
// return s[e]=!0,jqEach(t[e]||[],function(e,t){var n=t(i,o,a);return
// "string"!=typeof n||u||s[n]?u?!(r=n):void 0:(i.dataTypes.unshift(n),
// l(n),!1)}),r}return l(i.dataTypes[0])||!s["*"]&&l("*")}`. The factory
// is invoked at `var n=t(i,o,a)` where the jqEach callback's 2nd param
// `t` SHADOWS Vt's `t` and IS the array element (factory), `i` = Vt's
// options param (closure-captured). Adds: recursive `l(n)` worklist,
// `s[e]=!0` seen-set, `u=t===_t` identity, `!(r=n)` return-via-assign,
// `i.dataTypes.unshift(n)`. Isolates whether the demand chain connects
// the factory's `i` param across the REAL dispatch — the documented
// loop/recursion-fixpoint + deep-context frontier. Red until the
// grounded rule lands (no papering).
probe("F8-real-Vt-inspectPrefiltersOrTransports", `
var transports={};
var D=/[^\\x20\\t\\r\\n\\f]+/g;
function isFn(x){return typeof x==="function";}
function Ut(o){
  return function(e,t){
    if(typeof e!=="string"){t=e;e="*";}
    var n,r=0,i=e.toLowerCase().match(D)||[];
    if(isFn(t)) while(n=i[r++]) (o[n]=o[n]||[]).push(t);
  };
}
var ajaxTransport=Ut(transports);
function jqExtend(){var i=0,t=arguments[0];if(typeof t==="boolean"){t=arguments[1];i=2;}else{i=1;}for(;i<arguments.length;i++){var src=arguments[i];for(var k in src)t[k]=src[k];}return t;}
var jQuery={};
jQuery.ajaxSettings={xhr:function(){return new XMLHttpRequest();}};
jQuery.ajaxSetup=function(target,settings){return jqExtend(target,jQuery.ajaxSettings,settings);};
function jqEach(e,t){var n,r=0,len=e.length;for(;r<len;r++){if(t.call(e[r],r,e[r])===false)break;}return e;}
function Vt(t,i,o,a){
  var s={},u=t===transports;
  function l(e){
    var r;
    return s[e]=!0,jqEach(t[e]||[],function(e,t){
      var n=t(i,o,a);
      return "string"!=typeof n||u||s[n]?u?!(r=n):void 0:(i.dataTypes.unshift(n),l(n),!1);
    }),r;
  }
  return l(i.dataTypes[0])||!s["*"]&&l("*");
}
var cors=window.someFlag;
ajaxTransport("*",function(options){if(cors||!options.crossDomain){return{send:function(){var xhr=options.xhr();xhr.open(options.type,options.url);}};}});
function ajax(s){var opts=jqExtend(true,{dataTypes:["*"]},jQuery.ajaxSetup({},s));var transport=Vt(transports,opts);transport.send();}
ajax({type:"GET",url:"/api/f8"});
`);

// F9 — F8 + the REAL `Gt` (ajaxExtend) replacing the simple jqExtend-
// based ajaxSetup. jQuery: `function Gt(e,t){var n,r,i=ce.ajaxSettings
// .flatOptions||{};for(n in t)void 0!==t[n]&&((i[n]?e:r||(r={}))[n]=
// t[n]);return r&&ce.extend(!0,e,r),e}` and `ajaxSetup:function(e,t){
// return t?Gt(Gt(e,ce.ajaxSettings),t):Gt(ce.ajaxSettings,e)}`. The
// merge target is chosen per-key by `i[n]?e:r||(r={})` (flatOptions
// indirection) and deep-merged back via `r&&ce.extend(!0,e,r)` (§
// 14.7.5.9 for-in + § 13.14 conditional target + § 20.1.2.1-style deep
// extend). Isolates whether the real option-merge breaks `i.type`/
// `i.url` resolution — the only delta F8 → real `ce.ajax` options
// layer. Red until grounded (no papering).
probe("F9-real-Gt-ajaxExtend-options", `
var transports={};
var D=/[^\\x20\\t\\r\\n\\f]+/g;
function isFn(x){return typeof x==="function";}
function Ut(o){return function(e,t){if(typeof e!=="string"){t=e;e="*";}var n,r=0,i=e.toLowerCase().match(D)||[];if(isFn(t))while(n=i[r++])(o[n]=o[n]||[]).push(t);};}
var ajaxTransport=Ut(transports);
function jqExtend(){var i=0,t=arguments[0];if(typeof t==="boolean"){t=arguments[1];i=2;}else{i=1;}for(;i<arguments.length;i++){var src=arguments[i];for(var k in src)t[k]=src[k];}return t;}
var jQuery={};
jQuery.ajaxSettings={url:"/default",type:"GET",flatOptions:{url:true},xhr:function(){return new XMLHttpRequest();}};
function Gt(e,t){var n,r,fo=jQuery.ajaxSettings.flatOptions||{};for(n in t)void 0!==t[n]&&((fo[n]?e:r||(r={}))[n]=t[n]);return r&&jqExtend(true,e,r),e;}
jQuery.ajaxSetup=function(e,t){return t?Gt(Gt(e,jQuery.ajaxSettings),t):Gt(jQuery.ajaxSettings,e);};
function jqEach(e,t){var n,r=0,len=e.length;for(;r<len;r++){if(t.call(e[r],r,e[r])===false)break;}return e;}
function Vt(t,i,o,a){var s={},u=t===transports;function l(e){var r;return s[e]=!0,jqEach(t[e]||[],function(e,t){var n=t(i,o,a);return "string"!=typeof n||u||s[n]?u?!(r=n):void 0:(i.dataTypes.unshift(n),l(n),!1);}),r;}return l(i.dataTypes[0])||!s["*"]&&l("*");}
var cors=window.someFlag;
ajaxTransport("*",function(options){if(cors||!options.crossDomain){return{send:function(){var xhr=options.xhr();xhr.open(options.type,options.url);}};}});
function ajax(s){var opts=jQuery.ajaxSetup({},s);opts.dataTypes=["*"];var transport=Vt(transports,opts);transport.send();}
ajax({type:"GET",url:"/api/f9"});
`);
