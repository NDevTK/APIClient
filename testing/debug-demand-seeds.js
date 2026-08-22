// Step A verification: _demandSinkSeeds emits a seed ONLY for sink args
// the base k=0 pass left non-concrete. Expected: JQ-6 ≥1, jQuery $.get ≥1,
// innerHTML=location.search =1, fully-static fetch =0.
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;" +
  "globalThis._demandSinkSeeds=_demandSinkSeeds;" +
  "globalThis._specBuildSlice=_specBuildSlice;").call(globalThis);

function seedsFor(label, code, expectMin, expectMax) {
  var seeds = [];
  // analyzeJSBundle runs the base fixpoint; capture the program path via a
  // post-analysis traversal and run the scanner against the populated memo.
  var r = globalThis.analyzeJSBundle(code, "t://seed", true, null);
  globalThis.BabelBundle.traverse(r._ast, {
    Program: function(p) {
      globalThis._specBuildSlice(p);
      seeds = globalThis._demandSinkSeeds(p);
      p.stop();
    }
  });
  var argSeeds = seeds.filter(function(s){ return s.kind === "arg"; });
  var dispatchSeeds = seeds.filter(function(s){ return s.kind === "callee"; });
  var sig = argSeeds.map(function(s){ return s.sink + ":" + s.role + "@arg" + s.argIndex; });
  var ok = seeds.length >= expectMin && (expectMax == null || seeds.length <= expectMax);
  console.log((ok ? "PASS " : "FAIL ") + label + " → " + seeds.length +
    " seeds (" + argSeeds.length + " arg " + JSON.stringify(sig) +
    ", " + dispatchSeeds.length + " dispatch)" +
    " (expect " + expectMin + (expectMax != null ? ".." + expectMax : "+") + ")");
}

// 1. Fully-static fetch — URL is a literal → 0 seeds.
seedsFor("static fetch", 'fetch("/api/static");', 0, 0);

// 2. Computed fetch URL — non-concrete → ≥1 seed.
seedsFor("computed fetch", 'var p=location.search; fetch("/api/x?"+p);', 1, 2);

// 3. innerHTML = location.search style (DOM-XSS sink, non-concrete).
seedsFor("html sink", 'document.body.insertAdjacentHTML("beforeend", location.hash);', 1, 1);

// 4. JQ-6 atomic curried-registry transport dispatch.
seedsFor("JQ-6", `
function Ut(o){return function(e,t){if(typeof e!=="string"){t=e;e="*";}(o[e]=o[e]||[]).push(t);};}
function Vt(t,opts){var arr=t["*"]||[];for(var i=0;i<arr.length;i++){var r=arr[i](opts);if(r&&r.send){r.send();return;}}}
var _t={};var at=Ut(_t);
at(function(opts){return{send:function(){var x=new XMLHttpRequest();x.open(opts.method,opts.url);}};});
Vt(_t,{url:"/api/jq6",method:"POST"});
`, 1, 4);

// 5. Real jQuery $.get — transport xhr.open url non-concrete in base pass.
var jq = fs.readFileSync(path.join(rd, "testing/harness-dumps/jquery-3.7.1.min.js"), "utf8");
seedsFor("real jQuery $.get", jq + '\njQuery.get("/api/profile");', 1, null);
