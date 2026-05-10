// testing/test-xss.js — XSS taint-tracing tests verifying the analyzer
// produces sink findings in the format the popup expects.
//
// Each test exercises a known DOM-XSS / open-redirect / postMessage
// pattern and asserts:
//   1. `result.sinks` contains the expected entry
//   2. Entry has `source`, `sink`, `type`, `taintPath` (popup-required)
//   3. `taintPath[0].kind === "source"` (popup taint chain rendering)
//   4. `severity` is set
//
// Run: node testing/test-xss.js

var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");

var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();

var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode + "\nglobalThis.analyzeJSBundle = analyzeJSBundle;").call(globalThis);

var passed = 0, failed = 0, total = 0;

function xss(name, code, sinkCheck) {
  total++;
  try {
    var result = analyzeJSBundle(code, "test://" + name, true, null);
    var sinks = result.securitySinks || [];
    var ok = sinkCheck(sinks, result);
    if (ok) {
      passed++;
      console.log("  PASS: " + name);
    } else {
      failed++;
      console.log("  FAIL: " + name);
      console.log("    sinks=" + JSON.stringify(sinks.map(function(s) {
        return { type: s.type, sink: s.sink, source: s.source, severity: s.severity,
                 taintPathLen: s.taintPath ? s.taintPath.length : 0,
                 firstHop: s.taintPath && s.taintPath[0] ? s.taintPath[0].kind + ":" + s.taintPath[0].desc : null };
      })).slice(0, 800));
    }
  } catch (e) {
    failed++;
    console.log("  ERROR: " + name + " — " + e.message);
  }
}

// Helper: assert sink has popup-required fields.
function assertPopupShape(s) {
  if (!s) return false;
  if (typeof s.source !== "string" || !s.source) return false;
  if (typeof s.sink !== "string" || !s.sink) return false;
  if (typeof s.type !== "string" || !s.type) return false;
  if (!s.severity) return false;
  if (!Array.isArray(s.taintPath)) return false;
  if (s.taintPath.length === 0) return false;
  var first = s.taintPath[0];
  if (!first || first.kind !== "source") return false;
  return true;
}

console.log("\n=== DOM XSS sinks ===\n");

xss("innerHTML <- location.hash", `
  document.body.innerHTML = location.hash.slice(1);
`, function(sinks) {
  for (var i = 0; i < sinks.length; i++) {
    var s = sinks[i];
    if (s.sink === "innerHTML" && s.source === "location.hash" && assertPopupShape(s)) return true;
  }
  return false;
});

xss("innerHTML <- location.search", `
  var x = location.search;
  document.getElementById("box").innerHTML = x;
`, function(sinks) {
  for (var i = 0; i < sinks.length; i++) {
    var s = sinks[i];
    if (s.sink === "innerHTML" && s.source === "location.search" && assertPopupShape(s)) return true;
  }
  return false;
});

xss("eval <- location.hash", `
  eval(location.hash.slice(1));
`, function(sinks) {
  for (var i = 0; i < sinks.length; i++) {
    var s = sinks[i];
    if (s.sink === "eval" && s.source === "location.hash" && assertPopupShape(s)) return true;
  }
  return false;
});

xss("document.write <- document.referrer", `
  document.write(document.referrer);
`, function(sinks) {
  for (var i = 0; i < sinks.length; i++) {
    var s = sinks[i];
    if (s.sink === "document.write" && s.source === "document.referrer" && assertPopupShape(s)) return true;
  }
  return false;
});

xss("setTimeout(string-arg) <- location.hash", `
  setTimeout(location.hash.slice(1), 100);
`, function(sinks) {
  for (var i = 0; i < sinks.length; i++) {
    var s = sinks[i];
    if (s.sink === "setTimeout" && s.source === "location.hash" && assertPopupShape(s)) return true;
  }
  return false;
});

