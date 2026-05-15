// Instrument _demandDispatchSites ENTRY + seed + carrier transitions.
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var ast = fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8");

// Entry: log whether fnPath resolved.
ast = ast.replace(
  "function _demandDispatchSites(fn) {\n  var out = [];\n  var fnPath = _specFuncPathByNode.get(fn);\n  if (!fnPath) return out;",
  "function _demandDispatchSites(fn) {\n  var out = [];\n  var fnPath = _specFuncPathByNode.get(fn);\n  globalThis.__C3T&&globalThis.__C3T('ENTRY fn@L'+(fn&&fn.loc&&fn.loc.start.line)+' fnPath='+!!fnPath);\n  if (!fnPath) return out;");
// After seeding: log work length + seedName.
ast = ast.replace(
  "  } else {\n    pushVal(fnPath);\n  }",
  "  } else {\n    pushVal(fnPath);\n  }\n  globalThis.__C3T&&globalThis.__C3T('SEED seedName='+seedName+' work='+work.length);");
// Each carrier processed.
ast = ast.replace(
  'while (work.length > 0) {\n    var c = work.pop();\n    if (c.kind === "val") {',
  'while (work.length > 0) {\n    var c = work.pop();\n' +
  '    if(globalThis.__C3T){if(c.kind==="val"){var _n=c.path&&c.path.node;globalThis.__C3T("  val "+(_n&&_n.type)+"@L"+(_n&&_n.loc&&_n.loc.start.line)+" parent="+(c.path&&c.path.parent&&c.path.parent.type));}else if(c.kind==="param"){globalThis.__C3T("  param fn@L"+(c.fn&&c.fn.loc&&c.fn.loc.start.line)+" idx"+c.idx);}else{globalThis.__C3T("  slot decl@L"+(c.decl&&c.decl.loc&&c.decl.loc.start.line)+" key="+JSON.stringify(c.key));}}\n' +
  '    if (c.kind === "val") {');
ast = ast.replace("return out;\n}\n// Program-level fixpoint driver:",
  'globalThis.__C3T&&globalThis.__C3T("EXIT out="+out.length); return out;\n}\n// Program-level fixpoint driver:");

new Function(ast +
  "globalThis.analyzeJSBundle=analyzeJSBundle;" +
  "globalThis._specBuildSlice=_specBuildSlice;" +
  "globalThis._demandDispatchSites=_demandDispatchSites;").call(globalThis);
globalThis.__C3T = function(m){ console.log("[C3] " + m); };

var code = `
function Ut(o){return function(e,t){if(typeof e!=="string"){t=e;e="*";}(o[e]=o[e]||[]).push(t);};}
function Vt(t,opts){var arr=t["*"]||[];for(var i=0;i<arr.length;i++){var r=arr[i](opts);if(r&&r.send){r.send();return;}}}
var _t={};var at=Ut(_t);
at(function(opts){return{send:function(){var x=new XMLHttpRequest();x.open(opts.method,opts.url);}};});
Vt(_t,{url:"/api/jq6",method:"POST"});
`;
var r = globalThis.analyzeJSBundle(code, "t://jq6", true, null);
globalThis.BabelBundle.traverse(r._ast, {
  Program: function(p) {
    globalThis._specBuildSlice(p);
    var factory = null;
    p.traverse({ FunctionExpression: function(fp){ if(fp.node.params[0]&&fp.node.params[0].name==="opts"&&fp.parentPath&&fp.parentPath.isCallExpression()) factory=fp.node; } });
    var s = globalThis._demandDispatchSites(factory);
    console.log("RESULT dispatchSites=" + s.length);
    p.stop();
  }
});
