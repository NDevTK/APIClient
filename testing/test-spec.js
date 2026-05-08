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
  "\nglobalThis._specDetectPropagationFromEffects = _specDetectPropagationFromEffects;" +
  "\nglobalThis._specInstantiateEffects = _specInstantiateEffects;").call(globalThis);

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
// § 20.1.2.1 Object.assign (built-in)
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 20.1.2.1 Object.assign ===\n");

test("§ 20.1.2.1: Object.assign({...}, {fetchUser: fn}) — call traces literal-URL fetch", `
  var client = {};
  Object.assign(client, { fetchUser: function() { fetch("/api/users"); } });
  client.fetchUser();
`, function(r) {
  return r.fetchCallSites.some(function(s) { return s.url === "/api/users"; });
});

// ═════════════════════════════════════════════════════════════════════
// § 20.1.2.19 + § 23.1.3.15 — Object.keys + Array.prototype.forEach
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 20.1.2.19 + § 23.1.3.15: Object.keys / forEach ===\n");

specTest("§ 20.1.2.19+§ 23.1.3.15: Object.keys.forEach produces propagation effect (compositional)", `
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
// § 25.5.4 JSON.stringify
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 25.5.4 JSON.stringify ===\n");

test("§ 25.5.4: JSON.stringify(inline-literal) → body fields extracted", `
  function send() {
    fetch("/api/x", { method: "POST", body: JSON.stringify({ user: "alice", role: "admin" }) });
  }
`, function(r) {
  var site = r.fetchCallSites.find(function(s) { return s.url === "/api/x"; });
  if (!site || !site.params) return false;
  return site.params.some(function(p) { return p.name === "user" && p.defaultValue === "alice"; }) &&
         site.params.some(function(p) { return p.name === "role" && p.defaultValue === "admin"; });
});

test("§ 25.5.4: JSON.stringify(localBodyVar) with branch-conditional role surfaces both values", `
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

// ═════════════════════════════════════════════════════════════════════
// § 13.10.2 + § 7.1.17 — chained member with array literal (HTTP method
// table). The analyzer should resolve `methods[0]` when the array is
// known.
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 13.10.2 chained member access ===\n");

test("§ 13.10.2: array element access via const index resolves URL", `
  function f() {
    var endpoints = ["/api/users", "/api/posts"];
    fetch(endpoints[0]);
  }
  f();
`, function(r) {
  return r.fetchCallSites.some(function(s) { return s.url === "/api/users"; });
});

// ═════════════════════════════════════════════════════════════════════
// § 13.3.6 OptionalChainEvaluation — `obj?.x`
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 13.3.6 OptionalChainEvaluation ===\n");

specTest("§ 13.3.6: optional-chain member access produces member abstract value", `
  function f(o) { this.a = o?.b; }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "member";
});

// ═════════════════════════════════════════════════════════════════════
// § 14.6 IfStatement — chained if/else-if for value selection
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 14.6 IfStatement chains ===\n");

specTest("§ 14.6 chain: if/else-if assigning same var records all branch effects", `
  function f(level) {
    if (level === 1) { this.role = "guest"; }
    else if (level === 2) { this.role = "user"; }
    else if (level === 3) { this.role = "admin"; }
  }
`, function(effects) {
  // Each branch records its property-write into the shared effects list.
  // Per § 14.6 the test is evaluated, then the matching branch executes;
  // for static analysis ALL three writes are recorded as possible effects.
  var roleVals = effects
    .filter(function(e) { return e.target.kind === "this" && e.key.kind === "const" && e.key.value === "role"; })
    .map(function(e) { return e.value && e.value.kind === "const" ? e.value.value : null; })
    .filter(function(v) { return v !== null; });
  return roleVals.indexOf("guest") >= 0 &&
         roleVals.indexOf("user") >= 0 &&
         roleVals.indexOf("admin") >= 0;
});

// ═════════════════════════════════════════════════════════════════════
// § 13.2.4 ArrayLiteral — element access by const index
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 13.2.4 ArrayLiteral ===\n");

test("§ 13.2.4: array literal indexed by literal const resolves URL", `
  function f() {
    var hosts = ["api.x.com", "api.y.com"];
    fetch("https://" + hosts[1] + "/users");
  }
  f();
`, function(r) {
  return r.fetchCallSites.some(function(s) { return s.url === "https://api.y.com/users"; });
});

// ═════════════════════════════════════════════════════════════════════
// § 14.3.1 Let and Const Declarations
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 14.3.1 Let and Const Declarations ===\n");

specTest("§ 14.3.1: const declaration binding tracked same as var per Lexical Environments", `
  function f() {
    const url = "/api/x";
    this.url = url;
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  var e = effects[0];
  return e.target.kind === "this" &&
         e.value.kind === "const" && e.value.value === "/api/x";
});

specTest("§ 14.3.1: let declaration binding tracked same as var", `
  function f() {
    let role = "admin";
    this.role = role;
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  var e = effects[0];
  return e.target.kind === "this" &&
         e.value.kind === "const" && e.value.value === "admin";
});

// ═════════════════════════════════════════════════════════════════════
// § 13.10 chained MemberExpression — `obj.a.b.c`
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 13.10 chained MemberExpression ===\n");

specTest("§ 13.10: chained member access (a.b.c) yields nested member abstract value", `
  function f(o) { this.url = o.config.api.endpoint; }
`, function(effects) {
  if (effects.length !== 1) return false;
  var v = effects[0].value;
  // Should be member(member(member(param, "config"), "api"), "endpoint")
  if (!v || v.kind !== "member") return false;
  if (!v.key || v.key.kind !== "const" || v.key.value !== "endpoint") return false;
  if (!v.obj || v.obj.kind !== "member") return false;
  return true;
});

// ═════════════════════════════════════════════════════════════════════
// § 13.13.1 LogicalAndExpression (&&)
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 13.13.1 LogicalAndExpression ===\n");

specTest("§ 13.13.1: `a && b` value is or(a, b) — both operands reachable per spec short-circuit", `
  function f(req) { this.id = req.valid && req.id; }
`, function(effects) {
  if (effects.length !== 1) return false;
  var v = effects[0].value;
  return v && v.kind === "or" &&
         v.left && v.left.kind === "member" &&
         v.right && v.right.kind === "member";
});

specTest("§ 13.14: `a ?? b` value is or(a, b) — nullish-coalescing short-circuit", `
  function f(req) { this.id = req.id ?? 42; }
`, function(effects) {
  if (effects.length !== 1) return false;
  var v = effects[0].value;
  return v && v.kind === "or" &&
         v.left && v.left.kind === "member" &&
         v.right && v.right.kind === "const" && v.right.value === 42;
});

// ═════════════════════════════════════════════════════════════════════
// § 14.7.5 ForOfStatement — different iterator protocol from for-in
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 14.7.5 ForOfStatement ===\n");

specTest("§ 14.7.5: for-of over array iterates elements (each iteration's loop var is element)", `
  function f() {
    var items = ["a", "b", "c"];
    for (var item of items) {
      this.value = item;
    }
  }
`, function(effects) {
  // Each iteration writes this.value. Single property-write effect
  // recorded (single-pass abstraction); target=this, key=const("value").
  return effects.length === 1 && effects[0].target.kind === "this" &&
         effects[0].key.kind === "const" && effects[0].key.value === "value";
});

// ═════════════════════════════════════════════════════════════════════
// § 13.4 UpdateExpression (++, --)
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 13.4 UpdateExpression ===\n");

specTest("§ 13.4: postfix increment in for-loop body — body's writes still recorded", `
  function f() {
    var i = 0;
    while (i < 3) {
      this.idx = i;
      i++;
    }
  }
`, function(effects) {
  // The this.idx = i write should be recorded.
  return effects.length === 1 && effects[0].target.kind === "this" &&
         effects[0].key.kind === "const" && effects[0].key.value === "idx";
});

// ═════════════════════════════════════════════════════════════════════
// § 13.2.4 ArrayLiteral spread / § 13.2.5 ObjectLiteral spread
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 13.2.5 ObjectLiteral spread ===\n");

test("§ 13.2.5: object spread preserves both literal and spread fields", `
  function send() {
    var defaults = { method: "GET" };
    fetch("/api/x", { ...defaults, body: JSON.stringify({ a: 1 }) });
  }
  send();
`, function(r) {
  // The fetch should still be recognised at /api/x even with spread in opts.
  return r.fetchCallSites.some(function(s) { return s.url === "/api/x"; });
});

// ═════════════════════════════════════════════════════════════════════
// § 13.15.4 chained AssignmentExpression — `a = b = expr`
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 13.15.4 chained AssignmentExpression ===\n");

test("§ 13.15.4: `obj.X = obj2.X = function(){...}` — calling obj.X reaches the function", `
  var lib = {};
  var libExt = {};
  lib.method = libExt.method = function() { fetch("/api/x"); };
  lib.method();
`, function(r) {
  return r.fetchCallSites.some(function(s) { return s.url === "/api/x"; });
});

// ═════════════════════════════════════════════════════════════════════
// § 13.10.3 OptionalCallExpression — `obj?.method()`
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 13.10.3 OptionalCallExpression ===\n");

test("§ 13.10.3: optional method call traces fetch URL through declared function", `
  var lib = { call: function() { fetch("/api/x"); } };
  lib?.call();
`, function(r) {
  return r.fetchCallSites.some(function(s) { return s.url === "/api/x"; });
});

// ═════════════════════════════════════════════════════════════════════
// § 13.5 BinaryExpression — string concat for URL construction
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 13.5 BinaryExpression (string concat) ===\n");

test("§ 13.5: string concat for URL with two const segments", `
  function f() {
    var prefix = "/api/";
    var endpoint = "users";
    fetch(prefix + endpoint);
  }
  f();
`, function(r) {
  return r.fetchCallSites.some(function(s) { return s.url === "/api/users"; });
});

// ═════════════════════════════════════════════════════════════════════
// § 13.10 nested member with property chain through param
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 13.10 nested member through param ===\n");

specTest("§ 13.10: param.config.url retains 3-level abstract structure", `
  function f(p) { this.url = p.config.url; }
`, function(effects) {
  if (effects.length !== 1) return false;
  var v = effects[0].value;
  // member(member(param0, "config"), "url")
  return v && v.kind === "member" && v.key.kind === "const" && v.key.value === "url" &&
         v.obj && v.obj.kind === "member" && v.obj.key.kind === "const" && v.obj.key.value === "config" &&
         v.obj.obj && v.obj.obj.kind === "param" && v.obj.obj.idx === 0;
});

// ═════════════════════════════════════════════════════════════════════
// § 14.3.3 / § 8.6.2 Destructuring patterns in function parameters
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 14.3.3 destructuring params ===\n");

specTest("§ 14.3.3: ObjectPattern destructured param `{a, b}` resolves to member(param0, key)", `
  function f({a, b}) {
    this.first = a;
    this.second = b;
  }
`, function(effects) {
  if (effects.length !== 2) return false;
  var byKey = {};
  for (var i = 0; i < effects.length; i++) {
    var e = effects[i];
    if (e.target.kind === "this" && e.key.kind === "const") byKey[e.key.value] = e.value;
  }
  var firstOK = byKey.first && byKey.first.kind === "member" &&
                byKey.first.obj.kind === "param" && byKey.first.obj.idx === 0 &&
                byKey.first.key.kind === "const" && byKey.first.key.value === "a";
  var secondOK = byKey.second && byKey.second.kind === "member" &&
                 byKey.second.obj.kind === "param" && byKey.second.obj.idx === 0 &&
                 byKey.second.key.kind === "const" && byKey.second.key.value === "b";
  return firstOK && secondOK;
});

specTest("§ 14.3.3: ArrayPattern destructured param `[a, b]` resolves to member(param0, idx)", `
  function f([a, b]) {
    this.first = a;
    this.second = b;
  }
`, function(effects) {
  if (effects.length !== 2) return false;
  var byKey = {};
  for (var i = 0; i < effects.length; i++) {
    var e = effects[i];
    if (e.target.kind === "this" && e.key.kind === "const") byKey[e.key.value] = e.value;
  }
  return byKey.first && byKey.first.kind === "member" &&
         byKey.first.obj.kind === "param" && byKey.first.obj.idx === 0 &&
         byKey.first.key.kind === "const" && byKey.first.key.value === 0 &&
         byKey.second && byKey.second.kind === "member" &&
         byKey.second.key.value === 1;
});

specTest("§ 14.1: AssignmentPattern default-valued param `function f(x = 1)` binds x", `
  function f(x = 99) { this.v = x; }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "param" && effects[0].value.idx === 0;
});

