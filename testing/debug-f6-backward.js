// F6b vs F6c backward/points-to diagnostic (faithful — regex escaped
// exactly as testing/debug-jq-quirks.js's template literals, no heredoc
// mangling). Both register a transport via the real curried `Ut`
// registrar; the ONLY delta is the registry KEY:
//   F6c  key = INLINE `(e.toLowerCase().match(D)||[])[0]`   → PASSES
//   F6b  key via locals `var i=...; var n=i[0]; o[n]`        → FAILS
// Reports, for each: sites, whether the transport factory's points-to
// is recorded under key "*" (_specPointsToObjPropsReverseGet), and the
// backward demand of the xhr.open args — pinning which subsystem drops
// the factory when the key flows through an intermediate local.
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;" +
  "globalThis._specBuildSlice=_specBuildSlice;" +
  "globalThis._demandSinkSeeds=_demandSinkSeeds;" +
  "globalThis._demandResolve=_demandResolve;" +
  "globalThis._demandResolveCalleeSink=_demandResolveCalleeSink;" +
  "globalThis._specPointsToObjPropsReverseGet=_specPointsToObjPropsReverseGet;" +
  "globalThis._djc=function(){_demandJump=new Map();_demandProvisional=new Set();};").call(globalThis);

function body(keyStmt, url) {
  return "\nvar transports={};\nvar D=/[^\\x20\\t\\r\\n\\f]+/g;\n" +
    "function isFn(x){return typeof x===\"function\";}\n" +
    "function Ut(o){return function(e,t){if(typeof e!==\"string\"){t=e;e=\"*\";}" + keyStmt + "};}\n" +
    "var ajaxTransport=Ut(transports);\n" +
    "function jqExtend(){var i=0,t=arguments[0];if(typeof t===\"boolean\"){t=arguments[1];i=2;}else{i=1;}for(;i<arguments.length;i++){var src=arguments[i];for(var k in src)t[k]=src[k];}return t;}\n" +
    "var jQuery={};\njQuery.ajaxSettings={xhr:function(){return new XMLHttpRequest();}};\n" +
    "jQuery.ajaxSetup=function(target,settings){return jqExtend(target,jQuery.ajaxSettings,settings);};\n" +
    "function jqEach(arr,cb){var k=0,n=arr.length;for(;k<n;k++){if(cb.call(arr[k],k,arr[k])===false)break;}return arr;}\n" +
    "function inspect(structure,options){function inspectFn(dt){var selected;jqEach(structure[dt]||[],function(idx,fn){var r=fn(options);if(r&&r.send){selected=r;return false;}});return selected;}return inspectFn(options.dataTypes[0])||inspectFn(\"*\");}\n" +
    "var cors=window.someFlag;\n" +
    "ajaxTransport(\"*\",function(options){if(cors||!options.crossDomain){return{send:function(){var xhr=options.xhr();xhr.open(options.type,options.url);}};}});\n" +
    "function ajax(s){var opts=jqExtend(true,{dataTypes:[\"*\"]},jQuery.ajaxSetup({},s));var transport=inspect(transports,opts);transport.send();}\n" +
    "ajax({type:\"GET\",url:\"" + url + "\"});\n";
}
var F6c = body("if(isFn(t))(o[(e.toLowerCase().match(D)||[])[0]]=o[(e.toLowerCase().match(D)||[])[0]]||[]).push(t);", "/api/f6c");
var F6b = body("var i=e.toLowerCase().match(D)||[];var n=i[0];if(isFn(t))(o[n]=o[n]||[]).push(t);", "/api/f6b");

function dump(av, d) {
  d = d || 0; if (!av || d > 5) return av ? av.kind : "<none>";
  if (av.kind === "or" || av.kind === "logical") return av.kind + "(" + dump(av.left, d + 1) + "|" + dump(av.right, d + 1) + ")";
  if (av.kind === "obj-lit") return "{" + Object.keys(av.props || {}).join(",") + "}";
  if (av.kind === "array-lit") return "[" + (av.elements || []).length + "]";
  if (av.kind === "call") return "call(" + (av.callee ? av.callee.kind : "?") + ")";
  if (av.kind === "member") return "member(" + dump(av.obj, d + 1) + ")";
  if (av.kind === "str" || av.kind === "const") return JSON.stringify(av.value);
  return av.kind;
}

function probe(tag, code) {
  console.log("\n=== " + tag + " ===");
  var r = globalThis.analyzeJSBundle(code, "https://ex.com/app", "https://ex.com", null);
  console.log("sites=" + JSON.stringify((r.fetchCallSites || []).map(function (s) { return s.method + " " + s.url; })));
  globalThis.BabelBundle.traverse(r._ast, {
    Program: function (p) {
      globalThis._specBuildSlice(p);
      // The transport factory is the 2nd arg of ajaxTransport("*", fn).
      var factoryNode = null;
      p.traverse({
        CallExpression: function (cp) {
          var c = cp.node.callee;
          if (c && c.type === "Identifier" && c.name === "ajaxTransport" &&
              cp.node.arguments.length === 2 &&
              (cp.node.arguments[1].type === "FunctionExpression")) factoryNode = cp.node.arguments[1];
        }
      });
      if (factoryNode) {
        var rev = globalThis._specPointsToObjPropsReverseGet(factoryNode);
        console.log("factory points-to reverse: " + (rev ? JSON.stringify(rev.map ? rev.map(function (e) { return e.key; }) : Array.from(rev).map(function (e) { return e.key; })) : "<null>"));
      } else console.log("factory node NOT FOUND");
      var seeds = globalThis._demandSinkSeeds(p);
      var openCS = seeds.filter(function (s) {
        var c = s.callNode && s.callNode.callee;
        return s.kind === "callee" && c && c.type === "MemberExpression" && c.property && c.property.name === "open";
      });
      if (openCS.length) {
        var cn = openCS[0].callNode;
        globalThis._djc();
        var sink = globalThis._demandResolveCalleeSink(cn.callee);
        console.log("_demandResolveCalleeSink(xhr.open)=" + (sink ? sink.id : "<null>"));
        globalThis._djc();
        console.log("arg1 url → " + dump(globalThis._demandResolve(cn.arguments[1], null), 0));
      } else console.log("no open sink seed");
      p.stop();
    }
  });
}
probe("F6c (PASS expected)", F6c);
probe("F6b (FAIL expected)", F6b);
