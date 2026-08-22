// R1 fail-first diagnostic. Measures the PURE general spec-eval path
// (base memo + caller-arg backward + _specInstantiateAv composition) in
// isolation by suppressing the bespoke demand edges via
// __DEMAND_NO_EDGES. For every sink-seed arg in a corpus of shapes it
// prints: base AV kind, edges-ON resolved leaves (today's behaviour),
// edges-OFF resolved leaves (the general engine alone). Where OFF≠ON the
// delta is exactly the GENERAL ECMA rule R1 must strengthen — NOT a
// reason to keep the overfit edge.
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;" +
  "globalThis._specBuildSlice=_specBuildSlice;" +
  "globalThis._demandSinkSeeds=_demandSinkSeeds;" +
  "globalThis._demandResolve=_demandResolve;" +
  "globalThis._avFlattenStringLeaves=_avFlattenStringLeaves;" +
  "globalThis._demandJumpClear=function(){_demandJump=new Map();};" +
  "globalThis._pvm=function(){return _specPathValMemo;};").call(globalThis);

// Inlined taint-source leaf walk (mirrors the nested _demandTaintLeaves
// in the __DEMAND_PROBE hook — not a top-level export).
function taintLeaves(rootAv) {
  var ids = [], st = rootAv ? [rootAv] : [], sn = new Set();
  while (st.length > 0) {
    var x = st.pop();
    if (!x || typeof x !== "object" || sn.has(x)) continue;
    sn.add(x);
    if (x.kind === "taint-source") { ids.push(x.id || "taint"); continue; }
    if (x.kind === "or" || x.kind === "logical") { st.push(x.left); st.push(x.right); }
    else if (x.kind === "binop") { st.push(x.left); st.push(x.right); }
    else if (x.kind === "coerce") { st.push(x.arg); }
    else if (x.kind === "member") { st.push(x.obj); }
    else if (x.kind === "template" && x.exprs) { for (var ti = 0; ti < x.exprs.length; ti++) st.push(x.exprs[ti]); }
  }
  return ids;
}
function leavesOf(av) {
  var s = [];
  try { s = globalThis._avFlattenStringLeaves(av) || []; } catch (e) {}
  var t = [];
  try { t = taintLeaves(av) || []; } catch (e) {}
  return { str: s, taint: t };
}

function probe(label, code, jq) {
  console.log("\n=== " + label + " ===");
  var src = jq ? (jq + "\n" + code) : code;
  var r;
  try { r = globalThis.analyzeJSBundle(src, "t://" + label, true, null); }
  catch (e) { console.log("  analyze threw: " + e.message); return; }
  globalThis.BabelBundle.traverse(r._ast, {
    Program: function (p) {
      globalThis._specBuildSlice(p);
      var seeds = globalThis._demandSinkSeeds(p).filter(function (s) { return s.kind === "arg"; });
      if (seeds.length === 0) { console.log("  (no arg seeds — base pass resolved everything)"); p.stop(); return; }
      seeds.forEach(function (s, si) {
        var bAv = globalThis._pvm().get(s.argNode);
        console.log("  seed[" + si + "] " + s.sink + ":" + s.role +
          " base=" + (bAv ? bAv.kind : "<none>"));
        // edges ON (production behaviour today)
        globalThis.__DEMAND_NO_EDGES = false;
        globalThis._demandJumpClear();
        var on = leavesOf(globalThis._demandResolve(s.argNode, null));
        // edges OFF (pure general spec-eval path)
        globalThis.__DEMAND_NO_EDGES = true;
        globalThis._demandJumpClear();
        var off = leavesOf(globalThis._demandResolve(s.argNode, null));
        globalThis.__DEMAND_NO_EDGES = false;
        var onStr = JSON.stringify(on.str), offStr = JSON.stringify(off.str);
        var onT = JSON.stringify(on.taint), offT = JSON.stringify(off.taint);
        var same = onStr === offStr && onT === offT;
        console.log("    ON  str=" + onStr + " taint=" + onT);
        console.log("    OFF str=" + offStr + " taint=" + offT +
          (same ? "   [GENERAL OK]" : "   [DELTA — general gap]"));
      });
      p.stop();
    }
  });
}

// --- Corpus: pure-general shapes (must be [GENERAL OK]) ---
probe("G-param-backward", 'function f(u){fetch(u);} f("/api/g1");');
probe("G-concat", 'function f(u){fetch("/api/x?q="+u);} f(location.search);');
probe("X1-locsearch-html", 'document.body.insertAdjacentHTML("beforeend", location.search);');
probe("X2-param-backward-xss",
  'function render(h){document.body.insertAdjacentHTML("beforeend",h);} render(location.hash);');
probe("X3-concat-taint",
  'function render(h){document.body.insertAdjacentHTML("beforeend","<div>"+h+"</div>");} render(location.search);');
probe("X4-eval-param", 'function go(c){eval(c);} go(location.hash);');

// --- Corpus: shapes that today need a bespoke edge ---
probe("JQ6-curried-registry", `
function Ut(o){return function(e,t){if(typeof e!=="string"){t=e;e="*";}(o[e]=o[e]||[]).push(t);};}
function Vt(t,opts){var arr=t["*"]||[];for(var i=0;i<arr.length;i++){var r=arr[i](opts);if(r&&r.send){r.send();return;}}}
var _t={};var at=Ut(_t);
at(function(opts){return{send:function(){var x=new XMLHttpRequest();x.open(opts.method,opts.url);}};});
Vt(_t,{url:"/api/jq6",method:"POST"});
`);
probe("JQ-faithful", `
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
ajaxTransport("*", function(options){
  return { send: function(){ var xhr = options.xhr(); xhr.open(options.type, options.url); } };
});
function ajax(s){ var opts = jqExtend(true, {}, ajaxSettings, s); var transport = inspect(transports, opts); transport.send(); }
ajax({ type: "GET", url: "/api/jqfaithful" });
`);

// --- Real jQuery 3.7.1 ---
var jq = fs.readFileSync(path.join(rd, "testing/harness-dumps/jquery-3.7.1.min.js"), "utf8");
probe("real-jQuery-$.get", 'jQuery.get("/api/profile");', jq);
