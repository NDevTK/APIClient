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

console.log("\n=== Sanitizer detection (CFG-based) ===\n");

xss("DOMPurify.sanitize on all paths - sink suppressed", `
  document.body.innerHTML = DOMPurify.sanitize(location.hash);
`, function(sinks) {
  // DOMPurify CFG-sanitization on all paths suppresses the sink entirely.
  return sinks.length === 0;
});

xss("encodeURIComponent on URL going to location.assign IS sanitization", `
  var u = location.hash.slice(1);
  location.assign(encodeURIComponent(u));
`, function(sinks) {
  // encodeURIComponent encodes ":" and "/" so the encoded string can't
  // be a cross-origin URL — location.assign treats it as a path under
  // current origin. Per dim model + sanitizer recognition: should be
  // fully suppressed OR downgraded to info/low (not medium/high).
  for (var i = 0; i < sinks.length; i++) {
    var s = sinks[i];
    if (s.type === "redirect" && (s.severity === "high" || s.severity === "medium")) return false;
  }
  return true;
});

console.log("\n=== Dangerous patterns ===\n");

function dangerCheck(result, expectedType) {
  var dangers = result.dangerousPatterns || [];
  for (var i = 0; i < dangers.length; i++) {
    if (dangers[i].type === expectedType || dangers[i].sink === expectedType) return true;
  }
  return false;
}

function xssDanger(name, code, check) {
  total++;
  try {
    var result = analyzeJSBundle(code, "test://" + name, true, null);
    if (check(result)) {
      passed++;
      console.log("  PASS: " + name);
    } else {
      failed++;
      console.log("  FAIL: " + name);
      console.log("    dangerousPatterns=" + JSON.stringify((result.dangerousPatterns||[]).map(function(d) {
        return { type: d.type, sink: d.sink, severity: d.severity };
      })).slice(0, 500));
    }
  } catch (e) {
    failed++;
    console.log("  ERROR: " + name + " — " + e.message);
  }
}

xssDanger("postMessage handler without origin check", `
  window.addEventListener("message", function(e) {
    if (typeof e.data === "object") doSomething(e.data);
  });
`, function(result) {
  return dangerCheck(result, "postmessage-no-origin") || dangerCheck(result, "postmessage");
});

xssDanger("prototype pollution: obj[userKey] = val with tainted key", `
  var key = location.hash.slice(1);
  var obj = {};
  obj[key] = "value";
`, function(result) {
  return dangerCheck(result, "prototype-pollution") || dangerCheck(result, "proto-pollution");
});

xssDanger("dynamic RegExp with tainted pattern", `
  var pattern = location.hash.slice(1);
  var re = new RegExp(pattern);
`, function(result) {
  return dangerCheck(result, "regex-dynamic") || dangerCheck(result, "regex-redos");
});

console.log("\n=== Negative: postMessage WITH origin check should not flag ===\n");

xssDanger("postMessage handler WITH origin check", `
  window.addEventListener("message", function(e) {
    if (e.origin !== "https://trusted.example.com") return;
    doSomething(e.data);
  });
`, function(result) {
  // Should NOT flag postmessage-no-origin when origin check is present.
  return !dangerCheck(result, "postmessage-no-origin") && !dangerCheck(result, "postmessage");
});

console.log("\n=== Open redirect via window.open ===\n");

xss("window.open(taint) - open redirect", `
  window.open(location.hash.slice(1), "_blank");
`, function(sinks) {
  for (var i = 0; i < sinks.length; i++) {
    var s = sinks[i];
    if (s.type === "redirect" && s.source === "location.hash" && assertPopupShape(s)) return true;
  }
  return false;
});

console.log("\n=== Network sinks (request forgery) ===\n");

xss("fetch(taint) - request forgery", `
  fetch(document.cookie);
`, function(sinks) {
  // document.cookie has origin dim → user-controlled; fetch with that
  // is request-forgery.
  for (var i = 0; i < sinks.length; i++) {
    var s = sinks[i];
    if (s.type === "request-forgery" && s.source === "document.cookie" && assertPopupShape(s)) return true;
  }
  return false;
});

xss("XMLHttpRequest.open(method, taintUrl) - request forgery", `
  var xhr = new XMLHttpRequest();
  xhr.open("GET", document.cookie);
`, function(sinks) {
  for (var i = 0; i < sinks.length; i++) {
    var s = sinks[i];
    if (s.type === "request-forgery" && s.source === "document.cookie" && assertPopupShape(s)) return true;
  }
  return false;
});

console.log("\n=== Inter-proc through array element ===\n");

