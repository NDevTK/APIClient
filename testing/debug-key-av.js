// Pin the EXACT AV the real-Ut registry key resolves to. F6b isolated
// the gap to `n = (e.toLowerCase().match(D)||[])[0]` (D=/[^\x20...]+/g)
// not connecting `o[n].push(factory)` ⇄ `structure["*"]` read. Resolve
// each sub-expression's AV via the forward spec memo to see which hop
// degrades: toLowerCase → match(scope-var regex) → ||[] → [0].
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;" +
  "globalThis._specPathValMemo=function(){return _specPathValMemo;};").call(globalThis);

function dump(av, d) {
  d = d || 0; if (!av || d > 6) return av ? av.kind : "<none>";
  if (av.kind === "or" || av.kind === "logical") return av.kind + "(" + dump(av.left, d + 1) + "|" + dump(av.right, d + 1) + ")";
  if (av.kind === "obj-lit") return "{" + Object.keys(av.props || {}).join(",") + "}";
  if (av.kind === "array-lit") return "[" + (av.elements || []).map(function (e) { return dump(e, d + 1); }).join(",") + "]";
  if (av.kind === "call") return "call(" + (av.callee ? av.callee.kind : "?") + ")";
  if (av.kind === "member") return "member(" + dump(av.obj, d + 1) + "." + (av.key && (av.key.kind === "str" ? JSON.stringify(av.key.value) : av.key.kind)) + ")";
  if (av.kind === "str" || av.kind === "const") return JSON.stringify(av.value);
  if (av.kind === "coerce") return "coerce(" + dump(av.arg, d + 1) + ")";
  return av.kind;
}

var code = [
  'var D = /[^\\x20\\t\\r\\n\\f]+/g;',
  'var e = "*";',
  'var lc = e.toLowerCase();',          // expect "*"
  'var mm = lc.match(D);',              // expect array-lit(["*"])
  'var mo = mm || [];',                 // expect or(array-lit(["*"]) | array-lit([]))
  'var n0 = mo[0];',                    // expect "*"
  'var inlineMatch = "*".match(/[^\\x20\\t\\r\\n\\f]+/g);',  // inline-regex control
  'var inline0 = (inlineMatch || [])[0];',
  'fetch("/k/" + n0);',
  'fetch("/inline/" + inline0);'
].join("\n");

var r = globalThis.analyzeJSBundle(code, "https://ex.com/app", "https://ex.com", null);
console.log("sites=" + JSON.stringify((r.fetchCallSites || []).map(function (s) { return s.url; })));

var memo = globalThis._specPathValMemo();
var want = { D: 1, lc: 1, mm: 1, mo: 1, n0: 1, inlineMatch: 1, inline0: 1 };
globalThis.BabelBundle.traverse(r._ast, {
  VariableDeclarator: function (p) {
    var nm = p.node.id && p.node.id.name;
    if (nm && want[nm] && p.node.init) {
      var av = memo.get(p.node.init);
      console.log("  " + nm + " = " + dump(av, 0));
    }
  }
});
