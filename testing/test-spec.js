// testing/test-spec.js — ECMA spec compliance tests for the AST analyser.
//
// Tests are organised by ECMA-262 spec section. Each test exercises ONE
// spec construct's data-flow behaviour with realistic code that the
// analyser must trace through correctly. This file is dedicated to spec
// coverage; library-specific patterns (jQuery, axios, etc.) belong in
// test-ast.js.
//
// Run: node testing/test-spec.js

var fs = require("fs");
var path = require("path");
var rootDir = path.join(__dirname, "..");

var babelCode = fs.readFileSync(path.join(rootDir, "extension/lib/babel-bundle.js"), "utf8");
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();

var astCode = fs.readFileSync(path.join(rootDir, "extension/lib/ast.js"), "utf8");
new Function(astCode +
  "\nglobalThis.analyzeJSBundle = analyzeJSBundle;" +
  "\nglobalThis._specAnalyzePropertyFlow = _specAnalyzePropertyFlow;" +
  "\nglobalThis._specInitialFunctionBodyState = _specInitialFunctionBodyState;" +
  "\nglobalThis._specEvalExpression = _specEvalExpression;" +
  "\nglobalThis._specEqualAv = _specEqualAv;" +
  "\nglobalThis._specDetectPropagationFromEffects = _specDetectPropagationFromEffects;").call(globalThis);

var passed = 0, failed = 0, total = 0;

function test(name, code, check) {
  total++;
  try {
    var result = analyzeJSBundle(code, "test://" + name, true);
    if (check(result)) {
      passed++;
      console.log("  PASS: " + name);
    } else {
      failed++;
      console.log("  FAIL: " + name);
      console.log("    fetchCallSites: " + JSON.stringify(result.fetchCallSites, null, 2).slice(0, 500));
    }
  } catch (e) {
    failed++;
    console.log("  ERROR: " + name + " — " + e.message);
  }
}

function specTest(name, code, check) {
  total++;
  try {
    var result = analyzeJSBundle(code, "test://" + name, true);
    var fnPath = null;
    globalThis.BabelBundle.traverse(result._ast, {
      FunctionDeclaration: function(p) { if (!fnPath) fnPath = p; p.skip(); }
    });
    if (!fnPath) { failed++; console.log("  FAIL: " + name + " — no function found"); return; }
    var effects = globalThis._specAnalyzePropertyFlow(fnPath);
    if (check(effects, result)) {
      passed++;
      console.log("  PASS: " + name);
    } else {
      failed++;
      console.log("  FAIL: " + name);
      console.log("    effects: " + JSON.stringify(effects, null, 2).slice(0, 500));
    }
  } catch (e) {
    failed++;
    console.log("  ERROR: " + name + " — " + e.message);
  }
}

// ═════════════════════════════════════════════════════════════════════
// § 8.1.1 Lexical environments — scope shadowing
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 8.1.1 Lexical environments ===\n");

specTest("§ 8.1.1: shadowed Object identifier disables Object.assign built-in", `
  function f() {
    var Object = { assign: function(){} };
    var t = {};
    Object.assign(t, { x: 1 });
  }
`, function(effects) {
  // With Object shadowed, the call to Object.assign is just a local
  // method call — no recognition fires, no spec-built-in propagation
  // effect is recorded.
  return effects.length === 0 || effects.every(function(e) {
    return !(e.target && e.target.kind === "param" && e.target.idx === 0);
  });
});

// ═════════════════════════════════════════════════════════════════════
// § 13.10 MemberExpression
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 13.10 MemberExpression ===\n");

specTest("§ 13.10: static member access produces member abstract value", `
  function f(o) { this.a = o.b; }
`, function(effects) {
  if (effects.length !== 1) return false;
  var e = effects[0];
  return e.value && e.value.kind === "member" &&
         e.value.obj && e.value.obj.kind === "param" && e.value.obj.idx === 0 &&
         e.value.key && e.value.key.kind === "const" && e.value.key.value === "b";
});

specTest("§ 13.10: computed member access with const key produces member abstract value", `
  function f(o) { this.a = o["b"]; }
`, function(effects) {
  if (effects.length !== 1) return false;
  var e = effects[0];
  return e.value && e.value.kind === "member" &&
         e.value.obj && e.value.obj.kind === "param" && e.value.obj.idx === 0 &&
         e.value.key && e.value.key.kind === "const" && e.value.key.value === "b";
});

// ═════════════════════════════════════════════════════════════════════
// § 13.13 LogicalExpression (||, &&, ??)
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 13.13 LogicalExpression ===\n");

