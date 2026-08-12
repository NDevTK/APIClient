// Verify the merge-member piece in isolation: _demandResolve on the
// `m.xhr` MemberExpression node itself should now resolve to the factory
// fn-ref via _demandMergeMember (extend({},as).xhr ⇒ as.xhr).
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;" +
  "globalThis._specBuildSlice=_specBuildSlice;" +
  "globalThis._demandIndexPaths=_demandIndexPaths;" +
  "globalThis._demandMergeMember=_demandMergeMember;" +
  "globalThis._demandResolve=_demandResolve;" +
  "globalThis._pvm=function(){return _specPathValMemo;};").call(globalThis);

var code = `
function extend(t, s){ for (var k in s) t[k] = s[k]; return t; }
var as = { xhr: function(){ return new XMLHttpRequest(); } };
function ajax(o){ var m = extend({}, as); var x = m.xhr(); x.open(o.type, o.url); }
ajax({ type: "GET", url: "/api/v1" });
`;
var r = globalThis.analyzeJSBundle(code, "t://c5m", true, null);
globalThis.BabelBundle.traverse(r._ast, {
  Program: function(p) {
    globalThis._specBuildSlice(p);
    globalThis._demandIndexPaths(p);
    var mXhrMember = null, mXhrCall = null;
    p.traverse({
      MemberExpression: function(mp) {
        if (mp.node.object && mp.node.object.name === "m" &&
            mp.node.property && mp.node.property.name === "xhr") mXhrMember = mp.node;
      },
      CallExpression: function(cp) {
        if (cp.node.callee && cp.node.callee.type === "MemberExpression" &&
            cp.node.callee.object && cp.node.callee.object.name === "m") mXhrCall = cp.node;
      }
    });
    console.log("m.xhr member found?", !!mXhrMember);
    var mm = mXhrMember ? globalThis._demandMergeMember(mXhrMember) : null;
    console.log("_demandMergeMember(m.xhr) =", mm ?
      "{key=" + mm.key + ", argNodes=[" + mm.argNodes.map(function(a){ return a.type; }).join(",") + "]}" : "null");
    var dr = mXhrMember ? globalThis._demandResolve(mXhrMember, null) : null;
    console.log("_demandResolve(m.xhr) =", dr ? dr.kind +
      (dr.kind === "function-ref" ? "@L" + (dr.funcNode && dr.funcNode.loc.start.line) : "") : "<none>");
    p.stop();
  }
});
