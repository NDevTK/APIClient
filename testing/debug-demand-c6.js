// C6/C7 fail-first: atomic reproducers of jQuery 3.x's deeper ajax layers
// beyond V1-V3, probed via __DEMAND_PROBE. Pin where _demandResolve
// bottoms out on each specific layer.
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

// C7a: jQuery deep-extend signature — boolean-first deep flag shifts
// target to arg1, sources to arg2…  `extend(true, {}, settings, opts)`.
run("C7a-deep-extend-boolflag", `
function extend(){
  var i = 0, target = arguments[0], deep = false;
  if (typeof target === "boolean") { deep = target; target = arguments[1] || {}; i = 2; }
  else { i = 1; }
  for (; i < arguments.length; i++) {
    var s = arguments[i];
    for (var k in s) target[k] = s[k];
  }
  return target;
}
var ajaxSettings = { xhr: function(){ return new XMLHttpRequest(); } };
function ajax(o){ var s = extend(true, {}, ajaxSettings, o); var x = s.xhr(); x.open(o.type, o.url); }
ajax({ type: "GET", url: "/api/c7a" });
`);

// C6a: factory uses a closure-var global alias for XMLHttpRequest
// (jQuery wraps in IIFE `(function(g){ ... g.XMLHttpRequest ... })(window)`).
run("C6a-closure-global-xhr", `
(function(g){
  var ajaxSettings = { xhr: function(){ return new g.XMLHttpRequest(); } };
  function ajax(o){ var x = ajaxSettings.xhr(); x.open(o.type, o.url); }
  g.doAjax = function(o){ ajax(o); };
})(window);
window.doAjax({ type: "POST", url: "/api/c6a" });
`);

// C6b: transport-returned send() closure (JQ-6 Ut/Vt) + options.xhr()
// factory together — the actual jQuery transport shape.
run("C6b-transport-send-factory", `
function Ut(o){ return function(e,t){ if(typeof e!=="string"){t=e;e="*";} (o[e]=o[e]||[]).push(t); }; }
function Vt(t,opts){ var a=t["*"]||[]; for(var i=0;i<a.length;i++){ var r=a[i](opts); if(r&&r.send){ r.send(); return; } } }
var _t={}; var reg=Ut(_t);
reg(function(options){ return { send: function(){ var x=options.xhr(); x.open(options.type, options.url); } }; });
function ajax(o){ var s={ xhr:function(){return new XMLHttpRequest();}, type:o.type, url:o.url }; Vt(_t, s); }
ajax({ type:"GET", url:"/api/c6b" });
`);