specTest("§ 13.13 OR: `arguments[0] || {}` records or(args-elt, obj-lit) target", `
  function f() { var t = arguments[0] || {}; t.k = "v"; }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].target && effects[0].target.kind === "or" &&
         effects[0].target.left && effects[0].target.left.kind === "args-elt";
});

specTest("§ 13.13 OR: both operands reachable in value", `
  function f(req) { this.id = req.id || 42; }
`, function(effects) {
  if (effects.length !== 1) return false;
  var v = effects[0].value;
  return v && v.kind === "or" &&
         v.left && v.left.kind === "member" &&
         v.right && v.right.kind === "const" && v.right.value === 42;
});

// ═════════════════════════════════════════════════════════════════════
// § 13.14 ConditionalExpression
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 13.14 ConditionalExpression ===\n");

test("§ 13.14: ternary base URL produces both endpoints", `
  function call(env) {
    var base = env === "prod" ? "https://api.example.com" : "https://staging.example.com";
    fetch(base + "/v1/users");
  }
`, function(r) {
  var urls = r.fetchCallSites.map(function(s) { return s.url; });
  return urls.indexOf("https://api.example.com/v1/users") >= 0 &&
         urls.indexOf("https://staging.example.com/v1/users") >= 0;
});

// ═════════════════════════════════════════════════════════════════════
// § 13.15.4 AssignmentExpression
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 13.15.4 AssignmentExpression ===\n");

specTest("§ 13.15.4: identifier-LHS rebinds state, MemberExpression-LHS records effect", `
  function f() { this.a = 1; }
`, function(effects) {
  if (effects.length !== 1) return false;
  var e = effects[0];
  return e.target && e.target.kind === "this" &&
         e.key && e.key.kind === "const" && e.key.value === "a" &&
         e.value && e.value.kind === "const" && e.value.value === 1;
});

// ═════════════════════════════════════════════════════════════════════
// § 14.3.2 VariableDeclaration — chain through locals
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 14.3.2 VariableDeclaration ===\n");

test("§ 14.3.2: multi-hop indirection through locals reaches body field", `
  function send() {
    var step1 = "AUTH_TOKEN_VALUE";
    var step2 = step1;
    var step3 = step2;
    fetch("/api/x", { method: "POST", body: JSON.stringify({ token: step3 }) });
  }
`, function(r) {
  var site = r.fetchCallSites.find(function(s) { return s.url === "/api/x"; });
  if (!site || !site.params) return false;
  var tokenP = site.params.find(function(p) { return p.name === "token"; });
  if (!tokenP) return false;
  return tokenP.defaultValue === "AUTH_TOKEN_VALUE" ||
         (tokenP.validValues && tokenP.validValues.indexOf("AUTH_TOKEN_VALUE") >= 0);
});

// ═════════════════════════════════════════════════════════════════════
// § 14.6 IfStatement — branch effect accumulation
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 14.6 IfStatement ===\n");

specTest("§ 14.6: if-branch property write captured in effects", `
  function f(x) { if (x) { this.a = 1; } }
`, function(effects) {
  if (effects.length !== 1) return false;
  var e = effects[0];
  return e.target && e.target.kind === "this" &&
         e.key && e.key.kind === "const" && e.key.value === "a";
});

specTest("§ 14.6: both branches of if/else record effects (admin-vs-user pattern)", `
  function f(isAdmin) {
    if (isAdmin) { this.role = "admin"; }
    else { this.role = "user"; }
  }
`, function(effects) {
  if (effects.length !== 2) return false;
  var roleVals = effects
    .filter(function(e) { return e.key && e.key.kind === "const" && e.key.value === "role"; })
    .map(function(e) { return e.value && e.value.kind === "const" ? e.value.value : null; })
    .filter(function(v) { return v !== null; });
  return roleVals.indexOf("admin") >= 0 && roleVals.indexOf("user") >= 0;
});

// ═════════════════════════════════════════════════════════════════════
// § 14.7.5 ForInStatement
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 14.7.5 ForInStatement ===\n");

specTest("§ 14.7.5: for-in copy from param → this records loop-key target effect", `
  function copy(s) { for (var k in s) this[k] = s[k]; }
`, function(effects) {
  if (effects.length !== 1) return false;
  var e = effects[0];
  return e.target && e.target.kind === "this" &&
         e.key && e.key.kind === "loop-key" &&
         e.key.src && e.key.src.kind === "param" && e.key.src.idx === 0 &&
         e.value && e.value.kind === "member";
});

specTest("§ 14.7.5: two-arg for-in copy `(t, s) => for(k in s) t[k]=s[k]` records propagation", `
  function assign(t, s) { for (var k in s) t[k] = s[k]; }
`, function(effects) {
  if (effects.length !== 1) return false;
  var e = effects[0];
  return e.target && e.target.kind === "param" && e.target.idx === 0 &&
         e.key && e.key.kind === "loop-key" &&
         e.key.src && e.key.src.kind === "param" && e.key.src.idx === 1;
});

// ═════════════════════════════════════════════════════════════════════
// § 19.1.2.1 Object.assign (built-in)
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 19.1.2.1 Object.assign ===\n");

test("§ 19.1.2.1: Object.assign({...}, {fetchUser: fn}) — call traces literal-URL fetch", `
  var client = {};
  Object.assign(client, { fetchUser: function() { fetch("/api/users"); } });
  client.fetchUser();
`, function(r) {
  return r.fetchCallSites.some(function(s) { return s.url === "/api/users"; });
});

// ═════════════════════════════════════════════════════════════════════
// § 19.1.2.16 + § 22.1.3.7 — Object.keys + Array.prototype.forEach
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 19.1.2.16 + § 22.1.3.7: Object.keys / forEach ===\n");

specTest("§ 19.1.2.16+§ 22.1.3.7: Object.keys.forEach produces propagation effect (compositional)", `
  function copy(s) {
    Object.keys(s).forEach(function(k) { this[k] = s[k]; });
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  var e = effects[0];
  return e.target && e.target.kind === "this" &&
         e.key && e.key.kind === "loop-key" &&
         e.key.src && e.key.src.kind === "param" && e.key.src.idx === 0;
});

