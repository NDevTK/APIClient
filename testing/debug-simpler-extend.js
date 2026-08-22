var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
globalThis._DEBUG_CTX_FIND = true;
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode +
  "globalThis.analyzeJSBundle = analyzeJSBundle;\n" +
  "globalThis._pathValMemo = function(){ return _specPathValMemo; };\n" +
  "globalThis._ctxBySite = function(){ return _specCtxEffectsBySite; };\n" +
  "globalThis._effectsMemo = function(){ return _specEffectsMemo; };\n").call(globalThis);

var code = process.argv[2] === "toplevel" ? `
function extend(target, src) {
  for (var k in src) target[k] = src[k];
  return target;
}
function ajax(opts) {
  var s = {};
  extend(s, opts);
  var xhr = new XMLHttpRequest();
  xhr.open("GET", s.url);
  xhr.send();
}
ajax({url: "/api/toplevel-extend"});
` : process.argv[2] === "direct"
? `
(function() {
  function ajax(opts) {
    var xhr = new XMLHttpRequest();
    xhr.open("GET", opts.url);
    xhr.send();
  }
  window.lib3 = { ajax: ajax };
})();
lib3.ajax({url: "/api/direct"});
`
: `
(function() {
  function extend(target, src) {
    for (var k in src) target[k] = src[k];
    return target;
  }
  function ajax(opts) {
    var s = {};
    extend(s, opts);
    var xhr = new XMLHttpRequest();
    xhr.open("GET", s.url);
    xhr.send();
  }
  window.lib4 = { ajax: ajax };
})();
lib4.ajax({url: "/api/simpler-extend"});
`;
var result = globalThis.analyzeJSBundle(code, "test://simpler-extend", true, null);
console.log("fetchCallSites:", (result.fetchCallSites || []).length);
(result.fetchCallSites || []).forEach(function (s, i) {
  console.log("  [" + i + "]", s.method, s.url);
});

var ctxBy = globalThis._ctxBySite();
var memo = globalThis._pathValMemo();
var ctxCount = 0;
globalThis.BabelBundle.traverse(result._ast, {
  CallExpression: function(p) {
    if (ctxBy.has(p.node)) {
      ctxCount++;
      var entry = ctxBy.get(p.node);
      var calleeName = "?";
      if (p.node.callee.type === "Identifier") calleeName = p.node.callee.name;
      else if (p.node.callee.type === "MemberExpression" && p.node.callee.property && p.node.callee.property.name) {
        calleeName = "." + p.node.callee.property.name;
      }
      console.log("  ctx-refined site loc:" + (p.node.loc ? p.node.loc.start.line : "?") +
        " callee:" + calleeName +
        " fn.id:" + (entry.fn && entry.fn.id ? entry.fn.id.name : "(anon)") +
        " effects.len:" + (entry.effects ? entry.effects.length : "?"));
    }
  }
});
console.log("Total ctx-refined sites:", ctxCount);