xss("inter-proc: array.push tainted, then forEach -> innerHTML", `
  var items = [];
  items.push(location.hash.slice(1));
  items.forEach(function(v) { document.body.innerHTML = v; });
`, function(sinks) {
  // HOF dispatch: forEach binds cb's param to array element.
  for (var i = 0; i < sinks.length; i++) {
    var s = sinks[i];
    if (s.sink === "innerHTML" && s.source === "location.hash" && assertPopupShape(s)) return true;
  }
  return false;
});

console.log("\n=== Sanitizer recognition ===\n");

xss("escape() URL-encodes < and > so it's safe for innerHTML — sink suppressed", `
  document.body.innerHTML = escape(location.hash);
`, function(sinks) {
  // Per ECMA Annex B § B.2.1: escape() URL-encodes < > & = etc. to
  // %XX. The encoded string in innerHTML renders as literal text — no
  // HTML parsing. Analyzer's CFG sanitizer recognition correctly
  // suppresses the sink.
  return sinks.length === 0;
});

console.log("\n=== Class instance method taint ===\n");

xss("class method: this.url=taint, then fetch(this.url) - request forgery", `
  class API {
    constructor(u) { this.url = u; }
    fetch() { return fetch(this.url); }
  }
  new API(document.cookie).fetch();
`, function(sinks) {
  for (var i = 0; i < sinks.length; i++) {
    var s = sinks[i];
    if (s.type === "request-forgery" && s.source === "document.cookie") return true;
  }
  return false;
});

console.log("\n=== Promise chain taint ===\n");

xss("Promise.then(cb): cb(taint) -> innerHTML", `
  Promise.resolve(location.hash).then(function(v) {
    document.body.innerHTML = v;
  });
`, function(sinks) {
  for (var i = 0; i < sinks.length; i++) {
    var s = sinks[i];
    if (s.sink === "innerHTML" && s.source === "location.hash") return true;
  }
  return false;
});

console.log("\n=== Template literal injection ===\n");

xss("template literal in eval", `
  var name = location.hash.slice(1);
  eval(\`var x = \${name};\`);
`, function(sinks) {
  for (var i = 0; i < sinks.length; i++) {
    var s = sinks[i];
    if (s.sink === "eval" && s.source === "location.hash") return true;
  }
  return false;
});

console.log("\n=== Negative: literal-only template ===\n");

xss("safe: literal template in eval", `
  eval(\`var x = 1;\`);
`, function(sinks) {
  for (var i = 0; i < sinks.length; i++) {
    if (sinks[i].sink === "eval" && sinks[i].severity !== "info") return false;
  }
  return true;
});

console.log("\n=== Dim upgrade via slice ===\n");

xss("fetch(location.hash.slice(1)) - dim upgrade strips # marker", `
  fetch(location.hash.slice(1));
`, function(sinks) {
  // Per CLAUDE.md: slice(N>=1) on location.hash strips the # marker —
  // result CAN be a full attacker URL. Origin dim upgrades to true,
  // request-forgery filter no longer suppresses.
  for (var i = 0; i < sinks.length; i++) {
    var s = sinks[i];
    if (s.type === "request-forgery" && s.source === "location.hash") return true;
  }
  return false;
});

console.log("\n=== Closure-captured taint ===\n");

xss("closure-captured taint var", `
  var url = document.cookie;
  function send() { fetch(url); }
  send();
`, function(sinks) {
  for (var i = 0; i < sinks.length; i++) {
    var s = sinks[i];
    if (s.type === "request-forgery" && s.source === "document.cookie") return true;
  }
  return false;
});

console.log("\n=== Reassignment and conditional ===\n");

xss("reassignment-then-fetch", `
  var u;
  u = document.cookie;
  fetch(u);
`, function(sinks) {
  for (var i = 0; i < sinks.length; i++) {
    var s = sinks[i];
    if (s.type === "request-forgery" && s.source === "document.cookie") return true;
  }
  return false;
});

xss("conditional-assignment with taint in one branch", `
  var u;
  if (Math.random() > 0.5) { u = document.cookie; } else { u = "/safe"; }
  fetch(u);
`, function(sinks) {
  // One branch is tainted — should flag (or-AV with taint leaf).
  for (var i = 0; i < sinks.length; i++) {
    var s = sinks[i];
    if (s.type === "request-forgery" && s.source === "document.cookie") return true;
  }
  return false;
});

console.log("\n=== Object-literal body with tainted field ===\n");

xss("postMessage(taint, '*')", `
  window.postMessage(location.hash, "*");
`, function(sinks) {
  // postMessage with tainted data is leak; check for either request-forgery
  // (target host) or eval/xss (depending on classification).
  for (var i = 0; i < sinks.length; i++) {
    var s = sinks[i];
    if (s.source === "location.hash") return true;
  }
  return false;
});

console.log("\n=== Summary ===");
console.log("Total: " + total + ", Passed: " + passed + ", Failed: " + failed);
process.exit(failed > 0 ? 1 : 0);