xss("Function ctor <- location.search", `
  new Function(location.search.slice(1))();
`, function(sinks) {
  for (var i = 0; i < sinks.length; i++) {
    var s = sinks[i];
    if (s.sink === "new Function" && s.source === "location.search" && assertPopupShape(s)) return true;
  }
  return false;
});

console.log("\n=== Open-redirect sinks ===\n");

xss("location.href <- location.hash (post-strip)", `
  location.href = location.hash.slice(1);
`, function(sinks) {
  for (var i = 0; i < sinks.length; i++) {
    var s = sinks[i];
    if (s.type === "redirect" && s.source === "location.hash" && assertPopupShape(s)) return true;
  }
  return false;
});

xss("location.assign <- search param", `
  var u = new URLSearchParams(location.search).get("redirect");
  location.assign(u);
`, function(sinks) {
  for (var i = 0; i < sinks.length; i++) {
    var s = sinks[i];
    if (s.type === "redirect" && s.source === "location.search" && assertPopupShape(s)) return true;
  }
  return false;
});

console.log("\n=== postMessage / event.data sources ===\n");

xss("innerHTML <- event.data", `
  window.addEventListener("message", function(e) {
    document.body.innerHTML = e.data;
  });
`, function(sinks) {
  for (var i = 0; i < sinks.length; i++) {
    var s = sinks[i];
    if (s.sink === "innerHTML" && s.source === "event.data" && assertPopupShape(s)) return true;
  }
  return false;
});

xss("eval <- event.data.code (no origin check)", `
  window.addEventListener("message", function(e) {
    eval(e.data.code);
  });
`, function(sinks) {
  for (var i = 0; i < sinks.length; i++) {
    var s = sinks[i];
    if (s.sink === "eval" && s.source === "event.data" && assertPopupShape(s)) return true;
  }
  return false;
});

console.log("\n=== Inter-procedural taint ===\n");

xss("inter-proc: helper(location.hash) -> innerHTML", `
  function set(v) { document.body.innerHTML = v; }
  set(location.hash.slice(1));
`, function(sinks) {
  for (var i = 0; i < sinks.length; i++) {
    var s = sinks[i];
    if (s.sink === "innerHTML" && s.source === "location.hash" && assertPopupShape(s)) return true;
  }
  return false;
});

xss("inter-proc through wrapper chain", `
  function inner(x) { document.write(x); }
  function outer(y) { inner(y); }
  outer(document.referrer);
`, function(sinks) {
  for (var i = 0; i < sinks.length; i++) {
    var s = sinks[i];
    if (s.sink === "document.write" && s.source === "document.referrer" && assertPopupShape(s)) return true;
  }
  return false;
});

console.log("\n=== Negative cases (no false positives) ===\n");

xss("safe: literal innerHTML", `
  document.body.innerHTML = "<p>hello</p>";
`, function(sinks) {
  for (var i = 0; i < sinks.length; i++) {
    if (sinks[i].sink === "innerHTML" && sinks[i].severity !== "info") return false;
  }
  return true;
});

xss("safe: encodeURIComponent(location.hash) into innerHTML still flagged (encoded != sanitized)", `
  document.body.innerHTML = encodeURIComponent(location.hash);
`, function(sinks) {
  // encodeURIComponent doesn't sanitize HTML; should still flag.
  for (var i = 0; i < sinks.length; i++) {
    var s = sinks[i];
    if (s.sink === "innerHTML" && s.source === "location.hash" && assertPopupShape(s)) return true;
  }
  return false;
});

xss("safe: location.href to fetch (current-origin URL is not a sink)", `
  fetch(location.href);
`, function(sinks) {
  // location.href has no origin dim (page-locked); fetch isn't an open-redirect target either.
  for (var i = 0; i < sinks.length; i++) {
    if (sinks[i].sink === "fetch" && sinks[i].severity !== "info") return false;
  }
  return true;
});

console.log("\n=== Summary ===");
console.log("Total: " + total + ", Passed: " + passed + ", Failed: " + failed);
process.exit(failed > 0 ? 1 : 0);
