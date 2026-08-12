// Step B verification. Only cases where the BASE pass leaves the sink arg
// non-concrete (seed emitted) actually exercise _demandResolve; cases the
// base forward pass already folds (seeds=0) are reported as BASE-OK (the
// engine is correctly not needed there). A FAIL is: an arg seed exists but
// _demandResolve does not recover the real value.
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;" +
  "globalThis._specBuildSlice=_specBuildSlice;" +
  "globalThis._demandSinkSeeds=_demandSinkSeeds;" +
  "globalThis._demandResolve=_demandResolve;" +
  "globalThis._avFlattenStringLeaves=_avFlattenStringLeaves;").call(globalThis);

function check(label, code, expectLeaves) {
  var r = globalThis.analyzeJSBundle(code, "t://b", true, null);
  var line = label;
  globalThis.BabelBundle.traverse(r._ast, {
    Program: function(p) {
      globalThis._specBuildSlice(p);
      var seeds = globalThis._demandSinkSeeds(p);
      var argSeeds = seeds.filter(function(s){ return s.kind === "arg" &&
        (s.role === "url" || s.role === "html" || s.role === "eval"); });
      if (argSeeds.length === 0) { line += " → BASE-OK (no arg seed; base folded it)"; p.stop(); return; }
      var av = globalThis._demandResolve(argSeeds[0].argNode, null);
      var leaves = globalThis._avFlattenStringLeaves(av);
      var got = leaves ? leaves.slice().sort() : (av ? "<" + av.kind + ">" : "<none>");
      var exp = expectLeaves.slice().sort();
      var ok = Array.isArray(got) && JSON.stringify(got) === JSON.stringify(exp);
      line = (ok ? "PASS " : "FAIL ") + label + " seeds=" + argSeeds.length +
        " resolved=" + JSON.stringify(got) + " expect=" + JSON.stringify(exp);
      p.stop();
    }
  });
  console.log(line);
}

// Shapes that tend to defeat base forward folding (HOF / array-of-fn /
// conditional callee) so a real arg seed is produced for the engine.
check("B-HOF callback", `
function each(a,cb){ for(var i=0;i<a.length;i++) cb(a[i]); }
function send(u){ fetch(u); }
each(["/api/hof"], send);
`, ["/api/hof"]);

check("B-array-of-fn", `
function send(u){ fetch(u); }
var fns=[send];
fns[0]("/api/arr");
`, ["/api/arr"]);

check("B-indirect-binding", `
function a(u){ fetch(u); }
var pick = a;
pick("/api/cond");
`, ["/api/cond"]);

check("B-return-fn", `
function mk(){ return function(u){ fetch(u); }; }
mk()("/api/retfn");
`, ["/api/retfn"]);

check("B-2hop-param", `
function inner(z){ fetch(z); }
function outer(w){ inner(w); }
[outer][0]("/api/2hop");
`, ["/api/2hop"]);
