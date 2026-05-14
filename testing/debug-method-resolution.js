var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode +
  "globalThis.analyzeJSBundle = analyzeJSBundle;\n" +
  "globalThis._pathValMemo = function(){ return _specPathValMemo; };\n").call(globalThis);

var code =
  'var jq = {};\n' +
  'var transports = [];\n' +
  'jq.ajaxTransport = function(fn) { transports.push(fn); };\n' +
  'jq.ajaxTransport(function(opts){ fetch(opts.url); });\n';

var r = globalThis.analyzeJSBundle(code, "test://x", true, null);
var memo = globalThis._pathValMemo();

globalThis.BabelBundle.traverse(r._ast, {
  CallExpression: function(p) {
    if (p.node.callee.type === "MemberExpression" && p.node.callee.property.name === "ajaxTransport") {
      console.log("call site at L"+p.node.loc.start.line);
      console.log("  callee AV:", JSON.stringify(memo.get(p.node.callee), function(k,v){if(k==='funcNode'||k==='node'||k==='loc'||k==='_extraProps') return undefined; return v;}));
      console.log("  callee.object AV:", JSON.stringify(memo.get(p.node.callee.object), function(k,v){if(k==='funcNode'||k==='node'||k==='loc'||k==='_extraProps') return v && v.kind || '...'; return v;}, 2));
    }
  }
});