specTest("§ 14.3.3: VariableDeclarator destructuring `var {a, b} = obj`", `
  function f(o) {
    var { foo, bar } = o;
    this.x = foo;
    this.y = bar;
  }
`, function(effects) {
  if (effects.length !== 2) return false;
  var byKey = {};
  for (var i = 0; i < effects.length; i++) {
    var e = effects[i];
    if (e.target.kind === "this" && e.key.kind === "const") byKey[e.key.value] = e.value;
  }
  return byKey.x && byKey.x.kind === "member" &&
         byKey.x.obj.kind === "param" && byKey.x.obj.idx === 0 &&
         byKey.x.key.kind === "const" && byKey.x.key.value === "foo" &&
         byKey.y && byKey.y.kind === "member" &&
         byKey.y.key.kind === "const" && byKey.y.key.value === "bar";
});

specTest("§ 13.2.8.6: TemplateLiteral with const interpolation composes to single Const", `
  function f() {
    var ver = "v2";
    this.url = \`/api/\${ver}/users\`;
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" &&
         effects[0].value.value === "/api/v2/users";
});

specTest("§ 13.2.8.6: TemplateLiteral with non-const interpolation abstracts to Top (sound)", `
  function f(dynamic) {
    this.url = \`/api/\${dynamic}/users\`;
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "top";
});

specTest("§ 13.10 + § 13.15.3: BinaryExpression `+` composes two Const strings", `
  function f() {
    this.url = "/api/" + "users";
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" &&
         effects[0].value.value === "/api/users";
});

specTest("§ 13.10 + § 13.15.3: BinaryExpression with non-Const operand abstracts to Top (sound)", `
  function f(suffix) {
    this.url = "/api/" + suffix;
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "top";
});

// ═════════════════════════════════════════════════════════════════════
// § 15.7 ClassDeclaration — instance method invocation
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 15.7 ClassDeclaration ===\n");

test("§ 15.7: `new C().method()` resolves to the class method's body", `
  class C {
    send() { fetch("/api/x"); }
  }
  new C().send();
`, function(r) {
  return r.fetchCallSites.some(function(s) { return s.url === "/api/x"; });
});

test("§ 15.7: `var c = new C(); c.method()` resolves through instance variable", `
  class C {
    send() { fetch("/api/y"); }
  }
  var c = new C();
  c.send();
`, function(r) {
  return r.fetchCallSites.some(function(s) { return s.url === "/api/y"; });
});

// ═════════════════════════════════════════════════════════════════════
// § 15.3.5.13 ArrowFunction concise body
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 15.3.5.13 ArrowFunction concise body ===\n");

// ═════════════════════════════════════════════════════════════════════
// § 13.3 + § 13.15.4 + § 13.3 — callback-stored-then-invoked composition
// (function passed as arg to a registration function that stores it,
//  then the stored function is invoked elsewhere with concrete args)
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== Callback-stored-then-invoked (§ 13.3 + § 13.15.4 + § 13.3) ===\n");

test("§ 13.3 composition: register(fn) stores in handlers; handlers[0](opts) resolves opts.url", `
  var handlers = [];
  function register(handler) { handlers.push(handler); }
  register(function(opts) { fetch(opts.url); });
  handlers[0]({url: "/api/x"});
`, function(r) {
  return r.fetchCallSites.some(function(s) { return s.url === "/api/x"; });
});

// ═════════════════════════════════════════════════════════════════════
// § 13.2.5 ObjectLiteral with shorthand property
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 13.2.5 ObjectLiteral shorthand ===\n");

specTest("§ 13.2.5: shorthand `{ x }` binds property to identifier's value", `
  function f(role) {
    this.body = { role };
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  var v = effects[0].value;
  if (!v || v.kind !== "obj-lit") return false;
  return v.props && v.props.role && v.props.role.kind === "param" && v.props.role.idx === 0;
});

// ═════════════════════════════════════════════════════════════════════
// § 25.1 Iteration protocols — for-of binds element abstract value
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 25.1 for-of element binding ===\n");

specTest("§ 14.7.5 for-of: loop var bound to loop-key over iterated value", `
  function f(arr) {
    for (var item of arr) {
      this.element = item;
    }
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  var v = effects[0].value;
  return v && v.kind === "loop-key" &&
         v.src && v.src.kind === "param" && v.src.idx === 0;
});

// ═════════════════════════════════════════════════════════════════════
// § 13.5.5 UnaryExpression — typeof/void/!/~/+/-
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 13.5.5 UnaryExpression ===\n");

specTest("§ 13.5.6: `!Const(true)` resolves to Const(false)", `
  function f() {
    this.flag = !true;
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === false;
});

specTest("§ 13.5.7: `+Const(\"42\")` numeric coerces to Const(42)", `
  function f() {
    this.n = +"42";
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === 42;
});

specTest("§ 13.5.4: `void Const` resolves to Const(undefined)", `
  function f() {
    this.x = void 0;
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === undefined;
});

specTest("§ 13.5.3.5: `typeof Const(\"hello\")` resolves to Const(\"string\")", `
  function f() {
    this.t = typeof "hello";
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === "string";
});

specTest("§ 13.5.3.5: `typeof Const(42)` resolves to Const(\"number\")", `
  function f() {
    this.t = typeof 42;
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === "number";
});

// ═════════════════════════════════════════════════════════════════════
// § 13.3.6 CallExpression — Const-return propagation
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 13.3.6 CallExpression Const-return ===\n");

test("§ 13.3.6: call to const-returning function flows to fetch URL", `
  function getEndpoint() { return "/api/x"; }
  fetch(getEndpoint());
`, function(r) {
  return r.fetchCallSites.some(function(s) { return s.url === "/api/x"; });
});

// ═════════════════════════════════════════════════════════════════════
// Composition: multi-step spec composition
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== Multi-spec composition ===\n");

test("Composition: ternary base URL with concat resolves to both endpoints", `
  function send(env) {
    var base = env === "prod" ? "https://api.example.com" : "https://dev.example.com";
    fetch(base + "/v1/users");
  }
  send("prod");
`, function(r) {
  var urls = r.fetchCallSites.map(function(s) { return s.url; });
  return urls.indexOf("https://api.example.com/v1/users") >= 0 &&
         urls.indexOf("https://dev.example.com/v1/users") >= 0;
});

test("Composition: destructured config + member chain + concat", `
  function send({base, path}) {
    fetch(base + path);
  }
  send({base: "/api", path: "/items"});
`, function(r) {
  return r.fetchCallSites.some(function(s) { return s.url === "/api/items"; });
});

// ═════════════════════════════════════════════════════════════════════
// § 13.16 SequenceExpression — `(a, b, c)` evaluates to c
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 13.16 SequenceExpression ===\n");

specTest("§ 13.16: sequence expression value is the last element (comma operator)", `
  function f() {
    this.url = (1, 2, "/api/x");
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" &&
         effects[0].value.value === "/api/x";
});

specTest("§ 13.14: ternary value preserves both arms as or(cons, alt)", `
  function f(b) {
    this.url = b ? "/yes" : "/no";
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  var v = effects[0].value;
  return v && v.kind === "or" &&
         v.left && v.left.kind === "const" && v.left.value === "/yes" &&
         v.right && v.right.kind === "const" && v.right.value === "/no";
});

// ═════════════════════════════════════════════════════════════════════
// § 13.2.5.4 step 8 — SpreadElement in ObjectExpression
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 13.2.5.4 step 8: ObjectExpression spread ===\n");

// ═════════════════════════════════════════════════════════════════════
// § 13.2.4 ArrayLiteral evaluation
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 13.2.4 ArrayLiteral eval ===\n");

// ═════════════════════════════════════════════════════════════════════
// § 14.2.16 AwaitExpression — passthrough operand value
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 14.2.16 AwaitExpression ===\n");

// ═════════════════════════════════════════════════════════════════════
// § 10.2.10 Per-call-site instantiation — Param substitution
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 10.2.10 Per-call-site instantiation ===\n");

// ═════════════════════════════════════════════════════════════════════
// § 22.1.3 String.prototype built-ins
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 22.1.3 String.prototype built-ins ===\n");

specTest("§ 22.1.3.27: `Const(\"get\").toUpperCase()` resolves to Const(\"GET\")", `
  function f() {
    this.method = "get".toUpperCase();
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === "GET";
});

specTest("§ 22.1.3.25: `Const(\"POST\").toLowerCase()` resolves to Const(\"post\")", `
  function f() {
    this.method = "POST".toLowerCase();
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === "post";
});

specTest("§ 23.1.3 / § 9.4.2: array-lit `[a,b,c].length` resolves to Const(3)", `
  function f() {
    this.n = ["a", "b", "c"].length;
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === 3;
});

specTest("§ 23.1: array-lit numeric index access `[a,b][1]` resolves to Const(b)", `
  function f() {
    this.x = ["first", "second"][1];
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === "second";
});

specTest("§ 13.10: obj-lit static prop access `{a: 1, b: 2}.a` resolves to Const(1)", `
  function f() {
    this.x = { a: 1, b: 2 }.a;
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === 1;
});

specTest("§ 21.1.3.6: `Const(42).toString()` resolves to Const(\"42\")", `
  function f() {
    this.s = (42).toString();
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === "42";
});

specTest("§ 21.1.3.6: `Const(255).toString(16)` resolves to Const(\"ff\")", `
  function f() {
    this.s = (255).toString(16);
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === "ff";
});

// ═════════════════════════════════════════════════════════════════════
// § 19.2.6 URI handling built-ins
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 19.2.6 URI handling built-ins ===\n");

specTest("§ 19.2.6.5: `encodeURIComponent(Const(\"a b\"))` resolves to Const(\"a%20b\")", `
  function f() {
    this.q = encodeURIComponent("a b");
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === "a%20b";
});

specTest("§ 19.2.6.4: `decodeURIComponent(Const(\"a%20b\"))` resolves to Const(\"a b\")", `
  function f() {
    this.q = decodeURIComponent("a%20b");
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === "a b";
});

// ═════════════════════════════════════════════════════════════════════
// § 19.2.2 isFinite, § 19.2.3 isNaN — global predicate functions
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 19.2.2/19.2.3 isFinite/isNaN ===\n");

specTest("§ 19.2.2: `isFinite(Const(42))` resolves to Const(true)", `
  function f() {
    this.x = isFinite(42);
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === true;
});

specTest("§ 19.2.2: `isFinite(Const(Infinity))` resolves to Const(false) — wait, Infinity is special; use NaN-derived", `
  function f() {
    this.x = isFinite(0/0);
  }
`, function(effects) {
  // Note: BinaryExpression `0/0` evaluates per § 13.7 to NaN.
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === false;
});

specTest("§ 19.2.3: `isNaN(Const(\"hello\"))` resolves to Const(true)", `
  function f() {
    this.x = isNaN("hello");
  }
`, function(effects) {
  // isNaN coerces string "hello" via ToNumber → NaN → true.
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === true;
});

specTest("§ 19.2.3: `isNaN(Const(42))` resolves to Const(false)", `
  function f() {
    this.x = isNaN(42);
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === false;
});

// ═════════════════════════════════════════════════════════════════════
// § 19.2.4 parseFloat, § 19.2.5 parseInt
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 19.2.4/19.2.5 parseFloat/parseInt ===\n");

specTest("§ 19.2.4: `parseFloat(Const(\"3.14\"))` resolves to Const(3.14)", `
  function f() {
    this.pi = parseFloat("3.14");
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === 3.14;
});

specTest("§ 19.2.5: `parseInt(Const(\"42px\"))` resolves to Const(42) (single-arg form)", `
  function f() {
    this.n = parseInt("42px");
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === 42;
});

specTest("§ 19.2.5: `parseInt(Const(\"ff\"), Const(16))` resolves to Const(255) (two-arg form)", `
  function f() {
    this.n = parseInt("ff", 16);
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === 255;
});

// ═════════════════════════════════════════════════════════════════════
// § 21.1.1.1 Number, § 22.1.1.1 String, § 20.3.1.1 Boolean coercions
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 21.1.1.1/§ 22.1.1.1/§ 20.3.1.1 Number/String/Boolean ===\n");

specTest("§ 21.1.1.1: `Number(Const(\"42\"))` resolves to Const(42)", `
  function f() {
    this.n = Number("42");
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === 42;
});

specTest("§ 22.1.1.1: `String(Const(123))` resolves to Const(\"123\")", `
  function f() {
    this.s = String(123);
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === "123";
});

specTest("§ 20.3.1.1: `Boolean(Const(0))` resolves to Const(false)", `
  function f() {
    this.b = Boolean(0);
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === false;
});

specTest("§ 20.3.1.1: `Boolean(Const(\"x\"))` resolves to Const(true)", `
  function f() {
    this.b = Boolean("x");
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === true;
});

// Negative case: shadowed Number must NOT trigger built-in evaluation.
specTest("§ 21.1.1.1: shadowed `Number` (local var) does NOT evaluate as built-in", `
  function f() {
    var Number = function (x) { return "shadowed"; };
    this.n = Number("42");
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  // The local Number returns "shadowed" — but our Const-return path
  // evaluates the inner function body (single ReturnStatement returning
  // a StringLiteral). Either we get Const("shadowed") OR top — both are
  // acceptable; what's UNACCEPTABLE is Const(42) (built-in firing).
  var av = effects[0].value;
  if (av && av.kind === "const" && av.value === 42) return false;
  return true;
});

// ═════════════════════════════════════════════════════════════════════
// § 21.3.2 Math.* built-ins
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 21.3.2 Math.* built-ins ===\n");

specTest("§ 21.3.2.16: `Math.floor(Const(3.7))` resolves to Const(3)", `
  function f() {
    this.n = Math.floor(3.7);
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === 3;
});

specTest("§ 21.3.2.10: `Math.ceil(Const(3.2))` resolves to Const(4)", `
  function f() {
    this.n = Math.ceil(3.2);
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === 4;
});

specTest("§ 21.3.2.1: `Math.abs(Const(-7))` resolves to Const(7)", `
  function f() {
    this.n = Math.abs(-7);
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === 7;
});

specTest("§ 21.3.2.29: `Math.round(Const(2.5))` resolves to Const(3)", `
  function f() {
    this.n = Math.round(2.5);
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === 3;
});

specTest("§ 21.3.2.25: `Math.max(Const(1), Const(5), Const(3))` resolves to Const(5)", `
  function f() {
    this.n = Math.max(1, 5, 3);
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === 5;
});

specTest("§ 21.3.2.26: `Math.min(Const(8), Const(2), Const(5))` resolves to Const(2)", `
  function f() {
    this.n = Math.min(8, 2, 5);
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === 2;
});

specTest("§ 21.3.2.27: `Math.pow(Const(2), Const(10))` resolves to Const(1024)", `
  function f() {
    this.n = Math.pow(2, 10);
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === 1024;
});

// Negative case: shadowed Math must NOT trigger built-in evaluation.
specTest("§ 21.3.2: shadowed `Math` (local var) does NOT evaluate as built-in", `
  function f() {
    var Math = { floor: function (x) { return -1; } };
    this.n = Math.floor(3.7);
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  // Must NOT get Const(3) — the global built-in would have produced that.
  // The local Math.floor returns -1 from a Const-return body; the analyzer
  // may resolve to Const(-1) or top — both are acceptable.
  var av = effects[0].value;
  if (av && av.kind === "const" && av.value === 3) return false;
  return true;
});

// ═════════════════════════════════════════════════════════════════════
// § 23.1.2.3 Array.isArray
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 23.1.2.3 Array.isArray ===\n");

specTest("§ 23.1.2.3: `Array.isArray([1,2,3])` resolves to Const(true)", `
  function f() {
    this.b = Array.isArray([1, 2, 3]);
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === true;
});

specTest("§ 23.1.2.3: `Array.isArray({a:1})` resolves to Const(false)", `
  function f() {
    this.b = Array.isArray({a: 1});
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === false;
});

specTest("§ 23.1.2.3: `Array.isArray(\"x\")` resolves to Const(false)", `
  function f() {
    this.b = Array.isArray("x");
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === false;
});

// ═════════════════════════════════════════════════════════════════════
// § 25.5.2 JSON.parse, § 25.5.4 JSON.stringify
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 25.5.2/25.5.4 JSON.parse/JSON.stringify ===\n");

specTest("§ 25.5.4: `JSON.stringify({a:1, b:\"x\"})` resolves to canonical JSON Const string", `
  function f() {
    this.s = JSON.stringify({a: 1, b: "x"});
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" &&
         effects[0].value.value === '{"a":1,"b":"x"}';
});

specTest("§ 25.5.4: `JSON.stringify([1, \"two\", true])` resolves to Const(\"[1,\\\"two\\\",true]\")", `
  function f() {
    this.s = JSON.stringify([1, "two", true]);
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" &&
         effects[0].value.value === '[1,"two",true]';
});

specTest("§ 25.5.2: `JSON.parse(Const(\"{\\\"k\\\":42}\"))` resolves to obj-lit with Const(42) value", `
  function f() {
    var o = JSON.parse('{"k":42}');
    this.x = o.k;
  }
`, function(effects) {
  // After JSON.parse → obj-lit { k: Const(42) }, member access this.x = o.k
  // should resolve via _specMemberExpressionAv to Const(42).
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === 42;
});

specTest("§ 25.5.4: nested obj/array stringifies in source-property order", `
  function f() {
    this.s = JSON.stringify({first: 1, second: [2, 3], third: "x"});
  }
`, function(effects) {
  // Property insertion order per § 7.3.21 EnumerableOwnPropertyNames.
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" &&
         effects[0].value.value === '{"first":1,"second":[2,3],"third":"x"}';
});

// Negative case: JSON.stringify of param-derived value cannot be a Const.
specTest("§ 25.5.4: `JSON.stringify(param)` does NOT collapse to Const (param is opaque)", `
  function f(input) {
    this.s = JSON.stringify(input);
  }
`, function(effects) {
  // Effects has top — Const here would be a placeholder.
  if (effects.length !== 1) return false;
  var av = effects[0].value;
  return !av || av.kind !== "const";
});

// ═════════════════════════════════════════════════════════════════════
// § 23.1.3.18 Array.prototype.join
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 23.1.3.18 Array.prototype.join ===\n");

specTest("§ 23.1.3.18: array-lit join with default separator yields Const concat", `
  function f() {
    this.s = ["a", "b", "c"].join();
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  // Default separator per spec § 23.1.3.18 step 4 is ",".
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === "a,b,c";
});

specTest("§ 23.1.3.18: array-lit join with explicit separator yields Const concat", `
  function f() {
    this.path = ["api", "v1", "users"].join("/");
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === "api/v1/users";
});

specTest("§ 21.1.3.3: `Const(3.14159).toFixed(2)` resolves to Const(\"3.14\")", `
  function f() {
    this.s = (3.14159).toFixed(2);
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === "3.14";
});

specTest("§ 22.1.3.32: `Const(\"  hi  \").trim()` resolves to Const(\"hi\")", `
  function f() {
    this.h = "  hi  ".trim();
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === "hi";
});

test("§ 10.2.10: instantiating effects with caller-arg substitutes Param(0)", `
  function f(x) { this.role = x; }
`, function(r) {
  // Use the analyser + instantiator API directly to verify substitution.
  if (!r._ast) return false;
  var fnPath = null;
  globalThis.BabelBundle.traverse(r._ast, {
    FunctionDeclaration: function(p) { if (!fnPath) fnPath = p; p.skip(); }
  });
  if (!fnPath) return false;
  var effects = globalThis._specAnalyzePropertyFlow(fnPath);
  if (!effects || effects.length !== 1) return false;
  // Effect: target=this, key=const("role"), value=Param(0)
  var orig = effects[0].value;
  if (!orig || orig.kind !== "param") return false;
  // Instantiate with caller arg = Const("admin")
  var instantiated = globalThis._specInstantiateEffects([effects[0]], [{ kind: "const", value: "admin" }]);
  return instantiated && instantiated[0] && instantiated[0].value &&
         instantiated[0].value.kind === "const" && instantiated[0].value.value === "admin";
});

specTest("§ 14.2.16: `await Const` passes through to Const value", `
  async function f() {
    this.x = await "/api/x";
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === "/api/x";
});

specTest("§ 13.2.4: ArrayLiteral elements collected as array-lit abstract value", `
  function f() {
    this.list = ["a", "b", "c"];
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  var v = effects[0].value;
  if (!v || v.kind !== "array-lit" || !v.elements || v.elements.length !== 3) return false;
  return v.elements[0].kind === "const" && v.elements[0].value === "a" &&
         v.elements[1].kind === "const" && v.elements[1].value === "b" &&
         v.elements[2].kind === "const" && v.elements[2].value === "c";
});

specTest("§ 13.2.5.4 step 8: `{...other, x: 1}` merges other's known props", `
  function f() {
    var defaults = { method: "GET", role: "user" };
    this.body = { ...defaults, role: "admin" };
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  var v = effects[0].value;
  if (!v || v.kind !== "obj-lit") return false;
  // method comes from spread; role overridden by literal "admin".
  return v.props && v.props.method && v.props.method.kind === "const" &&
         v.props.method.value === "GET" &&
         v.props.role && v.props.role.kind === "const" && v.props.role.value === "admin";
});

// ═════════════════════════════════════════════════════════════════════
// § 14.7.2 WhileStatement — body's writes captured
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 14.7.2 WhileStatement ===\n");

specTest("§ 14.7.2: while-loop body writes recorded once (single-pass abstraction)", `
  function f() {
    var i = 0;
    while (i < 5) {
      this.value = i;
      i++;
    }
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].target.kind === "this" &&
         effects[0].key.kind === "const" && effects[0].key.value === "value";
});

// ═════════════════════════════════════════════════════════════════════
// § 14.15 TryStatement — try / catch / finally all walked
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 14.15 TryStatement ===\n");

// ═════════════════════════════════════════════════════════════════════
// § 13.2.5.4 ComputedPropertyName — `{[k]: v}` with const-resolvable k
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 13.2.5.4 ComputedPropertyName ===\n");

specTest("§ 13.2.5.4: computed key resolved from local const var becomes static prop", `
  function f(v) {
    var key = "role";
    this.body = { [key]: v };
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  var v = effects[0].value;
  // body's obj-lit should have `role` as a key (resolved from `key` Const).
  return v && v.kind === "obj-lit" && v.props && v.props.role &&
         v.props.role.kind === "param" && v.props.role.idx === 0;
});

specTest("§ 13.2.5.4: computed key from string-literal expression resolves directly", `
  function f(v) {
    this.body = { ["explicit"]: v };
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  var v = effects[0].value;
  return v && v.kind === "obj-lit" && v.props && v.props.explicit &&
         v.props.explicit.kind === "param" && v.props.explicit.idx === 0;
});

specTest("§ 14.15: try, catch AND finally blocks all contribute property writes", `
  function f() {
    try {
      this.attempt = 1;
    } catch (e) {
      this.fallback = 2;
    } finally {
      this.cleanup = 3;
    }
  }
`, function(effects) {
  var keys = effects
    .filter(function(e) { return e.target.kind === "this" && e.key.kind === "const"; })
    .map(function(e) { return e.key.value; });
  return keys.indexOf("attempt") >= 0 &&
         keys.indexOf("fallback") >= 0 &&
         keys.indexOf("cleanup") >= 0;
});

test("§ 13.3 composition with § 23.1.3.15 forEach: stored callback iterated", `
  var hooks = [];
  function on(fn) { hooks.push(fn); }
  on(function(cfg) { fetch(cfg.endpoint); });
  hooks.forEach(function(h) { h({endpoint: "/api/y"}); });
`, function(r) {
  return r.fetchCallSites.some(function(s) { return s.url === "/api/y"; });
});

test("§ 15.3.5.13: concise-body arrow function records property write", `
  var setter = (s) => this.x = s;
  setter("v");
`, function(r) {
  // Find the arrow function via traverse and analyse it directly.
  if (!r._ast) return false;
  var arrowPath = null;
  globalThis.BabelBundle.traverse(r._ast, {
    ArrowFunctionExpression: function(p) { if (!arrowPath) arrowPath = p; p.skip(); }
  });
  if (!arrowPath) return false;
  var arrowEffects = globalThis._specAnalyzePropertyFlow(arrowPath);
  if (arrowEffects.length !== 1) return false;
  var e = arrowEffects[0];
  return e.target.kind === "this" && e.key.kind === "const" && e.key.value === "x" &&
         e.value.kind === "param" && e.value.idx === 0;
});

specTest("§ 14.3.3: nested destructuring `var [{a}, {b}] = pair`", `
  function f(pair) {
    var [{a}, {b}] = pair;
    this.first = a;
    this.second = b;
  }
`, function(effects) {
  if (effects.length !== 2) return false;
  var byKey = {};
  for (var i = 0; i < effects.length; i++) {
    var e = effects[i];
    if (e.target.kind === "this" && e.key.kind === "const") byKey[e.key.value] = e.value;
  }
  // first = member(member(param0, 0), "a")
  // second = member(member(param0, 1), "b")
  var firstOk = byKey.first && byKey.first.kind === "member" &&
                byKey.first.key.kind === "const" && byKey.first.key.value === "a" &&
                byKey.first.obj && byKey.first.obj.kind === "member" &&
                byKey.first.obj.key.kind === "const" && byKey.first.obj.key.value === 0;
  var secondOk = byKey.second && byKey.second.kind === "member" &&
                 byKey.second.key.kind === "const" && byKey.second.key.value === "b";
  return firstOk && secondOk;
});

// ═════════════════════════════════════════════════════════════════════
// § 14.12 SwitchStatement
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 14.12 SwitchStatement ===\n");

specTest("§ 14.12: each case clause contributes its property writes", `
  function f(action) {
    switch (action) {
      case "create": this.url = "/api/create"; this.method = "POST"; break;
      case "update": this.url = "/api/update"; this.method = "PUT"; break;
      case "delete": this.url = "/api/delete"; this.method = "DELETE"; break;
    }
  }
`, function(effects) {
  // Each case independently produces writes: 3 cases * 2 writes = 6 effects.
  if (effects.length !== 6) return false;
  var urlVals = [];
  var methodVals = [];
  for (var i = 0; i < effects.length; i++) {
    var e = effects[i];
    if (e.target.kind !== "this" || e.key.kind !== "const") return false;
    if (e.key.value === "url" && e.value.kind === "const") urlVals.push(e.value.value);
    else if (e.key.value === "method" && e.value.kind === "const") methodVals.push(e.value.value);
  }
  urlVals.sort();
  methodVals.sort();
  return urlVals.length === 3 && methodVals.length === 3 &&
         urlVals[0] === "/api/create" && urlVals[1] === "/api/delete" && urlVals[2] === "/api/update" &&
         methodVals[0] === "DELETE" && methodVals[1] === "POST" && methodVals[2] === "PUT";
});

specTest("§ 14.12: default clause contributes its property writes", `
  function f(action) {
    switch (action) {
      case "create": this.url = "/api/create"; break;
      default: this.url = "/api/list";
    }
  }
`, function(effects) {
  if (effects.length !== 2) return false;
  var urls = [];
  for (var i = 0; i < effects.length; i++) {
    var e = effects[i];
    if (e.target.kind === "this" && e.key.kind === "const" && e.key.value === "url" && e.value.kind === "const") {
      urls.push(e.value.value);
    }
  }
  urls.sort();
  return urls.length === 2 && urls[0] === "/api/create" && urls[1] === "/api/list";
});

// ═════════════════════════════════════════════════════════════════════
// § 14.4 EmptyStatement, § 14.16 DebuggerStatement, § 14.13 LabelledStatement
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 14.4/14.13/14.16 Empty/Labeled/Debugger ===\n");

specTest("§ 14.4: empty statement is a no-op (sibling effects unaffected)", `
  function f() {
    ;
    this.x = 1;
    ;
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === 1;
});

specTest("§ 14.16: debugger statement is a no-op (sibling effects unaffected)", `
  function f() {
    debugger;
    this.x = 1;
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === 1;
});

specTest("§ 14.13: labelled statement walks its body transparently", `
  function f() {
    outer: {
      this.x = 1;
    }
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].target.kind === "this" && effects[0].key.kind === "const" &&
         effects[0].key.value === "x" && effects[0].value.kind === "const" && effects[0].value.value === 1;
});

// ═════════════════════════════════════════════════════════════════════
// § 14.8 ContinueStatement, § 14.9 BreakStatement (halt branch model)
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 14.8/14.9 Continue/Break ===\n");

specTest("§ 14.9: break inside loop halts the iteration body branch", `
  function f(items) {
    for (var k in items) {
      if (k === "stop") break;
      this.found = k;
    }
  }
`, function(effects) {
  // Single-pass loop: body is walked once; the break halts further
  // iteration but writes after break still run in subsequent stmts within
  // the SAME stmt-list — here, the break is inside the if's consequent,
  // so the assignment AFTER the if is reachable on the false-branch.
  // Effect: this.found = loop-key.
  if (effects.length !== 1) return false;
  return effects[0].target.kind === "this" && effects[0].key.kind === "const" && effects[0].key.value === "found" &&
         effects[0].value.kind === "loop-key";
});

specTest("§ 14.9: break inside switch case halts that case's branch", `
  function f(action) {
    switch (action) {
      case "a":
        this.first = 1;
        break;
        this.unreachable = "never"; // dead code post-break
      case "b":
        this.second = 2;
        break;
    }
  }
`, function(effects) {
  // Case A: this.first=1 is recorded; this.unreachable should NOT be
  // recorded because break halted the branch. Case B: this.second=2.
  // Total effects: 2 (no unreachable).
  if (effects.length !== 2) return false;
  var keys = effects.map(function(e) { return e.key.value; }).sort();
  return keys[0] === "first" && keys[1] === "second";
});

// ═════════════════════════════════════════════════════════════════════
// § 13.4 UpdateExpression (++, --), § 13.15.3 Compound Assignment
// ═════════════════════════════════════════════════════════════════════
console.log("\n=== § 13.4/13.15.3 Update + Compound Assignment ===\n");

specTest("§ 13.4: prefix ++ on Const(5) yields Const(6) and rebinds", `
  function f() {
    var x = 5;
    ++x;
    this.r = x;
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === 6;
});

specTest("§ 13.4: postfix ++ rebinds but expression value is OLD", `
  function f() {
    var x = 5;
    var y = x++;
    this.x = x;
    this.y = y;
  }
`, function(effects) {
  if (effects.length !== 2) return false;
  var byKey = {};
  for (var i = 0; i < effects.length; i++) {
    var e = effects[i];
    if (e.target.kind === "this" && e.key.kind === "const") byKey[e.key.value] = e.value;
  }
  return byKey.x && byKey.x.kind === "const" && byKey.x.value === 6 &&
         byKey.y && byKey.y.kind === "const" && byKey.y.value === 5;
});

specTest("§ 13.4: prefix -- on Const(10) yields Const(9)", `
  function f() {
    var x = 10;
    this.r = --x;
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === 9;
});

specTest("§ 13.15.3: `+=` on Const numeric performs Number::add", `
  function f() {
    var x = 10;
    x += 5;
    this.r = x;
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === 15;
});

specTest("§ 13.15.3: `+=` on Const string performs string concat", `
  function f() {
    var s = "hello, ";
    s += "world";
    this.r = s;
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  return effects[0].value && effects[0].value.kind === "const" && effects[0].value.value === "hello, world";
});

specTest("§ 13.15.3: `*=`, `-=`, `/=`, `%=` on Const numerics", `
  function f() {
    var a = 10; a *= 3;  // 30
    var b = 10; b -= 4;  // 6
    var c = 12; c /= 4;  // 3
    var d = 10; d %= 3;  // 1
    this.a = a; this.b = b; this.c = c; this.d = d;
  }
`, function(effects) {
  if (effects.length !== 4) return false;
  var byKey = {};
  for (var i = 0; i < effects.length; i++) {
    var e = effects[i];
    if (e.target.kind === "this" && e.key.kind === "const" && e.value.kind === "const") {
      byKey[e.key.value] = e.value.value;
    }
  }
  return byKey.a === 30 && byKey.b === 6 && byKey.c === 3 && byKey.d === 1;
});

specTest("§ 13.15.5: `||=` keeps the original Const non-falsy value", `
  function f() {
    var x = "kept";
    x ||= "fallback";
    this.r = x;
  }
`, function(effects) {
  if (effects.length !== 1) return false;
  // Per § 13.15.5 LogicalAssignment ||=, if LHS is truthy, RHS isn't
  // evaluated; result = LHS. Our or-model abstracts this as or(lhs, rhs)
  // — both branches are possible from analyzer view.
  var av = effects[0].value;
  if (!av) return false;
  if (av.kind === "const" && av.value === "kept") return true;
  if (av.kind === "or") {
    var leftOk = av.left && av.left.kind === "const" && av.left.value === "kept";
    var rightOk = av.right && av.right.kind === "const" && av.right.value === "fallback";
    return leftOk && rightOk;
  }
  return false;
});

// ── Summary ──
console.log("\n" + "=".repeat(50));
console.log("Spec Test Results: " + passed + "/" + total + " passed, " + failed + " failed");
if (failed > 0) {
  process.exit(1);
} else {
  console.log("All spec tests passed!");
}