// ═════════════════════════════════════════════════════════════════════
// § 25.5.2 JSON.stringify
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 25.5.2 JSON.stringify ===\n");

test("§ 25.5.2: JSON.stringify(inline-literal) → body fields extracted", `
  function send() {
    fetch("/api/x", { method: "POST", body: JSON.stringify({ user: "alice", role: "admin" }) });
  }
`, function(r) {
  var site = r.fetchCallSites.find(function(s) { return s.url === "/api/x"; });
  if (!site || !site.params) return false;
  return site.params.some(function(p) { return p.name === "user" && p.defaultValue === "alice"; }) &&
         site.params.some(function(p) { return p.name === "role" && p.defaultValue === "admin"; });
});

test("§ 25.5.2: JSON.stringify(localBodyVar) with branch-conditional role surfaces both values", `
  function send(isAdmin) {
    var body = {};
    body.user = "alice";
    if (isAdmin) { body.role = "admin"; }
    else { body.role = "guest"; }
    fetch("/api/x", { method: "POST", body: JSON.stringify(body) });
  }
`, function(r) {
  var site = r.fetchCallSites.find(function(s) { return s.url === "/api/x" && s.method === "POST"; });
  if (!site || !site.params) return false;
  var roleP = site.params.find(function(p) { return p.name === "role" && p.location === "body"; });
  if (!roleP || !roleP.validValues) return false;
  return roleP.validValues.indexOf("admin") >= 0 && roleP.validValues.indexOf("guest") >= 0;
});

// ═════════════════════════════════════════════════════════════════════
// § 13.5 TemplateLiteral
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 13.5 TemplateLiteral ===\n");

test("§ 13.5: template literal with literal-only segments resolves URL", `
  fetch(\`/api/v1/users\`);
`, function(r) {
  return r.fetchCallSites.some(function(s) { return s.url === "/api/v1/users"; });
});

test("§ 13.5: template literal with const interpolation resolves URL fully", `
  function f() {
    var v = "42";
    fetch(\`/api/\${v}/profile\`);
  }
  f();
`, function(r) {
  return r.fetchCallSites.some(function(s) { return s.url === "/api/42/profile"; });
});

// ═════════════════════════════════════════════════════════════════════
// § 14.7.4 ForStatement
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 14.7.4 ForStatement ===\n");

specTest("§ 14.7.4: for-loop body's property writes recorded once (single-pass abstraction)", `
  function f() {
    for (var i = 0; i < 3; i++) {
      this.idx = i;
    }
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].target.kind === "this" &&
         effects[0].key.kind === "const" && effects[0].key.value === "idx";
});

// ═════════════════════════════════════════════════════════════════════
// § 14.10 ReturnStatement
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 14.10 ReturnStatement ===\n");

specTest("§ 14.10: ReturnStatement halts branch — subsequent statements not analysed in that branch", `
  function f() {
    this.before = 1;
    return;
    this.after = 2;
  }
`, function(effects) {
  // Only the pre-return assignment should be recorded.
  return effects.length === 1 &&
         effects[0].key.kind === "const" && effects[0].key.value === "before";
});

// ═════════════════════════════════════════════════════════════════════
// § 13.3.5 Method invocation — `this` binding
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 13.3.5 Method invocation ===\n");

test("§ 13.3.5: object literal method invocation traces fetch URL", `
  var obj = {
    fetchData: function() { fetch("/api/data"); }
  };
  obj.fetchData();
`, function(r) {
  return r.fetchCallSites.some(function(s) { return s.url === "/api/data"; });
});

// ── Summary ──
console.log("\n" + "=".repeat(50));
console.log("Spec Test Results: " + passed + "/" + total + " passed, " + failed + " failed");
if (failed > 0) {
  process.exit(1);
} else {
  console.log("All spec tests passed!");
}
