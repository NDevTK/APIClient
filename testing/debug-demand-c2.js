// Step C2 probe: closure-captured OUTER param resolution through the
// demand engine. inner FE's `a` is mk's param (closure capture); `u` is
// the inner FE's own param. Engine must backward-resolve `a` via mk's
// call site and `u` via the inner-FE invocation.
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;" +
  "globalThis._specBuildSlice=_specBuildSlice;" +
  "globalThis._demandSinkSeeds=_demandSinkSeeds;" +
  "globalThis._demandResolve=_demandResolve;" +
  "globalThis._avFlattenStringLeaves=_avFlattenStringLeaves;" +
  "globalThis._pvm=function(){return _specPathValMemo;};").call(globalThis);

function check(label, code, expect) {
  var r = globalThis.analyzeJSBundle(code, "t://c2", true, null);
  var line = label + " → (no arg seed; base folded)";
  globalThis.BabelBundle.traverse(r._ast, {
    Program: function(p) {
      globalThis._specBuildSlice(p);
      var seeds = globalThis._demandSinkSeeds(p)
        .filter(function(s){ return s.kind === "arg" && s.role === "url"; });
      if (!seeds.length) { p.stop(); return; }
      var av = globalThis._demandResolve(seeds[0].argNode, null);
      var leaves = globalThis._avFlattenStringLeaves(av);
      var got = leaves ? leaves.slice().sort() : (av ? "<" + av.kind + ">" : "<none>");
      var ok = Array.isArray(got) && JSON.stringify(got) === JSON.stringify(expect.slice().sort());
      line = (ok ? "PASS " : "FAIL ") + label + " seeds=" + seeds.length +
        " resolved=" + JSON.stringify(got) + " expect=" + JSON.stringify(expect);
      p.stop();
    }
  });
  console.log(line);
}

// C2a: inner FE captures mk's param `a`; invoked separately.
check("C2a closure-captured outer param", `
function mk(a){ return function(u){ fetch(a + u); }; }
mk("/api/")("c2a");
`, ["/api/c2a"]);

// C2b: closure stored then invoked (registry-lite, no array).
check("C2b closure via var", `
function mk(base){ return function(p){ fetch(base + p); }; }
var send = mk("/svc/");
send("c2b");
`, ["/svc/c2b"]);
