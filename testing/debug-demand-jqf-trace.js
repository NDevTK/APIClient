// Trace the faithful jQuery-internals composition (atomic general ECMA
// shape: curried-closure registrar + registry + dispatch + deep-extend
// merge + 3-hop param chain + xhr factory) to pin exactly where the
// uniform (U-member)/(U-call) ECMA rules bottom out after the shortcut
// deletion. This is a GENERAL composition, not jQuery bytes — the
// "similar-shape pinned" regression test. Goal: resolve xhr.open's
// url/method via correct ECMA semantics (§10.2.10 FDI param←arg through
// the dispatch chain, §20.1.2.1 extend-merge effect, §13.3.6 call).
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;" +
  "globalThis._specBuildSlice=_specBuildSlice;" +
  "globalThis._demandSinkSeeds=_demandSinkSeeds;" +
  "globalThis._demandResolve=_demandResolve;" +
  "globalThis._demandResolveCalleeSink=_demandResolveCalleeSink;" +
  "globalThis._avFlattenStringLeaves=_avFlattenStringLeaves;" +
  "globalThis._pvm=function(){return _specPathValMemo;};").call(globalThis);

var code = [
  "var transports = {};",
  "function addTo(structure){ return function(dataTypeExpr, func){ if (typeof dataTypeExpr !== 'string') { func = dataTypeExpr; dataTypeExpr = '*'; } (structure[dataTypeExpr] = structure[dataTypeExpr] || []).push(func); }; }",
  "var ajaxTransport = addTo(transports);",
  "function inspect(structure, options){ var list = structure['*'] || []; for (var i = 0; i < list.length; i++) { var r = list[i](options); if (r && r.send) return r; } }",
  "function jqExtend(){ var i = 0, t = arguments[0]; if (typeof t === 'boolean') { t = arguments[1]; i = 2; } else { i = 1; } for (; i < arguments.length; i++) { var src = arguments[i]; for (var k in src) t[k] = src[k]; } return t; }",
  "var ajaxSettings = { xhr: function(){ return new XMLHttpRequest(); } };",
  "ajaxTransport('*', function(options){ return { send: function(){ var xhr = options.xhr(); xhr.open(options.type, options.url); } }; });",
  "function ajax(s){ var opts = jqExtend(true, {}, ajaxSettings, s); var transport = inspect(transports, opts); transport.send(); }",
  "ajax({ type: 'GET', url: '/api/jqf' });"
].join("\n");

var r = globalThis.analyzeJSBundle(code, "t://jqf", true, null);
globalThis.BabelBundle.traverse(r._ast, {
  Program: function (p) {
    globalThis._specBuildSlice(p);
    var seeds = globalThis._demandSinkSeeds(p);
    console.log("SEEDS=" + seeds.length + " " +
      JSON.stringify(seeds.map(function (s) { return s.kind + ":" + (s.role || "") + (s.sink ? "(" + s.sink + ")" : ""); })));
    // Directly resolve the deep-extend merge result `opts` and its
    // `.xhr` projection — the ECMA capability the faithful shape needs
    // and JQ-6 does NOT exercise (JQ-6 has no jqExtend).
    var jqExtendCall = null, optsId = null, optsXhr = null;
    p.traverse({
      CallExpression: function (cp) {
        var c = cp.node.callee;
        if (c && c.type === "Identifier" && c.name === "jqExtend") jqExtendCall = cp.node;
      },
      MemberExpression: function (mp) {
        if (mp.node.object && mp.node.object.type === "Identifier" &&
            mp.node.object.name === "options" && mp.node.property &&
            mp.node.property.name === "xhr") optsXhr = mp.node;
      }
    });
    if (jqExtendCall) {
      var je = globalThis._demandResolve(jqExtendCall, null);
      console.log("jqExtend(...) -> " + (je ? je.kind : "<none>") +
        (je && je.kind === "obj-lit" ? " props=" + JSON.stringify(Object.keys(je.props || {})) : ""));
    }
    if (optsXhr) {
      var ox = globalThis._demandResolve(optsXhr, null);
      console.log("options.xhr -> " + (ox ? ox.kind : "<none>"));
    }
    seeds.forEach(function (s, idx) {
      if (s.kind === "callee") {
        var ce = s.callNode.callee;
        console.log("callee[" + idx + "] " + (ce && ce.type) +
          (ce && ce.object ? " obj=" + ce.object.type + " ." + (ce.property && (ce.property.name || ce.property.value)) : ""));
        // Resolve the receiver (xhr) backward — the crux of the chain.
        if (ce && ce.object) {
          globalThis.__DEMAND_UC_TRACE = true;
          var recv = globalThis._demandResolve(ce.object, null);
          globalThis.__DEMAND_UC_TRACE = false;
          console.log("  recv(" + ce.object.type + ") -> " + (recv ? recv.kind : "<none>"));
        }
        var hit = globalThis._demandResolveCalleeSink(ce);
        console.log("  _demandResolveCalleeSink -> " + (hit ? JSON.stringify(hit) : "null"));
      } else if (s.kind === "arg") {
        var av = globalThis._demandResolve(s.argNode, null);
        console.log("arg[" + idx + "] " + s.role + " -> " + (av ? av.kind : "<none>") +
          " leaves=" + JSON.stringify(globalThis._avFlattenStringLeaves(av)));
      }
    });
    p.stop();
  }
});
