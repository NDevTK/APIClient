var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");
var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode +
  "globalThis.analyzeJSBundle = analyzeJSBundle;\n" +
  "globalThis._pathValMemo = function(){ return _specPathValMemo; };\n").call(globalThis);

function dumpAv(av, d) {
  d = d || 0;
  if (d > 4) return "...";
  if (!av) return "null";
  if (av.kind === "const") return JSON.stringify(av.value);
  if (av.kind === "or") return "or(" + dumpAv(av.left, d+1) + "," + dumpAv(av.right, d+1) + ")";
  if (av.kind === "top") return "TOP";
  return av.kind;
}

function runTest(name, code) {
  console.log("\n=== " + name + " ===");
  var r = globalThis.analyzeJSBundle(code, "test://" + name, true, null);
  console.log("fetchCallSites:", JSON.stringify((r.fetchCallSites||[]).map(function(s){return s.url})));
  var memo = globalThis._pathValMemo();
  globalThis.BabelBundle.traverse(r._ast, {
    Identifier: function(p) {
      if (p.node.name === "picked" && p.parent && p.parent.type === "CallExpression" && p.parent.callee.name === "fetch") {
        console.log("  picked@fetch AV:", dumpAv(memo.get(p.node)));
      }
    }
  });
}

runTest("T1: simple for, assign picked in body",
  "var picked = null;\n" +
  "for (var i = 0; i < 2; i++) {\n" +
  "  picked = '/api/T1';\n" +
  "}\n" +
  "fetch(picked);\n");

runTest("T2: simple for + if(true), assign in if",
  "var picked = null;\n" +
  "for (var i = 0; i < 2; i++) {\n" +
  "  if (true) picked = '/api/T2';\n" +
  "}\n" +
  "fetch(picked);\n");

runTest("T3: nested for, assign + break outer",
  "var picked = null;\n" +
  "outer: for (var i = 0; i < 2; i++) {\n" +
  "  for (var j = 0; j < 2; j++) {\n" +
  "    if (i === 0 && j === 1) {\n" +
  "      picked = '/api/T3';\n" +
  "      break outer;\n" +
  "    }\n" +
  "  }\n" +
  "}\n" +
  "fetch(picked);\n");
