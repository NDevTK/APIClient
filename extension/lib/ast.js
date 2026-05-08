"use strict";
// lib/ast.js — AST-based JS bundle analysis engine
// Uses Babel parser + traverse for scope-aware data flow tracing.
// Traces from call sites through wrapper functions to network sinks
// (fetch, XMLHttpRequest) to learn API parameters and valid values.
// Security code review: DOM XSS sinks, dangerous patterns (eval,
// postMessage, prototype pollution, open redirect), taint tracking.

var _babelParse = BabelBundle.parse;
var _babelTraverse = BabelBundle.traverse;
var _t = BabelBundle.t;

var _HTTP_METHODS_LC = { "get":1, "post":1, "put":1, "delete":1, "patch":1, "head":1, "options":1 };

// Per-analysis state
var _constraints = {};  // scopeUid:varName → { varName, values: Set, sources: [] }
var _stats = null;
var _globalAssignments = {};  // name → { valuePath, valueNode } — tracks window.X = value
var _windowAliases = new Set();  // parameter names known to alias window/self/globalThis
var _unboundIdRefs = null;  // name → [Path, ...] of unbound-global identifier references; populated by pre-pass
var _lastIIFEFuncPath = null;  // scope context from last _resolveIIFEReturnedProperty resolution
var _sourceCode = null;  // original source text for code context extraction
var _sourceUrl = null;   // tabUrl when analyzing via live pipeline; gives us
                         // `location.origin` etc. at analysis time.
var _globalCallerCache = {};  // key → [innerPath, ...] — caches _traverseGlobalCallers results
var _typeEnv = {};  // scopeUid:varName → type string — lightweight deterministic type tracking
var _callReturnEffectMemo = new WeakMap();  // CallExpression node → { propName: [resolved values] }
// Memo for _findPrototypeMethodCallerArgs — keyed by funcPath.node with
// a sub-map of "ctorName|methodName" → result array. Profile-proven hot
// via _resolveParamFromCallers's class-method route on github bundles.
// Without memo, run hung past 10 min on 1.4 MB github subset; with memo,
// 1.4 MB completes in 19 s, full 6 MB github page in 28 s. The instance
// walk is O(instances × refs) per call; memo bounds it to one walk per
// (function, class, method) triple.
var _findPrototypeMethodCallerArgsMemo = new WeakMap();
var _propAssignMemo = new WeakMap();        // binding node → { propName: [resolved values] from `obj.propName = X` assignments via that binding's referencePaths }
var _resolveToObjectMemo = new WeakMap();   // node → resolved ObjectExpression (or null) — caches _resolveToObject result so repeated lookups on the same node are O(1)
var _traceParamToArgsMemo = new WeakMap();  // root binding → caller-arg-paths array — avoids rewalking caller chains for hot bindings
var _resolveParamFromCallersMemo = new WeakMap(); // binding identifier node → { propName: [resolved values] } — avoids rewalking caller-chain resolution for hot bindings

// Structural-def state, populated during analyzeJSBundle's traversal so
// the viewer's AST_BUILD_DEFINITION_MAP only needs the `refMap` pass
// (scope-resolved identifier references). Previously these were built by
// a separate `buildDefinitionMap` traversal that duplicated most of
// analyze's visitor work on the same 6 MB AST — ~40 s of wasted walk per
// bundle on real github. Collecting in-line lets us reuse Babel's
// already-computed scopes instead of making a second traversal rebuild
// them from scratch.
var _defMap = null;         // { name: line }
var _propDefs = null;       // { "defLine:name": { propName: propDefLine } }
var _funcMap = null;        // { name: { line, endLine, calls: Set } }
var _allFuncRanges = null;  // [{ line, endLine, calls: Set }]
var _pendingProps = null;   // [{ refLine, propName, ownerKey, binding }]
var _funcStack = null;      // enter/exit stack for attributing calls to the
                            // innermost enclosing function (call-graph)

// ── Resolver ─────────────────────────────────────────────────────────────────
// Manages cycle detection, error collection, and caching for value resolution.
// Created fresh for each analyzeJSBundle() call.

class Resolver {
  constructor() {
    this.visited = new Set();  // unified cycle detection (replaces _resolveStack)
    this.errors = [];          // collected resolution errors (replaces silent catches)
  }

  // Cycle guard: returns true if we should proceed (not a cycle).
  // If the node is already being visited, returns false (cycle detected).
  // Otherwise, marks it as visited and returns true.
  guard(prefix, node) {
    var key = prefix + node.start + ":" + node.end;
    if (this.visited.has(key)) return false;
    this.visited.add(key);
    return true;
  }

  // Remove a node from the visited set (called in finally blocks).
  unguard(prefix, node) {
    this.visited.delete(prefix + node.start + ":" + node.end);
  }

  // Collect a resolution error instead of silently swallowing it.
  // context: short label describing what operation failed (e.g. "resolveCallReturn")
  collectError(e, context) {
    var entry = { context: context || "unknown", message: e.message || String(e) };
    if (e.stack) {
      var lines = e.stack.split("\n");
      entry.stack = lines.slice(0, 6).join("\n");
    }
    this.errors.push(entry);
  }
}

var _resolver = null;  // current Resolver instance (set per analysis)

// ── Shared helpers ──

// Visitor mixin: skip nested function scopes during sub-tree traversal.
// Usage: funcPath.traverse(Object.assign({ ... }, _SKIP_NESTED_FUNCS));
var _SKIP_NESTED_FUNCS = {
  FunctionDeclaration: function(p) { p.skip(); },
  FunctionExpression: function(p) { p.skip(); },
  ArrowFunctionExpression: function(p) { p.skip(); },
};

// ─── Minifier Pattern Decompression (display-only) ──────────────────────────

// Expand a single minified ExpressionStatement into readable statements.
// Converts: (a, b, c) → a; b; c;
//           cond && body → if (cond) { body }
//           cond || body → if (!cond) { body }
//           test ? a : b → if (test) { a } else { b }
function _expandMinifiedStatement(stmt) {
  if (!stmt || stmt.type !== "ExpressionStatement") return [stmt];
  var expr = stmt.expression;
  if (expr.type === "SequenceExpression") {
    return expr.expressions.map(function(e) { return _t.expressionStatement(e); });
  }
  if (expr.type === "LogicalExpression" && expr.operator === "&&") {
    return [_t.ifStatement(expr.left, _exprToBlock(expr.right))];
  }
  if (expr.type === "LogicalExpression" && expr.operator === "||") {
    return [_t.ifStatement(_t.unaryExpression("!", expr.left), _exprToBlock(expr.right))];
  }
  if (expr.type === "ConditionalExpression") {
    return [_t.ifStatement(expr.test, _exprToBlock(expr.consequent), _exprToBlock(expr.alternate))];
  }
  return [stmt];
}

// Wrap an expression in a BlockStatement, expanding SequenceExpressions.
function _exprToBlock(expr) {
  var stmts = expr.type === "SequenceExpression"
    ? expr.expressions.map(function(e) { return _t.expressionStatement(e); })
    : [_t.expressionStatement(expr)];
  return _t.blockStatement(stmts);
}

// Clone an AST node and expand minifier patterns in all statement bodies.
// Uses iterative stack traversal. Newly created BlockStatements from expansions
// are pushed onto the stack, so nested patterns (e.g. a && (b(), c && d())) are
// expanded recursively without actual recursion.
function _decompressForDisplay(node) {
  if (typeof _t.cloneDeep !== "function") return node;
  var clone = _t.cloneDeep(node);
  var stack = [clone];
  while (stack.length > 0) {
    var cur = stack.pop();
    if (!cur || typeof cur !== "object") continue;
    if (Array.isArray(cur.body)) {
      var newBody = [];
      for (var i = 0; i < cur.body.length; i++) {
        var expanded = _expandMinifiedStatement(cur.body[i]);
        for (var j = 0; j < expanded.length; j++) newBody.push(expanded[j]);
      }
      cur.body = newBody;
    }
    var keys = Object.keys(cur);
    for (var k = 0; k < keys.length; k++) {
      if (keys[k] === "type" || keys[k] === "start" || keys[k] === "end" || keys[k] === "loc") continue;
      var child = cur[keys[k]];
      if (child && typeof child === "object") {
        if (Array.isArray(child)) {
          for (var c = 0; c < child.length; c++) {
            if (child[c] && typeof child[c] === "object" && child[c].type) stack.push(child[c]);
          }
        } else if (child.type) {
          stack.push(child);
        }
      }
    }
  }
  return clone;
}

// Generate readable code from an AST node using @babel/generator.
// Decompresses minifier patterns (&&, ||, ternary, comma sequences) on a clone
// before generating, so code context in security findings is human-readable.
// Falls back to null if generator is unavailable. Caps output at maxLines lines.
function _generateCode(node, maxLines) {
  if (!maxLines) maxLines = 15;
  try {
    if (typeof BabelBundle.generate === "function") {
      var target = _decompressForDisplay(node);
      var out = BabelBundle.generate(target, { compact: false, concise: false }).code;
      var lines = out.split("\n");
      if (lines.length > maxLines) {
        lines = lines.slice(0, maxLines);
        lines.push("  \u2026");
      }
      return lines.join("\n");
    }
  } catch (e) { _resolver.collectError(e, "generateCode"); }
  return null;
}

// Check if an identifier name refers to a global window-like object (window, self, globalThis)
// that is not shadowed by a local binding, or a known window alias from IIFE detection.
function _isGlobalObject(name, scope) {
  if ((name === "window" || name === "self" || name === "globalThis") && !scope.getBinding(name)) return true;
  return _windowAliases.has(name);
}

// Check if a callee node is a call to the global fetch function.
// Handles: fetch(), window.fetch(), self.fetch(), globalThis.fetch(), alias.fetch(),
// and LogicalExpression guards: (s.fetch || fetch)(url), (fetch || s.fetch)(url).
// Iterative work-stack walk over LogicalExpression nodes. Replaces the
// recursive `left || right` recursion that grew the JS stack linearly
// with chain length.
function _isGlobalFetchCall(callee, scope) {
  var stack = [callee];
  while (stack.length > 0) {
    var c = stack.pop();
    if (!c) continue;
    if (_t.isIdentifier(c, { name: "fetch" }) && !scope.getBinding("fetch")) return true;
    if (_t.isMemberExpression(c) && _t.isIdentifier(c.property, { name: "fetch" }) &&
        _t.isIdentifier(c.object) && _isGlobalObject(c.object.name, scope)) return true;
    if (_t.isLogicalExpression(c)) {
      stack.push(c.right);
      stack.push(c.left);
    }
  }
  return false;
}

// Check if a MemberExpression's object node traces to an XMLHttpRequest instance.
// Uses the type tracker first, then falls back to binding init resolution.
// For factory call inits (`var x = factory.someMethod()`), resolves the
// callee through scope to its function definition and checks whether
// any return statement yields `new XMLHttpRequest()` — pure spec-grounded
// data flow per ECMA-262 § 15.3 (FunctionBody) and § 13.3.2
// (NewExpression). No method-name shortcut.
function _isXhrObject(path, objectNode) {
  if (!_t.isIdentifier(objectNode)) return false;
  var objType = _getTrackedType(path, objectNode);
  if (objType === "XMLHttpRequest") return true;
  if (objType) return false;
  var binding = path.scope.getBinding(objectNode.name);
  if (!binding || !_t.isVariableDeclarator(binding.path.node) || !binding.path.node.init) return false;
  var init = binding.path.node.init;
  if (_t.isNewExpression(init) && _t.isIdentifier(init.callee, { name: "XMLHttpRequest" }) &&
      !path.scope.getBinding("XMLHttpRequest")) return true;
  if (_t.isCallExpression(init) || _t.isOptionalCallExpression(init)) {
    var initPath = binding.path.get("init");
    var calleeFn = _resolveCalleeFuncPath(initPath, 0);
    if (calleeFn && calleeFn.node && _t.isFunction(calleeFn.node) && calleeFn.node.body) {
      // Scan body for `return new XMLHttpRequest()` — scope-checked
      // global. Stop at first match. Spec: function-body return
      // statements per § 15.3 produce the call result.
      var foundXhr = false;
      try {
        calleeFn.traverse({
          ReturnStatement: function(retPath) {
            if (foundXhr) return;
            var arg = retPath.node.argument;
            if (arg && _t.isNewExpression(arg) &&
                _t.isIdentifier(arg.callee, { name: "XMLHttpRequest" }) &&
                !retPath.scope.getBinding("XMLHttpRequest")) {
              foundXhr = true;
            }
          },
          // Stop into nested functions — their returns are for them, not us.
          FunctionDeclaration: function(p) { p.skip(); },
          FunctionExpression: function(p) { p.skip(); },
          ArrowFunctionExpression: function(p) { p.skip(); },
          ClassMethod: function(p) { p.skip(); },
          ObjectMethod: function(p) { p.skip(); },
        });
      } catch (e) { _resolver.collectError(e, "isXhrObjectFactoryReturn"); }
      if (foundXhr) return true;
    }
  }
  return false;
}

// Find the parameter index for a named parameter in a function's params array.
// Handles both plain identifiers and default values (AssignmentPattern).
// Returns -1 if not found.
function _findParamIndex(params, name) {
  for (var i = 0; i < params.length; i++) {
    var p = params[i];
    if (_t.isIdentifier(p) && p.name === name) return i;
    if (_t.isAssignmentPattern(p) && _t.isIdentifier(p.left) && p.left.name === name) return i;
  }
  return -1;
}

// Match `name` against the ctor's params including destructured ObjectPattern
// parameters. Returns {idx, key} — idx is the param index, key is the
// destructured property name when the param is `{key}` / `{key} = {}`, or
// null for a plain Identifier param. Returns null when no match.
//
// Real-world case: `class C { constructor({apiUrl}) { this.apiUrl = apiUrl } }`.
// `_findThisAssignedParam` spots `this.apiUrl = apiUrl` and reports the RHS
// name "apiUrl". A plain `_findParamIndex` lookup fails because param[0]
// is an ObjectPattern, not an Identifier "apiUrl" — so the resolver bails
// out and the fetch(this.apiUrl, …) site hits a gap even though the value
// is fully traceable via the caller's object-literal arg.
function _findCtorParamOrDestr(params, name) {
  for (var i = 0; i < params.length; i++) {
    var p = params[i];
    if (_t.isIdentifier(p) && p.name === name) return { idx: i, key: null };
    if (_t.isAssignmentPattern(p) && _t.isIdentifier(p.left) && p.left.name === name) return { idx: i, key: null };
    var pat = _t.isObjectPattern(p) ? p :
              (_t.isAssignmentPattern(p) && _t.isObjectPattern(p.left) ? p.left : null);
    if (pat) {
      var k = _findDestructuredKey(pat, name);
      if (k) return { idx: i, key: k };
    }
  }
  return null;
}

// Given a caller's argument path at a `new C(arg)` call, extract the value
// of a destructured property. If the arg is an ObjectExpression, pull the
// property value directly; otherwise resolve the arg through _resolveToObject
// and pull the property. Returns a [values] array.
function _resolveCtorArgDestrProp(argPath, key, depth) {
  if (!argPath) return [];
  if (_t.isObjectExpression(argPath.node)) {
    var props = argPath.node.properties;
    for (var pi = 0; pi < props.length; pi++) {
      var op = props[pi];
      if (!_t.isObjectProperty(op) || op.computed) continue;
      var opKey = _t.isIdentifier(op.key) ? op.key.name :
        (_t.isStringLiteral(op.key) ? op.key.value : null);
      if (opKey === key) {
        return _resolveAllValues(argPath.get("properties." + pi + ".value"), depth + 1);
      }
    }
  }
  var obj = _resolveToObject(argPath, depth);
  if (obj && obj.properties) {
    for (var pi2 = 0; pi2 < obj.properties.length; pi2++) {
      var op2 = obj.properties[pi2];
      if (!_t.isObjectProperty(op2) || op2.computed) continue;
      var op2Key = _t.isIdentifier(op2.key) ? op2.key.name :
        (_t.isStringLiteral(op2.key) ? op2.key.value : null);
      if (op2Key === key) {
        // Object.assign-merged synthetic obj has no node-level _path
        // (no Babel path owns the merged set). Each property's value
        // carries its own _path attached at synthesis time, so prefer
        // that. For real ObjectExpression obj, derive from obj._path.
        if (op2.value && op2.value._path) {
          return _resolveAllValues(op2.value._path, depth + 1);
        }
        if (obj._path) {
          return _resolveAllValues(obj._path.get("properties." + pi2 + ".value"), depth + 1);
        }
      }
    }
  }
  return [];
}

// Find indirect constructor-call arg paths for a class exposed via
// `Object.defineProperty(target, key, {get: () => CLASS})`. Reads of
// `<aliasOf(target)>.<key>` invoke the getter (returning the class);
// `new <expr>.<key>(args)` constructs the class with `args`. Returns
// the arg-paths at the corresponding constructor positions.
//
// Pure ECMAScript: getter invocation semantics + scope-resolved
// reference walks. No bundler-specific assumptions; the same flow works
// for any code that exposes a class via Object.defineProperty getter.
function _findClassIndirectCtorArgs(classBinding, classDecl, paramIdx, depth) {
  if (!classBinding || !classBinding.referencePaths) return [];
  var argPaths = [];
  var visited = new Set();
  for (var ri = 0; ri < classBinding.referencePaths.length; ri++) {
    var ref = classBinding.referencePaths[ri];
    // Match shape `() => CLASS` arrow whose body IS the class identifier.
    var arrow = ref.parentPath;
    if (!arrow || !arrow.isArrowFunctionExpression() || arrow.node.body !== ref.node) continue;
    // Arrow must be the value of an ObjectProperty `{key: arrow}`.
    var prop = arrow.parentPath;
    if (!prop || !prop.isObjectProperty() || prop.node.computed) continue;
    var propKey = _t.isIdentifier(prop.node.key) ? prop.node.key.name :
      (_t.isStringLiteral(prop.node.key) ? prop.node.key.value : null);
    if (!propKey) continue;
    var descObj = prop.parentPath;
    if (!descObj || !descObj.isObjectExpression()) continue;
    var defCall = descObj.parentPath;
    if (!defCall || !defCall.isCallExpression()) continue;
    // Case A: arrow is the descriptor's `get` AND descriptor is arg[2]
    // of Object.defineProperty(target, key, descriptor) — direct install.
    if (propKey === "get" && defCall.node.arguments[2] === descObj.node && _isObjectDefineProperty(defCall)) {
      // (handled below via instKey extraction)
    } else {
      // Case B: arrow is in a defs literal `{propKey: () => CLASS}`
      // passed to a wrapper call `wrapper(target, defs)` whose body
      // iterates defs and does `Object.defineProperty(target, k, {get: defs[k]})`
      // for each key. The effective install is `target.<propKey>` →
      // CLASS. Trace through the wrapper to find the target arg, then
      // find consumers.
      var wrapperFunc = _resolveExprToFunctionPath(defCall.get("callee"), depth);
      if (!wrapperFunc || !wrapperFunc.node.body || !_t.isBlockStatement(wrapperFunc.node.body)) continue;
      // Locate which param of wrapperFunc corresponds to defs (the obj
      // containing our arrow).
      var defsArgIdx = -1;
      var wrapperIsCallForm = false;
      var wcallee = defCall.node.callee;
      if (_t.isMemberExpression(wcallee) && !wcallee.computed && _t.isIdentifier(wcallee.property, { name: "call" })) {
        wrapperIsCallForm = true;
      }
      var wrapArgOffset = wrapperIsCallForm ? 1 : 0;
      for (var dai = 0; dai < defCall.node.arguments.length; dai++) {
        if (defCall.node.arguments[dai] === descObj.node) { defsArgIdx = dai - wrapArgOffset; break; }
      }
      if (defsArgIdx < 0 || defsArgIdx >= wrapperFunc.node.params.length) continue;
      var defsParam = wrapperFunc.node.params[defsArgIdx];
      if (!_t.isIdentifier(defsParam)) continue;
      // Inside wrapper body, find Object.defineProperty(<targetParam>, k, {get: defs[k]})
      // inside `for (var k in defs)` — this is the install pattern.
      var defsParamBinding = wrapperFunc.scope.getBinding(defsParam.name);
      if (!defsParamBinding || !defsParamBinding.referencePaths) continue;
      var foundTargetArgIdx = -1;
      for (var dpr = 0; dpr < defsParamBinding.referencePaths.length; dpr++) {
        var dpRef = defsParamBinding.referencePaths[dpr];
        // defs[k] reads — defs as object of MemberExpression with computed=true and property = loop var
        var dpMem = dpRef.parentPath;
        if (!dpMem || !dpMem.isMemberExpression() || dpMem.node.object !== dpRef.node || !dpMem.node.computed) continue;
        // Walk up to enclosing for..in.
        var fInside = _findEnclosingForIn(dpMem);
        if (!fInside) continue;
        var loopVar = _getForInLoopVarName(fInside);
        if (!loopVar) continue;
        if (!_t.isIdentifier(dpMem.node.property, { name: loopVar })) continue;
        // Containing call: must be the `get` of an Object.defineProperty descriptor.
        var dpProp = dpMem.parentPath;
        if (!dpProp || !dpProp.isObjectProperty() || dpProp.node.computed) continue;
        if (!_t.isIdentifier(dpProp.node.key, { name: "get" }) &&
            !(_t.isStringLiteral(dpProp.node.key) && dpProp.node.key.value === "get")) continue;
        var dpDesc = dpProp.parentPath;
        if (!dpDesc || !dpDesc.isObjectExpression()) continue;
        var dpDefCall = dpDesc.parentPath;
        if (!dpDefCall || !dpDefCall.isCallExpression()) continue;
        if (dpDefCall.node.arguments[2] !== dpDesc.node) continue;
        if (!_isObjectDefineProperty(dpDefCall)) continue;
        // The target is dpDefCall arg[0]. Find which wrapperFunc param
        // corresponds to it (must be a param identifier).
        var dpTargetNode = dpDefCall.node.arguments[0];
        if (!_t.isIdentifier(dpTargetNode)) continue;
        for (var wpi = 0; wpi < wrapperFunc.node.params.length; wpi++) {
          if (_t.isIdentifier(wrapperFunc.node.params[wpi], { name: dpTargetNode.name })) {
            foundTargetArgIdx = wpi;
            break;
          }
        }
        if (foundTargetArgIdx >= 0) break;
      }
      if (foundTargetArgIdx < 0) continue;
      // Map back to the wrapper-call's actual argument position
      // (account for .call thisArg shift).
      var callerTargetArgIdx = foundTargetArgIdx + wrapArgOffset;
      if (callerTargetArgIdx >= defCall.node.arguments.length) continue;
      var targetArgPath = defCall.get("arguments." + callerTargetArgIdx);
      if (!_t.isIdentifier(targetArgPath.node)) continue;
      var targetName = targetArgPath.node.name;
      var targetBinding = targetArgPath.scope.getBinding(targetName);
      if (!targetBinding || !targetBinding.referencePaths) continue;
      // Find `new <targetAlias>.<propKey>(args)` consumers.
      for (var tri = 0; tri < targetBinding.referencePaths.length; tri++) {
        var tRef = targetBinding.referencePaths[tri];
        var tMem = tRef.parentPath;
        if (!tMem || !tMem.isMemberExpression() || tMem.node.object !== tRef.node) continue;
        var tMatch = (!tMem.node.computed && _t.isIdentifier(tMem.node.property, { name: propKey })) ||
          (tMem.node.computed && _t.isStringLiteral(tMem.node.property) && tMem.node.property.value === propKey);
        if (!tMatch) continue;
        var tNew = tMem.parentPath;
        if (!tNew || !tNew.isNewExpression() || tNew.node.callee !== tMem.node) continue;
        if (paramIdx >= tNew.node.arguments.length) continue;
        argPaths.push(tNew.get("arguments." + paramIdx));
      }
      continue;
    }
    // Pull the property key the getter is installed under.
    var keyArg = defCall.node.arguments[1];
    var instKey = null;
    if (_t.isStringLiteral(keyArg)) instKey = keyArg.value;
    else if (_t.isIdentifier(keyArg)) {
      // Loop-variable case: defineProperty inside `for (var k in defs)`.
      // Resolve the loop variable's known iteration values via the
      // defs literal.
      var forIn = _findEnclosingForIn(defCall);
      if (forIn && _getForInLoopVarName(forIn) === keyArg.name) {
        var defsExpr = forIn.get("right");
        var defsObj2 = _resolveToObject(defsExpr, depth);
        if (defsObj2 && defsObj2.properties) {
          for (var di = 0; di < defsObj2.properties.length; di++) {
            var dp = defsObj2.properties[di];
            if (!_t.isObjectProperty(dp) || dp.computed) continue;
            // Verify this descriptor's value-expression is `defs[loopVar]`.
            var getValNode = prop.node.value;
            if (!(getValNode && _t.isMemberExpression(getValNode) && getValNode.computed &&
                  _t.isIdentifier(getValNode.property, { name: keyArg.name }))) continue;
            // For each defs key, find consumers reading that key as a constructor target.
            var dKey = _t.isIdentifier(dp.key) ? dp.key.name :
              (_t.isStringLiteral(dp.key) ? dp.key.value : null);
            if (!dKey) continue;
            // The class i shows up only at the value-position of THIS
            // defs entry — so the `<aliasOf(target)>.<dKey>` consumers
            // construct THIS class.
            var dpVal = dp.value;
            // Confirm: the dp value is itself an arrow or a Reference
            // that resolves to our class. If it's a literal `() => i`
            // arrow with body `i`, the Identifier matches our class
            // binding's own identifier.
            if (_t.isArrowFunctionExpression(dpVal) && _t.isIdentifier(dpVal.body) &&
                dpVal.body.name === (classDecl.node.id && classDecl.node.id.name)) {
              _findCallersOfPropertyOnTarget(defCall, dKey, paramIdx, argPaths, visited, depth);
            }
          }
        }
        continue;
      }
      // Identifier key that's not a for..in loop var — try resolving
      // through scope. `const KEY = "url"; Object.defineProperty(t, KEY, …)`
      // is fully resolvable: KEY traces to its var init.
      var idVals = _resolveAllValues(defCall.get("arguments.1"), depth + 1);
      if (idVals.length === 1 && typeof idVals[0] === "string") {
        instKey = idVals[0];
      } else {
        continue;
      }
    }
    if (instKey == null) continue;
    _findCallersOfPropertyOnTarget(defCall, instKey, paramIdx, argPaths, visited, depth);
  }
  return argPaths;
}

// Given an Object.defineProperty call's path and the key it installs,
// find all `new <aliasOf(target)>.<key>(args)` constructor call sites
// across the program by tracing target through scope: target is arg[0]
// of defineProperty; if it's a parameter, follow through caller args
// (with .call thisArg shift); for each materialized target binding,
// scan its referencePaths for `.<key>` accesses used as `new` targets.
// Pushes the matching argPath at paramIdx into out.
function _findCallersOfPropertyOnTarget(defCall, key, paramIdx, out, visited, depth) {
  if (visited.has(defCall.node)) return;
  visited.add(defCall.node);
  var targetArgPath = defCall.get("arguments.0");
  // Trace target back to caller-supplied arg if it's a parameter chain.
  var aliasArgPaths = _traceToCallerArg(targetArgPath, depth);
  for (var ai = 0; ai < aliasArgPaths.length; ai++) {
    var aliasPath = aliasArgPaths[ai];
    if (!_t.isIdentifier(aliasPath.node)) continue;
    var aliasName = aliasPath.node.name;
    var aliasBinding = aliasPath.scope.getBinding(aliasName);
    if (!aliasBinding || !aliasBinding.referencePaths) continue;
    for (var rj = 0; rj < aliasBinding.referencePaths.length; rj++) {
      var aRef = aliasBinding.referencePaths[rj];
      var mem = aRef.parentPath;
      if (!mem || !mem.isMemberExpression() || mem.node.object !== aRef.node) continue;
      var matchKey = (!mem.node.computed && _t.isIdentifier(mem.node.property, { name: key })) ||
        (mem.node.computed && _t.isStringLiteral(mem.node.property) && mem.node.property.value === key);
      if (!matchKey) continue;
      var newExpr = mem.parentPath;
      if (!newExpr || !newExpr.isNewExpression() || newExpr.node.callee !== mem.node) continue;
      if (paramIdx >= newExpr.node.arguments.length) continue;
      out.push(newExpr.get("arguments." + paramIdx));
    }
  }
}

// Trace `expr` back through parameter-binding chains to caller-supplied
// argument paths. If expr is bound to a function param, find that
// function's callers and return the corresponding arg paths. Both
// direct calls `fn(args)` and Function.prototype.call form
// `fn.call(thisArg, args)` are followed; the thisArg shift on .call
// means params[k] inside the function corresponds to caller arg[k+1].
function _traceToCallerArg(exprPath, depth) {
  if (!exprPath || !_t.isIdentifier(exprPath.node)) return exprPath ? [exprPath] : [];
  var b = exprPath.scope.getBinding(exprPath.node.name);
  if (!b || b.kind !== "param") return [exprPath];
  var hostFunc = b.scope.path;
  if (!hostFunc || !hostFunc.node.params) return [exprPath];
  var pIdx = -1;
  for (var i = 0; i < hostFunc.node.params.length; i++) {
    if (_t.isIdentifier(hostFunc.node.params[i], { name: exprPath.node.name })) { pIdx = i; break; }
  }
  if (pIdx < 0) return [exprPath];
  var out = [];
  // Direct callers via _findFunctionCallerArgs.
  var callers = _findFunctionCallerArgs(hostFunc);
  for (var ci = 0; ci < callers.length; ci++) {
    if (pIdx < callers[ci].length) out.push(callers[ci][pIdx]);
  }
  // .call form: scan the host function's own binding referencePaths
  // for `<expr>.call(thisArg, ...args)` invocations. The host's
  // identifier shows up in the call expression's `callee.object` slot.
  var hostBinding = _getFunctionBinding(hostFunc);
  if (hostBinding && hostBinding.referencePaths) {
    for (var hri = 0; hri < hostBinding.referencePaths.length; hri++) {
      var hRef = hostBinding.referencePaths[hri];
      var hMem = hRef.parentPath;
      if (!hMem || !hMem.isMemberExpression() || hMem.node.object !== hRef.node || hMem.node.computed) continue;
      if (!_t.isIdentifier(hMem.node.property, { name: "call" })) continue;
      var hCall = hMem.parentPath;
      if (!hCall || !hCall.isCallExpression() || hCall.node.callee !== hMem.node) continue;
      var callerIdx = pIdx + 1; // .call thisArg shift
      if (callerIdx < hCall.node.arguments.length) out.push(hCall.get("arguments." + callerIdx));
    }
  }
  return out.length ? out : [exprPath];
}

// Build a fetch call site object with standard schema.
// Tell _buildFetchSite when the URL it was given came from a pure
// StringLiteral in source (no interpolations). Only in that case can
// we safely treat the embedded query values as AST-observed literals —
// a TemplateLiteral's rendered form contains `{name}` placeholders
// that would otherwise be mistaken for real values.
// Push a built fetch-site onto the result, skipping empty/null sites.
// Callers that build via _buildFetchSite get null when the URL is empty,
// which should never be pushed onto fetchCallSites.
function _pushFetchSite(result, site) {
  if (site) result.fetchCallSites.push(site);
}
function _buildFetchSite(url, method, headers, type, params, opts) {
  // Empty-string URL is never a legitimate fetch site — it comes from
  // falsy-fallback patterns like `DOM-read || ""` or `undefined.toString()`
  // resolving to "". Drop the site; the fetch-call-site registration is
  // purely misleading and pollutes the learned-APIs list.
  if (url === "" || url == null) return null;
  var site = { url: url, method: method, headers: headers || {}, type: type };
  // If the URL was a pure StringLiteral (caller passed
  // opts.urlIsLiteral === true), lift its query-string key/value
  // pairs into `params` as AST-observed constraints. This is a
  // legitimate JS-derived signal (the author wrote `?q=hello` in
  // their source, so "hello" is a known-valid value for `q`) and
  // gives the Send form a prefill with AST provenance. We do NOT
  // perform this extraction for template-derived URLs — those carry
  // `{name}` placeholders for unresolved expressions and we'd
  // misattribute the placeholder as a real value.
  var extracted = params && params.slice ? params.slice() : (params ? params.slice() : []);
  if (opts && opts.urlIsLiteral && typeof url === "string" && url.length > 0 && url.indexOf("?") >= 0) {
    try {
      var parsed = url.match(/^[a-z][a-z0-9+.-]*:/i) ? new URL(url) : new URL(url, "https://_ast_placeholder/");
      if (parsed && parsed.searchParams && typeof parsed.searchParams.entries === "function") {
        var present = {};
        for (var i = 0; i < extracted.length; i++) {
          if (extracted[i] && extracted[i].name && extracted[i].location !== "body") {
            present[extracted[i].name] = true;
          }
        }
        for (var kv of parsed.searchParams.entries()) {
          var pname = kv[0], pval = kv[1];
          if (present[pname]) continue;
          if (!pval) continue; // empty values aren't useful examples
          extracted.push({ name: pname, location: "query", required: true, validValues: [pval] });
        }
      }
    } catch (_) {}
  }
  if (extracted && extracted.length > 0) site.params = extracted;
  if (opts) {
    if (opts.enclosingFunction) site.enclosingFunction = opts.enclosingFunction;
    if (opts.responseType) site.responseType = opts.responseType;
    if (opts.loc) site.loc = opts.loc;
  }
  return site;
}

// Unwrap JSON.stringify(x) → x. Returns the unwrapped node, or the original if not JSON.stringify.
function _unwrapJsonStringify(node, path) {
  if (_t.isCallExpression(node) && _isJsonStringify(node, path) && node.arguments.length > 0) {
    return node.arguments[0];
  }
  return node;
}

// Find `this.propName = param` in a constructor body. Returns the param name, or null.
function _findThisAssignedParam(funcPath, propName) {
  var assignedParamName = null;
  try {
    funcPath.traverse(Object.assign({
      AssignmentExpression: function(aPath) {
        if (assignedParamName) { aPath.stop(); return; }
        if (aPath.node.operator !== "=") return;
        var left = aPath.node.left;
        if (_t.isMemberExpression(left) && _t.isThisExpression(left.object) && !left.computed &&
            _t.isIdentifier(left.property, { name: propName })) {
          if (_t.isIdentifier(aPath.node.right)) assignedParamName = aPath.node.right.name;
        }
      },
    }, _SKIP_NESTED_FUNCS));
  } catch (e) { _resolver.collectError(e, "findThisAssignedParam"); }
  return assignedParamName;
}

// Richer variant: returns {paramName, propFromParam} where propFromParam is
// set when the ctor assigns `this.X = param.X` (or `this.X = param.Y`), a
// pattern minifiers emit in place of destructured params. Falls back to the
// simple `this.X = ident` match, where propFromParam is null.
//
// Example:
//   constructor(o) { this.url = o.url; }  → {paramName:"o", propFromParam:"url"}
//   constructor(u) { this.url = u; }      → {paramName:"u", propFromParam:null}
//
// Returns null when neither pattern matches.
function _findThisAssignedParamRich(funcPath, propName) {
  var result = null;
  try {
    funcPath.traverse(Object.assign({
      AssignmentExpression: function(aPath) {
        if (result) { aPath.stop(); return; }
        if (aPath.node.operator !== "=") return;
        var left = aPath.node.left;
        if (!(_t.isMemberExpression(left) && _t.isThisExpression(left.object) && !left.computed &&
              _t.isIdentifier(left.property, { name: propName }))) return;
        var right = aPath.node.right;
        // Case 1: this.X = ident
        if (_t.isIdentifier(right)) {
          result = { paramName: right.name, propFromParam: null };
          return;
        }
        // Case 2: this.X = ident.Y (non-computed member access).
        // Common minifier output for destructured ctor params.
        if (_t.isMemberExpression(right) && !right.computed &&
            _t.isIdentifier(right.object) && _t.isIdentifier(right.property)) {
          result = { paramName: right.object.name, propFromParam: right.property.name };
          return;
        }
      },
    }, _SKIP_NESTED_FUNCS));
  } catch (e) { _resolver.collectError(e, "findThisAssignedParamRich"); }
  return result;
}

// Return the Babel path of the RHS for the first `this.X = ...` assignment
// in funcPath's body, regardless of RHS shape. Used to resolve constructor
// assignments whose RHS is a call/expression (not a param ref) — the value
// is closed-over in the function's scope, so caller substitution doesn't
// apply and the RHS resolves directly.
function _findThisAssignmentRhsPath(funcPath, propName) {
  var resultPath = null;
  try {
    funcPath.traverse(Object.assign({
      AssignmentExpression: function(aPath) {
        if (resultPath) { aPath.stop(); return; }
        if (aPath.node.operator !== "=") return;
        var left = aPath.node.left;
        if (!(_t.isMemberExpression(left) && _t.isThisExpression(left.object) && !left.computed &&
              _t.isIdentifier(left.property, { name: propName }))) return;
        resultPath = aPath.get("right");
        aPath.stop();
      },
    }, _SKIP_NESTED_FUNCS));
  } catch (e) { _resolver.collectError(e, "findThisAssignmentRhsPath"); }
  return resultPath;
}

function analyzeJSBundle(code, sourceUrl, forceScript) {
  _constraints = {};
  _stats = { protoMethods: 0, protoMethodsNoField: 0, resolvedUrls: 0, interProcTraces: 0, globalAssignments: 0, windowAliases: 0 };
  _globalAssignments = {};
  _windowAliases = new Set();
  _unboundIdRefs = null;
  _resolver = new Resolver();
  _globalCallerCache = {};
  _typeEnv = {};
  _callReturnEffectMemo = new WeakMap();
  _findPrototypeMethodCallerArgsMemo = new WeakMap();
  _propAssignMemo = new WeakMap();
  _resolveToObjectMemo = new WeakMap();
  _sourceCode = code;
  _sourceLines = null;
  _sourceUrl = sourceUrl || null;
  // Structural-def state — populated by pre-pass visitors; exposed on the
  // result so buildDefinitionMap's lazy call doesn't rebuild them.
  _defMap = {};
  _propDefs = {};
  _funcMap = {};
  _allFuncRanges = [];
  _pendingProps = [];
  _funcStack = [];

  var result = {
    sourceUrl: sourceUrl,
    protoEnums: [],
    protoFieldMaps: [],
    fetchCallSites: [],
    valueConstraints: [],
    securitySinks: [],       // DOM XSS, eval, open redirect sinks
    dangerousPatterns: [],   // unsafe eval, postMessage, prototype pollution
    sourceMapUrl: extractSourceMapUrl(code),
  };

  var ast = null;
  try {
    if (forceScript) {
      // Combined cross-script analysis: parse as script (shared global scope, matching browser <script> semantics)
      ast = _babelParse(code, { sourceType: "script", plugins: ["jsx"], errorRecovery: true });
    } else {
      ast = _babelParse(code, { sourceType: "module", plugins: ["jsx"], errorRecovery: true });
    }
  } catch (e1) {
    try {
      ast = _babelParse(code, { sourceType: "script", plugins: ["jsx"], errorRecovery: true });
    } catch (e2) {
      console.debug("[AST] Parse FAILED for %s (%d chars) — %s", sourceUrl, code.length, e2.message);
      return result;
    }
  }

  // Pre-pass: collect global assignments and window aliases before main analysis
  // so that sink tracing can resolve global aliases (e.g., window.jQuery = lib),
  // and — since we already walk every node — populate the structural-def
  // indices (defMap/propDefs/funcMap/allFuncRanges/pendingProps) that the
  // source viewer needs. Bundling this into the pre-pass reuses Babel's
  // already-computed scopes instead of paying for a second full traversal
  // in a separate buildDefinitionMap pass.
  try {
  _babelTraverse(ast, {
    FunctionDeclaration: {
      enter: function(path) {
        _funcStack.push(_registerFunc(path, path.node.id ? path.node.id.name : null, path.node));
      },
      exit: function() { _funcStack.pop(); },
    },
    FunctionExpression: {
      enter: function(path) {
        var name = null;
        if (path.parent.type === "VariableDeclarator" && path.parent.id && path.parent.id.name)
          name = path.parent.id.name;
        else if (path.parent.type === "AssignmentExpression" && path.parent.left.type === "Identifier")
          name = path.parent.left.name;
        _funcStack.push(_registerFunc(path, name, path.node));
      },
      exit: function() { _funcStack.pop(); },
    },
    ArrowFunctionExpression: {
      enter: function(path) {
        var name = null;
        if (path.parent.type === "VariableDeclarator" && path.parent.id && path.parent.id.name)
          name = path.parent.id.name;
        else if (path.parent.type === "AssignmentExpression" && path.parent.left.type === "Identifier")
          name = path.parent.left.name;
        _funcStack.push(_registerFunc(path, name, path.node));
      },
      exit: function() { _funcStack.pop(); },
    },
    ClassDeclaration: function(path) {
      _registerFunc(path, path.node.id ? path.node.id.name : null, path.node);
    },
    ObjectMethod: {
      enter: function(path) {
        _funcStack.push(_registerFunc(path, path.node.key && path.node.key.name ? path.node.key.name : null, path.node));
      },
      exit: function() { _funcStack.pop(); },
    },
    ClassMethod: {
      enter: function(path) {
        var name = path.node.key && path.node.key.name ? path.node.key.name : null;
        _funcStack.push(_registerFunc(path, name, path.node));
        _registerClassMethodProp(path, name);
      },
      exit: function() { _funcStack.pop(); },
    },
    CallExpression: function(path) {
      _processIIFE(path);
      _trackCallForCallGraph(path);
    },
    MemberExpression: function(path) {
      _trackMemberPropAccess(path);
    },
    AssignmentExpression: function(path) {
      _trackGlobalAssignment(path);
      _collectPropDefsFromAssignment(path);
    },
    // Index unbound-global identifiers used as the OBJECT of a member
    // access (`$.method(args)` shape). Real-world need:
    // `var ce = {}; ce.post = fn; window.$ = ce;` then `$.post(args)`.
    // To find ce.post's callers via the `$` alias, the consumer needs
    // to enumerate `$`'s member-access usages. Restricting to the exact
    // shape we use keeps the index small and avoids stale paths from
    // unrelated identifier positions.
    MemberExpression: function(path) {
      if (path.node.computed) return;
      var obj = path.node.object;
      if (!_t.isIdentifier(obj)) return;
      // Use the path's own scope check — only TRULY unbound globals are
      // alias candidates; locally-shadowed names point at a different
      // value than the global.
      if (path.scope.getBinding(obj.name)) return;
      if (!_unboundIdRefs) _unboundIdRefs = {};
      var nm = obj.name;
      if (!_unboundIdRefs[nm]) _unboundIdRefs[nm] = [];
      _unboundIdRefs[nm].push(path.get("object"));
    },
    // Track ESM exports as global-like bindings: export { k as default, ... }
    ExportNamedDeclaration: function(path) {
      var specs = path.node.specifiers;
      if (!specs) return;
      for (var ei = 0; ei < specs.length; ei++) {
        var sp = specs[ei];
        if (_t.isIdentifier(sp.local) && !_globalAssignments[sp.local.name]) {
          _globalAssignments[sp.local.name] = { valuePath: null, valueNode: null };
          _stats.globalAssignments++;
        }
      }
    },
    ExportDefaultDeclaration: function(path) {
      var decl = path.node.declaration;
      if (_t.isIdentifier(decl) && !_globalAssignments[decl.name]) {
        _globalAssignments[decl.name] = { valuePath: null, valueNode: null };
        _stats.globalAssignments++;
      }
    },
    // Populate type tracker for deterministic constructor/literal types AND
    // capture object-literal property definitions.
    VariableDeclarator: function(path) {
      _trackTypeFromDeclarator(path);
      _collectPropDefsFromVarDecl(path);
    },
  });
  } catch (_prePassErr) {
    if (_prePassErr instanceof RangeError) {
      _resolver.collectError(_prePassErr, "prePassTraversal");
      console.debug("[AST] Pre-pass overflow on %s (%d chars) — continuing with main pass", sourceUrl, code.length);
    } else { throw _prePassErr; }
  }

  try {
  _babelTraverse(ast, {
    // ── Value constraint collection ──
    SwitchStatement: function(path) {
      _collectSwitchConstraints(path);
    },
    LogicalExpression: function(path) {
      _collectEqualityConstraints(path);
    },
    BinaryExpression: function(path) {
      if (path.node.operator === "in" &&
          _t.isIdentifier(path.node.left) && _t.isIdentifier(path.node.right)) {
        var binding = path.scope.getBinding(path.node.right.name);
        if (binding && binding.path.node.init && _t.isObjectExpression(binding.path.node.init)) {
          var keys = _getObjectKeys(binding.path.node.init);
          if (keys.length >= 1) {
            _addConstraint(path, path.node.left.name, keys, "in_object");
          }
        }
      }
      // Single equality: "value" == param.prop or param.prop === "value"
      if (path.node.operator === "==" || path.node.operator === "===" ||
          path.node.operator === "!=" || path.node.operator === "!==") {
        var eqLeft = path.node.left, eqRight = path.node.right;
        var eqLit = null, eqVar = null;
        if (_t.isStringLiteral(eqLeft)) { eqLit = eqLeft.value; eqVar = eqRight; }
        else if (_t.isStringLiteral(eqRight)) { eqLit = eqRight.value; eqVar = eqLeft; }
        if (eqLit && eqVar) {
          var eqVarName = _t.isIdentifier(eqVar) ? eqVar.name :
            (_t.isMemberExpression(eqVar) ? _memberChainKey(eqVar) : null);
          if (eqVarName) _addConstraint(path, eqVarName, [eqLit], "equality");
        }
      }
    },
    MemberExpression: function(path) {
      // Computed member access: obj[key] → key constrained to obj's property names
      if (path.node.computed && _t.isIdentifier(path.node.property)) {
        var cmObj = _resolveToObject(path.get("object"), 0);
        if (cmObj) {
          var cmKeys = _getObjectKeys(cmObj);
          if (cmKeys.length >= 1) {
            _addConstraint(path, path.node.property.name, cmKeys, "computed_member");
          }
        }
      }
    },
    CallExpression: function(path) {
      _collectIncludesConstraints(path);
      _collectIterationConstraints(path);
      _processIIFE(path);
      _processNetworkSink(path, result);
      _processExportMethodCall(path, result);
      _processSecurityCallSink(path, result);
      _processDangerousPattern(path, result);
    },
    NewExpression: function(path) {
      _processNewExpressionSink(path, result);
      _processSecurityNewSink(path, result);
    },
    // Babel 8: dynamic import() is ImportExpression, not CallExpression with Import callee
    ImportExpression: function(path) {
      var _impSrc = _traceValueSource(path.get("source"), 0);
      if (_impSrc.sourceType === "user-controlled") _pushSink(result, path.node, "eval", "import", _impSrc, path);
    },
    // ── Proto, enum, and framework-specific detection ──
    ObjectExpression: function(path) {
      _detectEnumObject(path.node, result);
      _collectObjectLiteralConstraints(path);
    },
    AssignmentExpression: function(path) {
      _trackGlobalAssignment(path);
      _detectProtoFieldAssignment(path, result);
      _processImageSrcSink(path, result);
      _processSecurityAssignSink(path, result);
      _processDangerousAssignment(path, result);
    },
  });
  } catch (_mainPassErr) {
    if (_mainPassErr instanceof RangeError) {
      _resolver.collectError(_mainPassErr, "mainPassTraversal");
      console.debug("[AST] Main pass overflow on %s (%d chars) — returning partial results", sourceUrl, code.length);
    } else { throw _mainPassErr; }
  }

  // ── Export constraints for background.js ──
  var byVar = {};
  var cKeys = Object.keys(_constraints);
  for (var i = 0; i < cKeys.length; i++) {
    var c = _constraints[cKeys[i]];
    if (!byVar[c.varName]) byVar[c.varName] = { values: new Set(), sources: [] };
    c.values.forEach(function(v) { byVar[c.varName].values.add(v); });
    byVar[c.varName].sources = byVar[c.varName].sources.concat(c.sources);
  }
  var varNames = Object.keys(byVar);
  for (var vi = 0; vi < varNames.length; vi++) {
    var vals = [];
    byVar[varNames[vi]].values.forEach(function(v) { vals.push(v); });
    result.valueConstraints.push({
      variable: varNames[vi],
      values: vals,
      sources: byVar[varNames[vi]].sources,
    });
  }

  // ── Deduplicate fetchCallSites by (method, url) — merge params and headers ──
  var _seenSites = {};
  var _dedupedSites = [];
  for (var si = 0; si < result.fetchCallSites.length; si++) {
    var _s = result.fetchCallSites[si];
    var _sk = _s.method + " " + _s.url;
    if (!_seenSites[_sk]) {
      _seenSites[_sk] = _dedupedSites.length;
      _dedupedSites.push(_s);
    } else {
      // Merge params and headers deterministically from both sites
      var existIdx = _seenSites[_sk];
      var existSite = _dedupedSites[existIdx];
      var newParams = _s.params || [];
      if (newParams.length > 0) {
        var existParamNames = {};
        var existParams = existSite.params || [];
        for (var ep = 0; ep < existParams.length; ep++) {
          existParamNames[existParams[ep].name] = true;
        }
        for (var np = 0; np < newParams.length; np++) {
          if (!existParamNames[newParams[np].name]) {
            existParams.push(newParams[np]);
          }
        }
        existSite.params = existParams;
      }
      if (_s.headers) {
        if (!existSite.headers) existSite.headers = {};
        for (var hk in _s.headers) {
          if (!existSite.headers[hk]) existSite.headers[hk] = _s.headers[hk];
        }
      }
    }
  }
  result.fetchCallSites = _dedupedSites;

  // Post-pass: upgrade postMessage severity to "high" if handler contains security sinks
  for (var _pmi = 0; _pmi < result.dangerousPatterns.length; _pmi++) {
    var _pmPat = result.dangerousPatterns[_pmi];
    if (!_pmPat._handlerRange) continue;
    var _pmStart = _pmPat._handlerRange[0], _pmEnd = _pmPat._handlerRange[1];
    for (var _si = 0; _si < result.securitySinks.length; _si++) {
      var _sink = result.securitySinks[_si];
      if (_sink.location.line >= _pmStart && _sink.location.line <= _pmEnd &&
          _sink.sourceType === "user-controlled") {
        _pmPat.severity = "high";
        _pmPat.description += " (flows to " + _sink.sink + ")";
        break;
      }
    }
    delete _pmPat._handlerRange;
  }

  // ── Summary ──
  var _secCount = result.securitySinks.length + result.dangerousPatterns.length;
  if (result.protoEnums.length || result.protoFieldMaps.length || result.fetchCallSites.length || varNames.length || _secCount) {
    console.debug("[AST] %s (%d chars) → %d enums, %d fieldMaps, %d fetchSites, %d constraints, %d interProc, %d globals, %d winAliases, sourceMap=%s",
      sourceUrl, code.length, result.protoEnums.length, result.protoFieldMaps.length,
      result.fetchCallSites.length, varNames.length, _stats.interProcTraces,
      _stats.globalAssignments, _stats.windowAliases,
      result.sourceMapUrl || "none");
  }
  if (_secCount > 0) {
    console.debug("[AST:security] %s — %d sinks, %d dangerous",
      sourceUrl, result.securitySinks.length, result.dangerousPatterns.length);
  }
  if (_stats.globalAssignments > 0) {
    var globalNames = Object.keys(_globalAssignments);
    console.debug("[AST:globals] %d global assignments: %s", globalNames.length, globalNames.slice(0, 20).join(", "));
  }
  if (_stats.protoMethods > 0) {
    console.debug("[AST:proto] %s — %d prototype methods, %d matched, %d unmatched",
      sourceUrl, _stats.protoMethods, _stats.protoMethods - _stats.protoMethodsNoField, _stats.protoMethodsNoField);
  }

  if (_resolver.errors.length > 0) {
    result.resolverErrors = _resolver.errors;
    for (var _ei = 0; _ei < _resolver.errors.length; _ei++) {
      var _err = _resolver.errors[_ei];
      console.debug("[AST:resolver] %s: %s", _err.context, _err.message);
      if (_err.stack) console.debug(_err.stack);
    }
  }

  _constraints = {};
  _stats = null;
  _globalAssignments = {};
  _windowAliases = new Set();
  _resolver = null;

  // Attach structural-def indices collected during the pre-pass so the
  // viewer's lazy AST_BUILD_DEFINITION_MAP only needs to add `refMap`
  // (scope-resolved identifier references — the expensive Identifier-
  // visitor pass). Serialise `calls` as a plain array so structured
  // clone across the worker boundary works.
  result.defMap = _defMap;
  result.propDefs = _propDefs;
  result.funcMap = {};
  for (var _fmName in _funcMap) {
    var _fmEntry = _funcMap[_fmName];
    result.funcMap[_fmName] = { line: _fmEntry.line, endLine: _fmEntry.endLine, calls: Array.from(_fmEntry.calls) };
  }
  result.allFuncRanges = _allFuncRanges.map(function (e) {
    return { line: e.line, endLine: e.endLine, calls: Array.from(e.calls) };
  });
  // pendingProps is populated by the pre-pass for possible future
  // viewer refMap resolution, but no current consumer reads it off the
  // result (the viewer's AST_BUILD_DEFINITION_MAP does its own pass).
  // On a 6 MB bundle this list grows to hundreds of thousands of
  // entries, so serialising it via structured-clone postMessage costs
  // real wall-time for a value nobody uses. Skip the export; if a
  // consumer ever needs it, re-enable the .map() projection here.
  _defMap = null;
  _propDefs = null;
  _funcMap = null;
  _allFuncRanges = null;
  _pendingProps = null;
  _funcStack = null;

  // Expose the parsed AST so callers that want to run a second pass
  // (e.g. buildDefinitionMap for the click-to-definition viewer) can
  // reuse it without re-parsing the whole bundle. Parsing a 6 MB
  // minified bundle is the dominant cost of the AST pipeline; halving
  // it pays for the cross-pass carry.
  result._ast = ast;
  return result;
}

// ─── Export/Global API Client Method Calls ──────────────────────────────────
// Detects patterns like k.get(url), k.post(url, {json: {...}}) where k is an
// ESM export or global and the method name is an HTTP method.
// This handles libraries (like ky) where the fetch sink is unreachable via
// static analysis (e.g., behind private fields).
function _processExportMethodCall(path, result) {
  var node = path.node;
  var callee = node.callee;
  if (!_t.isMemberExpression(callee) || callee.computed) return;
  if (!_t.isIdentifier(callee.object) || !_t.isIdentifier(callee.property)) return;

  var objName = callee.object.name;
  var methodName = callee.property.name;
  if (!_HTTP_METHODS_LC[methodName]) return;
  if (!_globalAssignments[objName]) return;
  // Skip if there's a local binding that shadows (function param, inner var, etc.)
  // but allow module-scope bindings (ESM exports are module-scoped consts)
  var emcBinding = path.scope.getBinding(objName);
  if (emcBinding && !emcBinding.scope.path.isProgram()) return;
  if (node.arguments.length < 1) return;

  // Extract URL from first argument
  var urlVals = _resolveAllValues(path.get("arguments.0"), 1);
  for (var ui = 0; ui < urlVals.length; ui++) {
    if (typeof urlVals[ui] !== "string") continue;
    var site = {
      url: urlVals[ui],
      method: methodName.toUpperCase(),
      headers: {},
      type: "fetch",
    };

    // Extract body params from the second argument options object.
    // Each property is treated as a potential body field — spec-correct
    // when the wrapper's body extraction semantic isn't known. Removed:
    // the ky-specific `json: {...}` flatten special case (per CLAUDE.md
    // L29 ban on framework recognition; ky's body extraction is reachable
    // through inter-procedural trace into ky's own implementation when
    // bundled).
    if (node.arguments.length >= 2 && _t.isObjectExpression(node.arguments[1])) {
      site.params = _extractObjectProperties(node.arguments[1]);
      for (var bpj = 0; bpj < site.params.length; bpj++) site.params[bpj].location = "body";
    }

    if (!site || site.url === "" || site.url == null) continue;
    console.debug("[AST:fetch] %s %s via %s.%s() (export/global API client)", site.method, site.url, objName, methodName);
    result.fetchCallSites.push(site);
  }
}

// ─── Network Sink Detection & Inter-Procedural Tracing ──────────────────────

function _processNetworkSink(path, result) {
  var node = path.node;
  var callee = node.callee;

  // ── Identify fetch() / window.fetch() — verify these are actual globals via scope ──
  if (_isGlobalFetchCall(callee, path.scope)) {
    _extractFetchCall(path, result, "fetch");
    return;
  }

  // ── Identify XMLHttpRequest.open(method, url) ──
  if (_t.isMemberExpression(callee) &&
      _t.isIdentifier(callee.property, { name: "open" }) &&
      node.arguments.length >= 2) {

    // Debug: describe what we found
    var _xhrObjDesc = _t.isIdentifier(callee.object) ? callee.object.name : callee.object.type;
    var methodArg = node.arguments[0];
    var _xhrArg0Desc = _t.isStringLiteral(methodArg) ? '"' + methodArg.value + '"' :
      (_t.isMemberExpression(methodArg) && _t.isIdentifier(methodArg.object) && _t.isIdentifier(methodArg.property)
        ? methodArg.object.name + "." + methodArg.property.name : methodArg.type);
    var _xhrArg1 = node.arguments[1];
    var _xhrArg1Desc = _t.isStringLiteral(_xhrArg1) ? '"' + _xhrArg1.value + '"' :
      (_t.isMemberExpression(_xhrArg1) && _t.isIdentifier(_xhrArg1.object) && _t.isIdentifier(_xhrArg1.property)
        ? _xhrArg1.object.name + "." + _xhrArg1.property.name : _xhrArg1.type);
    console.debug("[AST:trace] .open() found: %s.open(%s, %s) at line %d",
      _xhrObjDesc, _xhrArg0Desc, _xhrArg1Desc, node.loc ? node.loc.start.line : -1);

    // Verify the object is an XMLHttpRequest (new XMLHttpRequest() or factory.xhr())
    var isXhr = false;
    if (_t.isStringLiteral(methodArg) && _HTTP_METHODS_LC[methodArg.value.toLowerCase()]) {
      isXhr = true; // String literal method ⇒ almost certainly XHR
    }
    if (!isXhr && _t.isIdentifier(callee.object)) {
      // Delegate to _isXhrObject which does spec-grounded trace
      // through factory return statements (no method-name shortcut).
      if (_isXhrObject(path, callee.object)) isXhr = true;
    }
    // Try resolving method to confirm — but skip if the object is provably NOT XHR
    // (e.g., bound to `new BroadcastChannel()`, `new URL()`, or other known non-XHR constructors)
    if (!isXhr && _t.isIdentifier(callee.object)) {
      var _xhrFallbackBinding = path.scope.getBinding(callee.object.name);
      var _knownNonXhr = false;
      if (_xhrFallbackBinding && _t.isVariableDeclarator(_xhrFallbackBinding.path.node) && _xhrFallbackBinding.path.node.init) {
        var _fbInit = _xhrFallbackBinding.path.node.init;
        // If init is `new SomeConstructor()` and it's NOT XMLHttpRequest, this is NOT XHR
        if (_t.isNewExpression(_fbInit) && _t.isIdentifier(_fbInit.callee) &&
            _fbInit.callee.name !== "XMLHttpRequest") {
          _knownNonXhr = true;
        }
      }
      if (!_knownNonXhr) {
        var testVals = _resolveAllValues(path.get("arguments.0"), 0);
        if (testVals.length > 0 && typeof testVals[0] === "string" && _HTTP_METHODS_LC[testVals[0].toLowerCase()]) isXhr = true;
      }
    }

    if (isXhr) {
      // Extract headers and body from .setRequestHeader() and .send()
      var xhrHeaders = {};
      var xhrBodyParams = [];
      if (_t.isIdentifier(callee.object)) {
        var xhrBinding = path.scope.getBinding(callee.object.name);
        if (xhrBinding && xhrBinding.referencePaths) {
          for (var ri = 0; ri < xhrBinding.referencePaths.length; ri++) {
            var ref = xhrBinding.referencePaths[ri];
            var refParent = ref.parentPath;
            if (!refParent || !_t.isMemberExpression(refParent.node) || refParent.node.object !== ref.node) continue;
            var memberName = _t.isIdentifier(refParent.node.property) ? refParent.node.property.name : null;
            var callPath = refParent.parentPath;
            if (!callPath || !_t.isCallExpression(callPath.node) || callPath.node.callee !== refParent.node) continue;
            if (memberName === "send" && callPath.node.arguments.length > 0)
              xhrBodyParams = _extractBodyParams(callPath.node.arguments[0], callPath);
            if (memberName === "setRequestHeader" && callPath.node.arguments.length >= 2) {
              var hdrName = _t.isStringLiteral(callPath.node.arguments[0]) ? callPath.node.arguments[0].value : null;
              var hdrVal = _t.isStringLiteral(callPath.node.arguments[1]) ? callPath.node.arguments[1].value : null;
              if (hdrName) xhrHeaders[hdrName.toLowerCase()] = hdrVal || "(dynamic)";
            }
          }
        }
      }

      // ── Correlated resolution: detect shared-base-param pattern ──
      // When both args are P.prop1 and P.prop2 from the same parameter P, trace P
      // to concrete caller arguments and extract both properties together per-caller.
      var methodBase = _t.isMemberExpression(methodArg) && !methodArg.computed && _t.isIdentifier(methodArg.object) ? methodArg.object : null;
      var urlBase = _t.isMemberExpression(_xhrArg1) && !_xhrArg1.computed && _t.isIdentifier(_xhrArg1.object) ? _xhrArg1.object : null;

      if (methodBase && urlBase && methodBase.name === urlBase.name) {
        var sharedBinding = path.scope.getBinding(methodBase.name);
        if (sharedBinding && sharedBinding.kind === "param") {
          var methodPropName = _t.isIdentifier(methodArg.property) ? methodArg.property.name : null;
          var urlPropName = _t.isIdentifier(_xhrArg1.property) ? _xhrArg1.property.name : null;
          if (methodPropName && urlPropName) {
            console.debug("[AST:trace]   correlated resolution: %s.%s + %s.%s", methodBase.name, methodPropName, urlBase.name, urlPropName);
            var callerArgs = _resolveParamToCallerArgs(sharedBinding);
            // Deduplicate caller args by AST node position — the same expression
            // reached through different trace paths produces identical results
            var _seenArgNodes = {};
            var _uniqueArgs = [];
            for (var dai = 0; dai < callerArgs.length; dai++) {
              var _akey = callerArgs[dai].node.start + ":" + callerArgs[dai].node.end;
              if (!_seenArgNodes[_akey]) { _seenArgNodes[_akey] = true; _uniqueArgs.push(callerArgs[dai]); }
            }
            callerArgs = _uniqueArgs;
            console.debug("[AST:trace]   found %d caller arg paths (%d unique)", callerArgs.length, _uniqueArgs.length);
            // For method, also check alternate property names (method vs type)
            var methodProps = [methodPropName];
            if (methodPropName === "type") methodProps.push("method");
            else if (methodPropName === "method") methodProps.push("type");
            for (var cai = 0; cai < callerArgs.length; cai++) {
              var props = _resolvePropsFromArg(callerArgs[cai], methodProps.concat([urlPropName]));
              var resolvedMethods = [];
              for (var mpi = 0; mpi < methodProps.length; mpi++) {
                resolvedMethods = resolvedMethods.concat(props[methodProps[mpi]] || []);
              }
              var resolvedUrls = props[urlPropName] || [];
              // Filter to valid HTTP methods
              resolvedMethods = resolvedMethods.filter(function(m) { return typeof m === "string" && _HTTP_METHODS_LC[m.toLowerCase()]; });
              for (var ui = 0; ui < resolvedUrls.length; ui++) {
                // When methods and URLs have the same count, pair by index — the computed-member
                // route iterates values in the same order as the iteration variable, so index
                // correspondence is maintained (e.g., "get"→jQuery.get() callers, "post"→jQuery.post() callers).
                // When methods and URLs pair 1:1, use index correspondence.
                // When there are more methods than URLs, emit a site per method for this URL.
                var methodsForUrl = [];
                if (resolvedMethods.length === resolvedUrls.length) {
                  methodsForUrl = [resolvedMethods[ui].toUpperCase()];
                } else if (resolvedMethods.length > resolvedUrls.length) {
                  for (var emi = 0; emi < resolvedMethods.length; emi++) methodsForUrl.push(resolvedMethods[emi].toUpperCase());
                } else if (resolvedMethods.length > 0) {
                  methodsForUrl = [resolvedMethods[0].toUpperCase()];
                } else {
                  methodsForUrl = ["?"];
                }
                // Body params come from the XHR-internal trace
                // (xhrBodyParams) — that's the spec-correct source: it
                // observes what xhr.send() actually receives, regardless
                // of the caller-arg property name used. The previous
                // `cpKey === "data"` framework recognition (jQuery
                // $.ajax convention) is removed per CLAUDE.md L29.
                var corrBodyParams = xhrBodyParams.length > 0 ? xhrBodyParams : [];
                for (var mfi = 0; mfi < methodsForUrl.length; mfi++) {
                  _pushFetchSite(result, _buildFetchSite(resolvedUrls[ui], methodsForUrl[mfi], xhrHeaders, "xhr", corrBodyParams));
                  console.debug("[AST:fetch] xhr %s %s", methodsForUrl[mfi], resolvedUrls[ui]);
                }
              }
            }
            return; // Done — correlated resolution handled it
          }
        }
      }

      // ── Cross-parameter correlated resolution ──
      // Method and URL come from different parameters of the same function:
      // function(url, opts) { xhr.open(opts.method||"get", url) }
      // For each caller, extract both args at their respective param indices.
      var _methodParamInfo = _identifyParamSource(methodArg, path);
      var _urlParamInfo = _identifyParamSource(_xhrArg1, path);
      if (_methodParamInfo && _urlParamInfo &&
          _methodParamInfo.funcPath === _urlParamInfo.funcPath &&
          _methodParamInfo.paramIdx !== _urlParamInfo.paramIdx) {
        var xpFuncPath = _methodParamInfo.funcPath;
        var xpCallerArgs = _findFunctionCallerArgs(xpFuncPath);
        if (xpCallerArgs.length > 0) {
          console.debug("[AST:trace]   cross-param correlated: method=param[%d].%s url=param[%d] (%d callers)",
            _methodParamInfo.paramIdx, _methodParamInfo.propName || "(direct)", _urlParamInfo.paramIdx, xpCallerArgs.length);
          for (var xci = 0; xci < xpCallerArgs.length; xci++) {
            var xpArgs = xpCallerArgs[xci];
            // Resolve URL from caller
            var xpUrls = [];
            if (_urlParamInfo.paramIdx < xpArgs.length) {
              xpUrls = _resolveAllValues(xpArgs[_urlParamInfo.paramIdx], 0);
            }
            // Resolve method from caller
            var xpMethods = [];
            if (_methodParamInfo.paramIdx < xpArgs.length) {
              if (_methodParamInfo.propName) {
                // opts.method — resolve the opts arg to object, extract the property
                var xpObj = _resolveToObject(xpArgs[_methodParamInfo.paramIdx], 0);
                if (xpObj) {
                  for (var xpi = 0; xpi < xpObj.properties.length; xpi++) {
                    var xpp = xpObj.properties[xpi];
                    if (!_t.isObjectProperty(xpp) || xpp.computed) continue;
                    var xpk = _getKeyName(xpp.key);
                    if (xpk === _methodParamInfo.propName) {
                      if (_t.isStringLiteral(xpp.value)) xpMethods.push(xpp.value.value);
                    }
                  }
                }
              } else {
                xpMethods = _resolveAllValues(xpArgs[_methodParamInfo.paramIdx], 0);
              }
            }
            // Apply default from LogicalExpression: n.method || "get"
            if (xpMethods.length === 0 && _methodParamInfo.defaultValue) {
              xpMethods = [_methodParamInfo.defaultValue];
            }
            xpMethods = xpMethods.filter(function(m) { return typeof m === "string" && _HTTP_METHODS_LC[m.toLowerCase()]; });
            // Resolve body params from caller if available
            var xpBody = xhrBodyParams.length > 0 ? xhrBodyParams : [];
            if (xpBody.length === 0 && _methodParamInfo.paramIdx < xpArgs.length && _methodParamInfo.propName) {
              var xpBodyObj = _resolveToObject(xpArgs[_methodParamInfo.paramIdx], 0);
              if (xpBodyObj) {
                for (var xbi = 0; xbi < xpBodyObj.properties.length; xbi++) {
                  var xbp = xpBodyObj.properties[xbi];
                  if (!_t.isObjectProperty(xbp) || xbp.computed) continue;
                  if (_getKeyName(xbp.key) === "body") {
                    var bodyVal = xbp.value;
                    bodyVal = _unwrapJsonStringify(bodyVal, path);
                    if (_t.isObjectExpression(bodyVal)) {
                      xpBody = _extractObjectProperties(bodyVal);
                      for (var xbpi = 0; xbpi < xpBody.length; xbpi++) xpBody[xbpi].location = "body";
                    }
                    break;
                  }
                }
              }
            }
            // Extract headers from caller's opts object (e.g. opts.headers)
            var xpHeaders = Object.assign({}, xhrHeaders);
            if (_methodParamInfo.paramIdx < xpArgs.length && _methodParamInfo.propName) {
              var xpHdrObj = _resolveToObject(xpArgs[_methodParamInfo.paramIdx], 0);
              if (xpHdrObj) {
                for (var xhi = 0; xhi < xpHdrObj.properties.length; xhi++) {
                  var xhp = xpHdrObj.properties[xhi];
                  if (!_t.isObjectProperty(xhp) || xhp.computed) continue;
                  if (_getKeyName(xhp.key) === "headers" && _t.isObjectExpression(xhp.value)) {
                    xpHeaders = Object.assign(xpHeaders, _extractHeaders(xhp.value));
                    break;
                  }
                }
              }
            }
            for (var xui = 0; xui < xpUrls.length; xui++) {
              var xpMethod = xpMethods.length > 0 ? xpMethods[0].toUpperCase() : "GET";
              _pushFetchSite(result, _buildFetchSite(xpUrls[xui], xpMethod, xpHeaders, "xhr", xpBody));
              console.debug("[AST:fetch] xhr %s %s (cross-param)", xpMethod, xpUrls[xui]);
            }
          }
          return;
        }
      }

      // ── this.prop XHR correlated resolution (prototype methods) ──
      // When both args are this.method and this.url, trace through constructor per-caller
      var _thisMethodProp = null, _thisUrlProp = null;
      if (_t.isMemberExpression(methodArg) && _t.isThisExpression(methodArg.object) &&
          _t.isIdentifier(methodArg.property)) _thisMethodProp = methodArg.property.name;
      if (_t.isMemberExpression(_xhrArg1) && _t.isThisExpression(_xhrArg1.object) &&
          _t.isIdentifier(_xhrArg1.property)) _thisUrlProp = _xhrArg1.property.name;
      if (_thisMethodProp && _thisUrlProp) {
        var _encFunc = path.getFunctionParent();
        if (_encFunc) {
          // Find Ctor.prototype.method = function(){...} pattern
          var _ctorName = null;
          var _funcParentP = _encFunc.parentPath;
          if (_funcParentP && _t.isAssignmentExpression(_funcParentP.node) && _funcParentP.node.right === _encFunc.node) {
            var _aLeft = _funcParentP.node.left;
            if (_t.isMemberExpression(_aLeft) && _t.isMemberExpression(_aLeft.object) && !_aLeft.object.computed &&
                (_t.isIdentifier(_aLeft.object.property, {name:"prototype"}) ||
                 (_t.isStringLiteral(_aLeft.object.property) && _aLeft.object.property.value === "prototype")) &&
                _t.isIdentifier(_aLeft.object.object)) {
              _ctorName = _aLeft.object.object.name;
            }
          }
          if (_ctorName) {
            var _correlatedSites = _resolveThisPropXhrCorrelated(path, _ctorName, _thisMethodProp, _thisUrlProp, xhrHeaders, xhrBodyParams);
            if (_correlatedSites.length > 0) {
              for (var _csi = 0; _csi < _correlatedSites.length; _csi++) {
                var _csSite = _correlatedSites[_csi];
                if (_csSite && _csSite.url !== "" && _csSite.url != null) result.fetchCallSites.push(_csSite);
              }
              return;
            }
          }
        }
      }

      // ── Fallback: independent resolution (for simple cases / non-shared params) ──
      var xhrMethod = null;
      if (_t.isStringLiteral(methodArg) && _HTTP_METHODS_LC[methodArg.value.toLowerCase()]) {
        xhrMethod = methodArg.value.toUpperCase();
      }
      var xhrMethodVals = null;
      if (!xhrMethod) {
        var methodVals = _resolveAllValues(path.get("arguments.0"), 0);
        if (methodVals.length > 0 && typeof methodVals[0] === "string" && _HTTP_METHODS_LC[methodVals[0].toLowerCase()]) {
          xhrMethod = methodVals[0].toUpperCase();
          if (methodVals.length > 1) xhrMethodVals = methodVals;
        }
      }
      if (!xhrMethod) xhrMethod = "?";

      var urls = _resolveAllValues(path.get("arguments.1"), 0);
      console.debug("[AST:trace]   url resolve → [%s] (%d values)", urls.join(", "), urls.length);

      for (var i = 0; i < urls.length; i++) {
        var pairedMethod = xhrMethod;
        if (xhrMethodVals && xhrMethodVals.length === urls.length) {
          var pm = xhrMethodVals[i];
          if (typeof pm === "string" && _HTTP_METHODS_LC[pm.toLowerCase()]) pairedMethod = pm.toUpperCase();
        }
        var xhrMethodDisplay = pairedMethod === "(dynamic)" ? "?" : pairedMethod;
        _pushFetchSite(result, _buildFetchSite(urls[i], xhrMethodDisplay, xhrHeaders, "xhr", xhrBodyParams));
        console.debug("[AST:fetch] xhr %s %s", xhrMethodDisplay, urls[i]);
      }
    }
    return;
  }

  // ── navigator.sendBeacon(url, data) ──
  if (_t.isMemberExpression(callee) && _t.isIdentifier(callee.property, { name: "sendBeacon" }) &&
      node.arguments.length >= 1 &&
      _t.isIdentifier(callee.object, { name: "navigator" }) && !path.scope.getBinding("navigator")) {
    var beaconUrls = _resolveAllValues(path.get("arguments.0"), 0);
    for (var bi = 0; bi < beaconUrls.length; bi++) {
      var bParams = node.arguments.length > 1 ? _extractBodyParams(node.arguments[1], path) : [];
      _pushFetchSite(result, _buildFetchSite(beaconUrls[bi], "POST", {}, "beacon", bParams));
      console.debug("[AST:fetch] beacon POST %s", beaconUrls[bi]);
    }
    return;
  }

  // ── Check if this is a call to a function that contains a network sink ──
  // Inter-procedural: if callee resolves to a function definition that has fetch/XHR inside
  var funcPath = _resolveCalleeFuncPath(path, 0);
  if (funcPath) {
    // calleeBinding is used for finding OTHER callers (wrapper tracing).
    // For MemberExpression callees ($.ajax, axios.get), binding is null — we still
    // trace the current call site's arguments through both direct and deep sink paths.
    var calleeBinding = _t.isIdentifier(callee) ? path.scope.getBinding(callee.name) : null;
    _traceWrapperFunction(path, funcPath, calleeBinding, result);
  }
}

// ─── Additional Browser Sinks ────────────────────────────────────────────────

function _processNewExpressionSink(path, result) {
  var node = path.node;
  var callee = node.callee;
  // new EventSource(url)
  if (_t.isIdentifier(callee, { name: "EventSource" }) && !path.scope.getBinding("EventSource") &&
      node.arguments.length >= 1) {
    var urls = _resolveAllValues(path.get("arguments.0"), 0);
    for (var i = 0; i < urls.length; i++) {
      _pushFetchSite(result, _buildFetchSite(urls[i], "GET", {}, "eventsource"));
      console.debug("[AST:fetch] eventsource GET %s", urls[i]);
    }
  }
}

function _processImageSrcSink(path, result) {
  var node = path.node;
  if (node.operator !== "=") return;
  var left = node.left;
  if (!_t.isMemberExpression(left) || left.computed || !_t.isIdentifier(left.property, { name: "src" })) return;
  if (!_t.isIdentifier(left.object)) return;
  var binding = path.scope.getBinding(left.object.name);
  if (!binding || !_t.isVariableDeclarator(binding.path.node) || !binding.path.node.init) return;
  var init = binding.path.node.init;
  if (!_t.isNewExpression(init) || !_t.isIdentifier(init.callee, { name: "Image" }) || path.scope.getBinding("Image")) return;
  var urls = _resolveAllValues(path.get("right"), 0);
  for (var i = 0; i < urls.length; i++) {
    _pushFetchSite(result, _buildFetchSite(urls[i], "GET", {}, "pixel"));
    console.debug("[AST:fetch] pixel GET %s", urls[i]);
  }
}

// ─── IIFE Window Alias Detection ─────────────────────────────────────────────
// Detects (function(e){...})(window) and !function(e){...}(window) patterns,
// marking the parameter as a window alias. Also handles indirect IIFEs where
// a function parameter is called with a window alias (e.g., t(e) inside a UMD wrapper).

function _processIIFE(path) {
  var callee = path.node.callee;
  var funcExpr = null;
  var args = path.node.arguments;

  // Direct IIFE: (function(params) { ... })(args) or !function(params) { ... }(args)
  if (_t.isFunctionExpression(callee) || _t.isArrowFunctionExpression(callee)) {
    funcExpr = callee;
  }

  // Indirect IIFE: callee is a parameter that received a FunctionExpression argument
  // e.g., t(e) where t is bound to a FunctionExpression argument of an enclosing IIFE
  if (!funcExpr && _t.isIdentifier(callee)) {
    var binding = path.scope.getBinding(callee.name);
    if (binding && binding.kind === "param") {
      var enclosingFunc = binding.scope.path;
      if (_t.isFunctionExpression(enclosingFunc.node) || _t.isArrowFunctionExpression(enclosingFunc.node)) {
        var paramIdx = _findParamIndex(enclosingFunc.node.params, callee.name);
        if (paramIdx >= 0) {
          // Check if enclosing function is itself an IIFE callee
          var enclosingCall = enclosingFunc.parentPath;
          if (enclosingCall && _t.isCallExpression(enclosingCall.node) &&
              enclosingCall.node.callee === enclosingFunc.node &&
              paramIdx < enclosingCall.node.arguments.length) {
            var paramArgNode = enclosingCall.node.arguments[paramIdx];
            if (_t.isFunctionExpression(paramArgNode) || _t.isArrowFunctionExpression(paramArgNode)) {
              funcExpr = paramArgNode;
            }
          }
        }
      }
    }
  }

  if (!funcExpr || !funcExpr.params || !funcExpr.params.length || !args || !args.length) return;

  for (var i = 0; i < funcExpr.params.length && i < args.length; i++) {
    if (!_t.isIdentifier(funcExpr.params[i])) continue;
    var paramName = funcExpr.params[i].name;
    var argNode = args[i];

    // Direct window/self/globalThis reference
    if (_t.isIdentifier(argNode) && _isGlobalObject(argNode.name, path.scope)) {
      _windowAliases.add(paramName);
      _stats.windowAliases++;
      continue;
    }
    // Known window alias passed through (e.g., t(e) where e is a window alias)
    if (_t.isIdentifier(argNode) && _windowAliases.has(argNode.name)) {
      _windowAliases.add(paramName);
      _stats.windowAliases++;
      continue;
    }
    // typeof window !== "undefined" ? window : this  (UMD pattern)
    if (_t.isConditionalExpression(argNode)) {
      var hasWindow = (_t.isIdentifier(argNode.consequent, { name: "window" }) ||
                       _t.isIdentifier(argNode.alternate, { name: "window" })) &&
                      !path.scope.getBinding("window");
      var hasThis = (_t.isThisExpression(argNode.consequent) || _t.isThisExpression(argNode.alternate));
      if (hasWindow || hasThis) {
        _windowAliases.add(paramName);
        _stats.windowAliases++;
        continue;
      }
    }
    // this at global scope (common: (function(global) { ... })(this))
    if (_t.isThisExpression(argNode)) {
      _windowAliases.add(paramName);
      _stats.windowAliases++;
    }
  }
}

// ─── Global Assignment Tracking ──────────────────────────────────────────────
// Tracks window.X = value, self.X = value, windowAlias.X = value assignments.
// These create global bindings accessible from any script on the page.

// Recursively checks if a ConditionalExpression has window/self/globalThis/this in any branch
function _hasWindowBranch(node, path) {
  // Iterative: walk ConditionalExpression chains via explicit stack
  var stack = [node];
  while (stack.length > 0) {
    var n = stack.pop();
    if (_t.isIdentifier(n) && _isGlobalObject(n.name, path.scope)) return true;
    if (_t.isThisExpression(n)) return true;
    if (_t.isConditionalExpression(n)) {
      stack.push(n.consequent, n.alternate);
    }
  }
  return false;
}

function _trackGlobalAssignment(path) {
  var node = path.node;
  if (node.operator !== "=") return;
  var left = node.left;
  if (!_t.isMemberExpression(left) || left.computed) return;

  var objName = _t.isIdentifier(left.object) ? left.object.name : null;
  if (!objName) {
    // Handle (windowAlias || self).prop = value (UMD pattern)
    if (_t.isLogicalExpression(left.object)) {
      var logLeft = left.object.left;
      var logRight = left.object.right;
      if (_t.isIdentifier(logLeft) && _windowAliases.has(logLeft.name)) {
        objName = logLeft.name;
      } else if (_t.isIdentifier(logRight) && _windowAliases.has(logRight.name)) {
        objName = logRight.name;
      } else if (_t.isIdentifier(logLeft) && _isGlobalObject(logLeft.name, path.scope)) {
        objName = logLeft.name;
      } else if (_t.isIdentifier(logRight) && _isGlobalObject(logRight.name, path.scope)) {
        objName = logRight.name;
      }
    }
    // Handle ConditionalExpression: (typeof window !== "undefined" ? window : ...).prop = value (UMD)
    if (!objName && _t.isConditionalExpression(left.object)) {
      if (_hasWindowBranch(left.object, path)) objName = "window";
    }
    if (!objName) return;
  }

  var isGlobalObj = _isGlobalObject(objName, path.scope);
  if (!isGlobalObj) return;

  var propName = _t.isIdentifier(left.property) ? left.property.name : null;
  if (!propName) return;

  _globalAssignments[propName] = {
    valuePath: path.get("right"),
    valueNode: node.right,
  };
  _stats.globalAssignments++;
}

// Resolve a property on the return value of an IIFE.
// Handles: var e = function(){ n.get = function(url){...}; return n; }(); e.get(url)
// Also handles SequenceExpression returns: return (t=t||{}, n.get=fn, n)
function _resolveIIFEReturnedProperty(callExprPath, propName) {
  var callee = callExprPath.node.callee;
  var funcNode = null;
  var funcPath = null;

  // Direct IIFE: (function(){...})()
  if (_t.isFunctionExpression(callee) || _t.isArrowFunctionExpression(callee)) {
    funcNode = callee;
    funcPath = callExprPath.get("callee");
  }
  // Named function: factoryFn()
  if (!funcNode && _t.isIdentifier(callee)) {
    var binding = callExprPath.scope.getBinding(callee.name);
    if (binding) {
      if (_t.isFunctionDeclaration(binding.path.node)) {
        funcNode = binding.path.node;
        funcPath = binding.path;
      } else if (_t.isVariableDeclarator(binding.path.node) && binding.path.node.init &&
                 (_t.isFunctionExpression(binding.path.node.init) || _t.isArrowFunctionExpression(binding.path.node.init))) {
        funcNode = binding.path.node.init;
        funcPath = binding.path.get("init");
      }
    }
  }
  if (!funcNode || !funcPath) return null;

  // Find the returned identifier name
  var returnedName = null;
  try {
    funcPath.traverse(Object.assign({
      ReturnStatement: function(retPath) {
        if (returnedName) return;
        var arg = retPath.node.argument;
        if (!arg) return;
        // Direct return: return n
        if (_t.isIdentifier(arg)) {
          returnedName = arg.name;
        }
        // SequenceExpression: return (a=..., n.get=fn, n)
        if (_t.isSequenceExpression(arg)) {
          var last = arg.expressions[arg.expressions.length - 1];
          if (_t.isIdentifier(last)) returnedName = last.name;
        }
      },
    }, _SKIP_NESTED_FUNCS));
  } catch (e) { _resolver.collectError(e, "iifeReturnName"); }
  if (!returnedName) return null;

  // Find returnedName.propName = function(){} assignments inside the IIFE
  var foundFuncPath = null;
  try {
    funcPath.traverse(Object.assign({
      AssignmentExpression: function(assignPath) {
        if (foundFuncPath) return;
        var left = assignPath.node.left;
        if (!_t.isMemberExpression(left) || left.computed) return;
        if (!_t.isIdentifier(left.object) || left.object.name !== returnedName) return;
        if (!_t.isIdentifier(left.property) || left.property.name !== propName) return;
        var right = assignPath.node.right;
        if (_t.isFunctionExpression(right) || _t.isArrowFunctionExpression(right)) {
          foundFuncPath = assignPath.get("right");
        }
      },
    }, _SKIP_NESTED_FUNCS));
  } catch (e) { _resolver.collectError(e, "iifePropertyAssignment"); }
  if (foundFuncPath) _lastIIFEFuncPath = funcPath;
  return foundFuncPath;
}

// ═══════════════════════════════════════════════════════════════════════════
// Property-Flow Analyser (ECMA-262 spec-organised; one section per function)
// ───────────────────────────────────────────────────────────────────────────
// Forward dataflow over a function body's CFG to record every property-write
// as `(target, key, value)` with abstract values. NO syntactic shape matching
// — each spec section has its own transfer function and the driver composes
// them. Used by callee-resolution and security analysis to determine when a
// function copies one input's properties to another (Object.assign-equivalent
// per § 20.1.2.1) without library-name recognition.
//
// Spec sections covered:
//   § 8.4  Lexical Environments        → state model
//   § 10.2 Function bodies             → entry state (params bound, this)
//   § 13.5 / § 13.10 / § 13.13 / § 13.15 → expression eval (Member, OR, Assign)
//   § 14.3 Variable declarations       → declarator → state
//   § 14.6 If statement                → state split + join
//   § 14.7.4 For statement             → init + body (loop body once)
//   § 14.7.5 For-in statement          → loop var bound to key-of(rhs)
//   § 20.1.2.1 Object.assign           → recognised as direct propagation
//   § 23.1.3.15 Array.prototype.forEach → callback invocation effects
//
// Recursion is banned (CLAUDE.md): expression evaluation runs on a postorder
// enumeration; statement processing runs on an explicit worklist of CFG
// blocks.
// ═══════════════════════════════════════════════════════════════════════════

// AbstractValue — sum type built lazily as the lattice grows.
// Each constructor is a small record; LUB / equality are structural.
//
//   { kind: "param", idx }         — function parameter at position idx
//   { kind: "this" }                — receiver
//   { kind: "args-elt", idx }       — arguments[idx]; idx is AbstractValue
//   { kind: "args-len" }            — arguments.length
//   { kind: "loop-key", src }       — loop var bound by `for(k in src)`
//   { kind: "member", obj, key }    — obj[key] (read access)
//   { kind: "obj-lit", props }      — object expression with abstract values
//   { kind: "or", left, right }     — `a || b` (left when truthy per § 13.13)
//   { kind: "const", value }        — primitive literal
//   { kind: "top" }                 — unknown
//
// `_specEqualAv` and `_specJoinAv` operate over this lattice. Bottom is
// represented by the absence of a state entry for a variable name (not a
// distinct lattice element) — keeps the env-map small.

// § 8.4 Lexical Environments — state shape.
// State is a plain object whose keys are variable names. We use a
// null-prototype object so a variable named "toString" / "constructor"
// doesn't collide with Object prototype methods.
function _specStateCreate(initBindings) {
  var s = Object.create(null);
  if (initBindings) {
    for (var k in initBindings) if (Object.prototype.hasOwnProperty.call(initBindings, k)) s[k] = initBindings[k];
  }
  return s;
}
function _specStateClone(state) {
  return _specStateCreate(state);
}

// § 10.2.10 FunctionDeclarationInstantiation (simplified for our domain).
// Each formal parameter is bound to its abstract param-reference; `this`
// is implicit (the analyser checks `ThisExpression` directly via the
// `{kind:"this"}` constructor in expression eval — no explicit "this"
// state slot is required).
function _specInitialFunctionBodyState(funcNode) {
  var state = _specStateCreate();
  if (!funcNode || !funcNode.params) return state;
  for (var i = 0; i < funcNode.params.length; i++) {
    var p = funcNode.params[i];
    if (!p) continue;
    if (p.type === "Identifier") {
      state[p.name] = { kind: "param", idx: i };
    } else if (p.type === "AssignmentPattern" && p.left && p.left.type === "Identifier") {
      // Default-valued param `function f(x = 1)` per § 14.1 — the binding
      // resolves to either the arg's value or the default; abstractly Param(i).
      state[p.left.name] = { kind: "param", idx: i };
    } else if (p.type === "ObjectPattern") {
      // Destructured param `function f({a, b})` per § 14.3.3 / § 8.6.2:
      // each property of the pattern binds a separate identifier whose
      // value is `member(param-i, key)`.
      for (var pi = 0; pi < p.properties.length; pi++) {
        var op = p.properties[pi];
        if (!op || op.type !== "ObjectProperty" || op.computed) continue;
        var keyName = op.key && (op.key.type === "Identifier" ? op.key.name :
          (op.key.type === "StringLiteral" ? op.key.value : null));
        if (!keyName) continue;
        var bindIdent = op.value;
        // Default in destructured: `{a = 1}` parses as AssignmentPattern.
        if (bindIdent && bindIdent.type === "AssignmentPattern") bindIdent = bindIdent.left;
        if (bindIdent && bindIdent.type === "Identifier") {
          state[bindIdent.name] = {
            kind: "member",
            obj: { kind: "param", idx: i },
            key: { kind: "const", value: keyName }
          };
        }
      }
    } else if (p.type === "ArrayPattern") {
      // Destructured array param `function f([a, b])` per § 14.3.3 /
      // § 8.6.2 ArrayBindingPattern: each element binds to
      // `member(param-i, idx)` where idx is the position as a const.
      for (var ai = 0; ai < p.elements.length; ai++) {
        var ae = p.elements[ai];
        if (!ae) continue;
        if (ae.type === "AssignmentPattern") ae = ae.left;
        if (ae && ae.type === "Identifier") {
          state[ae.name] = {
            kind: "member",
            obj: { kind: "param", idx: i },
            key: { kind: "const", value: ai }
          };
        }
      }
    }
  }
  return state;
}

// Structural equality on AbstractValues — iterative worklist over
// (a, b) pairs. Termination is structural: every pushed pair is a strict
// subterm of a popped pair, and AbstractValue terms are finite trees.
function _specEqualAv(a, b) {
  var pairs = [[a, b]];
  while (pairs.length > 0) {
    var pair = pairs.pop();
    var x = pair[0], y = pair[1];
    if (x === y) continue;
    if (!x || !y) return false;
    if (x.kind !== y.kind) return false;
    switch (x.kind) {
      case "param":    if (x.idx !== y.idx) return false; break;
      case "this":     break;
      case "args-len": break;
      case "top":      break;
      case "obj-lit":  return false;  // identity-based
      case "const":    if (x.value !== y.value) return false; break;
      case "args-elt": pairs.push([x.idx, y.idx]); break;
      case "loop-key": pairs.push([x.src, y.src]); break;
      case "keys-of":  pairs.push([x.src, y.src]); break;
      case "member":   pairs.push([x.obj, y.obj]); pairs.push([x.key, y.key]); break;
      case "or":       pairs.push([x.left, y.left]); pairs.push([x.right, y.right]); break;
      default: return false;
    }
  }
  return true;
}

// LUB on AbstractValues. When two distinct tractable values meet at a
// merge point (e.g. `if (cond) x = this; else x = arg`), preserve the
// alternatives as `or(a, b)` so consumers (e.g. propagation detection)
// can examine each branch. Falls back to Top when either operand is
// already Top, since adding more cases under Top doesn't refine.
function _specJoinAv(a, b) {
  if (!a) return b;
  if (!b) return a;
  if (_specEqualAv(a, b)) return a;
  if (a.kind === "top" || b.kind === "top") return { kind: "top" };
  return { kind: "or", left: a, right: b };
}

// State equality (shallow) — used by the worklist's fixed-point detection.
function _specEqualState(a, b) {
  if (a === b) return true;
  if (!a || !b) return false;
  for (var k in a) if (Object.prototype.hasOwnProperty.call(a, k)) {
    if (!Object.prototype.hasOwnProperty.call(b, k)) return false;
    if (!_specEqualAv(a[k], b[k])) return false;
  }
  for (var k2 in b) if (Object.prototype.hasOwnProperty.call(b, k2)) {
    if (!Object.prototype.hasOwnProperty.call(a, k2)) return false;
  }
  return true;
}

// State LUB — pointwise on each binding name; missing bindings remain
// missing (Bottom).
function _specJoinState(a, b) {
  if (!a) return _specStateClone(b);
  if (!b) return _specStateClone(a);
  var out = _specStateCreate();
  for (var k in a) if (Object.prototype.hasOwnProperty.call(a, k)) {
    out[k] = Object.prototype.hasOwnProperty.call(b, k) ? _specJoinAv(a[k], b[k]) : a[k];
  }
  for (var k2 in b) if (Object.prototype.hasOwnProperty.call(b, k2)) {
    if (!Object.prototype.hasOwnProperty.call(out, k2)) out[k2] = b[k2];
  }
  return out;
}

// ───────────────────────────────────────────────────────────────────────────
// § 13.10 — Member Expression evaluation (read-side).
// Property accessor produces a reference whose value is `Get(obj, key)`.
// We model the result as `{kind:"member", obj, key}` since concrete property
// values are unknown at this dataflow level. No spec-section combining: this
// function ONLY produces a member-read result; the property-write side is
// handled by § 13.15.4 AssignmentExpression's MemberExpression LHS branch.
// ───────────────────────────────────────────────────────────────────────────
function _specMemberExpressionAv(memberNode, objAv, computedKeyAv) {
  if (!memberNode) return { kind: "top" };
  if (memberNode.computed) {
    return { kind: "member", obj: objAv, key: computedKeyAv || { kind: "top" } };
  }
  // Static accessor: obj.NAME — key is the identifier's name as a const.
  var p = memberNode.property;
  var keyAv;
  if (p && p.type === "Identifier") keyAv = { kind: "const", value: p.name };
  else if (p && p.type === "StringLiteral") keyAv = { kind: "const", value: p.value };
  else keyAv = { kind: "top" };
  return { kind: "member", obj: objAv, key: keyAv };
}

// ───────────────────────────────────────────────────────────────────────────
// § 13.13 — Logical OR Expression (`a || b`).
// Per spec: evaluate left; if ToBoolean(left) is true, return left's value;
// else return right's value. We can't evaluate ToBoolean abstractly, so we
// preserve both alternatives as `{kind:"or", left, right}` and let consumers
// (e.g. propagation-detection's target-tracing) unwrap to the truthy operand.
// ───────────────────────────────────────────────────────────────────────────
function _specLogicalOrAv(leftAv, rightAv) {
  return { kind: "or", left: leftAv, right: rightAv };
}

// ───────────────────────────────────────────────────────────────────────────
// § 13.15.4 — AssignmentExpression evaluation (plain `=` operator).
// Two LHS shapes per spec (PutValue at § 6.2.4.5):
//   (a) Identifier reference — rebinds variable in the current environment.
//       Mutates `state[name] = rhsAv`. Returns rhsAv (per spec "the value
//       assigned" is the expression's value).
//   (b) Property reference (MemberExpression LHS) — sets the property on
//       the base object. Records an effect tuple {target, key, value} so
//       downstream consumers can analyse the property writes. Returns rhsAv.
// Compound assignments (+=, -=, …) are handled by a separate spec section
// (§ 13.15.3 ApplyStringOrNumericBinaryOperator); routed there by callers.
// ───────────────────────────────────────────────────────────────────────────
function _specAssignmentExpressionApply(node, state, rhsAv, lhsObjAv, lhsKeyAv, effects) {
  if (!node || node.operator !== "=") return { kind: "top" };
  var left = node.left;
  if (left && left.type === "Identifier") {
    state[left.name] = rhsAv;
    return rhsAv;
  }
  if (left && left.type === "MemberExpression") {
    effects.push({ target: lhsObjAv, key: lhsKeyAv, value: rhsAv });
    return rhsAv;
  }
  return rhsAv;
}

// ───────────────────────────────────────────────────────────────────────────
// § 14.3.2 — VariableDeclaration (`var`/`let`/`const`).
// For each declarator: bind the identifier to the initializer's evaluated
// value, or to undefined per § 14.3.2.1 step 7 when no initializer.
// Handles destructuring patterns per § 14.3.3 / § 8.6.2 inline:
// `var {a, b} = obj` binds a to member(obj, "a") etc.
// `var [a, b] = arr` binds a to member(arr, 0) etc.
// Iterative — uses an explicit worklist to descend into nested patterns
// (e.g. `var [{a}, b] = arr`, `var {a = 1} = obj`).
// ───────────────────────────────────────────────────────────────────────────
function _specVariableDeclaratorBind(idNode, initAv, state) {
  if (!idNode) return;
  var stack = [{ id: idNode, av: initAv === undefined ? { kind: "const", value: undefined } : initAv }];
  while (stack.length > 0) {
    var top = stack.pop();
    var id = top.id, av = top.av;
    if (!id) continue;
    if (id.type === "Identifier") {
      state[id.name] = av;
      continue;
    }
    if (id.type === "AssignmentPattern" && id.left) {
      // Default-valued destructure (`{a = 1}` or `[x = 0]`) per § 14.1 /
      // § 8.6.2 — left descended with the same source value; default
      // applies at runtime when the resolved arg/element is undefined.
      stack.push({ id: id.left, av: av });
      continue;
    }
    if (id.type === "ObjectPattern") {
      var srcAv = av || { kind: "top" };
      for (var pi = id.properties.length - 1; pi >= 0; pi--) {
        var op = id.properties[pi];
        if (!op || op.type !== "ObjectProperty" || op.computed) continue;
        var keyName = op.key && (op.key.type === "Identifier" ? op.key.name :
          (op.key.type === "StringLiteral" ? op.key.value : null));
        if (keyName === null) continue;
        stack.push({
          id: op.value,
          av: { kind: "member", obj: srcAv, key: { kind: "const", value: keyName } }
        });
      }
      continue;
    }
    if (id.type === "ArrayPattern") {
      var srcAvA = av || { kind: "top" };
      for (var ai = id.elements.length - 1; ai >= 0; ai--) {
        var ae = id.elements[ai];
        if (!ae) continue;
        stack.push({
          id: ae,
          av: { kind: "member", obj: srcAvA, key: { kind: "const", value: ai } }
        });
      }
      continue;
    }
    if (id.type === "RestElement" && id.argument) {
      // `{...rest}` / `[...rest]` per § 14.3.3 — rest collects the
      // remaining properties/elements into a new object/array. Without
      // key-set tracking, the rest binding's value abstracts to Top
      // (sound conservative answer; a future enhancement can refine
      // by tracking consumed keys).
      stack.push({ id: id.argument, av: { kind: "top" } });
      continue;
    }
  }
}

// ───────────────────────────────────────────────────────────────────────────
// § 14.7.5 — ForInStatement.
// EnumerateObjectProperties per § 14.7.5.6 produces the enumerable property
// keys of the iterated object. Each iteration binds the loop variable to
// one such key, then executes the body. For our flow-insensitive-per-loop
// abstraction: bind the loop var to `{kind:"loop-key", src: rhsAv}` and
// process the body once. The bind is recorded in a fresh state clone so
// post-loop state doesn't see the loop var (matches block-scoped `let`
// semantics; for `var` declarations we conservatively join the loop var
// into post-state below).
// Returns the loop's body-entry state — the worklist driver consumes it.
// ───────────────────────────────────────────────────────────────────────────
function _specForInBodyEntryState(stmtNode, rhsAv, preState) {
  var bodyState = _specStateClone(preState);
  var loopVarName = null;
  var left = stmtNode.left;
  if (left && left.type === "VariableDeclaration" && left.declarations.length === 1 &&
      left.declarations[0].id && left.declarations[0].id.type === "Identifier") {
    loopVarName = left.declarations[0].id.name;
  } else if (left && left.type === "Identifier") {
    loopVarName = left.name;
  }
  if (loopVarName) bodyState[loopVarName] = { kind: "loop-key", src: rhsAv };
  return bodyState;
}

// ───────────────────────────────────────────────────────────────────────────
// Iterative expression evaluator — postorder enumeration of the expression
// tree, then leaf-up application of per-spec-section transfer functions.
// Termination is structural: each pushed sub-path is a strict child of a
// popped path, AST trees are finite.
// ───────────────────────────────────────────────────────────────────────────

// Enumerate all sub-paths of `rootPath` in postorder for the expression
// kinds the property-flow analyser cares about. Leaf nodes (Identifier,
// Literal, ThisExpression) appear before their parents; composite nodes
// appear after their children. Used by `_specEvalExpression` to drive the
// bottom-up evaluation without recursion.
function _specPostorderExprPaths(rootPath) {
  if (!rootPath || !rootPath.node) return [];
  var preorder = [];
  var stack = [rootPath];
  while (stack.length > 0) {
    var p = stack.pop();
    if (!p || !p.node) continue;
    preorder.push(p);
    var n = p.node;
    if (_t.isMemberExpression(n) || _t.isOptionalMemberExpression(n)) {
      stack.push(p.get("object"));
      if (n.computed) stack.push(p.get("property"));
    } else if (_t.isLogicalExpression(n)) {
      stack.push(p.get("left"));
      stack.push(p.get("right"));
    } else if (_t.isAssignmentExpression(n)) {
      // RHS is evaluated; LHS computed/object branches eval'd separately by
      // the assignment-statement driver, since they participate in the
      // effect tuple, not in the assignment's own value.
      stack.push(p.get("right"));
      if (_t.isMemberExpression(n.left)) {
        stack.push(p.get("left.object"));
        if (n.left.computed) stack.push(p.get("left.property"));
      }
    } else if (_t.isObjectExpression(n)) {
      for (var pi = n.properties.length - 1; pi >= 0; pi--) {
        var prop = n.properties[pi];
        if (_t.isObjectProperty(prop) && !prop.computed) {
          stack.push(p.get("properties." + pi + ".value"));
        }
      }
    } else if (_t.isUnaryExpression(n) || _t.isUpdateExpression(n)) {
      stack.push(p.get("argument"));
    } else if (_t.isConditionalExpression(n)) {
      stack.push(p.get("test"));
      stack.push(p.get("consequent"));
      stack.push(p.get("alternate"));
    } else if (_t.isSequenceExpression(n)) {
      for (var si = n.expressions.length - 1; si >= 0; si--) {
        stack.push(p.get("expressions." + si));
      }
    } else if (_t.isTemplateLiteral(n)) {
      // § 13.2.8 — interpolated expressions need eval before the
      // template's composition step.
      for (var ti = n.expressions.length - 1; ti >= 0; ti--) {
        stack.push(p.get("expressions." + ti));
      }
    } else if (_t.isCallExpression(n) || _t.isOptionalCallExpression(n) || _t.isNewExpression(n)) {
      // Argument expressions still get evaluated (their property writes
      // matter for effects); the call/construct's return value
      // abstracts to Top per § 13.3 / § 13.3.5.
      for (var ai = n.arguments.length - 1; ai >= 0; ai--) {
        stack.push(p.get("arguments." + ai));
      }
    } else if (_t.isBinaryExpression(n)) {
      stack.push(p.get("left"));
      stack.push(p.get("right"));
    }
    // Leaf kinds: Identifier, ThisExpression, Literals — no children pushed.
  }
  // preorder was built by DFS with right-first push so left is processed
  // first; postorder = reverse of the popped-order's reverse. Simpler to
  // just reverse preorder once.
  preorder.reverse();
  return preorder;
}

// Evaluate an expression to its abstract value.
// `state` is read-only here; assignment side effects are recorded into
// `effects` and the state map is mutated by `_specAssignmentExpressionApply`.
// Sub-evaluations are not recursive — `vals` is filled in postorder.
function _specEvalExpression(rootPath, state, effects) {
  var paths = _specPostorderExprPaths(rootPath);
  var vals = new Map();
  for (var i = 0; i < paths.length; i++) {
    var p = paths[i];
    var av = _specEvalLeaf(p, state, vals, effects);
    vals.set(p.node, av);
  }
  return vals.get(rootPath.node);
}

// ───────────────────────────────────────────────────────────────────────────
// § 23.1.3.15 — Array.prototype.forEach.
// Dispatch any `arr.forEach(callback)` call whose receiver evaluates to
// an iterable abstract value the analyser tracks. The callback's first
// param is bound to the array's element type:
//   - {kind:"keys-of", src} (returned by Object.keys per § 20.1.2.19)
//     → element type is {kind:"loop-key", src} (semantically identical
//       to a for-in loop's binding)
//   - other iterables abstract to Top for now.
// No compound shape matching: Object.keys is recognised separately by
// the expression evaluator and produces the abstract value passed in
// via the receiver. forEach dispatches purely on the receiver's
// abstract value — exactly the spec's compositional semantics.
// Returns true when the expression matched and was handled.
// ───────────────────────────────────────────────────────────────────────────
function _specHandleArrayForEach(exprNode, exprPath, state, effects, branchStack) {
  if (!exprNode || !_t.isCallExpression(exprNode)) return false;
  var callee = exprNode.callee;
  if (!_t.isMemberExpression(callee) || callee.computed) return false;
  if (!_t.isIdentifier(callee.property, { name: "forEach" })) return false;
  if (exprNode.arguments.length < 1) return false;
  var cb = exprNode.arguments[0];
  if (!_t.isFunctionExpression(cb) && !_t.isArrowFunctionExpression(cb)) return false;
  if (cb.params.length < 1 || !_t.isIdentifier(cb.params[0])) return false;

  // Evaluate the receiver abstractly through the spec-organised
  // expression evaluator. If it's a tracked iterable, derive the
  // element type per § 23.1.3.15's CallbackInvocation.
  var receiverAv = _specEvalExpression(exprPath.get("callee.object"), state, effects);
  var elementAv;
  if (receiverAv && receiverAv.kind === "keys-of") {
    elementAv = { kind: "loop-key", src: receiverAv.src };
  } else {
    return false; // Not a tracked iterable — let standard expression
                  // walk handle the call (its callback won't fire here,
                  // which preserves soundness for unknown receivers).
  }

  var cbState = _specStateClone(state);
  cbState[cb.params[0].name] = elementAv;

  var cbPath = exprPath.get("arguments.0");
  var bodyPath = cbPath.get("body");
  if (!bodyPath || !bodyPath.node) return true;
  if (_t.isBlockStatement(bodyPath.node)) {
    branchStack.push({ stmts: bodyPath.node.body, idx: 0, state: cbState, parentPath: bodyPath, isBody: true });
  } else {
    // Concise arrow body — evaluate as an expression with the binding.
    _specEvalExpression(bodyPath, cbState, effects);
  }
  return true;
}

// ───────────────────────────────────────────────────────────────────────────
// Statement walker — dispatch by spec section.
// Each branch routes a single statement to its dedicated transfer function.
// No combining of spec sections; mutation of state and effects happens
// inside the called transfer functions.
// ───────────────────────────────────────────────────────────────────────────
function _specApplyStatement(stmtPath, state, effects, branchStack) {
  if (!stmtPath || !stmtPath.node) return;
  var stmt = stmtPath.node;

  if (_t.isVariableDeclaration(stmt)) {
    // § 14.3.2 — bind each declarator.
    for (var di = 0; di < stmt.declarations.length; di++) {
      var d = stmt.declarations[di];
      var initAv;
      if (d.init) {
        initAv = _specEvalExpression(stmtPath.get("declarations." + di + ".init"), state, effects);
      }
      // Tag obj-lit values with the local var name they're bound to so
      // downstream consumers can match `JSON.stringify(varName)` body
      // arguments back to their literal-built shape. Pure metadata —
      // doesn't affect equality on AbstractValue.
      if (initAv && initAv.kind === "obj-lit" && d.id && d.id.type === "Identifier") {
        initAv = { kind: "obj-lit", props: initAv.props, _bindingName: d.id.name };
      }
      _specVariableDeclaratorBind(d.id, initAv, state);
    }
    return;
  }

  if (_t.isExpressionStatement(stmt)) {
    // § 14.5 — evaluate the expression for side effects.
    // First: recognise spec-built-in callback dispatchers like
    // Array.prototype.forEach (§ 23.1.3.15). The dispatcher derives the
    // callback's binding from the receiver's abstract value, so
    // composition works naturally — `Object.keys(src).forEach(cb)`
    // composes Object.keys (§ 20.1.2.19, in expression eval) with
    // forEach (here) without any compound shape match.
    var exprNode = stmt.expression;
    if (_specHandleArrayForEach(exprNode, stmtPath.get("expression"), state, effects, branchStack)) {
      return;
    }
    _specEvalExpression(stmtPath.get("expression"), state, effects);
    return;
  }

  if (_t.isForInStatement(stmt) || _t.isForOfStatement(stmt)) {
    // § 14.7.5 — for-in/for-of share the syntax skeleton.
    // for-in: loop var binds to each enumerable own/inherited STRING KEY
    //   of the iterated object (per EnumerateObjectProperties § 14.7.5.9).
    // for-of: loop var binds to each ELEMENT yielded by the iterator
    //   protocol (§ 7.4.6 IteratorStep / § 7.4.7 IteratorValue).
    // For property-flow analysis we model both as a generic loop-key:
    // the runtime-bound value depends on the iterated source, and
    // downstream propagation detection treats them uniformly.
    var rhsAv = _specEvalExpression(stmtPath.get("right"), state, effects);
    var bodyState = _specForInBodyEntryState(stmt, rhsAv, state);
    var bodyPath = stmtPath.get("body");
    if (bodyPath && bodyPath.node) {
      if (_t.isBlockStatement(bodyPath.node)) {
        branchStack.push({ stmts: bodyPath.node.body, idx: 0, state: bodyState, parentPath: bodyPath, isBody: true });
      } else {
        // Single-statement body — wrap in a one-element frame whose
        // parentPath points to the for-in node and resolves via "body".
        branchStack.push({ stmts: [bodyPath.node], idx: 0, state: bodyState, parentPath: stmtPath, _wrapBody: true, isBody: true });
      }
    }
    return;
  }

  if (_t.isForStatement(stmt)) {
    // § 14.7.4 — evaluate init, push body once. The increment is not
    // modeled; for property-flow, body's effects are the same per
    // iteration so a single pass suffices.
    if (stmt.init) {
      if (_t.isVariableDeclaration(stmt.init)) {
        for (var fdi = 0; fdi < stmt.init.declarations.length; fdi++) {
          var fd = stmt.init.declarations[fdi];
          var fdAv;
          if (fd.init) {
            fdAv = _specEvalExpression(stmtPath.get("init.declarations." + fdi + ".init"), state, effects);
          }
          _specVariableDeclaratorBind(fd.id, fdAv, state);
        }
      } else {
        _specEvalExpression(stmtPath.get("init"), state, effects);
      }
    }
    var forBody = stmt.body;
    var forBodyState = _specStateClone(state);
    if (_t.isBlockStatement(forBody)) {
      branchStack.push({ stmts: forBody.body, idx: 0, state: forBodyState, parentPath: stmtPath.get("body"), isBody: true });
    } else if (forBody) {
      branchStack.push({ stmts: [forBody], idx: 0, state: forBodyState, parentPath: stmtPath, _wrapBody: true, isBody: true });
    }
    return;
  }

  if (_t.isIfStatement(stmt)) {
    // § 14.6 — evaluate test for side effects, then push branches with
    // cloned states. CFG-style merge at the IfStatement's exit point:
    // each branch reports its end-state to a merge frame that joins
    // them back into the parent's state when popped. Subsequent
    // statements see the joined post-if state per § 14.6 semantics.
    _specEvalExpression(stmtPath.get("test"), state, effects);
    // Find the parent frame (the one whose stmt-array we're walking).
    // It's the frame currently on top of the stack — branchStack[length-1].
    var ifParent = branchStack.length > 0 ? branchStack[branchStack.length - 1] : null;
    if (!ifParent) return;
    // Save base state for branches that don't execute (alternate-less if).
    var ifPreState = _specStateClone(state);
    // Merge frame: pushed first (so it executes LAST due to LIFO).
    var mergeFrame = { _isMergeFrame: true, branchEndStates: [], parentFrame: ifParent, hasAlternate: !!stmt.alternate, baseState: ifPreState };
    branchStack.push(mergeFrame);
    if (stmt.alternate) {
      var altState = _specStateClone(state);
      var altNode = stmt.alternate;
      if (_t.isBlockStatement(altNode)) {
        branchStack.push({ stmts: altNode.body, idx: 0, state: altState, parentPath: stmtPath.get("alternate"), _reportTo: mergeFrame });
      } else {
        // Single-statement alternate (e.g. else-if chain) — stmtPath
        // resolves directly via the alternate's own path.
        branchStack.push({ stmts: [altNode], idx: 0, state: altState, _explicitStmtPath: stmtPath.get("alternate"), _reportTo: mergeFrame });
      }
    }
    if (stmt.consequent) {
      var consState = _specStateClone(state);
      var consNode = stmt.consequent;
      if (_t.isBlockStatement(consNode)) {
        branchStack.push({ stmts: consNode.body, idx: 0, state: consState, parentPath: stmtPath.get("consequent"), _reportTo: mergeFrame });
      } else {
        branchStack.push({ stmts: [consNode], idx: 0, state: consState, _explicitStmtPath: stmtPath.get("consequent"), _reportTo: mergeFrame });
      }
    }
    return;
  }

  if (_t.isBlockStatement(stmt)) {
    // § 14.2 — sub-block. Push as a new frame with cloned state to honor
    // block-scoping for `let`/`const`; for `var` the scope-blind walker
    // remains correct because lookups still find the binding.
    branchStack.push({ stmts: stmt.body, idx: 0, state: _specStateClone(state), parentPath: stmtPath });
    return;
  }

  if (_t.isReturnStatement(stmt)) {
    // § 14.10 — evaluate argument for any nested side effects, then stop
    // this branch. The current frame's index is left where it is; the
    // caller's check `if (idx >= stmts.length || frame._returned)` ends
    // execution.
    if (stmt.argument) _specEvalExpression(stmtPath.get("argument"), state, effects);
    if (branchStack.length > 0) branchStack[branchStack.length - 1]._returned = true;
    return;
  }

  if (_t.isThrowStatement(stmt)) {
    // § 14.14 — like return for our purposes; evaluate argument then halt.
    if (stmt.argument) _specEvalExpression(stmtPath.get("argument"), state, effects);
    if (branchStack.length > 0) branchStack[branchStack.length - 1]._returned = true;
    return;
  }

  if (_t.isWhileStatement(stmt) || _t.isDoWhileStatement(stmt)) {
    // § 14.7.2 / § 14.7.3 — evaluate test then body once.
    _specEvalExpression(stmtPath.get("test"), state, effects);
    var wBody = stmt.body;
    var wBodyState = _specStateClone(state);
    if (_t.isBlockStatement(wBody)) {
      branchStack.push({ stmts: wBody.body, idx: 0, state: wBodyState, parentPath: stmtPath.get("body"), isBody: true });
    } else if (wBody) {
      branchStack.push({ stmts: [wBody], idx: 0, state: wBodyState, parentPath: stmtPath, _wrapBody: true, isBody: true });
    }
    return;
  }

  // Other statement types (FunctionDeclaration, ClassDeclaration, …) —
  // no transfer for property flow. They don't contribute property writes.
}

// ───────────────────────────────────────────────────────────────────────────
// Property-flow analyser entry point.
// Walks a function body with the iterative statement walker; returns the
// effects list (every property write recorded as
// `{target: AbstractValue, key: AbstractValue, value: AbstractValue}`).
// Memoised per function node.
// ───────────────────────────────────────────────────────────────────────────
var _specEffectsMemo = new WeakMap();
function _specAnalyzePropertyFlow(funcPath) {
  if (!funcPath || !funcPath.node) return [];
  var fnNode = funcPath.node;
  if (!_t.isFunction(fnNode)) return [];
  if (_specEffectsMemo.has(fnNode)) return _specEffectsMemo.get(fnNode);

  var bodyPath = funcPath.get("body");
  if (!bodyPath || !bodyPath.node) {
    _specEffectsMemo.set(fnNode, []);
    return [];
  }

  var entryState = _specInitialFunctionBodyState(fnNode);
  var effects = [];
  var stack;
  if (_t.isBlockStatement(bodyPath.node)) {
    stack = [{ stmts: bodyPath.node.body, idx: 0, state: entryState, parentPath: bodyPath }];
  } else {
    // ArrowFunctionExpression concise body per § 15.3.5.13: the body
    // is a single expression whose value is implicitly returned.
    // Wrap it as a synthetic ExpressionStatement to feed the walker.
    _specEvalExpression(bodyPath, entryState, effects);
    _specEffectsMemo.set(fnNode, effects);
    return effects;
  }

  while (stack.length > 0) {
    var top = stack[stack.length - 1];

    // Merge frame: branches that report to it have all completed
    // (LIFO ordering — branches were pushed AFTER the merge frame, so
    // they pop first). Join collected end-states into the parent's state.
    if (top._isMergeFrame) {
      var ifPF = top.parentFrame;
      var joinedState = top.baseState;  // base state if no branches ran
      for (var bi = 0; bi < top.branchEndStates.length; bi++) {
        joinedState = _specJoinState(joinedState, top.branchEndStates[bi]);
      }
      // If the if had no alternate, the implicit "no-op" branch is
      // baseState — already in the join. Otherwise both branches
      // contributed.
      if (ifPF) ifPF.state = joinedState;
      stack.pop();
      continue;
    }

    if (top._returned || top.idx >= top.stmts.length) {
      // Branch frame completing: report end-state to its merge frame.
      if (top._reportTo) {
        top._reportTo.branchEndStates.push(top.state);
      }
      stack.pop();
      continue;
    }
    var stmt = top.stmts[top.idx];
    var stmtPath;
    if (top._explicitStmtPath) {
      // Single-statement frame with a pre-computed path (used for
      // if-statement consequent/alternate, and for-loop bodies that
      // aren't BlockStatements — see callers).
      stmtPath = top._explicitStmtPath;
    } else if (top._wrapBody) {
      // Legacy path — single-statement body where parentPath is the
      // parent statement and the body itself is at .body.
      stmtPath = top.parentPath.get("body");
    } else {
      stmtPath = top.parentPath.get("body." + top.idx);
    }
    top.idx++;
    if (!stmt) continue;
    _specApplyStatement(stmtPath, top.state, effects, stack);
  }

  _specEffectsMemo.set(fnNode, effects);
  return effects;
}

// Compute one node's abstract value given its already-evaluated children
// (looked up in `vals`). Any mutation of `state` (rebinding identifiers)
// or `effects` (property writes) happens here for AssignmentExpression.
function _specEvalLeaf(path, state, vals, effects) {
  var n = path.node;
  if (_t.isThisExpression(n)) return { kind: "this" };
  if (_t.isStringLiteral(n)) return { kind: "const", value: n.value };
  if (_t.isNumericLiteral(n)) return { kind: "const", value: n.value };
  if (_t.isBooleanLiteral(n)) return { kind: "const", value: n.value };
  if (_t.isNullLiteral(n)) return { kind: "const", value: null };
  if (_t.isTemplateLiteral(n)) {
    // § 13.2.8.6 Template Literal Evaluation: composes cooked quasis
    // with stringified expression results. When every expression
    // resolves to a Const value at this analysis level, compose the
    // full string; otherwise the value abstracts to Top per abstract
    // interpretation soundness (we don't fabricate the unknown bits).
    var composedStr = "";
    var allConst = true;
    for (var qi = 0; qi < n.quasis.length; qi++) {
      var quasi = n.quasis[qi];
      var qCooked = quasi && quasi.value && quasi.value.cooked;
      composedStr += (typeof qCooked === "string" ? qCooked : "");
      if (qi < n.expressions.length) {
        var exprAv = vals.get(n.expressions[qi]);
        if (!exprAv || exprAv.kind !== "const") { allConst = false; break; }
        // ToString per § 7.1.17: spec-grounded conversion.
        var s = exprAv.value;
        if (typeof s !== "string") s = String(s);
        composedStr += s;
      }
    }
    if (allConst) return { kind: "const", value: composedStr };
    return { kind: "top" };
  }
  if (_t.isIdentifier(n)) {
    if (Object.prototype.hasOwnProperty.call(state, n.name)) return state[n.name];
    if (n.name === "undefined" && !path.scope.getBinding("undefined")) {
      return { kind: "const", value: undefined };
    }
    // arguments references — § 10.4.4
    if (n.name === "arguments" && !path.scope.getBinding("arguments")) {
      return { kind: "top" }; // arguments object — handled at member access
    }
    return { kind: "top" };
  }
  if (_t.isMemberExpression(n) || _t.isOptionalMemberExpression(n)) {
    var objAv = vals.get(n.object) || { kind: "top" };
    // arguments.length per § 10.4.4.6 / arguments[N] per § 10.4.4
    if (objAv && objAv.kind === "top" &&
        _t.isIdentifier(n.object, { name: "arguments" }) &&
        !path.scope.getBinding("arguments")) {
      if (!n.computed && _t.isIdentifier(n.property, { name: "length" })) {
        return { kind: "args-len" };
      }
      if (n.computed) {
        var idxAv = vals.get(n.property) || { kind: "top" };
        return { kind: "args-elt", idx: idxAv };
      }
    }
    var keyAvComputed = n.computed ? (vals.get(n.property) || { kind: "top" }) : null;
    return _specMemberExpressionAv(n, objAv, keyAvComputed);
  }
  if (_t.isLogicalExpression(n)) {
    // § 13.13.1 (&&), § 13.13.2 (||), § 13.14 (??): each operator
    // short-circuits per ToBoolean / nullish testing. Without dynamic
    // truthiness, the value at runtime is either left or right —
    // model as `or(left, right)` for all three operators.
    if (n.operator === "||" || n.operator === "&&" || n.operator === "??") {
      return _specLogicalOrAv(vals.get(n.left) || { kind: "top" }, vals.get(n.right) || { kind: "top" });
    }
  }
  if (_t.isAssignmentExpression(n)) {
    var rhsAv = vals.get(n.right) || { kind: "top" };
    var lhsObjAv = null, lhsKeyAv = null;
    if (_t.isMemberExpression(n.left)) {
      lhsObjAv = vals.get(n.left.object) || { kind: "top" };
      if (n.left.computed) lhsKeyAv = vals.get(n.left.property) || { kind: "top" };
      else if (_t.isIdentifier(n.left.property)) lhsKeyAv = { kind: "const", value: n.left.property.name };
      else if (_t.isStringLiteral(n.left.property)) lhsKeyAv = { kind: "const", value: n.left.property.value };
      else lhsKeyAv = { kind: "top" };
    }
    return _specAssignmentExpressionApply(n, state, rhsAv, lhsObjAv, lhsKeyAv, effects);
  }
  if (_t.isBinaryExpression(n)) {
    // § 13.10 BinaryOperators — only `+` is interesting for property-flow
    // (string concat for URL/key building per § 13.15.3 ApplyStringOrNumeric-
    // BinaryOperator). When both operands are Const, perform the spec-
    // grounded concatenation; otherwise abstract to Top.
    var leftAv = vals.get(n.left);
    var rightAv = vals.get(n.right);
    if (n.operator === "+" && leftAv && rightAv &&
        leftAv.kind === "const" && rightAv.kind === "const") {
      var lv = leftAv.value, rv = rightAv.value;
      if (typeof lv === "string" || typeof rv === "string") {
        return { kind: "const", value: String(lv) + String(rv) };
      }
      if (typeof lv === "number" && typeof rv === "number") {
        return { kind: "const", value: lv + rv };
      }
    }
    return { kind: "top" };
  }
  if (_t.isObjectExpression(n)) {
    var props = Object.create(null);
    for (var pi = 0; pi < n.properties.length; pi++) {
      var prop = n.properties[pi];
      if (_t.isObjectProperty(prop) && !prop.computed) {
        var k = _t.isIdentifier(prop.key) ? prop.key.name :
          _t.isStringLiteral(prop.key) ? prop.key.value : null;
        if (k !== null) props[k] = vals.get(prop.value) || { kind: "top" };
      }
    }
    return { kind: "obj-lit", props: props };
  }
  if (_t.isCallExpression(n) || _t.isOptionalCallExpression(n)) {
    // § 20.1.2.19 — Object.keys(src) returns an array of src's
    // enumerable own keys. Recognised as a built-in only when the
    // `Object` identifier is the unshadowed global (§ 8.1.1).
    if (_t.isMemberExpression(n.callee) && !n.callee.computed &&
        _t.isIdentifier(n.callee.object, { name: "Object" }) &&
        !path.scope.getBinding("Object") &&
        _t.isIdentifier(n.callee.property, { name: "keys" }) &&
        n.arguments.length >= 1) {
      var srcAv = vals.get(n.arguments[0]) || { kind: "top" };
      return { kind: "keys-of", src: srcAv };
    }
    return { kind: "top" }; // Other call returns abstract to Top.
  }
  // Unmodelled expression kinds abstract to Top — sound conservative answer.
  return { kind: "top" };
}

// ───────────────────────────────────────────────────────────────────────────
// Propagation-pattern detection on accumulated effects.
// Scans the effect list for the canonical Object.assign-equivalent pattern:
//   target[loop-key(src)] = src[same loop-key]
// where target.kind ∈ {this, param, args-elt} and src is one of those too.
// Returns { source: <AbstractValue of src>, target: <AbstractValue of target> }
// or null. Per CLAUDE.md L29: this is recognising the SPEC-equivalent of
// Object.assign per ECMA § 20.1.2.1 by its dataflow effect, not by name —
// any function whose body's net effect is "copy src's enumerable own
// properties to target" qualifies, regardless of how it spells the loop.
// ───────────────────────────────────────────────────────────────────────────
function _specDetectPropagationFromEffects(effects) {
  if (!effects || effects.length === 0) return null;
  for (var i = 0; i < effects.length; i++) {
    var e = effects[i];
    if (!e.key || e.key.kind !== "loop-key") continue;
    if (!e.value || e.value.kind !== "member") continue;
    if (!_specEqualAv(e.key.src, e.value.obj)) continue;
    if (!_specEqualAv(e.value.key, e.key)) continue;
    // Walk every leaf of the target's or-tree; each represents a
    // possible value the target could hold per § 13.13 / § 14.6 join
    // semantics. A propagation matches if any leaf is a tractable
    // input slot (this / param / args-elt).
    var queue = [e.target];
    while (queue.length > 0) {
      var t = queue.pop();
      if (!t) continue;
      if (t.kind === "or") {
        if (t.left) queue.push(t.left);
        if (t.right) queue.push(t.right);
        continue;
      }
      if (t.kind === "this" || t.kind === "param" || t.kind === "args-elt") {
        return { source: e.key.src, target: t };
      }
    }
  }
  return null;
}

// (Removed `_resolveCalleeToFunction` — all routes now live in the
// canonical iterative state machine `_resolveCalleeFuncPath` /
// `_rcfpStep` per CLAUDE.md spec-organisation discipline. Identifier
// global-assignment / factory-return resolution is in `_RCFP_INIT`'s
// Branch 4 / Branch 5; MemberExpression IIFE-returned property and
// global-alias unwrap routes are in `_RCFP_OBJ_AFTER`. ONE callee
// resolver, no fallback chain.)

function _traceWrapperFunction(callPath, funcPath, funcBinding, result) {
  var funcNode = funcPath.node;
  // Check if the function body contains a direct fetch/XHR call
  var sinkInfo = _findSinkInFunction(funcPath);
  if (!sinkInfo) {
    // No direct sink — check for deep sink (nested inside closures/callbacks).
    // This handles libraries like jQuery where $.ajax() → transport.send() → xhr.open()
    // is buried several levels deep inside nested functions.
    var deepPropMap = _findDeepSinkPropertyMap(funcPath);
    if (deepPropMap) {
      _traceDeepSinkCall(callPath, funcNode, funcBinding, deepPropMap, result);
    }
    // Chained wrapper: function(e,t){return n(e,t,"get")} where n() has the sink.
    // Map caller args through the wrapper to the inner function's args, then trace.
    if (!deepPropMap) {
      _traceChainedWrapper(callPath, funcNode, result);
    }
    return;
  }

  _stats.interProcTraces++;

  // Map caller's arguments to function parameters (both resolved values and raw argument paths)
  var paramBindings = {};  // paramName → string[]
  var paramArgPaths = {};  // paramName → argPath (for object property extraction)
  var callArgs = callPath.node.arguments;
  for (var i = 0; i < funcNode.params.length && i < callArgs.length; i++) {
    var param = funcNode.params[i];
    var paramName = _t.isIdentifier(param) ? param.name :
      (_t.isAssignmentPattern(param) && _t.isIdentifier(param.left) ? param.left.name : null);
    if (paramName) {
      var argPath = callPath.get("arguments." + i);
      paramArgPaths[paramName] = argPath;
      var resolved = _resolveAllValues(argPath, 1);
      if (resolved.length > 0) {
        paramBindings[paramName] = resolved;
      }
    }
  }

  // If function was resolved from a higher-order call (var fn = factory(args)),
  // also map the factory's params (closure bindings) into paramBindings.
  if (funcBinding && _t.isVariableDeclarator(funcBinding.path.node) &&
      funcBinding.path.node.init && _t.isCallExpression(funcBinding.path.node.init)) {
    var outerCallPath = funcBinding.path.get("init");
    var outerCallArgs = funcBinding.path.node.init.arguments;
    var outerCallee = funcBinding.path.node.init.callee;
    if (_t.isIdentifier(outerCallee)) {
      var outerBinding = funcBinding.path.scope.getBinding(outerCallee.name);
      var outerFunc = null;
      if (outerBinding) {
        if (_t.isFunctionDeclaration(outerBinding.path.node)) outerFunc = outerBinding.path.node;
        else if (_t.isVariableDeclarator(outerBinding.path.node) && outerBinding.path.node.init &&
                 (_t.isFunctionExpression(outerBinding.path.node.init) || _t.isArrowFunctionExpression(outerBinding.path.node.init)))
          outerFunc = outerBinding.path.node.init;
      }
      if (outerFunc) {
        for (var oi = 0; oi < outerFunc.params.length && oi < outerCallArgs.length; oi++) {
          var outerParam = outerFunc.params[oi];
          var outerParamName = _t.isIdentifier(outerParam) ? outerParam.name : null;
          if (outerParamName && !paramBindings[outerParamName]) {
            var outerArgResolved = _resolveAllValues(outerCallPath.get("arguments." + oi), 1);
            if (outerArgResolved.length > 0) paramBindings[outerParamName] = outerArgResolved;
          }
        }
      }
    }
  }

  // Re-resolve headers using paramBindings (handles closure variables like "Bearer " + token)
  if (sinkInfo.headersNode) {
    var enhancedHeaders = {};
    for (var hi = 0; hi < sinkInfo.headersNode.properties.length; hi++) {
      var hProp = sinkInfo.headersNode.properties[hi];
      if (!_t.isObjectProperty(hProp) || hProp.computed) continue;
      var hName = _getKeyName(hProp.key);
      if (!hName) continue;
      if (_t.isStringLiteral(hProp.value)) {
        enhancedHeaders[hName] = hProp.value.value;
      } else {
        var hResolved = _resolveHeaderValue(hProp.value, paramBindings);
        if (hResolved !== null) enhancedHeaders[hName] = hResolved;
      }
    }
    sinkInfo.headers = enhancedHeaders;
  }

  // Build call sites using the sink info + resolved parameter values
  var urls = [];
  // Track whether the URL source was a pure StringLiteral — only in
  // that case can downstream extractors safely treat the URL's query
  // values as author-written literals (vs. resolver-rendered
  // placeholders like `{x}` produced when a template expression
  // couldn't be resolved to a concrete value).
  var urlsFromLiteral = false;
  if (sinkInfo.urlParamName && paramBindings[sinkInfo.urlParamName]) {
    urls = paramBindings[sinkInfo.urlParamName];
  } else if (sinkInfo.urlLiteral) {
    urls = [sinkInfo.urlLiteral];
    urlsFromLiteral = true;
  } else if (sinkInfo.urlMemberExpr && paramArgPaths[sinkInfo.urlMemberExpr.obj]) {
    // URL is opts.url — extract .url property from the caller's object argument
    urls = _resolvePropertyFromArg(paramArgPaths[sinkInfo.urlMemberExpr.obj], sinkInfo.urlMemberExpr.prop, 1);
  }
  if (urls.length === 0) return;

  var method = sinkInfo.method || "GET";
  if (sinkInfo.methodParamName && paramBindings[sinkInfo.methodParamName]) {
    method = paramBindings[sinkInfo.methodParamName][0];
    if (typeof method === "string") method = method.toUpperCase();
    else method = "GET";
  } else if (sinkInfo.methodMemberExpr && paramArgPaths[sinkInfo.methodMemberExpr.obj]) {
    // Method is opts.method — extract from caller's object argument
    var methodVals = _resolvePropertyFromArg(paramArgPaths[sinkInfo.methodMemberExpr.obj], sinkInfo.methodMemberExpr.prop, 1);
    if (methodVals.length > 0 && typeof methodVals[0] === "string" && _HTTP_METHODS_LC[methodVals[0].toLowerCase()]) {
      method = methodVals[0].toUpperCase();
    }
  }

  // Get enclosing function name for the CALLER
  var callerFunc = callPath.getFunctionParent();
  var callerName = null;
  if (callerFunc && callerFunc.node.id) callerName = callerFunc.node.id.name;

  // ── Resolve caller's body params ──
  // sinkInfo.params contains params extracted from the wrapper's fetch() body,
  // which is typically empty (body is a parameter identifier, not an object literal).
  // Instead, extract body params from the caller's actual argument.
  var callerBodyParams = sinkInfo.params || [];
  if (sinkInfo.bodyParamName && paramArgPaths[sinkInfo.bodyParamName]) {
    var cbp = _extractBodyParams(paramArgPaths[sinkInfo.bodyParamName].node, callPath);
    if (cbp.length > 0) callerBodyParams = cbp;
  } else if (sinkInfo.bodyMemberExpr && paramArgPaths[sinkInfo.bodyMemberExpr.obj]) {
    // Body comes from opts.data / opts.body — resolve the object, extract the property
    var bodyObjNode = null;
    try { bodyObjNode = _resolveToObject(paramArgPaths[sinkInfo.bodyMemberExpr.obj], 1); } catch(e) { _resolver.collectError(e, "bodyMemberResolve"); }
    if (bodyObjNode) {
      for (var bpi = 0; bpi < bodyObjNode.properties.length; bpi++) {
        var bp = bodyObjNode.properties[bpi];
        if (!_t.isObjectProperty(bp) || bp.computed) continue;
        if (_getKeyName(bp.key) === sinkInfo.bodyMemberExpr.prop) {
          var bodyPropNode = bp.value;
          if (_t.isObjectExpression(bodyPropNode)) {
            callerBodyParams = _extractObjectProperties(bodyPropNode);
            for (var cbpi = 0; cbpi < callerBodyParams.length; cbpi++) callerBodyParams[cbpi].location = "body";
          }
          break;
        }
      }
    }
  }

  // ── Build function-param metadata (which params are used as path, method, etc.) ──
  var wrapperFuncParams = [];
  for (var wpi = 0; wpi < funcNode.params.length; wpi++) {
    var wp = funcNode.params[wpi];
    var wpName = _t.isIdentifier(wp) ? wp.name :
      (_t.isAssignmentPattern(wp) && _t.isIdentifier(wp.left) ? wp.left.name : null);
    if (!wpName) continue;
    // Skip params already consumed as URL, method, or body
    if (wpName === sinkInfo.urlParamName || wpName === sinkInfo.methodParamName || wpName === sinkInfo.bodyParamName) continue;
    if (sinkInfo.urlMemberExpr && wpName === sinkInfo.urlMemberExpr.obj) continue;
    if (sinkInfo.methodMemberExpr && wpName === sinkInfo.methodMemberExpr.obj) continue;
    if (sinkInfo.bodyMemberExpr && wpName === sinkInfo.bodyMemberExpr.obj) continue;
    // This is a non-consumed param — determine its location
    if (paramBindings[wpName] && paramBindings[wpName].length > 0) {
      var wpLoc = "path";  // default: assume it contributes to URL if not body/method
      var wpRequired = !(_t.isAssignmentPattern(wp));
      var wpDefault = _t.isAssignmentPattern(wp) && _t.isStringLiteral(wp.right) ? wp.right.value : undefined;
      wrapperFuncParams.push({ name: wpName, location: wpLoc, required: wpRequired, defaultValue: wpDefault });
    }
  }

  // ── Combine params: body params from caller + function-level params ──
  var allParams = [];
  for (var abp = 0; abp < callerBodyParams.length; abp++) allParams.push(callerBodyParams[abp]);
  for (var afp = 0; afp < wrapperFuncParams.length; afp++) allParams.push(wrapperFuncParams[afp]);

  // ── Cross-reference params with value constraints ──
  for (var vc = 0; vc < allParams.length; vc++) {
    if (allParams[vc].spread) continue;
    var pName = allParams[vc].name;
    var constraint = _getConstraint(callPath, pName);
    if (!constraint && allParams[vc].source && allParams[vc].source !== pName) {
      constraint = _getConstraint(callPath, allParams[vc].source);
    }
    if (constraint && constraint.values.size >= 1) {
      var validValues = [];
      constraint.values.forEach(function(v) { validValues.push(v); });
      allParams[vc].validValues = validValues;
    }
  }

  // ── Property-flow effects → branch-conditional valid values ──
  // The user directive: "if there's a code path where role=admin and
  // role=guest we show both as options". The property-flow analyser
  // already records every property write (including conditional
  // branches) per § 14.6 IfStatement. Aggregate const-value writes per
  // body-param key and emit the union as `validValues` so the schema
  // dropdown surfaces all branch values.
  var enclosingFnPath = callPath.getFunctionParent();
  if (enclosingFnPath) {
    var pfeEffects = _specAnalyzePropertyFlow(enclosingFnPath);
    if (pfeEffects && pfeEffects.length > 0) {
      var byKey = Object.create(null);
      for (var pi = 0; pi < pfeEffects.length; pi++) {
        var pe = pfeEffects[pi];
        if (!pe.target || (pe.target.kind !== "this" && pe.target.kind !== "param" && pe.target.kind !== "args-elt")) continue;
        if (!pe.key || pe.key.kind !== "const") continue;
        if (!pe.value || pe.value.kind !== "const") continue;
        // Only keep primitive-valued writes; objects/loop-keys are not
        // dropdown-displayable values.
        var v = pe.value.value;
        if (typeof v !== "string" && typeof v !== "number" && typeof v !== "boolean") continue;
        var k = pe.key.value;
        if (!byKey[k]) byKey[k] = new Set();
        byKey[k].add(v);
      }
      for (var apIdx = 0; apIdx < allParams.length; apIdx++) {
        if (allParams[apIdx].spread) continue;
        var apName = allParams[apIdx].name;
        if (!byKey[apName]) continue;
        var pfeVals = [];
        byKey[apName].forEach(function(vv) { pfeVals.push(vv); });
        if (pfeVals.length === 0) continue;
        // Merge with existing validValues (constraint-derived); union of
        // both sets so neither is lost.
        if (allParams[apIdx].validValues && allParams[apIdx].validValues.length > 0) {
          var merged = allParams[apIdx].validValues.slice();
          for (var mi = 0; mi < pfeVals.length; mi++) {
            if (merged.indexOf(pfeVals[mi]) < 0) merged.push(pfeVals[mi]);
          }
          allParams[apIdx].validValues = merged;
        } else {
          allParams[apIdx].validValues = pfeVals;
        }
      }
    }
  }

  var _callLoc = _nodeLoc(callPath.node);
  for (var u = 0; u < urls.length; u++) {
    _pushFetchSite(result, _buildFetchSite(urls[u], method, sinkInfo.headers, "fetch", allParams, { enclosingFunction: callerName, urlIsLiteral: urlsFromLiteral, loc: _callLoc }));
    console.debug("[AST:fetch] traced %s %s via %s()", method, urls[u],
      (funcBinding ? funcBinding.identifier.name : _describeNode(callPath.node.callee)) || "?");
  }
}

// Trace a "deep sink" call — the callee's function body doesn't have a DIRECT sink,
// but nested functions inside it eventually reach xhr.open/fetch.
// Uses property name matching: if the deep sink reads opts.url and opts.type,
// search the caller's arguments for objects with matching property names.
function _traceDeepSinkCall(callPath, funcNode, funcBinding, propMap, result) {
  _stats.interProcTraces++;

  var callArgs = callPath.node.arguments;
  var urls = [];
  var method = propMap.methodLiteral || null;

  // If the deep sink has a literal URL, use it directly
  if (propMap.urlLiteral) {
    urls = [propMap.urlLiteral];
  }

  // Search call arguments for objects with matching property names
  for (var i = 0; i < callArgs.length && i < 5; i++) {
    var argPath = callPath.get("arguments." + i);

    // Look for URL property matches (e.g., .url on the deep sink → extract .url from caller's arg)
    if (urls.length === 0 && propMap.urlProps.length > 0) {
      for (var up = 0; up < propMap.urlProps.length; up++) {
        var urlVals = _resolvePropertyFromArg(argPath, propMap.urlProps[up], 1);
        if (urlVals.length > 0) {
          urls = urlVals;
          break;
        }
      }
    }

    // Look for method property matches (e.g., .type on the deep sink → extract .type from caller's arg)
    if (!method && propMap.methodProps.length > 0) {
      for (var mp = 0; mp < propMap.methodProps.length; mp++) {
        var methodVals = _resolvePropertyFromArg(argPath, propMap.methodProps[mp], 1);
        if (methodVals.length > 0 && typeof methodVals[0] === "string" &&
            _HTTP_METHODS_LC[methodVals[0].toLowerCase()]) {
          method = methodVals[0].toUpperCase();
          break;
        }
      }
    }
  }

  // Fallback: check if any argument is a direct URL string (function takes (url, options) pattern)
  if (urls.length === 0) {
    for (var si = 0; si < callArgs.length && si < 3; si++) {
      var strVals = _resolveAllValues(callPath.get("arguments." + si), 1);
      for (var sv = 0; sv < strVals.length; sv++) {
        if (typeof strVals[sv] === "string" && strVals[sv].length > 0 &&
            (strVals[sv].charAt(0) === "/" || strVals[sv].indexOf("://") > 0)) {
          urls.push(strVals[sv]);
        }
      }
      if (urls.length > 0) break;
    }
  }

  if (urls.length === 0) return;
  if (!method) method = "GET";

  // ── Extract body params from caller args ──
  // Properties not consumed as URL or method are potential body/config params.
  var consumedProps = {};
  for (var cp = 0; cp < propMap.urlProps.length; cp++) consumedProps[propMap.urlProps[cp]] = true;
  for (var cm = 0; cm < propMap.methodProps.length; cm++) consumedProps[propMap.methodProps[cm]] = true;

  var deepParams = [];
  for (var di = 0; di < callArgs.length && di < 5; di++) {
    var deepArgPath = callPath.get("arguments." + di);
    var deepArgObj = null;
    try { deepArgObj = _resolveToObject(deepArgPath, 1); } catch(e) { _resolver.collectError(e, "deepSinkArgResolve"); }
    if (!deepArgObj) continue;
    for (var dpi = 0; dpi < deepArgObj.properties.length; dpi++) {
      var dp = deepArgObj.properties[dpi];
      if (!_t.isObjectProperty(dp) || dp.computed) continue;
      var dpKey = _getKeyName(dp.key);
      if (!dpKey || consumedProps[dpKey]) continue;
      // Skip spec-defined non-body config properties (Fetch RequestInit
      // per Fetch Standard § 5.4 + XMLHttpRequest properties per XHR
      // Standard § 4.5). Framework-specific option names (jQuery's
      // success / error / complete / beforeSend / dataType / crossDomain /
      // processData / contentType) are NOT skipped — they fall through
      // to body extraction. Spec-correct treatment: when the wrapper is
      // jQuery $.ajax, those options are callbacks/config; when it's an
      // arbitrary user-defined wrapper, they could legitimately be body
      // fields. Without trace-through into the wrapper's body to see how
      // each option is actually used, the analyzer cannot tell — and
      // CLAUDE.md L29 bans framework-specific name recognition. The
      // spec-grounded skip list contains only ECMAScript / Fetch / XHR
      // spec property names.
      if (dpKey === "headers" ||         // Fetch RequestInit + XHR setRequestHeader
          dpKey === "method" ||          // Fetch RequestInit
          dpKey === "mode" ||            // Fetch RequestInit (no-cors / cors / same-origin)
          dpKey === "credentials" ||     // Fetch RequestInit (omit / same-origin / include)
          dpKey === "cache" ||           // Fetch RequestInit (cache mode)
          dpKey === "redirect" ||        // Fetch RequestInit (follow / error / manual)
          dpKey === "referrer" ||        // Fetch RequestInit
          dpKey === "referrerPolicy" ||  // Fetch RequestInit
          dpKey === "integrity" ||       // Fetch RequestInit (subresource integrity)
          dpKey === "keepalive" ||       // Fetch RequestInit
          dpKey === "signal" ||          // Fetch RequestInit (AbortSignal)
          dpKey === "priority" ||        // Fetch RequestInit (priority hints)
          dpKey === "duplex" ||          // Fetch RequestInit (half / full)
          dpKey === "window" ||          // Fetch RequestInit (must be null per spec)
          dpKey === "async" ||           // XHR open()'s third param
          dpKey === "withCredentials" || // XHR property
          dpKey === "responseType") continue;  // XHR responseType
      // (Removed `dpKey === "data"` flatten-as-body-fields special
      // case — that was jQuery $.ajax convention. Spec-correct
      // treatment per Fetch Standard § 5.4: the body property is
      // named `body`, and arbitrary wrapper functions may name their
      // body argument anything. The wrapper's actual body assignment
      // is reachable via inter-procedural trace-through; without it,
      // every non-skip-listed property is added as a single body
      // field below — which is the spec-grounded fallback when the
      // wrapper's body extraction semantic isn't known.)
    }
  }

  var callerFunc = callPath.getFunctionParent();
  var callerName = callerFunc && callerFunc.node.id ? callerFunc.node.id.name : null;
  var calleeName = funcBinding ? funcBinding.identifier.name : _describeNode(callPath.node.callee);

  for (var u = 0; u < urls.length; u++) {
    _pushFetchSite(result, _buildFetchSite(urls[u], method, {}, propMap.type === "xhr" ? "xhr" : "fetch", deepParams, { enclosingFunction: callerName }));
    console.debug("[AST:fetch] deep-traced %s %s via %s()", method, urls[u], calleeName || "?");
  }
}

// Trace a local variable in a function body back to a function parameter.
// e.g., function n(e,n,a,o,i){ var u = "string"!=typeof e?(n=e).url:e; ... }
// _traceLocalVarToParam(funcNode, "u") → "e" (param[0])
function _traceLocalVarToParam(funcNode, varName) {
  var stmts = funcNode.body && funcNode.body.body ? funcNode.body.body : [];
  var initExpr = null;
  for (var si = 0; si < stmts.length; si++) {
    if (_t.isVariableDeclaration(stmts[si])) {
      var decls = stmts[si].declarations;
      for (var di = 0; di < decls.length; di++) {
        if (_t.isIdentifier(decls[di].id, {name: varName}) && decls[di].init) {
          initExpr = decls[di].init;
          break;
        }
      }
    }
    if (initExpr) break;
  }
  if (!initExpr) return null;
  // Build param name set
  var paramNames = {};
  for (var pi = 0; pi < funcNode.params.length; pi++) {
    var p = funcNode.params[pi];
    if (_t.isIdentifier(p)) paramNames[p.name] = true;
    else if (_t.isAssignmentPattern(p) && _t.isIdentifier(p.left)) paramNames[p.left.name] = true;
  }
  return _findParamInExpr(initExpr, paramNames);
}
function _findParamInExpr(node, paramNames) {
  // Iterative: walk expression chains via explicit stack
  var stack = [node];
  while (stack.length > 0) {
    var n = stack.pop();
    if (!n) continue;
    if (_t.isIdentifier(n) && paramNames[n.name]) return n.name;
    if (_t.isConditionalExpression(n)) {
      stack.push(n.consequent, n.alternate);
    } else if (_t.isLogicalExpression(n)) {
      stack.push(n.left, n.right);
    } else if (_t.isAssignmentExpression(n)) {
      stack.push(n.right);
    } else if (_t.isMemberExpression(n)) {
      stack.push(n.object);
    }
  }
  return null;
}

// Chained wrapper tracing: function(e,t){return n(e,t,"get")} where n() contains the actual sink.
// Maps the outer call's arguments through the wrapper's params to the inner call's arguments,
// then recursively traces the inner function as a wrapper.
function _traceChainedWrapper(callPath, funcNode, result) {
  // Find the call expression in the function body (simple return-call or single expression)
  var innerCall = null;
  var body = funcNode.body;
  if (!_t.isBlockStatement(body)) return; // arrow with expression body handled separately
  var stmts = body.body;
  for (var si = 0; si < stmts.length; si++) {
    var stmt = stmts[si];
    if (_t.isReturnStatement(stmt) && stmt.argument && _t.isCallExpression(stmt.argument)) {
      innerCall = stmt.argument;
      break;
    }
    if (_t.isExpressionStatement(stmt) && _t.isCallExpression(stmt.expression)) {
      innerCall = stmt.expression;
    }
  }
  if (!innerCall) { return; }

  // Resolve the inner callee to a function path that contains a sink
  var innerCallee = innerCall.callee;
  var innerFuncPath = null;
  if (_t.isIdentifier(innerCallee)) {
    // Check if the inner callee resolves to a function containing a network sink
    var innerBinding = callPath.scope.getBinding(innerCallee.name);
    // Try the IIFE scope (when the wrapper was resolved from _resolveIIFEReturnedProperty)
    if (!innerBinding && _lastIIFEFuncPath) {
      try {
        innerBinding = _lastIIFEFuncPath.scope.getBinding(innerCallee.name);
      } catch(e) { _resolver.collectError(e, "iifeChainedScope"); }
    }
    if (innerBinding) {
      if (_t.isFunctionDeclaration(innerBinding.path.node)) innerFuncPath = innerBinding.path;
      else if (_t.isVariableDeclarator(innerBinding.path.node) && innerBinding.path.node.init &&
               (_t.isFunctionExpression(innerBinding.path.node.init) || _t.isArrowFunctionExpression(innerBinding.path.node.init)))
        innerFuncPath = innerBinding.path.get("init");
    }
  }
  if (!innerFuncPath) { return; }
  if (!_containsNetworkSink(innerFuncPath)) return;

  var innerFuncNode = innerFuncPath.node;
  // Map outer call args through wrapper params to inner call args
  // e.g., outerCall: e.get("/api/users") → wrapper: function(e,t){return n(e,t,"get")}
  // Maps: e→"/api/users", t→undefined, then builds synthetic inner call: n("/api/users", undefined, "get")
  var paramMap = {}; // wrapper param name → caller arg index
  for (var pi = 0; pi < funcNode.params.length; pi++) {
    var p = funcNode.params[pi];
    var pn = _t.isIdentifier(p) ? p.name : (_t.isAssignmentPattern(p) && _t.isIdentifier(p.left) ? p.left.name : null);
    if (pn) paramMap[pn] = pi;
  }

  // Build resolved arg values for the inner call by substituting wrapper params
  var resolvedArgs = [];
  for (var ai = 0; ai < innerCall.arguments.length; ai++) {
    var arg = innerCall.arguments[ai];
    if (_t.isIdentifier(arg) && paramMap[arg.name] !== undefined) {
      var outerIdx = paramMap[arg.name];
      if (outerIdx < callPath.node.arguments.length) {
        resolvedArgs.push({ fromCaller: true, callerArgIdx: outerIdx });
      } else {
        resolvedArgs.push({ literal: null });
      }
    } else if (_t.isStringLiteral(arg)) {
      resolvedArgs.push({ literal: arg.value });
    } else {
      resolvedArgs.push({ literal: null });
    }
  }

  // Now trace the inner function with the mapped arguments
  var innerSinkInfo = _findSinkInFunction(innerFuncPath);
  if (!innerSinkInfo) return;

  // Phase 1a: If URL identifier doesn't match an inner function param, trace through local var assignments
  // e.g., redaxios: var u = "string"!=typeof e?(n=e).url:e → u traces back to param e
  if (innerSinkInfo.urlParamName) {
    var _isUrlParam = false;
    for (var _iup = 0; _iup < innerFuncNode.params.length; _iup++) {
      if (_t.isIdentifier(innerFuncNode.params[_iup]) && innerFuncNode.params[_iup].name === innerSinkInfo.urlParamName)
        _isUrlParam = true;
    }
    if (!_isUrlParam) {
      var _srcParam = _traceLocalVarToParam(innerFuncNode, innerSinkInfo.urlParamName);
      if (_srcParam) {
        console.debug("[AST:trace] local var %s → param %s (url)", innerSinkInfo.urlParamName, _srcParam);
        innerSinkInfo.urlParamName = _srcParam;
      }
    }
  }
  // Same for methodParamName
  if (innerSinkInfo.methodParamName) {
    var _isMethParam = false;
    for (var _imp = 0; _imp < innerFuncNode.params.length; _imp++) {
      if (_t.isIdentifier(innerFuncNode.params[_imp]) && innerFuncNode.params[_imp].name === innerSinkInfo.methodParamName)
        _isMethParam = true;
    }
    if (!_isMethParam) {
      var _srcMethParam = _traceLocalVarToParam(innerFuncNode, innerSinkInfo.methodParamName);
      if (_srcMethParam) {
        console.debug("[AST:trace] local var %s → param %s (method)", innerSinkInfo.methodParamName, _srcMethParam);
        innerSinkInfo.methodParamName = _srcMethParam;
      }
    }
  }

  _stats.interProcTraces++;
  // Map inner function params to resolved values from the chained call
  var innerParamBindings = {};
  for (var ipi = 0; ipi < innerFuncNode.params.length && ipi < resolvedArgs.length; ipi++) {
    var ip = innerFuncNode.params[ipi];
    var ipName = _t.isIdentifier(ip) ? ip.name : null;
    if (!ipName) continue;
    var ra = resolvedArgs[ipi];
    if (ra.fromCaller) {
      var callerArgPath = callPath.get("arguments." + ra.callerArgIdx);
      var callerVals = _resolveAllValues(callerArgPath, 1);
      if (callerVals.length > 0) innerParamBindings[ipName] = callerVals;
    } else if (ra.literal !== null) {
      innerParamBindings[ipName] = [ra.literal];
    }
  }

  // Extract URL, method, body from inner sink using resolved param bindings
  var url = innerSinkInfo.urlLiteral || null;
  var method = innerSinkInfo.method || null;
  if (!url && innerSinkInfo.urlParamName && innerParamBindings[innerSinkInfo.urlParamName])
    url = innerParamBindings[innerSinkInfo.urlParamName];
  if (!method && innerSinkInfo.methodParamName && innerParamBindings[innerSinkInfo.methodParamName])
    method = innerParamBindings[innerSinkInfo.methodParamName];
  // MemberExpression method (e.g., opts.method) — resolve through param bindings
  if (!method && innerSinkInfo.methodMemberExpr) {
    var mmObj = innerSinkInfo.methodMemberExpr.obj;
    var mmProp = innerSinkInfo.methodMemberExpr.prop;
    // If the member base is a param, extract the property from caller's arg
    if (paramMap[mmObj] !== undefined || innerParamBindings[mmObj]) {
      // Resolve from caller's arg object
      for (var rai = 0; rai < resolvedArgs.length; rai++) {
        if (resolvedArgs[rai].fromCaller) {
          var argP = callPath.get("arguments." + resolvedArgs[rai].callerArgIdx);
          var propVals = _resolvePropertyFromArg(argP, mmProp, 1);
          if (propVals.length > 0) { method = propVals; break; }
        }
      }
    }
  }
  // MemberExpression URL — similar
  if (!url && innerSinkInfo.urlMemberExpr) {
    var umObj = innerSinkInfo.urlMemberExpr.obj;
    var umProp = innerSinkInfo.urlMemberExpr.prop;
    if (paramMap[umObj] !== undefined || innerParamBindings[umObj]) {
      for (var rai2 = 0; rai2 < resolvedArgs.length; rai2++) {
        if (resolvedArgs[rai2].fromCaller) {
          var argP2 = callPath.get("arguments." + resolvedArgs[rai2].callerArgIdx);
          var propVals2 = _resolvePropertyFromArg(argP2, umProp, 1);
          if (propVals2.length > 0) { url = propVals2; break; }
        }
      }
    }
  }

  var urls = Array.isArray(url) ? url : (url ? [url] : []);
  var methods = Array.isArray(method) ? method : (method ? [method] : ["?"]);
  methods = methods.filter(function(m) { return typeof m === "string"; }).map(function(m) { return m.toUpperCase(); });
  if (methods.length === 0) methods = ["?"];

  // Extract body params from caller's args mapped through inner params
  var bodyParams = [];
  // Check if inner sink has body info in its headers/params tracking
  if (innerSinkInfo.bodyParamName) {
    // Body is a direct param — extract from caller's corresponding arg
    // Don't require innerParamBindings to be set (ObjectExpression args don't resolve to strings)
    for (var bai = 0; bai < resolvedArgs.length; bai++) {
      var innerPN = innerFuncNode.params[bai];
      if (_t.isIdentifier(innerPN) && innerPN.name === innerSinkInfo.bodyParamName && resolvedArgs[bai].fromCaller) {
        var bArgPath = callPath.get("arguments." + resolvedArgs[bai].callerArgIdx);
        bodyParams = _extractBodyParams(bArgPath.node, bArgPath);
        break;
      }
    }
  }

  for (var ui = 0; ui < urls.length; ui++) {
    if (typeof urls[ui] !== "string") continue;
    for (var mi = 0; mi < methods.length; mi++) {
      _pushFetchSite(result, _buildFetchSite(urls[ui], methods[mi], innerSinkInfo.headers, "fetch", bodyParams));
      console.debug("[AST:fetch] chained %s %s", methods[mi], urls[ui]);
    }
  }
}

function _findSinkInFunction(funcPath) {
  var sinkInfo = null;
  // Walk the function body looking for fetch() or XHR.open()
  // Scope-aware: verify fetch/XMLHttpRequest aren't shadowed by local bindings
  funcPath.traverse(Object.assign({
    CallExpression: function(innerPath) {
      if (sinkInfo) { innerPath.stop(); return; }
      var c = innerPath.node.callee;

      // fetch() / window.fetch() / (s.fetch || fetch)() — only if fetch is the global
      var isFetch = _isGlobalFetchCall(c, innerPath.scope);

      if (isFetch && innerPath.node.arguments.length >= 1) {
        sinkInfo = _extractSinkInfo(innerPath);
        innerPath.stop();
        return;
      }

      // XHR.open(method, url) — verify object traces to XMLHttpRequest
      if (_t.isMemberExpression(c) && _t.isIdentifier(c.property, { name: "open" }) &&
          innerPath.node.arguments.length >= 2 && _isXhrObject(innerPath, c.object)) {
        var xhrM = innerPath.node.arguments[0];
        var xhrMethodStr = null;
        var xhrMethodParam = null;
        if (_t.isStringLiteral(xhrM) && _HTTP_METHODS_LC[xhrM.value.toLowerCase()]) {
          xhrMethodStr = xhrM.value.toUpperCase();
        } else if (_t.isIdentifier(xhrM)) {
          xhrMethodParam = xhrM.name;
        }
        // V2 fix: handle MemberExpression method arg via direct AST node traversal
        // instead of _describeNode() string conversion
        var xhrMethodMember = null;
        if (_t.isMemberExpression(xhrM) && !xhrM.computed &&
            _t.isIdentifier(xhrM.object) && _t.isIdentifier(xhrM.property)) {
          xhrMethodMember = { obj: xhrM.object.name, prop: xhrM.property.name };
          xhrMethodParam = null; // MemberExpression handled directly
        }
        if (xhrMethodStr || xhrMethodParam || xhrMethodMember) {
          var xhrUrlNode = innerPath.node.arguments[1];
          var xhrUrlMember = null;
          if (!_t.isIdentifier(xhrUrlNode) && !_t.isStringLiteral(xhrUrlNode) &&
              _t.isMemberExpression(xhrUrlNode) && !xhrUrlNode.computed) {
            var xuObj = _t.isIdentifier(xhrUrlNode.object) ? xhrUrlNode.object.name : null;
            var xuProp = _t.isIdentifier(xhrUrlNode.property) ? xhrUrlNode.property.name : null;
            if (xuObj && xuProp) xhrUrlMember = { obj: xuObj, prop: xuProp };
          }
          sinkInfo = {
            method: xhrMethodStr,
            methodParamName: xhrMethodParam,
            methodMemberExpr: xhrMethodMember,
            urlParamName: _t.isIdentifier(xhrUrlNode) ? xhrUrlNode.name : null,
            urlLiteral: _t.isStringLiteral(xhrUrlNode) ? xhrUrlNode.value : null,
            urlMemberExpr: xhrUrlMember,
            headers: {},
          };
          // Also locate xhr.send(...) on the SAME xhr binding to capture
          // body-source shape (Identifier or MemberExpression). The
          // wrapper-trace then resolves this through caller args to find
          // the literal body fields. Without this, sinkInfo for XHR
          // carries no body info and `_traceWrapperFunction`'s caller-
          // body-extraction branches never fire.
          if (_t.isIdentifier(c.object)) {
            var sendXhrBinding = innerPath.scope.getBinding(c.object.name);
            if (sendXhrBinding && sendXhrBinding.referencePaths) {
              for (var sri = 0; sri < sendXhrBinding.referencePaths.length; sri++) {
                var sendRef = sendXhrBinding.referencePaths[sri];
                var sendMember = sendRef.parentPath;
                if (!sendMember || !sendMember.isMemberExpression() ||
                    sendMember.node.object !== sendRef.node ||
                    sendMember.node.computed ||
                    !_t.isIdentifier(sendMember.node.property, { name: "send" })) continue;
                var sendCall = sendMember.parentPath;
                if (!sendCall || !sendCall.isCallExpression() ||
                    sendCall.node.callee !== sendMember.node ||
                    sendCall.node.arguments.length === 0) continue;
                var sendArg = sendCall.node.arguments[0];
                if (_t.isIdentifier(sendArg)) {
                  sinkInfo.bodyParamName = sendArg.name;
                } else if (_t.isMemberExpression(sendArg) && !sendArg.computed &&
                           _t.isIdentifier(sendArg.object) && _t.isIdentifier(sendArg.property)) {
                  sinkInfo.bodyMemberExpr = { obj: sendArg.object.name, prop: sendArg.property.name };
                }
                break;
              }
            }
          }
          innerPath.stop();
        }
      }
    },
  }, _SKIP_NESTED_FUNCS));
  return sinkInfo;
}

// ─── Lightweight Type Tracker ────────────────────────────────────────────────
// Tracks deterministic types from unambiguous patterns (new expressions, array literals).
// Keyed by scopeUid:varName so shadowed variables don't inherit outer types.

var _TYPED_CONSTRUCTORS = {
  "XMLHttpRequest": "XMLHttpRequest", "WebSocket": "WebSocket", "EventSource": "EventSource",
  "URL": "URL", "URLSearchParams": "URLSearchParams", "DOMParser": "DOMParser",
  "BroadcastChannel": "BroadcastChannel", "Worker": "Worker", "SharedWorker": "SharedWorker",
  "Headers": "Headers", "Request": "Request", "Response": "Response",
  "FormData": "FormData", "Blob": "Blob", "File": "File",
  "ReadableStream": "ReadableStream", "WritableStream": "WritableStream",
  "AbortController": "AbortController", "MutationObserver": "MutationObserver",
  "IntersectionObserver": "IntersectionObserver", "ResizeObserver": "ResizeObserver",
  // Collections. Distinct from URLSearchParams/Headers — their stored
  // values are independent of the keys, so `.get(taintedKey)` doesn't
  // return attacker-content unless `.set(taintedKey, attackerValue)`
  // also happened. Used by the method-call dim projection to skip
  // key-origin upgrade when the receiver is a Map/Set/Weak*.
  "Map": "Map", "Set": "Set", "WeakMap": "WeakMap", "WeakSet": "WeakSet",
};

function _setType(scope, name, type) {
  _typeEnv[scope.uid + ":" + name] = type;
}

function _getType(scope, name) {
  return _typeEnv[scope.uid + ":" + name] || null;
}

// Resolve the tracked type for a node. For Identifiers, looks up binding scope.
// For NewExpressions/ArrayExpressions, returns the type directly.
//
// For Identifiers with no pre-recorded type (e.g. `let url` without init
// that gets assigned later in each branch of an if/else), fall back to
// inspecting the binding's init + constantViolations. Only infers a type
// when EVERY assignment produces the same `_TYPED_CONSTRUCTORS` match —
// conservative so branch-divergent types (`x = new URL(); x = someString`)
// stay untyped. Pattern seen in github's approvedHandler:
//   let url
//   if (token) url = new URL(path, origin); else url = new URL('', href);
//   location.assign(url);
function _getTrackedType(path, node) {
  if (_t.isIdentifier(node)) {
    var binding = path.scope.getBinding(node.name);
    if (!binding) return null;
    var stored = _getType(binding.scope, node.name);
    if (stored) return stored;
    return _inferTypeFromAssignments(binding);
  }
  if (_t.isNewExpression(node) && _t.isIdentifier(node.callee) && !path.scope.getBinding(node.callee.name)) {
    return _TYPED_CONSTRUCTORS[node.callee.name] || null;
  }
  if (_t.isArrayExpression(node)) return "Array";
  return null;
}

function _inferTypeFromAssignments(binding) {
  if (!binding) return null;
  // Cache the inferred type on the binding itself so repeat
  // `_getTrackedType(path, x)` calls don't re-walk the same
  // constantViolations list. Heavy minified bundles reassign a
  // variable dozens of times; without this cache, every receiver-
  // type check on x pays O(reassignments × violation-depth) scope
  // walks — enough to stall the service worker on a 5MB bundle.
  // Sentinel __INFER_NONE__ distinguishes "cached: not inferable"
  // from "not yet computed" (cachedType === undefined).
  if (binding.__inferredType !== undefined) {
    return binding.__inferredType === "__INFER_NONE__" ? null : binding.__inferredType;
  }
  var inferred = null;
  // Recursive expr→type resolver: handles NewExpression, ArrayExpression,
  // ConditionalExpression branches (minifiers turn `if/else` reassign
  // blocks into `x = cond ? A : B`), and LogicalExpression branches.
  var _typeOfExpr = function (expr) {
    if (!expr) return null;
    if (_t.isNewExpression(expr) && _t.isIdentifier(expr.callee)) {
      return _TYPED_CONSTRUCTORS[expr.callee.name] || null;
    }
    if (_t.isArrayExpression(expr)) return "Array";
    if (_t.isConditionalExpression(expr)) {
      var tc = _typeOfExpr(expr.consequent);
      var ta = _typeOfExpr(expr.alternate);
      if (tc && tc === ta) return tc;
      return null;
    }
    if (_t.isLogicalExpression(expr)) {
      var tl = _typeOfExpr(expr.left);
      var tr = _typeOfExpr(expr.right);
      if (tl && tl === tr) return tl;
      return null;
    }
    if (_t.isAssignmentExpression(expr) && expr.operator === "=") {
      return _typeOfExpr(expr.right);
    }
    return null;
  };
  var _cacheResult = function (val) {
    binding.__inferredType = val == null ? "__INFER_NONE__" : val;
    return val;
  };
  // VariableDeclarator's own init is one possible source.
  if (binding.path && binding.path.isVariableDeclarator() && binding.path.node.init) {
    var initType = _typeOfExpr(binding.path.node.init);
    if (!initType) return _cacheResult(null);
    inferred = initType;
  }
  // Function parameter — infer type from caller args. All callers must
  // pass values whose type matches; otherwise inference is ambiguous
  // and we return null. Common case: `function go(u) { fetch(u.pathname) }`
  // called as `go(new URL("/api", base))` — u's type becomes "URL".
  if (binding.kind === "param" && binding.path && binding.path.node && binding.scope) {
    var hostFn = binding.scope.path;
    if (hostFn && hostFn.node && hostFn.node.params) {
      var paramName = binding.identifier ? binding.identifier.name : null;
      if (paramName) {
        var pIdxT = -1;
        for (var pii = 0; pii < hostFn.node.params.length; pii++) {
          if (_t.isIdentifier(hostFn.node.params[pii], { name: paramName })) { pIdxT = pii; break; }
        }
        if (pIdxT >= 0) {
          var callerArgsT = _findFunctionCallerArgs(hostFn);
          var paramInferred = inferred;
          var sawAny = false;
          for (var caT = 0; caT < callerArgsT.length; caT++) {
            if (pIdxT >= callerArgsT[caT].length) continue;
            var argP = callerArgsT[caT][pIdxT];
            if (!argP || !argP.node) continue;
            var argT = _typeOfExpr(argP.node);
            if (!argT) { paramInferred = null; break; }
            if (paramInferred && paramInferred !== argT) { paramInferred = null; break; }
            paramInferred = argT;
            sawAny = true;
          }
          if (sawAny && paramInferred) inferred = paramInferred;
        }
      }
    }
  }
  var viols = binding.constantViolations || [];
  for (var vi = 0; vi < viols.length; vi++) {
    var v = viols[vi];
    if (!v || !v.isAssignmentExpression || !v.isAssignmentExpression()) return _cacheResult(null);
    if (v.node.operator !== "=") return _cacheResult(null);
    var rhsType = _typeOfExpr(v.node.right);
    if (!rhsType) return _cacheResult(null);
    if (inferred && inferred !== rhsType) return _cacheResult(null);
    inferred = rhsType;
  }
  return _cacheResult(inferred);
}

// Populate type from a VariableDeclarator: var x = new XMLHttpRequest() → type "XMLHttpRequest"
// Register a function (named or anonymous) in the structural-def maps.
// Returns the entry added to _allFuncRanges so enter/exit visitors can
// push it onto the function stack for call-graph attribution. Anon
// functions are still range-tracked (finding.callers needs them) but
// don't appear in the named defMap/funcMap.
function _registerFunc(path, name, funcNode) {
  var loc = funcNode ? funcNode.loc : path.node.loc;
  if (!loc) return null;
  var entry = { line: loc.start.line, endLine: loc.end.line, calls: new Set() };
  _allFuncRanges.push(entry);
  if (name) {
    _funcMap[name] = entry;
    _defMap[name] = loc.start.line;
  }
  return entry;
}

// Record a class method as a property of the class binding, so viewer
// click-to-definition on `new Cls(...).method` can jump to the method.
function _registerClassMethodProp(path, methodName) {
  if (!methodName) return;
  var classBody = path.parentPath;
  var classNode = classBody ? classBody.parentPath : null;
  if (!classNode || !classNode.node.id || !_t.isIdentifier(classNode.node.id)) return;
  var cbinding = classNode.scope.getBinding(classNode.node.id.name);
  if (!cbinding || !cbinding.identifier || !cbinding.identifier.loc) return;
  var ckey = cbinding.identifier.loc.start.line + ":" + classNode.node.id.name;
  if (!_propDefs[ckey]) _propDefs[ckey] = {};
  _propDefs[ckey][methodName] = path.node.loc ? path.node.loc.start.line : 0;
}

// Extract property-definition records from a VariableDeclarator whose
// init is an ObjectExpression. Captures `var obj = { foo: fn, bar: 42 }`
// so clicking `obj.foo` in the viewer jumps to the `foo:` line.
function _collectPropDefsFromVarDecl(path) {
  var node = path.node;
  if (!_t.isIdentifier(node.id) || !_t.isObjectExpression(node.init)) return;
  var props = node.init.properties;
  if (!props.length) return;
  var ownerBinding = path.scope.getBinding(node.id.name);
  if (!ownerBinding || !ownerBinding.identifier || !ownerBinding.identifier.loc) return;
  var okey = ownerBinding.identifier.loc.start.line + ":" + node.id.name;
  if (!_propDefs[okey]) _propDefs[okey] = {};
  for (var pi = 0; pi < props.length; pi++) {
    var p = props[pi];
    if (p.computed || !p.key) continue;
    var pname = _t.isIdentifier(p.key) ? p.key.name : (_t.isStringLiteral(p.key) ? p.key.value : null);
    if (pname && p.key.loc) _propDefs[okey][pname] = p.key.loc.start.line;
  }
}

// Extract property-definition records from an AssignmentExpression.
// Handles three shapes:
//   obj.prop = value           — direct prop assignment
//   Cls.prototype.method = fn  — prototype chain
//   this.prop = value          — attributed to enclosing class/fn
//   obj = { prop: value }      — whole-object replacement
function _collectPropDefsFromAssignment(path) {
  var left = path.node.left;
  if (!left) return;
  // this.prop = value inside a class method / constructor function
  if (_t.isMemberExpression(left) && !left.computed &&
      _t.isThisExpression(left.object) && _t.isIdentifier(left.property)) {
    _collectThisPropAssignment(path, left);
  }
  // obj = { prop: value } — identifier LHS with object literal RHS
  if (_t.isIdentifier(left) && _t.isObjectExpression(path.node.right)) {
    var oprops = path.node.right.properties;
    if (oprops.length) {
      var olbinding = path.scope.getBinding(left.name);
      if (olbinding && olbinding.identifier && olbinding.identifier.loc) {
        var olkey = olbinding.identifier.loc.start.line + ":" + left.name;
        if (!_propDefs[olkey]) _propDefs[olkey] = {};
        for (var oi = 0; oi < oprops.length; oi++) {
          var op = oprops[oi];
          if (op.computed || !op.key) continue;
          var opname = _t.isIdentifier(op.key) ? op.key.name : (_t.isStringLiteral(op.key) ? op.key.value : null);
          if (opname && op.key.loc) _propDefs[olkey][opname] = op.key.loc.start.line;
        }
      }
    }
  }
  if (!_t.isMemberExpression(left) || left.computed) return;
  if (!_t.isIdentifier(left.property)) return;
  var propName = left.property.name;
  var propLine = left.property.loc ? left.property.loc.start.line : 0;
  if (!propLine) return;
  if (_t.isIdentifier(left.object)) {
    var abinding = path.scope.getBinding(left.object.name);
    if (abinding && abinding.identifier && abinding.identifier.loc) {
      var akey = abinding.identifier.loc.start.line + ":" + left.object.name;
      if (!_propDefs[akey]) _propDefs[akey] = {};
      _propDefs[akey][propName] = propLine;
    }
  } else if (_t.isMemberExpression(left.object) && !left.object.computed &&
             _t.isIdentifier(left.object.property, { name: "prototype" }) &&
             _t.isIdentifier(left.object.object)) {
    var pbinding = path.scope.getBinding(left.object.object.name);
    if (pbinding && pbinding.identifier && pbinding.identifier.loc) {
      var pkey = pbinding.identifier.loc.start.line + ":" + left.object.object.name;
      if (!_propDefs[pkey]) _propDefs[pkey] = {};
      _propDefs[pkey][propName] = propLine;
    }
  }
}

// Attribute `this.prop = value` to the enclosing class (for class methods)
// or the enclosing named function (for ES5-style `function Foo() { this.x=… }`
// constructors). Matches the buildDefinitionMap legacy behaviour exactly.
function _collectThisPropAssignment(path, left) {
  var thisPropName = left.property.name;
  var thisPropLine = left.property.loc ? left.property.loc.start.line : 0;
  if (!thisPropLine) return;
  var enclosing = path.getFunctionParent();
  if (!enclosing) return;
  var classOrFunc = null;
  if (enclosing.node.type === "ClassMethod" && enclosing.node.kind === "constructor") {
    var cbody = enclosing.parentPath;
    classOrFunc = cbody ? cbody.parentPath : null;
  }
  if (!classOrFunc) {
    if (enclosing.node.type === "FunctionDeclaration" && enclosing.node.id) {
      classOrFunc = enclosing;
    } else if (enclosing.node.type === "FunctionExpression") {
      if (enclosing.parent.type === "VariableDeclarator" && _t.isIdentifier(enclosing.parent.id)) {
        classOrFunc = enclosing.parentPath.parentPath ? enclosing.parentPath : enclosing;
      } else if (enclosing.parent.type === "AssignmentExpression" && _t.isIdentifier(enclosing.parent.left)) {
        classOrFunc = enclosing.parentPath;
      }
    }
  }
  if (!classOrFunc) return;
  var ctorName = null;
  var ctorNode = classOrFunc.node;
  if (ctorNode.type === "ClassDeclaration" || ctorNode.type === "ClassExpression") {
    if (ctorNode.id && _t.isIdentifier(ctorNode.id)) ctorName = ctorNode.id.name;
    else if (classOrFunc.parent.type === "VariableDeclarator" && _t.isIdentifier(classOrFunc.parent.id)) ctorName = classOrFunc.parent.id.name;
    else if (classOrFunc.parent.type === "AssignmentExpression" && _t.isIdentifier(classOrFunc.parent.left)) ctorName = classOrFunc.parent.left.name;
  } else if (ctorNode.type === "FunctionDeclaration" && ctorNode.id) {
    ctorName = ctorNode.id.name;
  } else if (ctorNode.type === "VariableDeclaration") {
    var decl = ctorNode.declarations && ctorNode.declarations[0];
    if (decl && _t.isIdentifier(decl.id)) ctorName = decl.id.name;
  } else if (ctorNode.type === "AssignmentExpression" && _t.isIdentifier(ctorNode.left)) {
    ctorName = ctorNode.left.name;
  }
  if (!ctorName) return;
  var ctorBinding = path.scope.getBinding(ctorName);
  if (!ctorBinding || !ctorBinding.identifier || !ctorBinding.identifier.loc) return;
  var ctorKey = ctorBinding.identifier.loc.start.line + ":" + ctorName;
  if (!_propDefs[ctorKey]) _propDefs[ctorKey] = {};
  if (!_propDefs[ctorKey][thisPropName]) _propDefs[ctorKey][thisPropName] = thisPropLine;
}

// Attribute a CallExpression to the innermost enclosing function on the
// visitor stack (so finding.callers can show "this function calls X").
// Scope-aware filter preserves the old behaviour: parameters named like
// functions don't count — only real function-valued bindings.
function _trackCallForCallGraph(inner) {
  if (!_funcStack.length) return;
  var top = _funcStack[_funcStack.length - 1];
  if (!top) return;
  var callee = inner.node.callee;
  if (_t.isIdentifier(callee)) {
    var binding = inner.scope.getBinding(callee.name);
    if (!binding || _t.isFunctionDeclaration(binding.path.node) ||
        (binding.path.isVariableDeclarator() && binding.path.node.init &&
         _t.isFunction(binding.path.node.init)) ||
        (binding.path.isAssignmentExpression && binding.constantViolations &&
         binding.constantViolations.some(function(cv) { return cv.isAssignmentExpression() && _t.isFunction(cv.node.right); })))
      top.calls.add(callee.name);
  } else if (_t.isMemberExpression(callee) && _t.isIdentifier(callee.property)) {
    top.calls.add(callee.property.name);
  }
}

// Record a pending property-access for later resolution against _propDefs.
// Queued during the pre-pass; flushed after the pre-pass completes so
// every propDef (including those from later assignments) is visible.
function _trackMemberPropAccess(path) {
  if (path.node.computed) return;
  var prop = path.node.property;
  if (!prop || !_t.isIdentifier(prop) || !prop.loc) return;
  var obj = path.node.object;
  if (!_t.isIdentifier(obj)) return;
  var mbinding = path.scope.getBinding(obj.name);
  if (!mbinding || !mbinding.identifier || !mbinding.identifier.loc) return;
  var ownerKey = mbinding.identifier.loc.start.line + ":" + obj.name;
  _pendingProps.push({ refLine: prop.loc.start.line, propName: prop.name, ownerKey: ownerKey, binding: mbinding });
}

function _trackTypeFromDeclarator(path) {
  var node = path.node;
  if (!_t.isIdentifier(node.id) || !node.init) return;
  var name = node.id.name;
  var init = node.init;
  // new Constructor() → typed constructor
  if (_t.isNewExpression(init) && _t.isIdentifier(init.callee) && !path.scope.getBinding(init.callee.name)) {
    var ctorType = _TYPED_CONSTRUCTORS[init.callee.name];
    if (ctorType) { _setType(path.scope, name, ctorType); return; }
  }
  // ArrayExpression: [...]
  if (_t.isArrayExpression(init)) { _setType(path.scope, name, "Array"); return; }
  // Array.from(x), Array.of(...), Object.keys(x), Object.values(x), Object.entries(x)
  if (_t.isCallExpression(init) && _t.isMemberExpression(init.callee) && !init.callee.computed &&
      _t.isIdentifier(init.callee.object) && _t.isIdentifier(init.callee.property)) {
    var obj = init.callee.object.name;
    var meth = init.callee.property.name;
    if ((obj === "Array" && (meth === "from" || meth === "of")) ||
        (obj === "Object" && (meth === "keys" || meth === "values" || meth === "entries"))) {
      if (!path.scope.getBinding(obj)) { _setType(path.scope, name, "Array"); return; }
    }
  }
  // .split(), .slice(), .filter(), .map(), .concat() on strings/arrays → Array
  if (_t.isCallExpression(init) && _t.isMemberExpression(init.callee) && !init.callee.computed &&
      _t.isIdentifier(init.callee.property)) {
    var arrayMethods = { "split":1, "slice":1, "filter":1, "map":1, "concat":1, "flat":1, "flatMap":1, "reverse":1, "sort":1 };
    if (arrayMethods[init.callee.property.name]) {
      _setType(path.scope, name, "Array");
      return;
    }
  }
  // document.createElement(tag) → Element, document.getElementById(id) → Element
  if (_t.isCallExpression(init) && _t.isMemberExpression(init.callee) && !init.callee.computed &&
      _t.isIdentifier(init.callee.object, { name: "document" }) && _t.isIdentifier(init.callee.property) &&
      !path.scope.getBinding("document")) {
    var docMeth = init.callee.property.name;
    if (docMeth === "createElement" || docMeth === "getElementById" || docMeth === "querySelector" ||
        docMeth === "getElementsByTagName" || docMeth === "getElementsByClassName") {
      _setType(path.scope, name, "Element");
    }
  }
}

// List of Array iteration methods — property names that survive minification
var _ITERATION_METHODS = {
  "forEach":1, "map":1, "filter":1, "some":1, "every":1,
  "find":1, "findIndex":1, "flatMap":1, "reduce":1, "reduceRight":1,
};

// Types known to be non-iterable (no .forEach/.map etc.)
var _NON_ITERABLE_TYPES = {
  "XMLHttpRequest":1, "WebSocket":1, "EventSource":1, "Element":1,
  "DOMParser":1, "BroadcastChannel":1, "Worker":1, "SharedWorker":1,
  "AbortController":1, "MutationObserver":1, "Headers":1, "Request":1,
  "Response":1, "Blob":1, "File":1,
};

// Deep sink check: does a function eventually reach a network sink through any code path?
// Unlike _findSinkInFunction, this traverses into ALL nested functions and only returns true/false.
// Used to identify high-level API functions (like jQuery.ajax) as network sinks.
function _containsNetworkSink(funcPath) {
  var found = false;
  // Scope-aware traversal: verify identifiers aren't shadowed by local bindings
  funcPath.traverse({
    CallExpression: function(innerPath) {
      if (found) { innerPath.stop(); return; }
      var c = innerPath.node.callee;
      // fetch() / window.fetch() / (s.fetch || fetch)() — only if fetch is the global
      if (_isGlobalFetchCall(c, innerPath.scope)) { found = true; innerPath.stop(); return; }
      // .open() — only a network sink if object traces to XMLHttpRequest
      if (_t.isMemberExpression(c) && _t.isIdentifier(c.property, { name: "open" }) &&
          innerPath.node.arguments.length >= 2 && _isXhrObject(innerPath, c.object)) {
        found = true; innerPath.stop(); return;
      }
    },
    NewExpression: function(innerPath) {
      if (found) { innerPath.stop(); return; }
      if (_t.isIdentifier(innerPath.node.callee, { name: "XMLHttpRequest" }) &&
          !innerPath.scope.getBinding("XMLHttpRequest")) {
        found = true; innerPath.stop();
      }
    },
    // DO search into nested functions (unlike _findSinkInFunction)
  });
  return found;
}

// Find the property names used at deep network sinks (traversing into nested functions).
// Unlike _findSinkInFunction (which skips nested functions and returns a param-name-based sinkInfo),
// this searches INTO closures/callbacks and extracts the PROPERTY NAMES used at the sink.
// E.g., xhr.open(opts.type, opts.url) → { urlProps: ["url"], methodProps: ["type"] }
// These property names can then be matched against the caller's object arguments.
function _findDeepSinkPropertyMap(funcPath) {
  var propMap = null;
  // Scope-aware: verify fetch/XMLHttpRequest aren't shadowed by local bindings
  funcPath.traverse({
    CallExpression: function(innerPath) {
      if (propMap) { innerPath.stop(); return; }
      var c = innerPath.node.callee;

      // fetch(url, opts) or window.fetch(url, opts) — only if not shadowed
      // fetch() / window.fetch() / (s.fetch || fetch)() — only if fetch is the global
      if (_isGlobalFetchCall(c, innerPath.scope) && innerPath.node.arguments.length >= 1) {
        propMap = _extractSinkPropertyMap(innerPath, "fetch");
        if (propMap && propMap.urlProps.length === 0 && !propMap.urlLiteral) propMap = null;
        if (propMap) innerPath.stop();
        return;
      }

      // xhr.open(method, url) — verify object traces to XMLHttpRequest
      if (_t.isMemberExpression(c) && _t.isIdentifier(c.property, { name: "open" }) &&
          innerPath.node.arguments.length >= 2 && _isXhrObject(innerPath, c.object)) {
        propMap = _extractSinkPropertyMap(innerPath, "xhr");
        if (propMap && propMap.urlProps.length === 0 && !propMap.urlLiteral) propMap = null;
        if (propMap) innerPath.stop();
        return;
      }
    },
    // DO search into nested functions — deep sinks are inside closures/callbacks
  });
  return propMap;
}

// Extract property names from a specific network sink's arguments.
// For xhr.open(method, url): method position → methodProps, url position → urlProps.
// For fetch(url, {method: M}): url position → urlProps, method from options → methodProps.
function _extractSinkPropertyMap(sinkPath, sinkType) {
  var map = {
    type: sinkType,
    urlProps: [],
    methodProps: [],
    methodLiteral: null,
    urlLiteral: null,
  };

  if (sinkType === "xhr") {
    // xhr.open(method, url)
    var methodArg = sinkPath.node.arguments[0];
    var urlArg = sinkPath.node.arguments[1];

    if (_t.isStringLiteral(methodArg) && _HTTP_METHODS_LC[methodArg.value.toLowerCase()]) {
      map.methodLiteral = methodArg.value.toUpperCase();
    } else {
      _collectMemberProps(methodArg, map.methodProps);
    }

    if (_t.isStringLiteral(urlArg)) {
      map.urlLiteral = urlArg.value;
    } else {
      _collectMemberProps(urlArg, map.urlProps);
    }
  } else {
    // fetch(url, opts)
    var fetchUrlArg = sinkPath.node.arguments[0];

    if (_t.isStringLiteral(fetchUrlArg)) {
      map.urlLiteral = fetchUrlArg.value;
    } else {
      _collectMemberProps(fetchUrlArg, map.urlProps);
    }

    // Look for method in options object (second arg)
    if (sinkPath.node.arguments.length >= 2) {
      var optsArg = sinkPath.node.arguments[1];
      if (_t.isObjectExpression(optsArg)) {
        for (var i = 0; i < optsArg.properties.length; i++) {
          var prop = optsArg.properties[i];
          if (!_t.isObjectProperty(prop) || prop.computed) continue;
          var key = _t.isIdentifier(prop.key) ? prop.key.name : (_t.isStringLiteral(prop.key) ? prop.key.value : null);
          if (key === "method") {
            if (_t.isStringLiteral(prop.value) && _HTTP_METHODS_LC[prop.value.value.toLowerCase()]) {
              map.methodLiteral = prop.value.value.toUpperCase();
            } else {
              _collectMemberProps(prop.value, map.methodProps);
            }
          }
        }
      } else if (_t.isIdentifier(optsArg) || _t.isMemberExpression(optsArg)) {
        // Options is a variable — record `opts.method` access pattern in
        // map.methodProps so the inter-procedural resolver can match it
        // against caller-side object literals and resolve the method
        // value once a real argument is bound.
        _collectMemberProps(optsArg, map.methodProps);
      }
    }
  }

  return map;
}

// Collect terminal property names from MemberExpression chains.
// E.g., options.url → ["url"], options.type → ["type"]
// Also handles BinaryExpression (string concat): options.url + path → ["url"]
function _collectMemberProps(node, out) {
  // Iterative: walk BinaryExpression(+) chains via explicit stack
  var stack = [node];
  while (stack.length > 0) {
    var n = stack.pop();
    if (_t.isMemberExpression(n) && !n.computed && _t.isIdentifier(n.property)) {
      out.push(n.property.name);
    }
    // BinaryExpression: options.url + "/path" → still extract "url"
    if (_t.isBinaryExpression(n) && n.operator === "+") {
      stack.push(n.left, n.right);
    }
  }
}

function _extractSinkInfo(fetchPath) {
  var args = fetchPath.node.arguments;
  var urlNode = args[0];
  var info = {
    urlParamName: _t.isIdentifier(urlNode) ? urlNode.name : null,
    urlLiteral: _t.isStringLiteral(urlNode) ? urlNode.value : null,
    // MemberExpression URL: fetch(opts.url) → {obj: "opts", prop: "url"}
    urlMemberExpr: null,
    method: null,
    methodParamName: null,
    // MemberExpression method: fetch(url, {method: opts.method})
    methodMemberExpr: null,
    headers: {},
    params: undefined,
  };
  // Capture MemberExpression URL argument (opts.url, config.endpoint, etc.)
  if (!info.urlParamName && !info.urlLiteral && _t.isMemberExpression(urlNode) && !urlNode.computed) {
    var urlObj = _t.isIdentifier(urlNode.object) ? urlNode.object.name : null;
    var urlProp = _t.isIdentifier(urlNode.property) ? urlNode.property.name : null;
    if (urlObj && urlProp) info.urlMemberExpr = { obj: urlObj, prop: urlProp };
  }

  // Extract from options object
  if (args[1] && _t.isObjectExpression(args[1])) {
    var opts = args[1].properties;
    for (var i = 0; i < opts.length; i++) {
      if (!_t.isObjectProperty(opts[i]) || opts[i].computed) continue;
      var key = _getKeyName(opts[i].key);
      var val = opts[i].value;

      if (key === "method") {
        if (_t.isStringLiteral(val)) info.method = val.value.toUpperCase();
        else if (_t.isIdentifier(val)) info.methodParamName = val.name;
        else if (_t.isMemberExpression(val) && !val.computed) {
          var mObj = _t.isIdentifier(val.object) ? val.object.name : null;
          var mProp = _t.isIdentifier(val.property) ? val.property.name : null;
          if (mObj && mProp) info.methodMemberExpr = { obj: mObj, prop: mProp };
        }
        // Unwrap .toUpperCase()/.toLowerCase() and LogicalExpression chains
        // e.g., (a||s.method||"get").toUpperCase() → extract param name "a"
        if (!info.method && !info.methodParamName && !info.methodMemberExpr) {
          var _mVal = val;
          if (_t.isCallExpression(_mVal) && _t.isMemberExpression(_mVal.callee) &&
              _t.isIdentifier(_mVal.callee.property) &&
              (_mVal.callee.property.name === "toUpperCase" || _mVal.callee.property.name === "toLowerCase")) {
            _mVal = _mVal.callee.object;
          }
          if (_t.isIdentifier(_mVal)) {
            info.methodParamName = _mVal.name;
          } else if (_t.isLogicalExpression(_mVal)) {
            // Walk left-first: (a || b || c) is parsed as ((a || b) || c)
            var _cur = _mVal;
            while (_cur) {
              if (_t.isIdentifier(_cur)) { info.methodParamName = _cur.name; break; }
              if (_t.isLogicalExpression(_cur)) {
                if (_t.isIdentifier(_cur.left)) { info.methodParamName = _cur.left.name; break; }
                _cur = _t.isLogicalExpression(_cur.left) ? _cur.left : _cur.right;
              } else break;
            }
          }
        }
      }
      if (key === "headers" && _t.isObjectExpression(val)) {
        info.headers = _extractHeaders(val);
        info.headersNode = val;  // Store raw node for scope-aware resolution later
      }
      // `headers: new Headers({...})` — W3C Headers constructor wraps the
      // same object-literal shape. Unwrap to the inner object for header
      // extraction. `new Headers(arrayOfPairs)` form not extracted (rarely
      // used; would need a separate pair-iteration path).
      if (key === "headers" && _t.isNewExpression(val) &&
          _t.isIdentifier(val.callee, { name: "Headers" }) &&
          !fetchPath.scope.getBinding("Headers") &&
          val.arguments.length >= 1 && _t.isObjectExpression(val.arguments[0])) {
        info.headers = _extractHeaders(val.arguments[0]);
        info.headersNode = val.arguments[0];
      }
      if (key === "body") {
        info.params = _extractBodyParams(val);
        // Track body source so _traceWrapperFunction can resolve through caller args
        var bodyValNode = val;
        bodyValNode = _unwrapJsonStringify(val, fetchPath);
        if (_t.isIdentifier(bodyValNode)) info.bodyParamName = bodyValNode.name;
        else if (_t.isMemberExpression(bodyValNode) && !bodyValNode.computed) {
          var bObj = _t.isIdentifier(bodyValNode.object) ? bodyValNode.object.name : null;
          var bProp = _t.isIdentifier(bodyValNode.property) ? bodyValNode.property.name : null;
          if (bObj && bProp) info.bodyMemberExpr = { obj: bObj, prop: bProp };
        }
      }
    }
  }
  return info;
}

// Record a resolver gap on the fetch URL argument. Cheaper than
// re-walking to pinpoint the exact leaf — just labels the argument's
// shape (Identifier name / `.prop` / `call()` / node type) and its
// location. Reviewer reads the `fetch(` at that position to see
// which identifier didn't trace.
function _describeMemberChain(node) {
  // Walk the MemberExpression chain left-to-right and emit `root.a.b.c`.
  // For identifier roots we emit the name; for `this`/`super` we emit
  // the keyword; for anything else (CallExpression root, etc.) we emit
  // that node's type so the reviewer at least sees the shape.
  // OptionalMemberExpression (`a?.b`) is semantically identical to
  // MemberExpression for chain description — handle both so gap
  // descriptors don't fall back to the opaque type name.
  var parts = [];
  var cur = node;
  while ((_t.isMemberExpression(cur) || _t.isOptionalMemberExpression(cur)) && !cur.computed) {
    var propName = _t.isIdentifier(cur.property) ? cur.property.name :
      (_t.isStringLiteral(cur.property) ? cur.property.value : null);
    if (!propName) return null;
    parts.unshift(propName);
    cur = cur.object;
  }
  if (_t.isIdentifier(cur)) parts.unshift(cur.name);
  else if (_t.isThisExpression(cur)) parts.unshift("this");
  else if (_t.isSuper(cur)) parts.unshift("super");
  else if (_t.isCallExpression(cur) && _t.isIdentifier(cur.callee)) parts.unshift(cur.callee.name + "()");
  else parts.unshift("<" + cur.type + ">");
  return parts.join(".");
}
// Follow `Identifier → its init` and `new Request(X, …) → X` transparently
// so the gap descriptor points at the actual unresolvable leaf instead
// of the outer reference. Without this, `fetch(c)` where
// `c = new Request(new URL(dom.attr, origin).toString(), init)` reports
// the gap at `c` — the reviewer has to chase back through two bindings
// to find the real DOM-runtime leaf.
// Iteratively unwrap a path to its semantic "root cause" leaf for gap
// reporting. The original recursive form was a series of tail-recursive
// branches — each unwrapping one level. Iterative loop preserves
// identical semantics with O(1) JS call-stack depth regardless of
// unwrap chain length. Cycle-safe via visited Set keyed on node identity.
function _unwrapGapToRootCause(argPath, visited) {
  if (!argPath || !argPath.node) return argPath;
  if (!visited) visited = new Set();
  while (true) {
    if (!argPath || !argPath.node) return argPath;
    if (visited.has(argPath.node)) return argPath;
    visited.add(argPath.node);
    var n = argPath.node;
    // Identifier → var init (only if constant-init binding)
    if (_t.isIdentifier(n)) {
      var binding = argPath.scope.getBinding(n.name);
      if (binding && binding.constant && _t.isVariableDeclarator(binding.path.node) && binding.path.node.init) {
        argPath = binding.path.get("init");
        continue;
      }
      return argPath;
    }
    // new Request(URL, init) — URL arg is what matters
    if (_t.isNewExpression(n) && _t.isIdentifier(n.callee, { name: "Request" }) &&
        !argPath.scope.getBinding("Request") && n.arguments.length >= 1) {
      argPath = argPath.get("arguments.0");
      continue;
    }
    // x.toString() / x.valueOf() — receiver is what matters
    if (_t.isCallExpression(n) && _t.isMemberExpression(n.callee) && !n.callee.computed &&
        _t.isIdentifier(n.callee.property) &&
        (n.callee.property.name === "toString" || n.callee.property.name === "valueOf")) {
      argPath = argPath.get("callee.object");
      continue;
    }
    // IIFE / arrow IIFE — first return's argument is the root cause.
    if (_t.isCallExpression(n) &&
        (_t.isFunctionExpression(n.callee) || _t.isArrowFunctionExpression(n.callee))) {
      var fn = n.callee;
      if (_t.isArrowFunctionExpression(fn) && !_t.isBlockStatement(fn.body)) {
        argPath = argPath.get("callee.body");
        continue;
      }
      if (_t.isBlockStatement(fn.body)) {
        var unwrapped = false;
        for (var bi = 0; bi < fn.body.body.length; bi++) {
          if (_t.isReturnStatement(fn.body.body[bi]) && fn.body.body[bi].argument) {
            argPath = argPath.get("callee.body.body." + bi + ".argument");
            unwrapped = true;
            break;
          }
        }
        if (unwrapped) continue;
      }
    }
    // SequenceExpression — last element only.
    if (_t.isSequenceExpression(n) && n.expressions.length > 0) {
      var lastIdx = n.expressions.length - 1;
      argPath = argPath.get("expressions." + lastIdx);
      continue;
    }
    // ConditionalExpression — unwrap to consequent (concrete leaf).
    if (_t.isConditionalExpression(n)) {
      argPath = argPath.get("consequent");
      continue;
    }
    // TemplateLiteral with interpolations — unwrap to first interpolation.
    if (_t.isTemplateLiteral(n) && n.expressions.length > 0) {
      argPath = argPath.get("expressions.0");
      continue;
    }
    // BinaryExpression `+` — unwrap to left.
    if (_t.isBinaryExpression(n) && n.operator === "+") {
      argPath = argPath.get("left");
      continue;
    }
    // LogicalExpression — unwrap to left.
    if (_t.isLogicalExpression(n)) {
      argPath = argPath.get("left");
      continue;
    }
    // UnaryExpression — unwrap to argument.
    if (_t.isUnaryExpression(n)) {
      argPath = argPath.get("argument");
      continue;
    }
    // Await/Yield — unwrap to argument.
    if (_t.isAwaitExpression(n) || _t.isYieldExpression(n)) {
      if (n.argument) {
        argPath = argPath.get("argument");
        continue;
      }
    }
    return argPath;
  }
}
function _recordUrlResolveGap(argPath) {
  if (!argPath || !argPath.node) return;
  // Preserve the ORIGINAL fetch-arg location — audit.sinks cross-
  // references gaps by (script, line) and needs the sink-call site,
  // not the root cause's location. Unwrap the node for description
  // quality, but keep the original loc.
  var origLoc = argPath.node.loc;
  argPath = _unwrapGapToRootCause(argPath);
  var n = argPath.node;
  var desc = null;
  if (_t.isIdentifier(n)) desc = n.name;
  else if (_t.isMemberExpression(n) || _t.isOptionalMemberExpression(n)) desc = _describeMemberChain(n) || n.type;
  else if (_t.isCallExpression(n)) {
    if (_t.isIdentifier(n.callee)) desc = n.callee.name + "()";
    else if (_t.isMemberExpression(n.callee)) {
      var calleeDesc = _describeMemberChain(n.callee);
      desc = calleeDesc ? calleeDesc + "()" : "CallExpression";
    }
    else if (_t.isFunctionExpression(n.callee) || _t.isArrowFunctionExpression(n.callee)) desc = "<IIFE>()";
    else if (_t.isNewExpression(n.callee)) desc = "new " + (_t.isIdentifier(n.callee.callee) ? n.callee.callee.name : n.callee.callee.type) + "()()";
    else desc = n.callee.type + "()";
  }
  else if (_t.isBinaryExpression(n)) desc = "<concat>";
  else if (_t.isTemplateLiteral(n)) desc = "<template>";
  else if (_t.isNewExpression(n)) {
    var ctorName = _t.isIdentifier(n.callee) ? n.callee.name : n.callee.type;
    // For new URL(X, Y) specifically, annotate what the arguments look
    // like: `DOM-read` for `.getAttribute(…)` / `.textContent`, `same-origin`
    // for `location.origin` family. Helps reviewers see at a glance
    // whether this is a genuinely-dynamic URL or just a resolver gap.
    if (ctorName === "URL" && n.arguments.length >= 1) {
      var argHints = [];
      for (var aI = 0; aI < Math.min(n.arguments.length, 2); aI++) {
        var arg = n.arguments[aI];
        if (_t.isCallExpression(arg) && _t.isMemberExpression(arg.callee) &&
            _t.isIdentifier(arg.callee.property) && arg.callee.property.name === "getAttribute") {
          var gaArg = arg.arguments[0];
          argHints.push("getAttribute(" + (_t.isStringLiteral(gaArg) ? '"' + gaArg.value + '"' : "…") + ")");
        } else if (_t.isMemberExpression(arg) && !arg.computed &&
            _t.isIdentifier(arg.property) &&
            (arg.property.name === "origin" || arg.property.name === "href" || arg.property.name === "pathname")) {
          var base = arg.object;
          var basePrefix = "";
          if (_t.isIdentifier(base) && (base.name === "location" || base.name === "window" || base.name === "self")) basePrefix = base.name + ".";
          else if (_t.isMemberExpression(base) && _t.isIdentifier(base.property, { name: "location" })) basePrefix = (_t.isIdentifier(base.object) ? base.object.name : "") + ".location.";
          argHints.push(basePrefix + arg.property.name);
        } else if (_t.isStringLiteral(arg)) {
          argHints.push('"' + (arg.value.length > 30 ? arg.value.slice(0, 27) + "…" : arg.value) + '"');
        } else if (_t.isIdentifier(arg)) {
          argHints.push(arg.name);
        } else {
          argHints.push("…");
        }
      }
      desc = "new URL(" + argHints.join(", ") + ")";
    } else {
      desc = "new " + ctorName + "(…)";
    }
  }
  else desc = n.type;
  // Report the ORIGINAL fetch-arg location so audit.sinks cross-refs
  // resolve correctly. If unwrap moved to a different location, include
  // the root cause's coords as a trailing annotation so reviewers can
  // navigate to the actual leaf.
  var reportLoc = origLoc || n.loc;
  var loc = reportLoc ? "@L" + reportLoc.start.line + ":C" + reportLoc.start.column : "";
  var rootTag = "";
  if (n.loc && reportLoc && (n.loc.start.line !== reportLoc.start.line || n.loc.start.column !== reportLoc.start.column)) {
    rootTag = " (root cause @L" + n.loc.start.line + ":C" + n.loc.start.column + ")";
  }
  _resolver.collectError(
    new Error("fetch URL resolver gap: " + desc + loc + rootTag + " didn't resolve — add a resolution path"),
    "fetchUrlResolve"
  );
}

// Extract structural URL shape from a URL argument when the full value
// doesn't resolve. Returns { path, queryParamNames, gapPath } when the
// literal-prefix portion of a concat / template literal can be parsed as
// a URL with at least one statically-visible query-param name; otherwise
// null. Path-only literal prefixes (no `?`) are valid too — caller emits
// the path with no query learned.
//
// Examples handled:
//   "/api/check?nwo=" + encodeURIComponent(repo)
//     → { path: "/api/check", queryParamNames: ["nwo"], gapPath: <repo> }
//   `/users/${id}/posts?page=${n}`
//     → { path: "/users/<id>/posts", … }  (when id is opaque, path itself
//        contains a marker — only emit when path is fully literal up to ?)
//   "/list?type=" + t + "&page=" + n
//     → { path: "/list", queryParamNames: ["type", "page"], … }
//
// What's NOT done here: scheme/host invention. If the literal prefix
// doesn't start with "/" or a known absolute URL, return null — we won't
// invent a host.
function _resolveUrlStructuralShape(urlArgPath) {
  if (!urlArgPath || !urlArgPath.node) return null;

  // Iterative unwrap of `new URL(input, base).href` / `.toString()` chains.
  // Each level descends one MemberExpression / CallExpression peel; the
  // inner URL constructor's first arg becomes the new urlArgPath. Loop
  // continues until the node stops matching the unwrap pattern.
  while (urlArgPath && urlArgPath.node) {
    var nu = urlArgPath.node;
    // `new URL(input, base).href` / `.toString()` — MemberExpression form.
    if (_t.isMemberExpression(nu) && !nu.computed &&
        _t.isIdentifier(nu.property) && (nu.property.name === "href" || nu.property.name === "toString") &&
        _t.isNewExpression(nu.object) && _t.isIdentifier(nu.object.callee, { name: "URL" }) &&
        !urlArgPath.scope.getBinding("URL") &&
        nu.object.arguments.length >= 1) {
      urlArgPath = urlArgPath.get("object.arguments.0");
      continue;
    }
    // `(new URL(input, base)).toString()` — CallExpression form.
    if (_t.isCallExpression(nu) && _t.isMemberExpression(nu.callee) && !nu.callee.computed &&
        _t.isIdentifier(nu.callee.property, { name: "toString" }) &&
        _t.isNewExpression(nu.callee.object) && _t.isIdentifier(nu.callee.object.callee, { name: "URL" }) &&
        !urlArgPath.scope.getBinding("URL") &&
        nu.callee.object.arguments.length >= 1) {
      urlArgPath = urlArgPath.get("callee.object.arguments.0");
      continue;
    }
    break;
  }
  if (!urlArgPath || !urlArgPath.node) return null;
  var n = urlArgPath.node;

  // Build an ordered list of [literal-or-null] segments. For BinaryExpression+
  // walk left-to-right; for TemplateLiteral interleave quasis and expressions.
  var segments = []; // each: {literal: string|null}  null = unresolved expression
  function pushTerm(termPath) {
    var vals = _resolveAllValues(termPath, 0);
    if (vals.length > 0 && typeof vals[0] === "string") {
      segments.push({ literal: vals[0] });
    } else {
      segments.push({ literal: null, gapPath: termPath });
    }
  }
  if (_t.isBinaryExpression(n, { operator: "+" })) {
    var parts = [];
    var cur = urlArgPath;
    while (_t.isBinaryExpression(cur.node, { operator: "+" })) {
      parts.push(cur.get("right"));
      cur = cur.get("left");
    }
    parts.push(cur);
    parts.reverse();
    for (var pi = 0; pi < parts.length; pi++) pushTerm(parts[pi]);
  } else if (_t.isTemplateLiteral(n)) {
    for (var qi = 0; qi < n.quasis.length; qi++) {
      var raw = n.quasis[qi].value.cooked || n.quasis[qi].value.raw || "";
      if (raw) segments.push({ literal: raw });
      if (qi < n.expressions.length) pushTerm(urlArgPath.get("expressions." + qi));
    }
  } else {
    return null;
  }

  // Need at least one literal segment that starts the path. The first
  // segment must be a literal beginning with "/" or "http(s)://" — we
  // refuse to invent a host.
  if (!segments.length || segments[0].literal == null) return null;
  var firstLit = segments[0].literal;
  if (!firstLit.startsWith("/") && !/^https?:\/\//i.test(firstLit)) return null;

  // Walk segments, accumulate path until we hit `?`, then accumulate
  // query-key names. Each unresolved segment in query-value position is
  // skipped (its name was already established by the preceding literal
  // ending in `=`). An unresolved segment in PATH position (before `?`)
  // means the path itself isn't fully static — bail (path placeholders
  // would be misleading).
  var sawQuery = false;
  var pathBuf = "";
  var queryBuf = "";
  for (var si = 0; si < segments.length; si++) {
    var seg = segments[si];
    if (!sawQuery) {
      if (seg.literal == null) {
        // Unresolved in path position — refuse to invent.
        return null;
      }
      var qIdx = seg.literal.indexOf("?");
      if (qIdx < 0) {
        pathBuf += seg.literal;
      } else {
        pathBuf += seg.literal.slice(0, qIdx);
        queryBuf += seg.literal.slice(qIdx + 1);
        sawQuery = true;
      }
    } else {
      if (seg.literal == null) {
        // Unresolved value — its key was set by the preceding literal
        // ending in `=`. Don't append anything; the key is already in
        // queryBuf followed by `=` with no value. The next literal (if
        // any) should start with `&` to introduce a new key.
      } else {
        queryBuf += seg.literal;
      }
    }
  }

  if (!sawQuery) {
    // Pure-path resolution: just emit the path. No query params learned.
    return pathBuf ? { path: pathBuf, queryParamNames: [] } : null;
  }

  // Parse queryBuf: split by '&', then for each part take the substring
  // before '='. Empty key segments are skipped. Trailing '=' (or no '=')
  // means the key is named but value was unresolved.
  var keys = [];
  var pieces = queryBuf.split("&");
  for (var ki = 0; ki < pieces.length; ki++) {
    var p = pieces[ki];
    if (!p) continue;
    var eqIdx = p.indexOf("=");
    var key = eqIdx >= 0 ? p.slice(0, eqIdx) : p;
    if (key && keys.indexOf(key) < 0) keys.push(key);
  }
  if (!keys.length) return null;
  return { path: pathBuf, queryParamNames: keys };
}

function _extractFetchCall(path, result, type) {
  var args = path.node.arguments;
  if (!args.length) return;

  // ── Resolve URL (may produce multiple values via inter-procedural tracing) ──
  var urlArgPath = path.get("arguments.0");
  // Provable-undefined / null short-circuit. `fetch(void 0)`, `fetch(null)`,
  // and `fetch(undefined)` (where `undefined` is the global) aren't real
  // network calls — at runtime the URL coerces to "undefined"/"null" and
  // the request fails immediately. They show up via inter-procedural
  // tracing (e.g. react-query's `t.fetch(void 0, s)` where the analyzer
  // follows the method into its body and sees an internal fetch using the
  // forwarded arg). Skip without recording a resolver gap — the value IS
  // resolved, it just isn't a URL.
  var _urlArgN = urlArgPath.node;
  if (_t.isUnaryExpression(_urlArgN) && _urlArgN.operator === "void") return;
  if (_t.isNullLiteral(_urlArgN)) return;
  if (_t.isIdentifier(_urlArgN, { name: "undefined" }) && !urlArgPath.scope.getBinding("undefined")) return;
  var urls = _resolveAllValues(urlArgPath, 0);
  // Empty result = resolver couldn't trace to a concrete value. For a
  // URL argument that's a resolver gap we want visible on the analysis
  // result so the identifier needing a new resolution path is named.
  // _recordUrlResolveGap walks the argument tree and collects the
  // first unresolved Identifier / MemberExpression / CallExpression
  // with its location; silently dropping the fetch site would erase
  // a pointer to work we still need to do.
  // Track structurally-learned param names for shape-only emission.
  // Populated by the structural-shape fallback below when full value
  // resolution fails; consumed at fetch-site construction time so the
  // schema gets the param NAMES even when their VALUES are opaque.
  var structuralParamNames = null;
  if (urls.length === 0) {
    var shape = _resolveUrlStructuralShape(urlArgPath);
    if (shape && shape.path) {
      urls = [shape.path];
      structuralParamNames = shape.queryParamNames;
      // Still record the value gap — reviewer needs to know which
      // expression(s) didn't resolve, even though we extracted the shape.
      _recordUrlResolveGap(urlArgPath);
    }
  }
  if (urls.length === 0) {
    _recordUrlResolveGap(urlArgPath);
    return;
  }
  // Filter out empty-string URLs from the resolution set. Common source:
  // `el.getAttribute("x") || ""` — the DOM read doesn't resolve, the ||
  // fallback is "". Emitting "" as a learned URL is a false positive
  // (no one actually fetches ""). If ALL resolved values are empty, it's
  // effectively unresolved — surface as a gap instead.
  urls = urls.filter(function(u) { return u !== "" && u != null; });
  if (urls.length === 0) {
    _recordUrlResolveGap(urlArgPath);
    return;
  }
  // The URL came from a pure StringLiteral iff the first argument
  // node IS a StringLiteral. Knowing this at the source-node level
  // lets _buildFetchSite's query-string extractor trust the URL's
  // embedded values (no interpolations, no placeholders to confuse
  // with real literals).
  var urlFromLiteral = _t.isStringLiteral(args[0]);

  // If URL couldn't be resolved to a concrete value, skip this call
  // site. Library-internal sinks (jQuery xhr.open(i.type, i.url),
  // axios fetch(w), etc.) either need a new resolution path added to
  // _resolveAllValues or don't belong in the concrete method list —
  // emitting placeholders would give the reviewer URLs that don't
  // actually fire at runtime.
  if (urls.length === 0) return;

  // ── Extract method, headers, body from options ──
  var httpMethod = null;
  var httpMethods = null;  // array for per-caller pairing when multiple values
  var headers = {};
  var bodyParams = [];

  // Resolve options object — inline or via variable reference or function parameter.
  // `fetch(new Request(url, init))` — Request init is the second arg of
  // the constructor, not of fetch. Unwrap so method/headers/body extract
  // the same way as `fetch(url, init)`.
  var optsNode = args[1] || null;
  var optsPath = args[1] ? path.get("arguments.1") : null;
  if (!optsNode && _t.isNewExpression(args[0]) &&
      _t.isIdentifier(args[0].callee, { name: "Request" }) &&
      !path.scope.getBinding("Request") &&
      args[0].arguments.length >= 2) {
    optsNode = args[0].arguments[1];
    optsPath = path.get("arguments.0.arguments.1");
  }
  if (optsNode && _t.isIdentifier(optsNode) && optsPath) {
    var optsBinding = path.scope.getBinding(optsNode.name);
    if (optsBinding && _t.isVariableDeclarator(optsBinding.path.node) && optsBinding.path.node.init) {
      optsNode = optsBinding.path.node.init;
      optsPath = optsBinding.path.get("init");
    } else if (optsBinding && optsBinding.kind === "param") {
      // Options passed as function parameter — resolve from callers
      var optsFuncPath = optsBinding.scope.path;
      var optsFuncBinding = null;
      if (optsFuncPath.node.id) optsFuncBinding = optsFuncPath.scope.parent ? optsFuncPath.scope.parent.getBinding(optsFuncPath.node.id.name) : null;
      if (!optsFuncBinding && _t.isVariableDeclarator(optsFuncPath.parent)) optsFuncBinding = optsFuncPath.scope.parent ? optsFuncPath.scope.parent.getBinding(optsFuncPath.parent.id.name) : null;
      if (optsFuncBinding && optsFuncBinding.referencePaths) {
        var optsParamIdx = -1;
        for (var opi = 0; opi < optsFuncPath.node.params.length; opi++) {
          var opn = optsFuncPath.node.params[opi];
          var opnName = _t.isIdentifier(opn) ? opn.name : (_t.isAssignmentPattern(opn) && _t.isIdentifier(opn.left) ? opn.left.name : null);
          if (opnName === optsNode.name) { optsParamIdx = opi; break; }
        }
        if (optsParamIdx >= 0) {
          var callerRefs = optsFuncBinding.referencePaths;
          for (var cri = 0; cri < callerRefs.length; cri++) {
            var cRef = callerRefs[cri];
            if (_t.isCallExpression(cRef.parent) && cRef.parent.callee === cRef.node &&
                optsParamIdx < cRef.parent.arguments.length) {
              var callerOptsArg = cRef.parent.arguments[optsParamIdx];
              if (_t.isObjectExpression(callerOptsArg)) {
                optsNode = callerOptsArg;
                optsPath = cRef.parentPath.get("arguments." + optsParamIdx);
                break;
              }
              if (_t.isIdentifier(callerOptsArg)) {
                var callerOptsB = cRef.parentPath.scope.getBinding(callerOptsArg.name);
                if (callerOptsB && _t.isVariableDeclarator(callerOptsB.path.node) && _t.isObjectExpression(callerOptsB.path.node.init)) {
                  optsNode = callerOptsB.path.node.init;
                  optsPath = callerOptsB.path.get("init");
                  break;
                }
              }
            }
          }
        }
      }
    }
  }

  // Try _resolveToObject for non-ObjectExpression opts (e.g. Object.assign, call returns)
  if (optsNode && !_t.isObjectExpression(optsNode) && optsPath) {
    var resolvedObj = _resolveToObject(optsPath, 0);
    if (resolvedObj && resolvedObj.type === "ObjectExpression") {
      optsNode = resolvedObj;
      if (resolvedObj._path) optsPath = resolvedObj._path;
    }
  }

  if (optsNode && _t.isObjectExpression(optsNode)) {
    var opts = optsNode.properties;
    for (var o = 0; o < opts.length; o++) {
      if (!_t.isObjectProperty(opts[o]) || opts[o].computed) continue;
      var optName = _getKeyName(opts[o].key);
      var optVal = opts[o].value;

      if (optName === "method") {
        var methodPath = null;
        try { methodPath = optsPath.get("properties." + o + ".value"); } catch(e) { _resolver.collectError(e, "fetchMethodPath"); }
        var methodVals = [];
        if (_t.isStringLiteral(optVal)) {
          methodVals = [optVal.value];
        } else if (methodPath && methodPath.node) {
          methodVals = _resolveAllValues(methodPath, 0);
        }
        var validMethods = [];
        for (var mi = 0; mi < methodVals.length; mi++) {
          if (typeof methodVals[mi] === "string" && _HTTP_METHODS_LC[methodVals[mi].toLowerCase()]) {
            validMethods.push(methodVals[mi].toUpperCase());
          }
        }
        if (validMethods.length > 0) {
          httpMethod = validMethods[0];
          if (validMethods.length > 1) httpMethods = validMethods;
        }
      }
      if (optName === "headers" && _t.isObjectExpression(optVal)) {
        headers = _extractHeaders(optVal);
      }
      // `headers: new Headers({...})` — W3C Headers constructor wraps the
      // header object. Unwrap to extract the same shape.
      if (optName === "headers" && _t.isNewExpression(optVal) &&
          _t.isIdentifier(optVal.callee, { name: "Headers" }) &&
          !path.scope.getBinding("Headers") &&
          optVal.arguments.length >= 1 && _t.isObjectExpression(optVal.arguments[0])) {
        headers = _extractHeaders(optVal.arguments[0]);
      }
      if (optName === "body") {
        // Direct path to optVal (the body argument expression). Used to
        // pass through to _extractObjectProperties so identifier-valued
        // body fields can resolve via _resolveAllValues.
        var optValPath = null;
        try { optValPath = optsPath.get("properties." + o + ".value"); } catch (e) { _resolver.collectError(e, "fetchBodyOptValPath"); }
        // For body = JSON.stringify(<inline ObjectExpression>), extract
        // properties path-aware so identifier-valued fields resolve via
        // the existing iterative value resolver per ECMA § 25.5.4.
        if (optValPath && _t.isCallExpression(optVal) && _isJsonStringify(optVal, path) &&
            optVal.arguments.length >= 1 && _t.isObjectExpression(optVal.arguments[0])) {
          var jsonObjPath = optValPath.get("arguments.0");
          if (jsonObjPath && jsonObjPath.node) {
            bodyParams = _extractObjectProperties(optVal.arguments[0], jsonObjPath);
            for (var bpj = 0; bpj < bodyParams.length; bpj++) bodyParams[bpj].location = "body";
          }
        }
        if (bodyParams.length === 0) bodyParams = _extractBodyParams(optVal, path);
        // When the body is `JSON.stringify(localVar)` and `_extractBodyParams`
        // didn't recover the inner shape (because the var's literal lives
        // elsewhere in the function), fall back to the property-flow
        // analyser's effects: the local var is tagged with `_bindingName`
        // when declared, and every property write to it shows up as an
        // obj-lit-targeted effect. Each unique const key becomes a body
        // param; const values for that key become its validValues.
        if (bodyParams.length === 0 && _t.isCallExpression(optVal) &&
            _isJsonStringify(optVal, path) &&
            optVal.arguments.length >= 1 && _t.isIdentifier(optVal.arguments[0])) {
          var varName = optVal.arguments[0].name;
          var encFn = path.getFunctionParent();
          if (encFn) {
            var pfBodyEffects = _specAnalyzePropertyFlow(encFn);
            if (pfBodyEffects && pfBodyEffects.length > 0) {
              var byKey2 = Object.create(null);
              for (var pbi = 0; pbi < pfBodyEffects.length; pbi++) {
                var pbe = pfBodyEffects[pbi];
                if (!pbe.target || pbe.target.kind !== "obj-lit") continue;
                if (pbe.target._bindingName !== varName) continue;
                if (!pbe.key || pbe.key.kind !== "const") continue;
                var keyName = pbe.key.value;
                if (!byKey2[keyName]) byKey2[keyName] = { values: new Set(), type: null };
                if (pbe.value && pbe.value.kind === "const") {
                  var vv = pbe.value.value;
                  if (typeof vv === "string" || typeof vv === "number" || typeof vv === "boolean") {
                    byKey2[keyName].values.add(vv);
                    if (!byKey2[keyName].type) byKey2[keyName].type = typeof vv;
                  }
                }
              }
              for (var bk in byKey2) {
                var entry = byKey2[bk];
                var newParam = { name: bk, location: "body", required: true };
                if (entry.type) newParam.type = entry.type;
                if (entry.values.size > 0) {
                  var vals = [];
                  entry.values.forEach(function(vv) { vals.push(vv); });
                  newParam.validValues = vals;
                  // First value as defaultValue for the dropdown's initial pick.
                  newParam.defaultValue = vals[0];
                }
                bodyParams.push(newParam);
              }
            }
          }
        }
      }
    }
  }

  // ── Response type from enclosing function ──
  var responseType = null;
  var funcParent = path.getFunctionParent();
  if (funcParent) {
    responseType = _detectResponseParsing(funcParent);
  }

  // ── Enclosing function params — determine location from usage in the fetch call ──
  var funcInfo = funcParent ? _extractFuncParams(funcParent.node) : null;
  var funcParams = [];
  if (funcInfo && funcInfo.params.length > 0) {
    // Build a set of param names used in specific roles
    var _usedAsUrl = new Set();    // param used as URL argument or in URL concatenation
    var _usedAsMethod = new Set(); // param used as method option value
    var _usedAsBody = new Set();   // param used in body option
    var _usedAsOpts = new Set();   // param used as the options object
    var _usedAsHeader = new Set(); // param used in headers object
    // URL argument: walk entire expression tree (handles concat, ternary, template)
    _collectIdentifiers(args[0], _usedAsUrl);
    // Options argument: fetch(url, paramName)
    if (args[1] && _t.isIdentifier(args[1])) _usedAsOpts.add(args[1].name);
    // Walk options object properties
    if (optsNode && _t.isObjectExpression(optsNode)) {
      for (var mo = 0; mo < optsNode.properties.length; mo++) {
        if (!_t.isObjectProperty(optsNode.properties[mo]) || optsNode.properties[mo].computed) continue;
        var moKey = _getKeyName(optsNode.properties[mo].key);
        var moVal = optsNode.properties[mo].value;
        if (moKey === "method") _collectIdentifiers(moVal, _usedAsMethod);
        if (moKey === "body") _collectIdentifiers(moVal, _usedAsBody);
        if (moKey === "headers" && _t.isObjectExpression(moVal)) {
          for (var hp = 0; hp < moVal.properties.length; hp++) {
            if (_t.isObjectProperty(moVal.properties[hp])) {
              _collectIdentifiers(moVal.properties[hp].value, _usedAsHeader);
            }
          }
        }
      }
    }

    // Expand through local variable bindings — if a collected name is a local
    // variable (not a function param), resolve its init and collect from that.
    // This traces e.g. var url = id ? "..." + id : "..." → id contributes to URL.
    var _funcParamNames = new Set();
    for (var _fpi = 0; _fpi < funcInfo.params.length; _fpi++) {
      _funcParamNames.add(funcInfo.params[_fpi].name);
    }
    var _allSets = [_usedAsUrl, _usedAsMethod, _usedAsBody, _usedAsOpts, _usedAsHeader];
    for (var _si = 0; _si < _allSets.length; _si++) {
      var _set = _allSets[_si];
      var _toExpand = [];
      _set.forEach(function(name) { _toExpand.push(name); });
      for (var _ei = 0; _ei < _toExpand.length; _ei++) {
        var _eName = _toExpand[_ei];
        if (_funcParamNames.has(_eName)) continue;
        var _eBinding = path.scope.getBinding(_eName);
        if (_eBinding && _t.isVariableDeclarator(_eBinding.path.node) && _eBinding.path.node.init) {
          _collectIdentifiers(_eBinding.path.node.init, _set);
        }
      }
    }

    for (var fp = 0; fp < funcInfo.params.length; fp++) {
      var fParam = funcInfo.params[fp];
      var matched = false;
      for (var ep = 0; ep < bodyParams.length; ep++) {
        if (bodyParams[ep].source === fParam.name || bodyParams[ep].name === fParam.name) {
          if (!fParam.required) {
            bodyParams[ep].required = false;
            if (fParam.defaultValue !== undefined) bodyParams[ep].defaultValue = fParam.defaultValue;
          }
          matched = true;
        }
      }
      if (!matched && !fParam.rest) {
        var loc = "unknown";
        if (_usedAsUrl.has(fParam.name)) loc = "path";
        else if (_usedAsMethod.has(fParam.name)) loc = "method";
        else if (_usedAsBody.has(fParam.name)) loc = "body";
        else if (_usedAsOpts.has(fParam.name)) loc = "options";
        else if (_usedAsHeader.has(fParam.name)) loc = "header";
        // For body params that are function parameters, try to resolve the caller's
        // actual argument to extract concrete body field names (e.g., {item: "widget"})
        // instead of just recording the wrapper's parameter name.
        if (loc === "body") {
          var bodyBinding = path.scope.getBinding(fParam.name);
          if (bodyBinding && bodyBinding.kind === "param" && bodyBinding.referencePaths) {
            var callerBodyResolved = false;
            var funcPath = bodyBinding.scope.path;
            var funcBindingForBody = null;
            if (funcPath.node.id) funcBindingForBody = funcPath.scope.parent ? funcPath.scope.parent.getBinding(funcPath.node.id.name) : null;
            if (!funcBindingForBody && _t.isVariableDeclarator(funcPath.parent)) funcBindingForBody = funcPath.scope.parent ? funcPath.scope.parent.getBinding(funcPath.parent.id.name) : null;
            // Determine paramIdx for body param
            var paramIdx = -1;
            paramIdx = _findParamIndex(funcPath.node.params, fParam.name);
            // Direct function binding: callers are funcName(args)
            if (funcBindingForBody && funcBindingForBody.referencePaths && paramIdx >= 0) {
              var refs = funcBindingForBody.referencePaths;
              for (var ri = 0; ri < refs.length; ri++) {
                var ref = refs[ri];
                if (ref.parent && _t.isCallExpression(ref.parent) && ref.parent.callee === ref.node &&
                    paramIdx < ref.parent.arguments.length) {
                  var callerArgNode = ref.parent.arguments[paramIdx];
                  var callerBodyExtracted = _extractBodyParams(callerArgNode, ref.parentPath);
                  if (callerBodyExtracted.length > 0) {
                    for (var cbe = 0; cbe < callerBodyExtracted.length; cbe++) funcParams.push(callerBodyExtracted[cbe]);
                    callerBodyResolved = true;
                  }
                }
              }
            }
            // Method-call pattern: function is a property of an object or prototype
            if (!callerBodyResolved && !funcBindingForBody && paramIdx >= 0) {
              var methodName = null;
              var objBindings = []; // bindings for variables that hold the object instance
              // Case: ObjectProperty — { method: function(body){...} }
              if (_t.isObjectProperty(funcPath.parent)) {
                methodName = _getKeyName(funcPath.parent.key);
                if (methodName && funcPath.parentPath && funcPath.parentPath.parentPath) {
                  var objExprPath = funcPath.parentPath.parentPath;
                  if (_t.isObjectExpression(objExprPath.node)) {
                    // Sub-case A: var obj = { method: function(){} }
                    if (_t.isVariableDeclarator(objExprPath.parent) && _t.isIdentifier(objExprPath.parent.id)) {
                      var ovb = objExprPath.scope.getBinding(objExprPath.parent.id.name);
                      if (ovb) objBindings.push(ovb);
                    }
                    // Sub-case B: return { method: function(){} } inside factory function
                    else if (_t.isReturnStatement(objExprPath.parent)) {
                      var factoryFunc = objExprPath.getFunctionParent();
                      if (factoryFunc) {
                        var ffb = null;
                        if (factoryFunc.node.id) ffb = factoryFunc.scope.parent ? factoryFunc.scope.parent.getBinding(factoryFunc.node.id.name) : null;
                        if (!ffb && _t.isVariableDeclarator(factoryFunc.parent)) ffb = factoryFunc.scope.parent ? factoryFunc.scope.parent.getBinding(factoryFunc.parent.id.name) : null;
                        if (ffb && ffb.referencePaths) {
                          for (var fci = 0; fci < ffb.referencePaths.length; fci++) {
                            var fRef = ffb.referencePaths[fci];
                            if (_t.isCallExpression(fRef.parent) && fRef.parent.callee === fRef.node) {
                              var fcParent = fRef.parentPath ? fRef.parentPath.parent : null;
                              if (fcParent && _t.isVariableDeclarator(fcParent) && _t.isIdentifier(fcParent.id)) {
                                var instB = fRef.parentPath.scope.getBinding(fcParent.id.name);
                                if (instB) objBindings.push(instB);
                              }
                            }
                          }
                        }
                      }
                    }
                  }
                }
              }
              // Case: Prototype — Constructor.prototype.method = function(body){...}
              else if (_t.isAssignmentExpression(funcPath.parent) && funcPath.parent.operator === "=") {
                var aLeft = funcPath.parent.left;
                if (_t.isMemberExpression(aLeft) && !aLeft.computed && _t.isIdentifier(aLeft.property) &&
                    _t.isMemberExpression(aLeft.object) && !aLeft.object.computed &&
                    _t.isIdentifier(aLeft.object.property, { name: "prototype" }) && _t.isIdentifier(aLeft.object.object)) {
                  methodName = aLeft.property.name;
                  var ctorName = aLeft.object.object.name;
                  var ctorBinding = funcPath.scope.getBinding(ctorName);
                  if (ctorBinding && ctorBinding.referencePaths) {
                    for (var nci = 0; nci < ctorBinding.referencePaths.length; nci++) {
                      var nRef = ctorBinding.referencePaths[nci];
                      if (_t.isNewExpression(nRef.parent) && nRef.parent.callee === nRef.node) {
                        var newParent = nRef.parentPath ? nRef.parentPath.parent : null;
                        if (newParent && _t.isVariableDeclarator(newParent) && _t.isIdentifier(newParent.id)) {
                          var niB = nRef.parentPath.scope.getBinding(newParent.id.name);
                          if (niB) objBindings.push(niB);
                        }
                      }
                    }
                  }
                }
              }
              // Search obj bindings for .method() calls and extract body args
              if (methodName && objBindings.length > 0) {
                for (var obi = 0; obi < objBindings.length; obi++) {
                  if (!objBindings[obi].referencePaths) continue;
                  var orefs = objBindings[obi].referencePaths;
                  for (var ori = 0; ori < orefs.length; ori++) {
                    var oRef = orefs[ori];
                    if (_t.isMemberExpression(oRef.parent) && oRef.parent.object === oRef.node &&
                        !oRef.parent.computed && _t.isIdentifier(oRef.parent.property, { name: methodName })) {
                      var mcExpr = oRef.parentPath ? oRef.parentPath.parent : null;
                      if (mcExpr && _t.isCallExpression(mcExpr) && mcExpr.callee === oRef.parent &&
                          paramIdx < mcExpr.arguments.length) {
                        var mcArg = mcExpr.arguments[paramIdx];
                        var mcBody = _extractBodyParams(mcArg, oRef.parentPath.parentPath);
                        if (mcBody.length > 0) {
                          for (var mci = 0; mci < mcBody.length; mci++) funcParams.push(mcBody[mci]);
                          callerBodyResolved = true;
                        }
                      }
                    }
                  }
                }
              }
            }
            if (callerBodyResolved) continue;  // Skip adding the wrapper param name
          }
        }
        if (loc !== "unknown") {
          funcParams.push({ name: fParam.name, location: loc, required: fParam.required, defaultValue: fParam.defaultValue, source: fParam.name });
        }
      }
    }
  }

  // ── URL template params ──
  var urlTemplateParams = [];
  if (_t.isTemplateLiteral(args[0]) && args[0].expressions.length > 0) {
    urlTemplateParams = _extractTemplateParams(args[0]);
  }

  // ── Build param list ──
  var params = [];
  for (var tp = 0; tp < urlTemplateParams.length; tp++) {
    params.push({ name: urlTemplateParams[tp], location: "path", required: true });
  }
  for (var bp = 0; bp < bodyParams.length; bp++) {
    params.push(bodyParams[bp]);
  }
  for (var fpi = 0; fpi < funcParams.length; fpi++) {
    params.push(funcParams[fpi]);
  }

  // ── Cross-reference params with value constraints ──
  for (var vc = 0; vc < params.length; vc++) {
    if (params[vc].spread) continue;
    var pName = params[vc].name;
    // For function params, also try the source variable name for constraint lookup
    var constraint = _getConstraint(path, pName);
    if (!constraint && params[vc].source && params[vc].source !== pName) {
      constraint = _getConstraint(path, params[vc].source);
    }
    if (constraint && constraint.values.size >= 1) {
      var validValues = [];
      constraint.values.forEach(function(v) { validValues.push(v); });
      params[vc].validValues = validValues;
    }
  }

  // ── Property-flow effects → branch-conditional valid values ──
  // For direct fetch calls inside a function, run the property-flow
  // analyser on the enclosing function and aggregate const-value
  // writes per body-param key. Both branches of a conditional are
  // surfaced (per § 14.6 IfStatement effect-accumulation).
  var pfEnclosingFn = path.getFunctionParent();
  if (pfEnclosingFn) {
    var pfEffects = _specAnalyzePropertyFlow(pfEnclosingFn);
    if (pfEffects && pfEffects.length > 0) {
      var pfByKey = Object.create(null);
      for (var pfi = 0; pfi < pfEffects.length; pfi++) {
        var pfe = pfEffects[pfi];
        if (!pfe.target) continue;
        // Accept this/param/args-elt/obj-lit targets — local-var bodies
        // (`var body = {}`) become obj-lit at decl, and subsequent
        // writes target it.
        var tk = pfe.target.kind;
        if (tk !== "this" && tk !== "param" && tk !== "args-elt" && tk !== "obj-lit") continue;
        if (!pfe.key || pfe.key.kind !== "const") continue;
        if (!pfe.value || pfe.value.kind !== "const") continue;
        var pv = pfe.value.value;
        if (typeof pv !== "string" && typeof pv !== "number" && typeof pv !== "boolean") continue;
        var pk = pfe.key.value;
        if (!pfByKey[pk]) pfByKey[pk] = new Set();
        pfByKey[pk].add(pv);
      }
      for (var apIdx = 0; apIdx < params.length; apIdx++) {
        if (params[apIdx].spread) continue;
        var apName = params[apIdx].name;
        if (!pfByKey[apName]) continue;
        var pfVals = [];
        pfByKey[apName].forEach(function(vv) { pfVals.push(vv); });
        if (pfVals.length === 0) continue;
        if (params[apIdx].validValues && params[apIdx].validValues.length > 0) {
          var pfMerged = params[apIdx].validValues.slice();
          for (var pfmi = 0; pfmi < pfVals.length; pfmi++) {
            if (pfMerged.indexOf(pfVals[pfmi]) < 0) pfMerged.push(pfVals[pfmi]);
          }
          params[apIdx].validValues = pfMerged;
        } else {
          params[apIdx].validValues = pfVals;
        }
      }
    }
  }

  // Append structurally-learned query param names (from
  // _resolveUrlStructuralShape) when the full URL value didn't resolve
  // but the path + param-name shape did. Marked _astValueSource:
  // "param-derived" so consumers know the value side wasn't statically
  // determined — the schema gets a real param NAME with no example.
  if (structuralParamNames && structuralParamNames.length) {
    for (var sni = 0; sni < structuralParamNames.length; sni++) {
      var pn = structuralParamNames[sni];
      var alreadyHave = false;
      for (var pj = 0; pj < params.length; pj++) {
        if (params[pj].name === pn && params[pj].location === "query") { alreadyHave = true; break; }
      }
      if (!alreadyHave) {
        params.push({
          name: pn,
          location: "query",
          required: true,
          source: "param-derived",
          _astValueSource: "param-derived",
        });
      }
    }
  }

  // ── Create call sites (with per-caller method pairing) ──
  for (var u = 0; u < urls.length; u++) {
    var siteMethod = httpMethods && u < httpMethods.length ? httpMethods[u] : (httpMethod || "GET");
    _pushFetchSite(result, _buildFetchSite(urls[u], siteMethod, headers, type, params, { enclosingFunction: funcInfo ? funcInfo.name : undefined, responseType: responseType, urlIsLiteral: urlFromLiteral }));
  }

  if (urls.length > 0) {
    var paramSummary = "";
    if (params.length > 0) {
      paramSummary = " params=[" + params.map(function(p) {
        var desc = (p.required ? "" : "?") + p.name + ":" + (p.location || "body");
        if (p.validValues) desc += "={" + p.validValues.slice(0, 5).join("|") + (p.validValues.length > 5 ? "|..." : "") + "}";
        return desc;
      }).join(", ") + "]";
    }
    var urlDisplay = urls[0].length > 80 ? urls[0].substring(0, 80) + "..." : urls[0];
    console.debug("[AST:fetch] %s %s %s%s%s",
      type, httpMethod || "GET", urlDisplay,
      urls.length > 1 ? " (+" + (urls.length - 1) + " more)" : "",
      paramSummary);
  }
}

// ─── Value Resolution ───────────────────────────────────────────────────────

// State IDs for _ravStep (state machine for _resolveAllValues).
// INIT (0) is the entry case; every recursive site has a corresponding
// AFTER state where F.result holds the sub-trace's array result.
var _RAV_INIT = 0;
var _RAV_ENC_AFTER = 100;
var _RAV_TL_LOOP = 110;
var _RAV_BIN_LOOP = 120;
var _RAV_COND_LOOP = 130;
var _RAV_LOG_LOOP = 140;
var _RAV_NEW_REQ_AFTER = 150;
var _RAV_NEW_URL_INPUT_AFTER = 160;
var _RAV_NEW_URL_BASE_AFTER = 161;
var _RAV_USP_LOOP = 170;
var _RAV_CALL_RETVAL_AFTER = 199;       // call-return resolution via _rcrvStep
var _RAV_CALL_RECV_AFTER = 200;        // string method passthrough chain
var _RAV_CALL_GA_LOOP = 210;            // .getAttribute roundtrip loop
var _RAV_CALL_JOIN_LOOP = 220;          // .join elements loop
var _RAV_IDENT_GLOBAL_AFTER = 300;
var _RAV_IDENT_CONST_AFTER = 310;
var _RAV_IDENT_NONCONST_INIT_AFTER = 320;
var _RAV_IDENT_NONCONST_CV_LOOP = 321;
var _RAV_IDENT_FOROF_LOOP = 330;
var _RAV_IDENT_DESTR_ARR_AFTER = 340;
var _RAV_IDENT_DESTR_ARR_RESOLVED_AFTER = 341;
var _RAV_IDENT_DESTR_OBJ_AFTER = 350;
var _RAV_IDENT_DESTR_OBJ_RESOLVED_AFTER = 351;
var _RAV_MEM_URL_PROP_AFTER = 410;
var _RAV_MEM_DS_LOOP = 420;
var _RAV_MEM_THIS_OBJ_LOOP = 430;
var _RAV_MEM_THIS_GETTER_LOOP = 440;
var _RAV_MEM_OBJ_LOOP = 450;
var _RAV_MEM_PROPASSIGN_LOOP = 460;
var _RAV_MEM_ARR_PROP_LOOP = 480;
var _RAV_COMP_MU_KEY_AFTER = 500;
var _RAV_COMP_MU_LOOP = 501;
var _RAV_COMP_MU_AKEY_AFTER = 502;
var _RAV_COMP_MU_RHS_AFTER = 503;
var _RAV_COMP_OBJ_KEY_AFTER = 510;
var _RAV_COMP_OBJ_PROP_LOOP = 511;
var _RAV_COMP_OBJ_DISC_LOOP = 512;
var _RAV_COMP_OBJ_ALL_LOOP = 520;
var _RAV_COMP_ARR_LOOP = 530;

function _ravMakeFrame(path, node, depth, stepFn, shortCircuit, guardPrefix, defaultResult) {
  return {
    path: path, node: node, depth: depth || 0,
    state: _RAV_INIT, L: {}, result: defaultResult !== undefined ? defaultResult : [],
    stepFn: stepFn || _ravStep,
    makeFrame: _ravMakeFrame,
    shortCircuit: shortCircuit || _ravShortCircuit,
    guardPrefix: guardPrefix !== undefined ? guardPrefix : "V",
    defaultResult: defaultResult !== undefined ? defaultResult : [],
  };
}

// Explicit-frame state-machine driver for _ravStep. JS call stack stays
// at depth 1 regardless of AST depth. Cycle detection via _resolver.guard
// at frame push and _resolver.unguard at frame pop.
// Shared driver for all resolver state machines. Each frame carries its
// own step function (frame.stepFn), per-resolver short-circuit/guard
// config (frame.shortCircuit, frame.guardPrefix), and default result for
// cycle/error fallback (frame.defaultResult).
//
// Step function returns {done: value} (frame complete) or
// {trace: subPath, state: AFTER_ID, stepFn?, makeFrame?, guardPrefix?,
// shortCircuit?, defaultResult?} (push sub-frame, optionally with
// different step function for cross-resolver calls). When stepFn is
// omitted from the trace step, the sub-frame inherits the parent's
// stepFn — same-resolver recursion stays cheap.
//
// The driver runs ONE loop, processing all frames regardless of step
// function. Cross-function "recursion" never grows the JS call stack.
function _runResolverStack(initialFrame) {
  var stack = [initialFrame];
  var lastResult = initialFrame.defaultResult;
  try {
    while (stack.length > 0) {
      var top = stack[stack.length - 1];
      top.result = lastResult;
      lastResult = top.defaultResult;
      var step;
      try { step = top.stepFn(top); }
      catch (e) {
        if (e instanceof RangeError) {
          _resolver.collectError(e, "runResolverStack");
          if (top.guardPrefix) _resolver.unguard(top.guardPrefix, top.node);
          stack.pop();
          lastResult = top.defaultResult;
          continue;
        }
        throw e;
      }
      if (step.done !== undefined) {
        lastResult = step.done;
        if (top.guardPrefix) _resolver.unguard(top.guardPrefix, top.node);
        stack.pop();
        continue;
      }
      var subPath = step.trace;
      top.state = step.state;
      if (!subPath || !subPath.node) { lastResult = top.defaultResult; continue; }
      // Inherit parent's config when not overridden by step.
      var subStepFn = step.stepFn || top.stepFn;
      var subMakeFrame = step.makeFrame || top.makeFrame;
      var subGuardPrefix = step.guardPrefix !== undefined ? step.guardPrefix : top.guardPrefix;
      var subShortCircuit = step.shortCircuit !== undefined ? step.shortCircuit : top.shortCircuit;
      var subDefaultResult = step.defaultResult !== undefined ? step.defaultResult : top.defaultResult;
      var subNode = subPath.node;
      // Per-resolver short-circuit (literals etc.) before frame allocation.
      if (subShortCircuit) {
        var sc = subShortCircuit(subNode, subPath);
        if (sc !== undefined) { lastResult = sc; continue; }
      }
      if (subGuardPrefix && !_resolver.guard(subGuardPrefix, subNode)) {
        lastResult = subDefaultResult;
        continue;
      }
      stack.push(subMakeFrame(subPath, subNode, top.depth + 1, subStepFn, subShortCircuit, subGuardPrefix, subDefaultResult));
    }
    return lastResult;
  } finally {
    while (stack.length > 0) {
      var f = stack.pop();
      if (f.guardPrefix) _resolver.unguard(f.guardPrefix, f.node);
    }
  }
}

// _resolveAllValues short-circuit: literals resolve directly without
// allocating a frame.
function _ravShortCircuit(node, path) {
  if (_t.isStringLiteral(node)) return [node.value];
  if (_t.isNumericLiteral(node)) return [String(node.value)];
  return undefined;  // no short-circuit, push frame
}

function _resolveAllValues(initialPath, initialDepth) {
  var initialNode = initialPath && initialPath.node;
  if (!initialNode) return [];
  // Apply short-circuit at entry to avoid driver setup for literals.
  var sc = _ravShortCircuit(initialNode, initialPath);
  if (sc !== undefined) return sc;
  if (!_resolver.guard("V", initialNode)) return [];
  var initialFrame = _ravMakeFrame(initialPath, initialNode, initialDepth || 0,
    _ravStep, _ravShortCircuit, "V", []);
  return _runResolverStack(initialFrame);
}

// Step function for the _resolveAllValues state machine. Each branch from
// the original recursive form is preserved 1:1 here as a case (or chain
// of cases for branches with internal recursion). Sub-traces request the
// driver to push a new frame via {trace: subPath, state: AFTER_X}.
// Internal-goto via F.state = X; continue; mimics the original
// "if (X) {…} if (Y) {…}" sequential dispatch fall-through.
function _ravStep(F) {
  var path = F.path, node = F.node, depth = F.depth, L = F.L;
  ravLoop:
  while (true) {
    switch (F.state) {
      case _RAV_INIT: {
  var skip = L.branchSkip || 0;

  // BRANCH 1: String-encoding transforms (encodeURIComponent / encodeURI /
  // decodeURIComponent / decodeURI / btoa / atob). Pure ECMAScript: when
  // the argument resolves to a string, apply the transform and return the
  // encoded value. Each is scope-checked (must be the unshadowed global).
  if (skip < 1 && _t.isCallExpression(node) && _t.isIdentifier(node.callee) &&
      node.arguments.length >= 1 && !path.scope.getBinding(node.callee.name)) {
    var ecName = node.callee.name;
    if (ecName === "encodeURIComponent" || ecName === "encodeURI" ||
        ecName === "decodeURIComponent" || ecName === "decodeURI" ||
        ecName === "btoa" || ecName === "atob") {
      L.ecName = ecName;
      return { trace: path.get("arguments.0"), state: _RAV_ENC_AFTER };
    }
  }

  // BRANCH 2: Simple template literal without interpolations (non-recursive).
  if (skip < 2 && _t.isTemplateLiteral(node) && node.expressions.length === 0 && node.quasis.length === 1) {
    return { done: [node.quasis[0].value.cooked || node.quasis[0].value.raw] };
  }

  // BRANCH 3: Template literal with resolvable expressions.
  // State-machine loop: alternately push quasi raw text and trace each
  // expression; when an expression resolves to empty the whole literal
  // is unresolvable (return done with []).
  if (skip < 3 && _t.isTemplateLiteral(node) && node.expressions.length > 0) {
    L.tlParts = [];
    L.tlQi = 0;
    L.branchSkip = 3;
    F.state = _RAV_TL_LOOP;
    continue;
  }

  // String concatenation — flatten left-recursive chain iteratively to avoid stack overflow.
  // ((a + b) + c) + d is walked as: collect [d, c, b, a] from the left spine, reverse, then zip.
  //
  // Every term must resolve to a literal value via the existing
  // resolution machinery. If any term doesn't resolve, return [] —
  // returning partial parts would produce a false-relative URL that
  // background.js misattributes to the page origin. The caller
  // (_extractFetchCall for URL args) converts [] into a
  // resolverError on the analysis result so the identifier-that-
  // didn't-resolve is named and can have a resolution path added.
  // BRANCH 4: BinaryExpression `+` chain. Iterative left-spine collection
  // (no stack overflow from chain length); each term is then traced via
  // sub-frame, results zipped on the way back. State machine handles the
  // per-term trace loop.
  if (skip < 4 && _t.isBinaryExpression(node, { operator: "+" })) {
    var binConcatParts = [];
    var binCur = path;
    while (_t.isBinaryExpression(binCur.node, { operator: "+" })) {
      binConcatParts.push(binCur.get("right"));
      binCur = binCur.get("left");
    }
    binConcatParts.push(binCur); // leftmost non-+ term
    binConcatParts.reverse();
    L.binParts = binConcatParts;
    L.binCi = 0;
    L.binTermResults = [];
    L.branchSkip = 4;
    F.state = _RAV_BIN_LOOP;
    continue;
  }

  // BRANCH 5: ConditionalExpression alternate chain (a ? b : (c ? d : e)).
  // Walk down alternates collecting consequents; each consequent + the
  // leaf alternate is traced via sub-frame.
  if (skip < 5 && _t.isConditionalExpression(node)) {
    var condParts = [];
    var condCur = path;
    while (_t.isConditionalExpression(condCur.node)) {
      condParts.push(condCur.get("consequent"));
      condCur = condCur.get("alternate");
    }
    condParts.push(condCur); // leaf alternate
    L.condParts = condParts;
    L.condCi = 0;
    L.condAcc = [];
    L.branchSkip = 5;
    F.state = _RAV_COND_LOOP;
    continue;
  }

  // BRANCH 6: Logical OR (a || b) || c chain. Iterative left-spine
  // collection; each part traced via sub-frame; results concatenated.
  if (skip < 6 && _t.isLogicalExpression(node, { operator: "||" })) {
    var orPartsLocal = [];
    var orCur = path;
    while (_t.isLogicalExpression(orCur.node, { operator: "||" })) {
      orPartsLocal.push(orCur.get("right"));
      orCur = orCur.get("left");
    }
    orPartsLocal.push(orCur);
    orPartsLocal.reverse();
    L.orParts = orPartsLocal;
    L.orOi = 0;
    L.orAcc = [];
    L.branchSkip = 6;
    F.state = _RAV_LOG_LOOP;
    continue;
  }

  // BRANCH 7: new Request(input, init?). Iterative passthrough chain:
  // new Request(new Request(...(url))) resolves to the innermost url.
  // After chain walk the leaf input is traced via sub-frame.
  if (skip < 7 && _t.isNewExpression(node) && _t.isIdentifier(node.callee, { name: "Request" }) &&
      !path.scope.getBinding("Request") && node.arguments.length >= 1) {
    var reqPath = path.get("arguments.0");
    var reqNode = reqPath.node;
    while (_t.isNewExpression(reqNode) && _t.isIdentifier(reqNode.callee, { name: "Request" }) &&
           !reqPath.scope.getBinding("Request") && reqNode.arguments.length >= 1) {
      reqPath = reqPath.get("arguments.0");
      reqNode = reqPath.node;
    }
    L.branchSkip = 7;
    return { trace: reqPath, state: _RAV_NEW_REQ_AFTER };
  }

  // BRANCH 8: new URL(input, base?). Trace input first, then base (if
  // present); cross-product the resolved strings via URL constructor.
  if (skip < 8 && _t.isNewExpression(node) && _t.isIdentifier(node.callee, { name: "URL" }) &&
      !path.scope.getBinding("URL") && node.arguments.length >= 1) {
    L.urlHasBase = node.arguments.length >= 2;
    L.branchSkip = 8;
    return { trace: path.get("arguments.0"), state: _RAV_NEW_URL_INPUT_AFTER };
  }

  // new URLSearchParams({k: v, ...}) / new URLSearchParams("a=1&b=2")
  // coerces to a querystring via implicit toString() — resolve to the
  // encoded form so concats like `"/api?" + params` surface a concrete
  // URL. Every value term must resolve; if the analyzer can't trace
  // one of them that's a resolver gap, surfaced by the concat-level
  // throw below once control returns here.
  // BRANCH 9: new URLSearchParams({k: v, ...}) / new URLSearchParams("a=1&b=2").
  // Object-literal form requires every value to resolve via sub-trace; loop
  // collects encoded key=value pairs. String-literal form returns directly.
  if (skip < 9 && _t.isNewExpression(node) && _t.isIdentifier(node.callee, { name: "URLSearchParams" }) &&
      !path.scope.getBinding("URLSearchParams")) {
    var uspArg = node.arguments && node.arguments[0];
    if (uspArg && _t.isObjectExpression(uspArg)) {
      // Pre-validate keys (no recursion needed); if any computed/missing
      // key, return done [] immediately.
      var uspKeys = [];
      for (var upiPre = 0; upiPre < uspArg.properties.length; upiPre++) {
        var uspPropPre = uspArg.properties[upiPre];
        if (!_t.isObjectProperty(uspPropPre) || uspPropPre.computed) return { done: [] };
        var uspKeyNamePre = _t.isIdentifier(uspPropPre.key) ? uspPropPre.key.name :
          (_t.isStringLiteral(uspPropPre.key) ? uspPropPre.key.value : null);
        if (!uspKeyNamePre) return { done: [] };
        uspKeys.push(uspKeyNamePre);
      }
      L.uspProps = uspArg.properties;
      L.uspKeys = uspKeys;
      L.uspParts = [];
      L.uspUpi = 0;
      L.branchSkip = 9;
      F.state = _RAV_USP_LOOP;
      continue;
    }
    if (uspArg && _t.isStringLiteral(uspArg)) {
      return { done: [uspArg.value.replace(/^\?/, "")] };
    }
  }

  // BRANCH 10: Call expression — multiple sub-cases.
  if (skip < 10 && _t.isCallExpression(node)) {
    var callSubSkip = L.callSubSkip || 0;
    // Sub-case 10a: dispatch to _rcrvStep (shared driver) for call-return
    // resolution. Was direct recursive call to _resolveCallReturnValues
    // — now flattened via shared-stack frame push.
    if (callSubSkip < 1) {
      L.branchSkip = 10;
      L.callSubSkip = 1;
      return {
        trace: path,
        state: _RAV_CALL_RETVAL_AFTER,
        stepFn: _rcrvStep,
        makeFrame: _rcrvMakeFrame,
        shortCircuit: null,
        guardPrefix: "R",
        defaultResult: [],
      };
    }

    // Sub-case 10b: String method passthrough (.replace, .trim, .toString,
    // etc.). Walk receiver chain iteratively, then trace the leaf via
    // sub-frame.
    if (_t.isMemberExpression(node.callee) && !node.callee.computed) {
      var _smPassNames = { replace: 1, trim: 1, toLowerCase: 1, toUpperCase: 1,
                            slice: 1, substring: 1, substr: 1, toString: 1, valueOf: 1 };
      var smName = _t.isIdentifier(node.callee.property) ? node.callee.property.name : null;
      if (smName && _smPassNames[smName]) {
        var smRecvPath = path.get("callee.object");
        var smRecvNode = smRecvPath.node;
        while (_t.isCallExpression(smRecvNode) &&
               _t.isMemberExpression(smRecvNode.callee) && !smRecvNode.callee.computed &&
               _t.isIdentifier(smRecvNode.callee.property) &&
               _smPassNames[smRecvNode.callee.property.name]) {
          smRecvPath = smRecvPath.get("callee.object");
          smRecvNode = smRecvPath.node;
        }
        L.branchSkip = 10;
        return { trace: smRecvPath, state: _RAV_CALL_RECV_AFTER };
      }
      // Sub-case 10c: el.getAttribute(KEY) roundtrip via setAttribute.
      // Loop over receiver binding's referencePaths; for each setAttribute
      // call matching the key, trace its value arg.
      if (smName === "getAttribute" && node.arguments.length >= 1 &&
          _t.isStringLiteral(node.arguments[0])) {
        var gaKey = node.arguments[0].value;
        var gaReceiverPath = path.get("callee.object");
        var gaReceiver = gaReceiverPath.node;
        if (_t.isIdentifier(gaReceiver)) {
          var gaBinding = path.scope.getBinding(gaReceiver.name);
          if (gaBinding && gaBinding.referencePaths) {
            // Pre-compute the matching setAttribute call paths so the
            // loop state has a fixed work list.
            var gaCallPaths = [];
            for (var gariPre = 0; gariPre < gaBinding.referencePaths.length; gariPre++) {
              var gaRefPre = gaBinding.referencePaths[gariPre];
              var gaMemPre = gaRefPre.parentPath;
              if (!gaMemPre || !gaMemPre.isMemberExpression() || gaMemPre.node.object !== gaRefPre.node || gaMemPre.node.computed) continue;
              if (!_t.isIdentifier(gaMemPre.node.property, { name: "setAttribute" })) continue;
              var gaCallPre = gaMemPre.parentPath;
              if (!gaCallPre || !gaCallPre.isCallExpression() || gaCallPre.node.callee !== gaMemPre.node) continue;
              if (gaCallPre.node.arguments.length < 2) continue;
              var gaSetKeyPre = gaCallPre.node.arguments[0];
              if (!_t.isStringLiteral(gaSetKeyPre) || gaSetKeyPre.value !== gaKey) continue;
              gaCallPaths.push(gaCallPre);
            }
            if (gaCallPaths.length > 0) {
              L.gaCallPaths = gaCallPaths;
              L.gaSetVals = [];
              L.gaGi = 0;
              L.branchSkip = 10;
              F.state = _RAV_CALL_GA_LOOP;
              continue;
            }
          }
        }
      }
      // Sub-case 10d: Array.join(separator). Resolve receiver to array,
      // then trace each element. If any element doesn't resolve, return [].
      if (smName === "join" && node.arguments.length <= 1) {
        var joinArrNode = _resolveToArray(path.get("callee.object"), 0);
        if (joinArrNode && joinArrNode.elements && joinArrNode.elements.length > 0 && joinArrNode._path) {
          var jsep = ",";
          if (node.arguments.length === 1 && _t.isStringLiteral(node.arguments[0])) jsep = node.arguments[0].value;
          L.joinArrNode = joinArrNode;
          L.joinSep = jsep;
          L.joinParts = [];
          L.joinJi = 0;
          L.branchSkip = 10;
          F.state = _RAV_CALL_JOIN_LOOP;
          continue;
        }
      }
    }
  }

  // BRANCH 11: Identifier — multiple sub-cases (binding init, param,
  // reassignments, for-in/of, destructure). Each sub-case dispatches to
  // its own AFTER state. Sub-cases use L.identSubSkip to fall-through:
  // an AFTER handler that gets an empty result sets identSubSkip past
  // the failed sub-case and re-enters at _RAV_INIT, allowing the
  // remaining sub-cases of branch 11 to try in original-recursive order.
  if (skip < 11 && _t.isIdentifier(node)) {
    var identSubSkip = L.identSubSkip || 0;
    var binding = path.scope.getBinding(node.name);
    if (!binding) {
      // 11a: Global-assignment fallback. Iterative chain walk; the leaf
      // valuePath is traced via sub-frame.
      if (identSubSkip < 1) {
        var globalDef = _globalAssignments[node.name];
        if (!globalDef || !globalDef.valuePath) return { done: [] };
        var gdPath = globalDef.valuePath;
        var gdNode = gdPath.node;
        while (_t.isIdentifier(gdNode)) {
          if (gdPath.scope && gdPath.scope.getBinding(gdNode.name)) break;
          var nextGd = _globalAssignments[gdNode.name];
          if (!nextGd || !nextGd.valuePath) break;
          gdPath = nextGd.valuePath;
          gdNode = gdPath.node;
        }
        L.branchSkip = 11;
        return { trace: gdPath, state: _RAV_IDENT_GLOBAL_AFTER };
      }
      return { done: [] };
    }

    // 11b: Constant initializer with iterative aliasing chain walk.
    if (identSubSkip < 2 && binding.constant && _t.isVariableDeclarator(binding.path.node) && binding.path.node.init) {
      var aliasPath = binding.path.get("init");
      var aliasNode = binding.path.node.init;
      while (_t.isIdentifier(aliasNode)) {
        var aliasBinding = aliasPath.scope.getBinding(aliasNode.name);
        if (!aliasBinding || !aliasBinding.constant) break;
        if (!_t.isVariableDeclarator(aliasBinding.path.node) || !aliasBinding.path.node.init) break;
        aliasPath = aliasBinding.path.get("init");
        aliasNode = aliasBinding.path.node.init;
      }
      L.identBinding = binding;
      L.branchSkip = 11;
      L.identSubSkip = 2;
      return { trace: aliasPath, state: _RAV_IDENT_CONST_AFTER };
    }

    // 11c: Function parameter — inter-procedural via _resolveParamFromCallers
    // (external resolver, direct call ok).
    if (identSubSkip < 3 && binding.kind === "param") {
      var callerValues = _resolveParamFromCallers(binding, depth);
      if (callerValues.length > 0) {
        _stats.interProcTraces++;
        return { done: callerValues };
      }
    }

    // 11d: Non-constant variable with reassignments — init + each CV
    // traced sequentially. Setup loop state.
    if (identSubSkip < 4 && !binding.constant && binding.constantViolations.length > 0) {
      L.identBinding = binding;
      L.identNcAcc = [];
      L.identNcPhase = "init";  // "init" → trace init; "cv" → trace CVs
      L.identNcCi = 0;
      L.branchSkip = 11;
      L.identSubSkip = 4;
      // If init exists, trace it first (with iterative alias walk); else
      // skip directly to CV loop.
      if (_t.isVariableDeclarator(binding.path.node) && binding.path.node.init) {
        var ncInitPath = binding.path.get("init");
        var ncInitNode = binding.path.node.init;
        while (_t.isIdentifier(ncInitNode)) {
          var ncAliasBinding = ncInitPath.scope.getBinding(ncInitNode.name);
          if (!ncAliasBinding || !ncAliasBinding.constant) break;
          if (!_t.isVariableDeclarator(ncAliasBinding.path.node) || !ncAliasBinding.path.node.init) break;
          ncInitPath = ncAliasBinding.path.get("init");
          ncInitNode = ncAliasBinding.path.node.init;
        }
        return { trace: ncInitPath, state: _RAV_IDENT_NONCONST_INIT_AFTER };
      }
      // No init — go straight to CV loop.
      F.state = _RAV_IDENT_NONCONST_CV_LOOP;
      continue;
    }

    // 11e: for-in / for-of loop variable. for-in returns the resolved
    // object's literal keys (no recursion). for-of traces each array
    // element via sub-frame.
    if (identSubSkip < 5 && _t.isVariableDeclarator(binding.path.node) && !binding.path.node.init) {
      var loopOwner = binding.path.parentPath && binding.path.parentPath.parentPath;
      if (loopOwner && (loopOwner.isForInStatement() || loopOwner.isForOfStatement()) &&
          loopOwner.node.left === binding.path.parent) {
        var iterableP = loopOwner.get("right");
        if (loopOwner.isForInStatement()) {
          var iterObj = _resolveToObject(iterableP, depth);
          if (iterObj && iterObj.properties) {
            var keys = [];
            for (var ki = 0; ki < iterObj.properties.length; ki++) {
              var ko = iterObj.properties[ki];
              if (_t.isObjectProperty(ko) && !ko.computed) {
                var kn = _getKeyName(ko.key);
                if (kn != null) keys.push(kn);
              }
            }
            if (keys.length > 0) return { done: keys };
          }
        } else {
          var iterArr = _resolveToArray(iterableP, depth);
          if (iterArr && iterArr._path && iterArr.elements && iterArr.elements.length > 0) {
            L.iterArrPath = iterArr._path;
            L.iterArrElements = iterArr.elements;
            L.iterAcc = [];
            L.iterEli = 0;
            L.branchSkip = 11;
            F.state = _RAV_IDENT_FOROF_LOOP;
            continue;
          }
        }
      }
    }

    // 11f/g: Destructured binding via ArrayPattern or ObjectPattern.
    if (identSubSkip < 6 && _t.isVariableDeclarator(binding.path.node) && binding.path.node.init &&
        (_t.isArrayPattern(binding.path.node.id) || _t.isObjectPattern(binding.path.node.id))) {
      var pat = binding.path.node.id;
      var initExpr = binding.path.node.init;
      if (_t.isArrayPattern(pat)) {
        var elemIdx = -1;
        for (var aei = 0; aei < pat.elements.length; aei++) {
          var aElem = pat.elements[aei];
          if (_t.isIdentifier(aElem, { name: node.name })) { elemIdx = aei; break; }
          if (_t.isAssignmentPattern(aElem) && _t.isIdentifier(aElem.left, { name: node.name })) { elemIdx = aei; break; }
        }
        if (elemIdx >= 0) {
          if (_t.isArrayExpression(initExpr) && elemIdx < initExpr.elements.length && initExpr.elements[elemIdx]) {
            L.branchSkip = 11;
            return { trace: binding.path.get("init.elements." + elemIdx), state: _RAV_IDENT_DESTR_ARR_AFTER };
          }
          var arrInit = _resolveToArray(binding.path.get("init"), depth);
          if (arrInit && arrInit._path && elemIdx < arrInit.elements.length && arrInit.elements[elemIdx]) {
            L.branchSkip = 11;
            return { trace: arrInit._path.get("elements." + elemIdx), state: _RAV_IDENT_DESTR_ARR_RESOLVED_AFTER };
          }
        }
      } else {
        var keyForName = _findDestructuredKey(pat, node.name);
        if (keyForName) {
          if (_t.isObjectExpression(initExpr)) {
            for (var oei = 0; oei < initExpr.properties.length; oei++) {
              var oep = initExpr.properties[oei];
              if (_t.isObjectProperty(oep) && !oep.computed && _getKeyName(oep.key) === keyForName) {
                L.branchSkip = 11;
                return { trace: binding.path.get("init.properties." + oei + ".value"), state: _RAV_IDENT_DESTR_OBJ_AFTER };
              }
            }
          }
          var initObj = _resolveToObject(binding.path.get("init"), depth);
          if (initObj && initObj._path) {
            for (var oej = 0; oej < initObj.properties.length; oej++) {
              var oeq = initObj.properties[oej];
              if (_t.isObjectProperty(oeq) && !oeq.computed && _getKeyName(oeq.key) === keyForName) {
                L.branchSkip = 11;
                return { trace: initObj._path.get("properties." + oej + ".value"), state: _RAV_IDENT_DESTR_OBJ_RESOLVED_AFTER };
              }
            }
          }
        }
      }
    }
  }

  // BRANCH 12: MemberExpression non-computed (obj.prop). Multi sub-case.
  // L.memSubSkip tracks which sub-case to skip past on re-entry (after a
  // sub-case's AFTER state returned empty). 0 means "try all".
  if (skip < 12 && _t.isMemberExpression(node) && !node.computed) {
    var memSubSkip = L.memSubSkip || 0;
    var propName = _t.isIdentifier(node.property) ? node.property.name : null;
    if (propName) {
      // Object.defineProperty(obj, "key", descriptor) — ECMAScript
      // property definition. When the descriptor has `get: fn`, reading
      // obj.key invokes fn and returns its result; when it has `value: V`,
      // reading obj.key returns V directly. This is the pure-JS mechanism
      // for installing late-bound properties on an object.
      // Inter-procedural: obj may be mutated inside a function it's
      // passed to. We collect descriptors from:
      //   (a) Direct:        Object.defineProperty(obj, propName, desc)
      //   (b) Through call:  someFn(obj, ...) → param-of-someFn used as
      //                       defineProperty target inside someFn's body
      //   (c) Through .call/.apply: fn.call(thisArg, obj, …) → arg shift
      //   (d) Through for..in over literal-keyed object: unroll loop body
      //   (e) Through function return: obj is `f(args)` — resolve f's
      //       return expression then collect defineProperty effects on
      //       it (including effects from other call sites within f's
      //       body that pass the returned value as an arg).
      if (_t.isIdentifier(node.object)) {
        var dpVals = _collectDefinePropertyEffects(path.get("object"), propName, depth, new Set());
        if (dpVals.length > 0) return { done: dpVals };
      }
      if (_t.isCallExpression(node.object)) {
        var rcVals = _collectDefinePropertyEffectsOnCallReturn(path.get("object"), propName, depth, new Set());
        if (rcVals.length > 0) return { done: rcVals };
      }
      // 12c: `<urlExpr>.<URL-prop>` — extract WHATWG URL instance properties.
      if (memSubSkip < 1 && _URL_INSTANCE_PROPS[propName]) {
        var canExtract = false;
        if (_t.isNewExpression(node.object) && _t.isIdentifier(node.object.callee, { name: "URL" }) &&
            !path.scope.getBinding("URL")) {
          canExtract = true;
        } else if (_t.isIdentifier(node.object) && _getTrackedType(path, node.object) === "URL") {
          canExtract = true;
        }
        if (canExtract) {
          L.urlPropName = propName;
          // Don't set branchSkip yet — sub-case might fall through to next sub-case.
          return { trace: path.get("object"), state: _RAV_MEM_URL_PROP_AFTER };
        }
      }
      // 12d: `el.dataset.X` — read corresponding data-* attribute. Scan
      // receiver binding refs for matching dataset writes / setAttribute
      // calls; trace each value via sub-frame.
      if (memSubSkip < 2 && _t.isMemberExpression(node.object) && !node.object.computed &&
          _t.isIdentifier(node.object.property, { name: "dataset" })) {
        var dsReceiver = node.object.object;
        var dsKey = propName;
        var attrKey = "data-" + dsKey.replace(/[A-Z]/g, function(c) { return "-" + c.toLowerCase(); });
        if (_t.isIdentifier(dsReceiver)) {
          var dsBinding = path.scope.getBinding(dsReceiver.name);
          if (dsBinding && dsBinding.referencePaths) {
            // Pre-compute matching write paths (no recursion): both
            // `el.dataset.<key> = V` and `el.setAttribute("data-<key>", V)`.
            var dsRhsPaths = [];
            for (var dsi = 0; dsi < dsBinding.referencePaths.length; dsi++) {
              var dsRef = dsBinding.referencePaths[dsi];
              var dsMem = dsRef.parentPath;
              if (!dsMem || !dsMem.isMemberExpression() || dsMem.node.object !== dsRef.node) continue;
              if (!dsMem.node.computed && _t.isIdentifier(dsMem.node.property, { name: "dataset" })) {
                var dsInner = dsMem.parentPath;
                if (!dsInner || !dsInner.isMemberExpression() || dsInner.node.object !== dsMem.node) continue;
                var dsInnerKey = null;
                if (!dsInner.node.computed && _t.isIdentifier(dsInner.node.property)) dsInnerKey = dsInner.node.property.name;
                else if (dsInner.node.computed && _t.isStringLiteral(dsInner.node.property)) dsInnerKey = dsInner.node.property.value;
                if (dsInnerKey !== dsKey) continue;
                var dsAssign = dsInner.parentPath;
                if (!dsAssign || !dsAssign.isAssignmentExpression() || dsAssign.node.operator !== "=" || dsAssign.node.left !== dsInner.node) continue;
                dsRhsPaths.push(dsAssign.get("right"));
              }
              if (!dsMem.node.computed && _t.isIdentifier(dsMem.node.property, { name: "setAttribute" })) {
                var saCall = dsMem.parentPath;
                if (!saCall || !saCall.isCallExpression() || saCall.node.callee !== dsMem.node) continue;
                if (saCall.node.arguments.length < 2) continue;
                var saKeyArg = saCall.node.arguments[0];
                if (!_t.isStringLiteral(saKeyArg) || saKeyArg.value !== attrKey) continue;
                dsRhsPaths.push(saCall.get("arguments.1"));
              }
            }
            if (dsRhsPaths.length > 0) {
              L.dsRhsPaths = dsRhsPaths;
              L.dsAcc = [];
              L.dsDi = 0;
              F.state = _RAV_MEM_DS_LOOP;
              continue;
            }
          }
        }
      }
      // 12e: `location.origin` / `window.location.origin` / `self.location.origin`
      // — non-recursive; resolvable from the analysis tab URL.
      if (memSubSkip < 3 && _sourceUrl && (propName === "origin" || propName === "protocol" || propName === "hostname" || propName === "host")) {
        var locNode = node.object;
        var locGlobal = false;
        if (_t.isIdentifier(locNode, { name: "location" }) && !path.scope.getBinding("location")) locGlobal = true;
        else if (_t.isMemberExpression(locNode) && !locNode.computed &&
            _t.isIdentifier(locNode.property, { name: "location" })) {
          var locBase = locNode.object;
          if ((_t.isIdentifier(locBase, { name: "window" }) && !path.scope.getBinding("window")) ||
              (_t.isIdentifier(locBase, { name: "self" }) && !path.scope.getBinding("self")) ||
              (_t.isIdentifier(locBase, { name: "document" }) && !path.scope.getBinding("document"))) {
            locGlobal = true;
          }
        }
        if (locGlobal) {
          try {
            var pageUrl = new URL(_sourceUrl);
            if (propName === "origin") return { done: [pageUrl.origin] };
            if (propName === "protocol") return { done: [pageUrl.protocol] };
            if (propName === "hostname") return { done: [pageUrl.hostname] };
            if (propName === "host") return { done: [pageUrl.host] };
          } catch (_) { /* sourceUrl wasn't a parseable URL — fall through */ }
        }
      }
      // 12f/g/h: this.prop — multiple sub-shapes (object literal method,
      // prototype method, ES6 class method/getter/constructor).
      if (memSubSkip < 4 && _t.isThisExpression(node.object)) {
        var funcPath = path.getFunctionParent();
        if (funcPath) {
          var funcParentPath = funcPath.parentPath;
          // 12f: this.prop in object literal method — pre-collect matching
          // property paths and dispatch to loop state.
          if (funcParentPath && _t.isObjectProperty(funcParentPath.node) && funcParentPath.node.value === funcPath.node) {
            var objExprPath = funcParentPath.parentPath;
            if (objExprPath && _t.isObjectExpression(objExprPath.node)) {
              var thisObjPaths = [];
              for (var ti = 0; ti < objExprPath.node.properties.length; ti++) {
                var tp = objExprPath.node.properties[ti];
                if (_t.isObjectProperty(tp) && !tp.computed) {
                  var tpKey = _t.isIdentifier(tp.key) ? tp.key.name :
                    (_t.isStringLiteral(tp.key) ? tp.key.value : null);
                  if (tpKey === propName) {
                    thisObjPaths.push(objExprPath.get("properties." + ti + ".value"));
                  }
                }
              }
              if (thisObjPaths.length > 0) {
                L.thisObjPaths = thisObjPaths;
                L.thisObjAcc = [];
                L.thisObjOi = 0;
                F.state = _RAV_MEM_THIS_OBJ_LOOP;
                continue;
              }
            }
          }
          // 12g: prototype method — direct call to external _resolveConstructorProperty.
          if (funcParentPath && _t.isAssignmentExpression(funcParentPath.node) && funcParentPath.node.right === funcPath.node) {
            var assignLeft = funcParentPath.node.left;
            if (_t.isMemberExpression(assignLeft) && _t.isMemberExpression(assignLeft.object) &&
                (_t.isIdentifier(assignLeft.object.property, { name: "prototype" }) ||
                 (_t.isStringLiteral(assignLeft.object.property) && assignLeft.object.property.value === "prototype"))) {
              var ctorIdent = assignLeft.object.object;
              var ctorName = _t.isIdentifier(ctorIdent) ? ctorIdent.name : null;
              if (ctorName) {
                var ctorVals = _resolveConstructorProperty(path, ctorName, propName, depth);
                if (ctorVals.length > 0) return { done: ctorVals };
              }
            }
          }
          // 12h: ES6 class method — getter inline + class constructor trace.
          if (_t.isClassMethod(funcPath.node) && _t.isClassBody(funcPath.parent)) {
            var classDecl = funcPath.parentPath.parentPath;
            if (classDecl && (_t.isClassDeclaration(classDecl.node) || _t.isClassExpression(classDecl.node)) && classDecl.node.id) {
              var className = classDecl.node.id.name;
              // Getter — collect ReturnStatement paths inside a matching getter.
              var classBody = classDecl.node.body.body;
              for (var gmi = 0; gmi < classBody.length; gmi++) {
                var gm = classBody[gmi];
                if (_t.isClassMethod(gm) && gm.kind === "get" && !gm.computed) {
                  var getterName = _t.isIdentifier(gm.key) ? gm.key.name :
                    (_t.isStringLiteral(gm.key) ? gm.key.value : null);
                  if (getterName === propName) {
                    var getterRetPaths = [];
                    var getterPath = classDecl.get("body.body." + gmi);
                    try {
                      getterPath.traverse(Object.assign({
                        ReturnStatement: function(retPath) {
                          if (retPath.node.argument) getterRetPaths.push(retPath.get("argument"));
                        },
                      }, _SKIP_NESTED_FUNCS));
                    } catch (e) { _resolver.collectError(e, "resolveGetter"); }
                    if (getterRetPaths.length > 0) {
                      L.getterRetPaths = getterRetPaths;
                      L.getterAcc = [];
                      L.getterGi = 0;
                      L.classDecl = classDecl;
                      L.className = className;
                      F.state = _RAV_MEM_THIS_GETTER_LOOP;
                      continue ravLoop;
                    }
                  }
                }
              }
              // Class constructor trace (external resolver — direct call ok).
              var classCtorVals = _resolveClassConstructorProperty(path, classDecl, className, propName, depth);
              if (classCtorVals.length > 0) return { done: classCtorVals };
            }
          }
        }
      }

      // 12i: Inline object property lookup. Resolve receiver to an object
      // node, find matching property, trace its value.
      if (memSubSkip < 5) {
        var objVals = _resolveToObject(path.get("object"), depth);
        if (objVals) {
          // Pre-collect literal-resolved values; non-literal matching property
          // paths get traced via state-machine.
          var memObjLiteral = [];
          var memObjPaths = [];
          for (var oi = 0; oi < objVals.properties.length; oi++) {
            var op = objVals.properties[oi];
            if (_t.isObjectProperty(op) && !op.computed) {
              var opKey = _t.isIdentifier(op.key) ? op.key.name :
                (_t.isStringLiteral(op.key) ? op.key.value : null);
              if (opKey === propName) {
                if (_t.isStringLiteral(op.value)) memObjLiteral.push(op.value.value);
                else if (_t.isNumericLiteral(op.value)) memObjLiteral.push(String(op.value.value));
                else if (objVals._path) memObjPaths.push(objVals._path.get("properties." + oi + ".value"));
              }
            }
          }
          if (memObjLiteral.length > 0) return { done: memObjLiteral };
          if (memObjPaths.length > 0) {
            L.memObjPaths = memObjPaths;
            L.memObjAcc = [];
            L.memObjOi = 0;
            F.state = _RAV_MEM_OBJ_LOOP;
            continue;
          }
        }
      }
      // 12j: Property assignments `obj.propName = value` via referencePaths.
      // Memoized per (bindingNode, propName). State-machine loop traces
      // each matching RHS.
      if (memSubSkip < 6 && _t.isIdentifier(node.object)) {
        var objBinding = path.scope.getBinding(node.object.name);
        if (objBinding) {
          var bindingNode = objBinding.path.node;
          var bindingMemo = _propAssignMemo.get(bindingNode);
          var refs = objBinding.referencePaths;
          if (bindingMemo && propName in bindingMemo) {
            if (bindingMemo[propName].length > 0) return { done: bindingMemo[propName] };
          } else {
            // Pre-collect matching RHS paths for state-machine loop.
            var paRhsPaths = [];
            for (var ri = 0; ri < refs.length; ri++) {
              var refParent = refs[ri].parent;
              if (_t.isMemberExpression(refParent) && refParent.object === refs[ri].node &&
                  !refParent.computed && _t.isIdentifier(refParent.property, { name: propName })) {
                var assignNode = refs[ri].parentPath ? refs[ri].parentPath.parent : null;
                if (assignNode && _t.isAssignmentExpression(assignNode) && assignNode.operator === "=" &&
                    assignNode.left === refParent) {
                  paRhsPaths.push(refs[ri].parentPath.parentPath.get("right"));
                }
              }
            }
            if (paRhsPaths.length > 0) {
              L.paRhsPaths = paRhsPaths;
              L.paBindingNode = bindingNode;
              L.paPropName = propName;
              L.paAcc = [];
              L.paPi = 0;
              F.state = _RAV_MEM_PROPASSIGN_LOOP;
              continue;
            }
            // No matching property assignments — cache empty and continue.
            if (!bindingMemo) { bindingMemo = {}; _propAssignMemo.set(bindingNode, bindingMemo); }
            bindingMemo[propName] = [];
          }

          // 12k: TypeScript-compiled enum / namespace pattern. Purely
          // structural — no _resolveAllValues recursion needed. Returns
          // first matching literal value.
          for (var rj = 0; rj < refs.length; rj++) {
            var ref = refs[rj];
            while (ref && !_t.isLogicalExpression(ref.node) && !_t.isFunction(ref.node)) {
              ref = ref.parentPath;
            }
            if (!ref || !_t.isLogicalExpression(ref.node)) continue;
            var callArg = ref.parentPath;
            if (!callArg || !callArg.isCallExpression()) continue;
            var argIdx = -1;
            for (var ai = 0; ai < callArg.node.arguments.length; ai++) {
              if (callArg.node.arguments[ai] === ref.node) { argIdx = ai; break; }
            }
            if (argIdx < 0) continue;
            var callFn = callArg.node.callee;
            if (!_t.isFunctionExpression(callFn) && !_t.isArrowFunctionExpression(callFn)) continue;
            if (argIdx >= callFn.params.length) continue;
            var paramId = callFn.params[argIdx];
            if (!_t.isIdentifier(paramId)) continue;
            var body = callFn.body;
            var stmts = _t.isBlockStatement(body) ? body.body : [body];
            for (var si = 0; si < stmts.length; si++) {
              var st = stmts[si];
              var expr = _t.isExpressionStatement(st) ? st.expression : st;
              var seq = _t.isSequenceExpression(expr) ? expr.expressions : [expr];
              for (var ei = 0; ei < seq.length; ei++) {
                var expr2 = seq[ei];
                if (!expr2 || !_t.isAssignmentExpression(expr2) || expr2.operator !== "=") continue;
                var lhs = expr2.left;
                if (!_t.isMemberExpression(lhs) || lhs.computed) continue;
                if (!_t.isIdentifier(lhs.object, { name: paramId.name })) continue;
                if (!_t.isIdentifier(lhs.property, { name: propName })) continue;
                if (_t.isStringLiteral(expr2.right)) return { done: [expr2.right.value] };
                if (_t.isNumericLiteral(expr2.right)) return { done: [String(expr2.right.value)] };
                if (_t.isTemplateLiteral(expr2.right) && expr2.right.expressions.length === 0 && expr2.right.quasis.length === 1) {
                  return { done: [expr2.right.quasis[0].value.cooked || expr2.right.quasis[0].value.raw] };
                }
              }
            }
          }
        }
      }

      // 12l: Inter-procedural — obj is a function parameter → call external
      // _resolveParamFromCallers to extract property from caller's args.
      if (memSubSkip < 7 && _t.isIdentifier(node.object)) {
        var objParamBinding = path.scope.getBinding(node.object.name);
        if (objParamBinding && objParamBinding.kind === "param") {
          var paramPropValues = _resolveParamFromCallers(objParamBinding, depth, propName);
          if (paramPropValues.length > 0) {
            _stats.interProcTraces++;
            return { done: paramPropValues };
          }
        }
      }

      // 12m: arr[i].prop — extract prop from all elements of resolved array.
      // Pre-collect matching property paths, then trace via state-machine loop.
      if (memSubSkip < 8 && _t.isMemberExpression(node.object) && node.object.computed) {
        var arrNode = _resolveToArray(path.get("object.object"), depth);
        if (arrNode) {
          var arrPropPaths = [];
          for (var ai = 0; ai < arrNode.elements.length; ai++) {
            var aElem = arrNode.elements[ai];
            if (aElem && _t.isObjectExpression(aElem)) {
              for (var api = 0; api < aElem.properties.length; api++) {
                var aep = aElem.properties[api];
                if (_t.isObjectProperty(aep) && !aep.computed && _getKeyName(aep.key) === propName) {
                  arrPropPaths.push(arrNode._path.get("elements." + ai + ".properties." + api + ".value"));
                }
              }
            }
          }
          if (arrPropPaths.length > 0) {
            L.arrPropPaths = arrPropPaths;
            L.arrPropAcc = [];
            L.arrPropAi = 0;
            F.state = _RAV_MEM_ARR_PROP_LOOP;
            continue;
          }
        }
      }
    }
  }

  // BRANCH 13: Computed member access (obj[key] / arr[idx]).
  // Sub-cases handled via compSubSkip:
  //   13a: Mutation-aware reads — scan receiver's binding refs for
  //        matching `recv[K] = V` assignments, resolve key+key match,
  //        accumulate matched RHS values.
  //   13b: Object lookup — resolve receiver to object, resolve key,
  //        return matching property values (+ all values for variable keys).
  //   13c: All-values fallback — receiver is object but key didn't resolve.
  //   13d: Array element loop — receiver is array.
  if (skip < 13 && _t.isMemberExpression(node) && node.computed) {
    var compSubSkip = L.compSubSkip || 0;

    // 13a: Mutation-aware reads. Setup: resolve read key first.
    // branchSkip=12 keeps branch 13 reachable on re-entry; compSubSkip
    // skips past completed sub-cases.
    if (compSubSkip < 1) {
      var muRootName = null;
      if (_t.isIdentifier(node.object)) muRootName = node.object.name;
      else if (_t.isMemberExpression(node.object) && !node.object.computed && _t.isIdentifier(node.object.object)) {
        muRootName = node.object.object.name;
      }
      if (muRootName) {
        var muBinding = path.scope.getBinding(muRootName);
        if (muBinding && muBinding.referencePaths) {
          L.muBinding = muBinding;
          L.muRootName = muRootName;
          L.branchSkip = 12;
          return { trace: path.get("property"), state: _RAV_COMP_MU_KEY_AFTER };
        }
      }
    }

    // 13b/c: Object lookup — resolve receiver to object, then resolve key.
    if (compSubSkip < 2) {
      var compObj = _resolveToObject(path.get("object"), depth);
      if (compObj) {
        L.compObj = compObj;
        L.branchSkip = 12;
        return { trace: path.get("property"), state: _RAV_COMP_OBJ_KEY_AFTER };
      }
    }

    // 13d: Array element loop.
    if (compSubSkip < 4) {
      var compArr = _resolveToArray(path.get("object"), depth);
      if (compArr && compArr._path && compArr.elements && compArr.elements.length > 0) {
        L.compArrPath = compArr._path;
        L.compArrElements = compArr.elements;
        L.compArrAcc = [];
        L.compArrEi = 0;
        L.branchSkip = 12;
        F.state = _RAV_COMP_ARR_LOOP;
        continue;
      }
    }
  }

  // No INIT branch matched / no result — return empty.
  return { done: [] };
      }

      case _RAV_ENC_AFTER: {
        var argVals = F.result;
        if (argVals.length === 0) return { done: [] };
        var fn = L.ecName === "encodeURIComponent" ? encodeURIComponent :
                 L.ecName === "encodeURI" ? encodeURI :
                 L.ecName === "decodeURIComponent" ? decodeURIComponent :
                 L.ecName === "decodeURI" ? decodeURI :
                 L.ecName === "btoa" ? (typeof btoa !== "undefined" ? btoa : function(s) { return Buffer.from(s, "binary").toString("base64"); }) :
                                       (typeof atob !== "undefined" ? atob : function(s) { return Buffer.from(s, "base64").toString("binary"); });
        var out = [];
        for (var avi = 0; avi < argVals.length; avi++) {
          if (typeof argVals[avi] === "string") {
            try { out.push(fn(argVals[avi])); } catch (_) { /* malformed input — skip */ }
          }
        }
        if (out.length > 0) return { done: out };
        // Encoded result empty — fall through to remaining INIT branches.
        L.branchSkip = 1;
        F.state = _RAV_INIT;
        continue;
      }

      case _RAV_TL_LOOP: {
        // L.tlQi is the next quasi index. After a sub-trace, F.result holds
        // the previous expression's resolved values; push its first value
        // (or bail if empty — every interpolation must resolve).
        if (L.tlQi > 0 && L.tlQi <= node.expressions.length) {
          if (F.result.length === 0) return { done: [] };
          L.tlParts.push(F.result[0]);
        }
        // Push the next quasi raw text and trace the corresponding
        // expression (if any). When all quasis are pushed and no more
        // expressions remain, the joined result is the template's value.
        while (L.tlQi < node.quasis.length) {
          L.tlParts.push(node.quasis[L.tlQi].value.cooked || node.quasis[L.tlQi].value.raw || "");
          if (L.tlQi < node.expressions.length) {
            var traceIdx = L.tlQi;
            L.tlQi++;
            return { trace: path.get("expressions." + traceIdx), state: _RAV_TL_LOOP };
          }
          L.tlQi++;
        }
        return { done: [L.tlParts.join("")] };
      }

      case _RAV_BIN_LOOP: {
        // Per-term trace loop for BinaryExpression `+` chain.
        if (L.binCi > 0) {
          if (F.result.length === 0) return { done: [] };
          L.binTermResults.push(F.result);
        }
        if (L.binCi < L.binParts.length) {
          var binTraceIdx = L.binCi;
          L.binCi++;
          return { trace: L.binParts[binTraceIdx], state: _RAV_BIN_LOOP };
        }
        var concatResult = [""];
        for (var tri = 0; tri < L.binTermResults.length; tri++) {
          var nextVals = [];
          var maxLen = Math.max(concatResult.length, L.binTermResults[tri].length);
          for (var zi = 0; zi < maxLen; zi++) {
            var l = concatResult[Math.min(zi, concatResult.length - 1)];
            var r = L.binTermResults[tri][Math.min(zi, L.binTermResults[tri].length - 1)];
            nextVals.push(String(l) + String(r));
          }
          concatResult = nextVals;
        }
        return { done: concatResult };
      }

      case _RAV_COND_LOOP: {
        // Per-branch trace for ConditionalExpression chain. Each consequent
        // (and the leaf alternate) is traced; results accumulated. Original
        // semantics: if accumulated values is empty after all branches,
        // fall through to the next INIT branch.
        if (L.condCi > 0) {
          L.condAcc = L.condAcc.concat(F.result);
        }
        if (L.condCi < L.condParts.length) {
          var condTraceIdx = L.condCi;
          L.condCi++;
          return { trace: L.condParts[condTraceIdx], state: _RAV_COND_LOOP };
        }
        if (L.condAcc.length > 0) return { done: L.condAcc };
        // Empty — fall through to next branch (LogicalExpression || etc.)
        F.state = _RAV_INIT;
        continue;
      }

      case _RAV_LOG_LOOP: {
        if (L.orOi > 0) {
          L.orAcc = L.orAcc.concat(F.result);
        }
        if (L.orOi < L.orParts.length) {
          var orTraceIdx = L.orOi;
          L.orOi++;
          return { trace: L.orParts[orTraceIdx], state: _RAV_LOG_LOOP };
        }
        if (L.orAcc.length > 0) return { done: L.orAcc };
        F.state = _RAV_INIT;
        continue;
      }

      case _RAV_NEW_REQ_AFTER: {
        // Passthrough — the leaf input's resolved values ARE the new
        // Request's URL values.
        return { done: F.result };
      }

      case _RAV_NEW_URL_INPUT_AFTER: {
        L.urlInputVals = F.result;
        if (L.urlInputVals.length === 0) return { done: [] };
        if (L.urlHasBase) {
          return { trace: path.get("arguments.1"), state: _RAV_NEW_URL_BASE_AFTER };
        }
        // No base — single-arg new URL(input). Try to construct each.
        var urlOutSingle = [];
        for (var uli2 = 0; uli2 < L.urlInputVals.length; uli2++) {
          try {
            var absOnly = new URL(String(L.urlInputVals[uli2])).href;
            if (urlOutSingle.indexOf(absOnly) < 0) urlOutSingle.push(absOnly);
          } catch (_) { /* relative input throws; matches runtime */ }
        }
        return { done: urlOutSingle };
      }

      case _RAV_NEW_URL_BASE_AFTER: {
        var urlBaseVals = F.result;
        if (urlBaseVals.length === 0) return { done: [] };
        var urlOut = [];
        for (var uli3 = 0; uli3 < L.urlInputVals.length; uli3++) {
          var urlInStr = String(L.urlInputVals[uli3]);
          for (var ulb = 0; ulb < urlBaseVals.length; ulb++) {
            try {
              var abs = new URL(urlInStr, String(urlBaseVals[ulb])).href;
              if (urlOut.indexOf(abs) < 0) urlOut.push(abs);
            } catch (_) { /* (input, base) pair throws at runtime too */ }
          }
        }
        return { done: urlOut };
      }

      case _RAV_USP_LOOP: {
        if (L.uspUpi > 0) {
          if (F.result.length === 0) return { done: [] };
          L.uspParts.push(encodeURIComponent(L.uspKeys[L.uspUpi - 1]) + "=" + String(F.result[0]));
        }
        if (L.uspUpi < L.uspProps.length) {
          var uspIdx = L.uspUpi;
          L.uspUpi++;
          return { trace: path.get("arguments.0.properties." + uspIdx + ".value"), state: _RAV_USP_LOOP };
        }
        return { done: [L.uspParts.join("&")] };
      }

      case _RAV_CALL_RETVAL_AFTER: {
        // F.result is array from _rcrvStep — call-return values for the
        // current CallExpression. If non-empty, that's the resolved value;
        // otherwise advance to next CallExpression sub-case (string method
        // passthrough, .getAttribute roundtrip, .join, etc.).
        if (F.result && F.result.length > 0) return { done: F.result };
        // Empty — fall through. branchSkip drops below 10 so branch 10
        // re-enters; callSubSkip stays at 1 so 10a is skipped, allowing
        // 10b/c/d to try.
        L.branchSkip = 9;
        F.state = _RAV_INIT;
        continue;
      }

      case _RAV_CALL_RECV_AFTER: {
        // String method passthrough: receiver leaf has been traced.
        // If non-empty, those ARE the call-expression's values (passthrough);
        // otherwise fall through to remaining INIT branches.
        if (F.result.length > 0) return { done: F.result };
        L.branchSkip = 11;
        F.state = _RAV_INIT;
        continue;
      }

      case _RAV_CALL_GA_LOOP: {
        // setAttribute roundtrip — accumulate each matching call's value.
        if (L.gaGi > 0) {
          L.gaSetVals = L.gaSetVals.concat(F.result);
        }
        if (L.gaGi < L.gaCallPaths.length) {
          var gaIdx = L.gaGi;
          L.gaGi++;
          return { trace: L.gaCallPaths[gaIdx].get("arguments.1"), state: _RAV_CALL_GA_LOOP };
        }
        if (L.gaSetVals.length > 0) return { done: L.gaSetVals };
        L.branchSkip = 11;
        F.state = _RAV_INIT;
        continue;
      }

      case _RAV_CALL_JOIN_LOOP: {
        if (L.joinJi > 0) {
          if (L._joinPrevWasTrace) {
            if (F.result.length === 0) return { done: [] };
            L.joinParts.push(String(F.result[0]));
          }
        }
        L._joinPrevWasTrace = false;
        while (L.joinJi < L.joinArrNode.elements.length) {
          var joinElem = L.joinArrNode.elements[L.joinJi];
          if (joinElem === null) {
            L.joinParts.push("");
            L.joinJi++;
            continue;
          }
          var joinIdx = L.joinJi;
          L.joinJi++;
          L._joinPrevWasTrace = true;
          return { trace: L.joinArrNode._path.get("elements." + joinIdx), state: _RAV_CALL_JOIN_LOOP };
        }
        return { done: [L.joinParts.join(L.joinSep)] };
      }

      case _RAV_IDENT_GLOBAL_AFTER: {
        // Global-fallback chain leaf trace. Pass through.
        return { done: F.result };
      }

      case _RAV_IDENT_CONST_AFTER: {
        // Constant init alias-chain leaf. Stats counter on success.
        if (F.result.length > 0) {
          _stats.resolvedUrls++;
          return { done: F.result };
        }
        // Empty result — fall through to remaining sub-cases of branch
        // 11 (param/non-const/for-in/for-of/destructure). Re-enter at
        // INIT with identSubSkip past the failed const-init sub-case;
        // branchSkip stays < 11 so branch 11 re-enters.
        L.branchSkip = 0;
        L.identSubSkip = 2;
        F.state = _RAV_INIT;
        continue;
      }

      case _RAV_IDENT_NONCONST_INIT_AFTER: {
        // Non-constant init traced — accumulate, then loop CVs.
        L.identNcAcc = L.identNcAcc.concat(F.result);
        L.identNcPhase = "cv";
        F.state = _RAV_IDENT_NONCONST_CV_LOOP;
        continue;
      }

      case _RAV_IDENT_NONCONST_CV_LOOP: {
        // Per-CV trace loop. If a previous trace just returned, accumulate.
        if (L.identNcPhase === "cv-after") {
          L.identNcAcc = L.identNcAcc.concat(F.result);
          L.identNcPhase = "cv";
        }
        // Find the next AssignmentExpression CV with iterative alias walk.
        while (L.identNcCi < L.identBinding.constantViolations.length) {
          var violation = L.identBinding.constantViolations[L.identNcCi];
          L.identNcCi++;
          if (_t.isAssignmentExpression(violation.node) && violation.node.operator === "=") {
            var rhsPath = violation.get("right");
            var rhsNode = violation.node.right;
            while (_t.isIdentifier(rhsNode)) {
              var rhsAliasBinding = rhsPath.scope.getBinding(rhsNode.name);
              if (!rhsAliasBinding || !rhsAliasBinding.constant) break;
              if (!_t.isVariableDeclarator(rhsAliasBinding.path.node) || !rhsAliasBinding.path.node.init) break;
              rhsPath = rhsAliasBinding.path.get("init");
              rhsNode = rhsAliasBinding.path.node.init;
            }
            L.identNcPhase = "cv-after";
            return { trace: rhsPath, state: _RAV_IDENT_NONCONST_CV_LOOP };
          }
        }
        if (L.identNcAcc.length > 0) return { done: L.identNcAcc };
        // Empty CV result — fall through to remaining sub-cases of branch
        // 11 (for-in/of, destructure). branchSkip stays < 11 so branch 11
        // re-enters; identSubSkip skips past completed sub-cases up to 11d.
        L.branchSkip = 0;
        L.identSubSkip = 4;
        F.state = _RAV_INIT;
        continue;
      }

      case _RAV_IDENT_FOROF_LOOP: {
        // for-of element trace loop. Accumulate each element's resolved values.
        if (L.iterEli > 0) {
          L.iterAcc = L.iterAcc.concat(F.result);
        }
        // Skip null/hole elements.
        while (L.iterEli < L.iterArrElements.length && !L.iterArrElements[L.iterEli]) L.iterEli++;
        if (L.iterEli < L.iterArrElements.length) {
          var iterIdx = L.iterEli;
          L.iterEli++;
          return { trace: L.iterArrPath.get("elements." + iterIdx), state: _RAV_IDENT_FOROF_LOOP };
        }
        if (L.iterAcc.length > 0) return { done: L.iterAcc };
        L.branchSkip = 12;
        F.state = _RAV_INIT;
        continue;
      }

      case _RAV_IDENT_DESTR_ARR_AFTER:
      case _RAV_IDENT_DESTR_ARR_RESOLVED_AFTER:
      case _RAV_IDENT_DESTR_OBJ_AFTER:
      case _RAV_IDENT_DESTR_OBJ_RESOLVED_AFTER: {
        // Destructured element/property — passthrough.
        return { done: F.result };
      }

      case _RAV_MEM_URL_PROP_AFTER: {
        // URL instance property extraction. Apply per resolved URL string.
        var urlAbs = F.result;
        if (urlAbs.length > 0) {
          var urlPropOut = [];
          for (var upi = 0; upi < urlAbs.length; upi++) {
            try {
              var u = new URL(String(urlAbs[upi]));
              var v = u[L.urlPropName];
              if (v != null && urlPropOut.indexOf(v) < 0) urlPropOut.push(typeof v === "string" ? v : String(v));
            } catch (_) { /* not a parseable URL; runtime would also fail */ }
          }
          if (urlPropOut.length > 0) return { done: urlPropOut };
        }
        // Empty — fall through to NEXT MemberExpression sub-case.
        L.memSubSkip = 1;
        F.state = _RAV_INIT;
        continue;
      }

      case _RAV_MEM_DS_LOOP: {
        if (L.dsDi > 0) {
          L.dsAcc = L.dsAcc.concat(F.result);
        }
        if (L.dsDi < L.dsRhsPaths.length) {
          var dsIdx = L.dsDi;
          L.dsDi++;
          return { trace: L.dsRhsPaths[dsIdx], state: _RAV_MEM_DS_LOOP };
        }
        if (L.dsAcc.length > 0) return { done: L.dsAcc };
        L.memSubSkip = 2;
        F.state = _RAV_INIT;
        continue;
      }

      case _RAV_MEM_THIS_OBJ_LOOP: {
        // this.prop in object literal method — trace each matching property.
        if (L.thisObjOi > 0) {
          L.thisObjAcc = L.thisObjAcc.concat(F.result);
        }
        if (L.thisObjOi < L.thisObjPaths.length) {
          var thisIdx = L.thisObjOi;
          L.thisObjOi++;
          return { trace: L.thisObjPaths[thisIdx], state: _RAV_MEM_THIS_OBJ_LOOP };
        }
        if (L.thisObjAcc.length > 0) return { done: L.thisObjAcc };
        L.memSubSkip = 4;
        F.state = _RAV_INIT;
        continue;
      }

      case _RAV_MEM_THIS_GETTER_LOOP: {
        if (L.getterGi > 0) {
          L.getterAcc = L.getterAcc.concat(F.result);
        }
        if (L.getterGi < L.getterRetPaths.length) {
          var getterIdx = L.getterGi;
          L.getterGi++;
          return { trace: L.getterRetPaths[getterIdx], state: _RAV_MEM_THIS_GETTER_LOOP };
        }
        if (L.getterAcc.length > 0) return { done: L.getterAcc };
        var classCtorValsAfter = _resolveClassConstructorProperty(path, L.classDecl, L.className, propName, depth);
        if (classCtorValsAfter.length > 0) return { done: classCtorValsAfter };
        L.memSubSkip = 4;
        F.state = _RAV_INIT;
        continue;
      }

      case _RAV_MEM_OBJ_LOOP: {
        if (L.memObjOi > 0) {
          L.memObjAcc = L.memObjAcc.concat(F.result);
        }
        if (L.memObjOi < L.memObjPaths.length) {
          var memObjIdx = L.memObjOi;
          L.memObjOi++;
          return { trace: L.memObjPaths[memObjIdx], state: _RAV_MEM_OBJ_LOOP };
        }
        if (L.memObjAcc.length > 0) return { done: L.memObjAcc };
        L.memSubSkip = 5;
        F.state = _RAV_INIT;
        continue;
      }

      case _RAV_MEM_PROPASSIGN_LOOP: {
        if (L.paPi > 0) {
          L.paAcc = L.paAcc.concat(F.result);
        }
        if (L.paPi < L.paRhsPaths.length) {
          var paIdx = L.paPi;
          L.paPi++;
          return { trace: L.paRhsPaths[paIdx], state: _RAV_MEM_PROPASSIGN_LOOP };
        }
        var paBindingMemo = _propAssignMemo.get(L.paBindingNode);
        if (!paBindingMemo) { paBindingMemo = {}; _propAssignMemo.set(L.paBindingNode, paBindingMemo); }
        paBindingMemo[L.paPropName] = L.paAcc;
        if (L.paAcc.length > 0) return { done: L.paAcc };
        L.memSubSkip = 6;
        F.state = _RAV_INIT;
        continue;
      }

      case _RAV_MEM_ARR_PROP_LOOP: {
        if (L.arrPropAi > 0) {
          L.arrPropAcc = L.arrPropAcc.concat(F.result);
        }
        if (L.arrPropAi < L.arrPropPaths.length) {
          var arrPropIdx = L.arrPropAi;
          L.arrPropAi++;
          return { trace: L.arrPropPaths[arrPropIdx], state: _RAV_MEM_ARR_PROP_LOOP };
        }
        if (L.arrPropAcc.length > 0) return { done: L.arrPropAcc };
        L.branchSkip = 13;
        L.memSubSkip = 0;
        F.state = _RAV_INIT;
        continue;
      }

      case _RAV_COMP_MU_KEY_AFTER: {
        // Read key resolved — set up the mutation-aware loop over receiver
        // binding's referencePaths.
        L.muReadKeyVals = F.result;
        if (L.muReadKeyVals.length === 0) {
          // Read key didn't resolve — skip 13a, fall through to 13b.
          L.compSubSkip = 1;
          F.state = _RAV_INIT;
          continue;
        }
        L.muRefs = L.muBinding.referencePaths;
        L.muRefIdx = 0;
        L.muVals = [];
        F.state = _RAV_COMP_MU_LOOP;
        continue;
      }

      case _RAV_COMP_MU_LOOP: {
        // Find the next matching `recv[K] = V` assignment. Returns trace
        // for the assignment key; key match handled in COMP_MU_AKEY_AFTER.
        while (L.muRefIdx < L.muRefs.length) {
          var muRef = L.muRefs[L.muRefIdx];
          L.muRefIdx++;
          var muRecvPath = muRef.parentPath;
          if (!muRecvPath) continue;
          if (_t.isMemberExpression(node.object) && !node.object.computed) {
            if (!muRecvPath.isMemberExpression() || muRecvPath.node.object !== muRef.node || muRecvPath.node.computed) continue;
            if (!_t.isIdentifier(muRecvPath.node.property, { name: node.object.property.name })) continue;
            muRecvPath = muRecvPath.parentPath;
            if (!muRecvPath) continue;
          }
          if (!muRecvPath.isMemberExpression() || !muRecvPath.node.computed) continue;
          var muAssign = muRecvPath.parent;
          if (!muAssign || !_t.isAssignmentExpression(muAssign) || muAssign.operator !== "=" || muAssign.left !== muRecvPath.node) continue;
          // Save the current ref's recvPath so the AFTER state can resolve the RHS.
          L.muCurRecvPath = muRecvPath;
          return { trace: muRecvPath.get("property"), state: _RAV_COMP_MU_AKEY_AFTER };
        }
        // All refs exhausted.
        if (L.muVals.length > 0) return { done: L.muVals };
        // Fall through to 13b.
        L.compSubSkip = 1;
        F.state = _RAV_INIT;
        continue;
      }

      case _RAV_COMP_MU_AKEY_AFTER: {
        // Assignment key resolved — match against read keys.
        var muAssignKeyVals = F.result;
        if (muAssignKeyVals.length > 0) {
          var muMatched = false;
          for (var mki = 0; mki < L.muReadKeyVals.length && !muMatched; mki++) {
            for (var mai = 0; mai < muAssignKeyVals.length && !muMatched; mai++) {
              if (String(L.muReadKeyVals[mki]) === String(muAssignKeyVals[mai])) muMatched = true;
            }
          }
          if (muMatched) {
            return { trace: L.muCurRecvPath.parentPath.get("right"), state: _RAV_COMP_MU_RHS_AFTER };
          }
        }
        // No match — continue loop.
        F.state = _RAV_COMP_MU_LOOP;
        continue;
      }

      case _RAV_COMP_MU_RHS_AFTER: {
        // RHS values resolved for matched key — accumulate, continue loop.
        L.muVals = L.muVals.concat(F.result);
        F.state = _RAV_COMP_MU_LOOP;
        continue;
      }

      case _RAV_COMP_OBJ_KEY_AFTER: {
        // Object key resolved — try matching specific properties first.
        L.compKeyVals = F.result;
        L.compResolvedVals = [];
        L.compObjKi = 0;  // index over key vals
        L.compObjVi = 0;  // index over compObj.properties (per key)
        if (L.compKeyVals.length > 0) {
          F.state = _RAV_COMP_OBJ_PROP_LOOP;
          continue;
        }
        // Key didn't resolve — fall through to all-values fallback (13c).
        F.state = _RAV_COMP_OBJ_ALL_LOOP;
        L.compObjAllVi = 0;
        L.compObjAllAcc = [];
        continue;
      }

      case _RAV_COMP_OBJ_PROP_LOOP: {
        // Specific-key property match loop. For each (key, prop) pair
        // where prop's name matches key's value, trace the property's value.
        if (L.compObjKi > 0 || L.compObjVi > 0) {
          // Just got back from a sub-trace. Accumulate.
          L.compResolvedVals = L.compResolvedVals.concat(F.result);
        }
        // Find next matching (key, prop) pair.
        while (L.compObjKi < L.compKeyVals.length) {
          while (L.compObjVi < L.compObj.properties.length) {
            var vp = L.compObj.properties[L.compObjVi];
            var vpIdx = L.compObjVi;
            L.compObjVi++;
            if (_t.isObjectProperty(vp) && !vp.computed && _getKeyName(vp.key) === String(L.compKeyVals[L.compObjKi])) {
              if (L.compObj._path) {
                return { trace: L.compObj._path.get("properties." + vpIdx + ".value"), state: _RAV_COMP_OBJ_PROP_LOOP };
              }
            }
          }
          L.compObjKi++;
          L.compObjVi = 0;
        }
        // Specific-key matches exhausted.
        if (L.compResolvedVals.length > 0) {
          // For variable keys (not literal), also include remaining property values for discovery.
          if (!_t.isStringLiteral(node.property) && !_t.isNumericLiteral(node.property)) {
            L.compObjDpi = 0;
            L.compObjDiscAcc = L.compResolvedVals;
            F.state = _RAV_COMP_OBJ_DISC_LOOP;
            continue;
          }
          return { done: L.compResolvedVals };
        }
        // No specific match — fall through to all-values fallback.
        L.compObjAllVi = 0;
        L.compObjAllAcc = [];
        F.state = _RAV_COMP_OBJ_ALL_LOOP;
        continue;
      }

      case _RAV_COMP_OBJ_DISC_LOOP: {
        // Discovery: include all property values (deduped) when key was variable.
        if (L.compObjDpi > 0) {
          for (var dvi = 0; dvi < F.result.length; dvi++) {
            if (L.compObjDiscAcc.indexOf(F.result[dvi]) < 0) L.compObjDiscAcc.push(F.result[dvi]);
          }
        }
        while (L.compObjDpi < L.compObj.properties.length) {
          var dp = L.compObj.properties[L.compObjDpi];
          var dpIdx = L.compObjDpi;
          L.compObjDpi++;
          if (_t.isObjectProperty(dp) && !dp.computed) {
            if (L.compObj._path) {
              return { trace: L.compObj._path.get("properties." + dpIdx + ".value"), state: _RAV_COMP_OBJ_DISC_LOOP };
            }
          }
        }
        return { done: L.compObjDiscAcc };
      }

      case _RAV_COMP_OBJ_ALL_LOOP: {
        // All-property fallback when key didn't resolve (or no specific match).
        if (L.compObjAllVi > 0) {
          L.compObjAllAcc = L.compObjAllAcc.concat(F.result);
        }
        while (L.compObjAllVi < L.compObj.properties.length) {
          var fp = L.compObj.properties[L.compObjAllVi];
          var fpIdx = L.compObjAllVi;
          L.compObjAllVi++;
          if (_t.isObjectProperty(fp) && !fp.computed) {
            if (L.compObj._path) {
              return { trace: L.compObj._path.get("properties." + fpIdx + ".value"), state: _RAV_COMP_OBJ_ALL_LOOP };
            }
          }
        }
        if (L.compObjAllAcc.length > 0) return { done: L.compObjAllAcc };
        // Fall through to 13d (array element loop).
        L.compSubSkip = 3;
        F.state = _RAV_INIT;
        continue;
      }

      case _RAV_COMP_ARR_LOOP: {
        // Array element loop.
        if (L.compArrEi > 0) {
          L.compArrAcc = L.compArrAcc.concat(F.result);
        }
        // Skip null/hole elements.
        while (L.compArrEi < L.compArrElements.length && !L.compArrElements[L.compArrEi]) L.compArrEi++;
        if (L.compArrEi < L.compArrElements.length) {
          var compArrIdx = L.compArrEi;
          L.compArrEi++;
          return { trace: L.compArrPath.get("elements." + compArrIdx), state: _RAV_COMP_ARR_LOOP };
        }
        if (L.compArrAcc.length > 0) return { done: L.compArrAcc };
        // All sub-cases exhausted.
        return { done: [] };
      }

      default:
        return { done: [] };
    }
  }
}

// Resolve a call expression's callee to its function path (with scope info).
// Covers the common cases: identifier → scope binding, member expr → object property.
// Returns the Babel path to the function node, or null.
// State IDs for _rcfpStep (state machine for _resolveCalleeFuncPath).
var _RCFP_INIT = 0;
var _RCFP_OBJ_AFTER = 100;

function _rcfpMakeFrame(path, node, depth, stepFn, shortCircuit, guardPrefix, defaultResult) {
  return {
    path: path, node: node, depth: depth || 0,
    state: _RCFP_INIT, L: {}, result: null,
    stepFn: stepFn || _rcfpStep,
    makeFrame: _rcfpMakeFrame,
    shortCircuit: shortCircuit,
    guardPrefix: guardPrefix !== undefined ? guardPrefix : "C",
    defaultResult: defaultResult !== undefined ? defaultResult : null,
  };
}

function _resolveCalleeFuncPath(callPath, depth) {
  var node = callPath && callPath.node;
  if (!node) return null;
  if (!_resolver.guard("C", node)) return null;
  var initialFrame = _rcfpMakeFrame(callPath, node, depth || 0,
    _rcfpStep, null, "C", null);
  return _runResolverStack(initialFrame);
}

function _rcfpStep(F) {
  var callPath = F.path, depth = F.depth, L = F.L;
  while (true) {
    switch (F.state) {
      case _RCFP_INIT: {
        var callee = callPath.node.callee;
        // Branch 1 (Identifier): direct function binding.
        if (_t.isIdentifier(callee)) {
          var binding = callPath.scope.getBinding(callee.name);
          if (binding) {
            if (_t.isFunctionDeclaration(binding.path.node)) return { done: binding.path };
            if (_t.isVariableDeclarator(binding.path.node) && binding.path.node.init) {
              var init = binding.path.node.init;
              if (_t.isFunctionExpression(init) || _t.isArrowFunctionExpression(init))
                return { done: binding.path.get("init") };
            }
          }
          // Branch 3 (Identifier aliases a callable): `var f = X.bind(Y); f()`
          // or `var f = obj.method; f()`. Only follow through CallExpression
          // init (e.g. .bind chains) — direct function-init already covered
          // by branch 1 above.
          if (binding && _t.isVariableDeclarator(binding.path.node) && binding.path.node.init) {
            var initNode2 = binding.path.node.init;
            if (_t.isCallExpression(initNode2)) {
              // _resolveExprToFunctionPath is iterative; one wrapper frame.
              return { done: _resolveExprToFunctionPath(binding.path.get("init"), depth || 0) };
            }
          }
          // Branch 4 (no binding — unbound identifier resolves via global
          // assignments): handles `window.X = function() {…}` then a later
          // call site referencing `X` directly. Module-scope aliases via
          // _globalAssignments per the analyser's global tracking.
          if (!binding) {
            var globalDef = _globalAssignments[callee.name];
            if (globalDef && globalDef.valueNode && globalDef.valuePath) {
              if (_t.isFunctionExpression(globalDef.valueNode) || _t.isArrowFunctionExpression(globalDef.valueNode)) {
                return { done: globalDef.valuePath };
              }
              if (_t.isCallExpression(globalDef.valueNode)) {
                var gRetFunc = _resolveCallReturnToFunction(globalDef.valuePath, depth || 0);
                if (gRetFunc && gRetFunc._path) return { done: gRetFunc._path };
              }
            }
          }
          // Branch 5 (higher-order: `var f = factory()`): when no direct
          // function-init match but the binding is `var = call(...)`, the
          // call's return may be a function per ECMA § 13.3.
          if (binding && _t.isVariableDeclarator(binding.path.node) && binding.path.node.init &&
              _t.isCallExpression(binding.path.node.init)) {
            var bRetFunc = _resolveCallReturnToFunction(binding.path.get("init"), depth || 0);
            if (bRetFunc && bRetFunc._path) return { done: bRetFunc._path };
          }
          return { done: null };
        }
        // Branch 2 (MemberExpression callee).
        if (_t.isMemberExpression(callee) && !callee.computed) {
          var propName = _t.isIdentifier(callee.property) ? callee.property.name : null;
          if (!propName) return { done: null };
          L.propName = propName;
          // Function.prototype.call / .apply — `f.call(thisArg, ...args)`
          // invokes f with args. Native ECMAScript semantics: callee.object
          // IS the function. Resolve receiver to function path.
          if (propName === "call" || propName === "apply") {
            var recvFunc = _resolveExprToFunctionPath(callPath.get("callee.object"), depth || 0);
            if (recvFunc) return { done: recvFunc };
          }
          // Dispatch _resolveToObject via shared driver — keeps the JS
          // call stack flat across mutual call/object resolution.
          return {
            trace: callPath.get("callee.object"),
            state: _RCFP_OBJ_AFTER,
            stepFn: _rtoiStep, makeFrame: _rtoiMakeFrame,
            shortCircuit: _rtoiShortCircuit, guardPrefix: "T", defaultResult: null,
          };
        }
        return { done: null };
      }

      case _RCFP_OBJ_AFTER: {
        var callee2 = callPath.node.callee;
        var propName2 = L.propName;
        var objNode = F.result;
        if (objNode) {
          for (var i = 0; i < objNode.properties.length; i++) {
            var prop = objNode.properties[i];
            if (!_t.isObjectProperty(prop) || prop.computed) continue;
            var key = _t.isIdentifier(prop.key) ? prop.key.name :
              (_t.isStringLiteral(prop.key) ? prop.key.value : null);
            if (key === propName2 && (_t.isFunctionExpression(prop.value) || _t.isArrowFunctionExpression(prop.value))) {
              return { done: objNode._path ? objNode._path.get("properties." + i + ".value") : null };
            }
          }
        }
        // Class instance method: `new C().method()` per § 15.7
        // (ClassDeclaration). Resolve the constructor identifier to its
        // class declaration and look up the method.
        if (_t.isNewExpression(callee2.object) && _t.isIdentifier(callee2.object.callee)) {
          var ctorName = callee2.object.callee.name;
          var ctorBind = callPath.scope.getBinding(ctorName);
          if (ctorBind && ctorBind.path) {
            var classNode = ctorBind.path.node;
            if (_t.isClassDeclaration(classNode) || _t.isClassExpression(classNode)) {
              var classBody = classNode.body;
              if (classBody && classBody.body) {
                for (var ci = 0; ci < classBody.body.length; ci++) {
                  var classMember = classBody.body[ci];
                  if (!_t.isClassMethod(classMember) || classMember.computed) continue;
                  if (classMember.kind !== "method") continue;
                  var memberKey = _t.isIdentifier(classMember.key) ? classMember.key.name :
                    (_t.isStringLiteral(classMember.key) ? classMember.key.value : null);
                  if (memberKey === propName2) {
                    return { done: ctorBind.path.get("body.body." + ci) };
                  }
                }
              }
            }
          }
        }
        // Variable bound to a class instance: `var c = new C(); c.method()`
        // — same lookup as above but obj is an Identifier whose binding
        // is `var c = new C()`.
        if (_t.isIdentifier(callee2.object)) {
          var instBind = callPath.scope.getBinding(callee2.object.name);
          if (instBind && _t.isVariableDeclarator(instBind.path.node) &&
              instBind.path.node.init && _t.isNewExpression(instBind.path.node.init) &&
              _t.isIdentifier(instBind.path.node.init.callee)) {
            var instCtorName = instBind.path.node.init.callee.name;
            var instCtorBind = callPath.scope.getBinding(instCtorName);
            if (instCtorBind && instCtorBind.path) {
              var iClassNode = instCtorBind.path.node;
              if (_t.isClassDeclaration(iClassNode) || _t.isClassExpression(iClassNode)) {
                var iClassBody = iClassNode.body;
                if (iClassBody && iClassBody.body) {
                  for (var ici = 0; ici < iClassBody.body.length; ici++) {
                    var iMember = iClassBody.body[ici];
                    if (!_t.isClassMethod(iMember) || iMember.computed) continue;
                    if (iMember.kind !== "method") continue;
                    var iKey = _t.isIdentifier(iMember.key) ? iMember.key.name :
                      (_t.isStringLiteral(iMember.key) ? iMember.key.value : null);
                    if (iKey === propName2) {
                      return { done: instCtorBind.path.get("body.body." + ici) };
                    }
                  }
                }
              }
            }
          }
        }
        // IIFE-returned property: `var e = function(){…n.X=fn…return n}()`
        // followed by `e.X(…)`. The IIFE's return value is an object
        // whose internal mutations include `X`; resolve through.
        if (_t.isIdentifier(callee2.object)) {
          var iifeBind = callPath.scope.getBinding(callee2.object.name);
          if (iifeBind && _t.isVariableDeclarator(iifeBind.path.node) &&
              iifeBind.path.node.init && _t.isCallExpression(iifeBind.path.node.init)) {
            var iifePropFn = _resolveIIFEReturnedProperty(iifeBind.path.get("init"), propName2);
            if (iifePropFn) return { done: iifePropFn };
          }
          // Global-assigned IIFE: `window.X = function(){…}()` then `X.method()`.
          if (!iifeBind) {
            var iifeGDef = _globalAssignments[callee2.object.name];
            if (iifeGDef && iifeGDef.valuePath && _t.isCallExpression(iifeGDef.valueNode)) {
              var iifeGFn = _resolveIIFEReturnedProperty(iifeGDef.valuePath, propName2);
              if (iifeGFn) return { done: iifeGFn };
            }
          }
        }
        // Global-alias unwrap: `window.jQuery = jQuery` (chained
        // assignments) — the receiver's binding isn't local so resolve
        // through `_globalAssignments` to find the underlying binding,
        // then continue with the standard ref-walk routes below.
        if (_t.isIdentifier(callee2.object) && !callPath.scope.getBinding(callee2.object.name)) {
          var aliasGDef = _globalAssignments[callee2.object.name];
          if (aliasGDef && aliasGDef.valueNode) {
            var aliasVal = aliasGDef.valueNode;
            while (_t.isAssignmentExpression(aliasVal)) aliasVal = aliasVal.right;
            if (_t.isIdentifier(aliasVal)) {
              var aliasBind = aliasGDef.valuePath.scope.getBinding(aliasVal.name);
              if (aliasBind) {
                // Re-run the same search on the aliased binding's refs.
                callee2 = { object: { type: "Identifier", name: aliasVal.name }, property: callee2.property, computed: false };
                // Note: synthetic callee2 — only object.name is used by the
                // ref-walk below, so it's safe.
              }
            }
          }
        }
        // `obj.key = function/arrow` mutation pattern — walk the
        // receiver identifier's referencePaths for assignment expressions
        // whose left side is `obj.key` and right side is a function.
        // Native ECMAScript scope/value flow — same mechanism that backs
        // module-exports (`t.fn = function() {...}`) and prototype
        // patterns (`Ctor.prototype.method = function() {...}`).
        if (_t.isIdentifier(callee2.object)) {
          var rcvBind = callPath.scope.getBinding(callee2.object.name);
          if (rcvBind && rcvBind.referencePaths) {
            for (var ri = 0; ri < rcvBind.referencePaths.length; ri++) {
              var rp = rcvBind.referencePaths[ri];
              var rpParent = rp.parent;
              // Direct assignment `obj.key = …`
              if (rpParent && _t.isMemberExpression(rpParent) && rpParent.object === rp.node &&
                  !rpParent.computed && _t.isIdentifier(rpParent.property, { name: propName2 })) {
                var assn = rp.parentPath ? rp.parentPath.parent : null;
                if (assn && _t.isAssignmentExpression(assn) && assn.operator === "=" && assn.left === rpParent) {
                  // Chained-assignment unwrap per § 13.15.4: in
                  // `a = b = expr`, the rightmost expr provides the value;
                  // intermediate AssignmentExpressions cascade their result.
                  // Walk through nested AssignmentExpressions to reach
                  // the actual RHS function/factory.
                  var rhs = assn.right;
                  var rhsPath = rp.parentPath.parentPath.get("right");
                  while (_t.isAssignmentExpression(rhs) && rhs.operator === "=") {
                    rhs = rhs.right;
                    rhsPath = rhsPath.get("right");
                  }
                  if (_t.isFunctionExpression(rhs) || _t.isArrowFunctionExpression(rhs)) {
                    return { done: rhsPath };
                  }
                  // Factory-call value: `obj.key = factory(...)` per § 13.3.
                  if (_t.isCallExpression(rhs)) {
                    var rhsRetFn = _resolveCallReturnToFunction(rhsPath, depth + 1);
                    if (rhsRetFn && rhsRetFn._path) return { done: rhsRetFn._path };
                    if (rhsRetFn && rhsRetFn.node && _t.isFunction(rhsRetFn.node)) return { done: rhsPath };
                  }
                }
              }
              // Object.assign(obj, {key: value}) — built-in property
              // propagation per ECMA § 20.1.2.1. Scope-checked on the
              // unshadowed `Object` global.
              if (rpParent && _t.isCallExpression(rpParent) &&
                  rpParent.arguments[0] === rp.node &&
                  _t.isMemberExpression(rpParent.callee) && !rpParent.callee.computed &&
                  _t.isIdentifier(rpParent.callee.object, { name: "Object" }) &&
                  !rp.scope.getBinding("Object") &&
                  _t.isIdentifier(rpParent.callee.property, { name: "assign" })) {
                for (var oai = 1; oai < rpParent.arguments.length; oai++) {
                  var oas = rpParent.arguments[oai];
                  if (!_t.isObjectExpression(oas)) continue;
                  for (var oap = 0; oap < oas.properties.length; oap++) {
                    var oapp = oas.properties[oap];
                    if (!_t.isObjectProperty(oapp) || oapp.computed) continue;
                    var oapk = _t.isIdentifier(oapp.key) ? oapp.key.name :
                      (_t.isStringLiteral(oapp.key) ? oapp.key.value : null);
                    if (oapk === propName2) {
                      var oavp = rp.parentPath.get("arguments." + oai + ".properties." + oap + ".value");
                      if (_t.isFunctionExpression(oapp.value) || _t.isArrowFunctionExpression(oapp.value)) {
                        return { done: oavp };
                      }
                      if (_t.isCallExpression(oapp.value)) {
                        var oart = _resolveCallReturnToFunction(oavp, depth + 1);
                        if (oart && oart._path) return { done: oart._path };
                        if (oart && oart.node && _t.isFunction(oart.node)) return { done: oavp };
                      }
                    }
                  }
                }
              }
              // Property-flow propagation: `obj.copier(srcLit)` where
              // copier's body has Object.assign-equivalent dataflow
              // (target=this, source=param[0]/args-elt). The function is
              // resolved via the analyser's effect detection — pure
              // spec-grounded data flow per ECMA § 20.1.2.1 semantics
              // expressed via for-in / Object.keys.forEach copy patterns.
              if (rpParent && _t.isMemberExpression(rpParent) && rpParent.object === rp.node && !rpParent.computed) {
                var copyCallPath = rp.parentPath ? rp.parentPath.parentPath : null;
                var copyCallNode = copyCallPath ? copyCallPath.node : null;
                if (copyCallNode && _t.isCallExpression(copyCallNode) && copyCallNode.callee === rpParent) {
                  var copyFn = _resolveCalleeFuncPath(copyCallPath, depth + 1);
                  if (copyFn) {
                    var copyEffects = _specAnalyzePropertyFlow(copyFn);
                    var pp = _specDetectPropagationFromEffects(copyEffects);
                    if (pp && pp.target.kind === "this") {
                      var ppArgIdx = -1;
                      if (pp.source.kind === "param") ppArgIdx = pp.source.idx;
                      else if (pp.source.kind === "args-elt" &&
                               pp.source.idx && pp.source.idx.kind === "const" &&
                               typeof pp.source.idx.value === "number") {
                        ppArgIdx = pp.source.idx.value;
                      }
                      if (ppArgIdx >= 0 && ppArgIdx < copyCallNode.arguments.length) {
                        var ppArg = copyCallNode.arguments[ppArgIdx];
                        if (_t.isObjectExpression(ppArg)) {
                          for (var ppe = 0; ppe < ppArg.properties.length; ppe++) {
                            var ppp = ppArg.properties[ppe];
                            if (!_t.isObjectProperty(ppp) || ppp.computed) continue;
                            var ppk = _t.isIdentifier(ppp.key) ? ppp.key.name :
                              (_t.isStringLiteral(ppp.key) ? ppp.key.value : null);
                            if (ppk === propName2) {
                              var ppvp = copyCallPath.get("arguments." + ppArgIdx + ".properties." + ppe + ".value");
                              if (_t.isFunctionExpression(ppp.value) || _t.isArrowFunctionExpression(ppp.value)) {
                                return { done: ppvp };
                              }
                              if (_t.isCallExpression(ppp.value)) {
                                var ppr = _resolveCallReturnToFunction(ppvp, depth + 1);
                                if (ppr && ppr._path) return { done: ppr._path };
                                if (ppr && ppr.node && _t.isFunction(ppr.node)) return { done: ppvp };
                              }
                            }
                          }
                        }
                      }
                    }
                  }
                }
              }
            }
          }
        }
        return { done: null };
      }

      default: return { done: null };
    }
  }
}

// Resolve an expression to a function path when possible. Accepts:
//   - Identifier bound to FunctionDeclaration / FunctionExpression init
//     / ArrowFunctionExpression init / VariableDeclarator with function
//     value
//   - MemberExpression `obj.prop` where obj is an ObjectExpression with
//     a function-valued property at `prop`
//   - Computed MemberExpression `obj[k]` where k resolves to a literal
//     and obj's literal-keyed property is a function
// Used by Function.prototype.call / .apply resolution and dynamic
// property access into module tables.
// State IDs for _retfpStep (state machine for _resolveExprToFunctionPath).
var _RETFP_INIT = 0;          // pull next candidate from queue, dispatch by node type
var _RETFP_MEM_KEY_AFTER = 100;  // after _resolveAllValues for computed key
var _RETFP_MEM_OBJ_AFTER = 110;  // after _resolveToObject for object
var _RETFP_REF_LOOP = 120;       // walk objBind.referencePaths (sub-loop with possible key dispatch)
var _RETFP_REF_KEY_AFTER = 121;  // after _resolveAllValues for ref-walk computed property

function _retfpMakeFrame(path, node, depth, stepFn, shortCircuit, guardPrefix, defaultResult) {
  return {
    path: path, node: node, depth: depth || 0,
    state: _RETFP_INIT, L: { queue: [path], seen: new Set() },
    result: null,
    stepFn: stepFn || _retfpStep,
    makeFrame: _retfpMakeFrame,
    shortCircuit: shortCircuit,
    guardPrefix: guardPrefix !== undefined ? guardPrefix : "F",
    defaultResult: defaultResult !== undefined ? defaultResult : null,
  };
}

function _resolveExprToFunctionPath(initialExprPath, depth) {
  if (!initialExprPath || !initialExprPath.node) return null;
  if (!_resolver.guard("F", initialExprPath.node)) return null;
  var initialFrame = _retfpMakeFrame(initialExprPath, initialExprPath.node, depth || 0,
    _retfpStep, null, "F", null);
  return _runResolverStack(initialFrame);
}

// Iterative .bind chain unwind: f.bind(t1).bind(t2).bind(t3) resolves to
// f. Walk callee.object while we see .bind() calls. Pure ECMAScript:
// Function.prototype.bind returns a callable that when invoked behaves
// as the receiver function.
function _retfpUnwrapBindChain(p) {
  while (p && p.node &&
         _t.isCallExpression(p.node) &&
         _t.isMemberExpression(p.node.callee) && !p.node.callee.computed &&
         _t.isIdentifier(p.node.callee.property, { name: "bind" })) {
    p = p.get("callee.object");
  }
  return p;
}

// Identifier alias chain walk for `var f = g; var g = h; var h = function() {}`.
// Returns a Babel path or null. Each iteration follows one alias level;
// the loop is bounded by the seen-set on binding identifiers (not on
// arbitrary node identity, since the same Identifier node can appear in
// multiple references).
function _retfpWalkAliasChain(initExprPath) {
  var exprPath = initExprPath;
  var node = exprPath.node;
  var seenBindings = new Set();
  while (true) {
    var b = exprPath.scope.getBinding(node.name);
    if (!b) return null;
    if (seenBindings.has(b.identifier)) return null;
    seenBindings.add(b.identifier);
    if (_t.isFunctionDeclaration(b.path.node)) return b.path;
    if (!_t.isVariableDeclarator(b.path.node) || !b.path.node.init) return null;
    var init = b.path.node.init;
    if (_t.isFunctionExpression(init) || _t.isArrowFunctionExpression(init)) {
      return b.path.get("init");
    }
    // `.bind(…)` on init — unwrap iteratively to get back to a function.
    if (_t.isCallExpression(init) && _t.isMemberExpression(init.callee) && !init.callee.computed &&
        _t.isIdentifier(init.callee.property, { name: "bind" })) {
      var bindPath = _retfpUnwrapBindChain(b.path.get("init"));
      if (bindPath && _t.isIdentifier(bindPath.node)) {
        exprPath = bindPath;
        node = bindPath.node;
        continue;
      }
      if (bindPath && (_t.isFunctionExpression(bindPath.node) || _t.isArrowFunctionExpression(bindPath.node))) {
        return bindPath;
      }
      return null;
    }
    // init is not a passthrough — chain ends.
    return null;
  }
}

// Param-substitution branch (synchronous): caller passes some expression
// as the corresponding arg; that expression is what `obj.key` resolves
// against. For each caller's arg, probe argP.keyName for a function.
function _retfpResolveParamSub(node, exprPath, keyName, depth) {
  if (!_t.isIdentifier(node.object)) return null;
  var objBind = exprPath.scope.getBinding(node.object.name);
  if (!objBind || objBind.kind !== "param") return null;
  var paramName = node.object.name;
  var hostFunc = objBind.scope.path;
  if (!hostFunc || !hostFunc.node.params) return null;
  var paramIdx = -1;
  for (var pi = 0; pi < hostFunc.node.params.length; pi++) {
    if (_t.isIdentifier(hostFunc.node.params[pi], { name: paramName })) { paramIdx = pi; break; }
  }
  if (paramIdx < 0) return null;
  var callerArgs = _findFunctionCallerArgs(hostFunc);
  for (var ci = 0; ci < callerArgs.length; ci++) {
    if (paramIdx >= callerArgs[ci].length) continue;
    var argP = callerArgs[ci][paramIdx];
    if (!argP) continue;
    // Probe argP directly with the same key-resolution logic — _probePropertyOnExpr
    // doesn't re-enter _resolveExprToFunctionPath, so no recursion concern.
    var argSubFn = _probePropertyOnExpr(argP, keyName, depth);
    if (argSubFn) return argSubFn;
  }
  return null;
}

function _retfpStep(F) {
  var depth = F.depth, L = F.L;
  while (true) {
    switch (F.state) {
      case _RETFP_INIT: {
        // Pull next candidate from queue. Skip seen, drained, or null entries.
        var exprPath = null;
        while (L.queue.length > 0) {
          var cand = L.queue.shift();
          if (!cand || !cand.node) continue;
          if (L.seen.has(cand.node)) continue;
          L.seen.add(cand.node);
          exprPath = _retfpUnwrapBindChain(cand);
          if (exprPath && exprPath.node) break;
          exprPath = null;
        }
        if (!exprPath) return { done: null };

        var node = exprPath.node;
        if (_t.isFunctionExpression(node) || _t.isArrowFunctionExpression(node)) {
          return { done: exprPath };
        }
        if (_t.isIdentifier(node)) {
          // Alias chain walk is purely synchronous — no recursion concern.
          // Original semantics: a failing alias walk returns null (aborts
          // the whole search) — preserved verbatim. Callers depend on
          // this short-circuit; relaxing it surfaces partial matches that
          // don't actually point at executable functions.
          var aliasResult = _retfpWalkAliasChain(exprPath);
          if (aliasResult) return { done: aliasResult };
          return { done: null };
        }
        if (_t.isMemberExpression(node)) {
          L.exprPath = exprPath;
          L.node = node;
          L.keyName = null;
          if (!node.computed && _t.isIdentifier(node.property)) {
            L.keyName = node.property.name;
            // Skip key dispatch — go straight to object resolution.
            return {
              trace: exprPath.get("object"),
              state: _RETFP_MEM_OBJ_AFTER,
              stepFn: _rtoiStep, makeFrame: _rtoiMakeFrame,
              shortCircuit: _rtoiShortCircuit, guardPrefix: "T", defaultResult: null,
            };
          }
          if (node.computed) {
            return {
              trace: exprPath.get("property"),
              state: _RETFP_MEM_KEY_AFTER,
              stepFn: _ravStep, makeFrame: _ravMakeFrame,
              shortCircuit: _ravShortCircuit, guardPrefix: "V", defaultResult: [],
            };
          }
        }
        // Other node types — no resolution path.
        continue;
      }

      case _RETFP_MEM_KEY_AFTER: {
        var keyVals = F.result;
        if (Array.isArray(keyVals) && keyVals.length > 0) L.keyName = String(keyVals[0]);
        // Original semantics: !keyName returns null (aborts). Preserved.
        if (!L.keyName) return { done: null };
        return {
          trace: L.exprPath.get("object"),
          state: _RETFP_MEM_OBJ_AFTER,
          stepFn: _rtoiStep, makeFrame: _rtoiMakeFrame,
          shortCircuit: _rtoiShortCircuit, guardPrefix: "T", defaultResult: null,
        };
      }

      case _RETFP_MEM_OBJ_AFTER: {
        var objNode = F.result;
        var keyName = L.keyName;
        if (objNode && objNode._path) {
          for (var i = 0; i < objNode.properties.length; i++) {
            var p = objNode.properties[i];
            if (_t.isObjectProperty(p) && !p.computed) {
              var k = _t.isIdentifier(p.key) ? p.key.name :
                (_t.isStringLiteral(p.key) ? p.key.value : (_t.isNumericLiteral(p.key) ? String(p.key.value) : null));
              if (k === keyName && (_t.isFunctionExpression(p.value) || _t.isArrowFunctionExpression(p.value))) {
                return { done: objNode._path.get("properties." + i + ".value") };
              }
            }
            if (_t.isObjectMethod(p) && !p.computed) {
              var mk = _t.isIdentifier(p.key) ? p.key.name :
                (_t.isStringLiteral(p.key) ? p.key.value : (_t.isNumericLiteral(p.key) ? String(p.key.value) : null));
              if (mk === keyName) return { done: objNode._path.get("properties." + i) };
            }
          }
        }
        // Fall through to receiver-reference walk: `obj.key = function/arrow`
        // assignments via the receiver's referencePaths (mutation after
        // initial declaration). Used by webpack-style `f.d = (e, a) => {...}`
        // and module-table installs `d[97088] = function(...) {...}`.
        if (_t.isIdentifier(L.node.object)) {
          var objBind = L.exprPath.scope.getBinding(L.node.object.name);
          if (objBind && objBind.referencePaths) {
            L.objBind = objBind;
            L.refIdx = 0;
            F.state = _RETFP_REF_LOOP;
            continue;
          }
        }
        // No ref walk for this object — try param-substitution branch.
        var paramFn = _retfpResolveParamSub(L.node, L.exprPath, keyName, depth);
        if (paramFn) return { done: paramFn };
        // Original semantics: no match → done(null). Queue continuation
        // is reserved for ref-walk RHS fallbacks (handled inside REF_LOOP).
        return { done: null };
      }

      case _RETFP_REF_LOOP: {
        // If returning from a sub-trace for a computed key, integrate the
        // result into propMatches state.
        if (L.refKeyJustTraced) {
          L.refKeyJustTraced = false;
          var rpKeyVals = F.result;
          var matched = Array.isArray(rpKeyVals) && rpKeyVals.length > 0 && String(rpKeyVals[0]) === L.keyName;
          if (matched) {
            // Recover the ref-walk state we paused on; check assignment.
            var resumed = _retfpHandleRefAssignment(L.pausedRp, L.queue);
            if (resumed) return { done: resumed };
          }
          // Either non-match or assignment had no usable RHS — fall through
          // to next ref iteration.
        }
        var refs = L.objBind.referencePaths;
        while (L.refIdx < refs.length) {
          var rp = refs[L.refIdx];
          L.refIdx++;
          var rpParent = rp.parent;
          if (!rpParent || !_t.isMemberExpression(rpParent) || rpParent.object !== rp.node) continue;
          var keyName2 = L.keyName;
          // Synchronous match check first (literal/identifier key).
          if (!rpParent.computed && _t.isIdentifier(rpParent.property, { name: keyName2 })) {
            var done1 = _retfpHandleRefAssignment(rp, L.queue);
            if (done1) return { done: done1 };
            continue;
          }
          if (rpParent.computed) {
            if (_t.isStringLiteral(rpParent.property) && rpParent.property.value === keyName2) {
              var done2 = _retfpHandleRefAssignment(rp, L.queue);
              if (done2) return { done: done2 };
              continue;
            }
            if (_t.isNumericLiteral(rpParent.property) && String(rpParent.property.value) === keyName2) {
              var done3 = _retfpHandleRefAssignment(rp, L.queue);
              if (done3) return { done: done3 };
              continue;
            }
            // Computed key with non-literal property — dispatch _resolveAllValues.
            L.pausedRp = rp;
            L.refKeyJustTraced = true;
            return {
              trace: rp.parentPath.get("property"),
              state: _RETFP_REF_KEY_AFTER,
              stepFn: _ravStep, makeFrame: _ravMakeFrame,
              shortCircuit: _ravShortCircuit, guardPrefix: "V", defaultResult: [],
            };
          }
        }
        // Ref walk done — try param-substitution branch.
        var paramFn2 = _retfpResolveParamSub(L.node, L.exprPath, L.keyName, depth);
        if (paramFn2) return { done: paramFn2 };
        // No match for this candidate — try next queued.
        F.state = _RETFP_INIT;
        continue;
      }

      case _RETFP_REF_KEY_AFTER: {
        // Sub-trace returned to the loop; control falls back through
        // _RETFP_REF_LOOP via L.refKeyJustTraced flag.
        F.state = _RETFP_REF_LOOP;
        continue;
      }

      default: return { done: null };
    }
  }
}

// Process an `obj.key = rhs` assignment match: if RHS is a function,
// return its path. If RHS is a CallExpression / MemberExpression /
// Identifier that itself resolves to a function, queue it for the outer
// candidate loop to try next.
function _retfpHandleRefAssignment(rp, queue) {
  var rpParent = rp.parent;
  var assignNode = rp.parentPath ? rp.parentPath.parent : null;
  if (!assignNode || !_t.isAssignmentExpression(assignNode) || assignNode.operator !== "=" ||
      assignNode.left !== rpParent) return null;
  var rhsNode = assignNode.right;
  var rhsPath = rp.parentPath.parentPath.get("right");
  if (_t.isFunctionExpression(rhsNode) || _t.isArrowFunctionExpression(rhsNode)) {
    return rhsPath;
  }
  // RHS is a CallExpression / MemberExpression / Identifier that itself
  // resolves to a function — e.g. `c.push = a.bind(null, …)`. Queue
  // for next outer-candidate iteration; the alias/bind logic in
  // _RETFP_INIT handles unwinding.
  if (_t.isCallExpression(rhsNode) || _t.isMemberExpression(rhsNode) || _t.isIdentifier(rhsNode)) {
    queue.push(rhsPath);
  }
  return null;
}

// Probe whether `objExprPath.<propName>` resolves to a function path,
// using the same logic as _resolveExprToFunctionPath's MemberExpression
// branch but without needing to construct a fresh MemberExpression node.
// This is the param-substitution helper for cross-call resolution.
function _probePropertyOnExpr(objExprPath, propName, depth) {
  if (!objExprPath || !objExprPath.node) return null;
  if (!_t.isIdentifier(objExprPath.node)) return null;
  var bind = objExprPath.scope.getBinding(objExprPath.node.name);
  if (!bind) {
    // Global: scan global assignments.
    return null;
  }
  if (bind.referencePaths) {
    for (var ri = 0; ri < bind.referencePaths.length; ri++) {
      var rp = bind.referencePaths[ri];
      var rpParent = rp.parent;
      if (!rpParent || !_t.isMemberExpression(rpParent) || rpParent.object !== rp.node) continue;
      var matches = (!rpParent.computed && _t.isIdentifier(rpParent.property, { name: propName })) ||
        (rpParent.computed && _t.isStringLiteral(rpParent.property) && rpParent.property.value === propName) ||
        (rpParent.computed && _t.isNumericLiteral(rpParent.property) && String(rpParent.property.value) === propName);
      if (!matches) continue;
      var aN = rp.parentPath ? rp.parentPath.parent : null;
      if (!aN || !_t.isAssignmentExpression(aN) || aN.operator !== "=" || aN.left !== rpParent) continue;
      var rhs = aN.right;
      if (_t.isFunctionExpression(rhs) || _t.isArrowFunctionExpression(rhs)) {
        return rp.parentPath.parentPath.get("right");
      }
    }
  }
  return null;
}

// Find Object.defineProperty effects on the value RETURNED by a call
// expression. Resolve callee's body, find each return statement's
// argument expression, then collect defineProperty effects on that
// argument's heap identity (which includes effects from other call
// sites in the function body that pass the returned value as an arg).
//
// Real-world shape: webpack's `f(id)` returns `b[id].exports`, an object
// that gets mutated by `d[id].call(b[id].exports, ...)` earlier in f's
// body. So `f(id).url` reads happen on the same object that the call
// installs `url` on.
function _collectDefinePropertyEffectsOnCallReturn(callPath, propName, depth, visited) {
  if (!callPath || !callPath.isCallExpression()) return [];
  if (visited.has(callPath.node)) return [];
  visited.add(callPath.node);
  // Memoize: same (callPath, propName) repeated across a bundle returns
  // the same effects (function bodies are immutable in the AST). Without
  // this, every MemberExpression-on-CallExpression in github's 6MB
  // bundle re-traverses the same callee bodies — quadratic work that
  // appears as a hang. The cache is per-analysis (cleared via
  // _nodePathCache reset at analyzeJSBundle entry).
  var memoKey = callPath.node;
  var memoSlot = _callReturnEffectMemo.get(memoKey);
  if (memoSlot && propName in memoSlot) return memoSlot[propName];
  // Use the global resolver-guard set ALSO so cross-call cycles are
  // caught even when each top-level entry creates its own per-call
  // visited Set. Without this, _resolveAllValues recursing into
  // _readDefinePropertyDescriptor → _resolveAllValues → here would
  // re-enter this function with a fresh visited Set and fail to
  // detect that the same call site is already being processed.
  if (!_resolver.guard("DPCR:" + propName + ":", callPath.node)) return [];
  try {
  var calleeFunc = _resolveExprToFunctionPath(callPath.get("callee"), depth);
  if (!calleeFunc) return [];
  var body = calleeFunc.node.body;
  if (!_t.isBlockStatement(body)) return [];
  var results = [];
  try {
    calleeFunc.get("body").traverse(Object.assign({
      ReturnStatement: function(retPath) {
        if (!retPath.node.argument) return;
        var argPath = retPath.get("argument");
        if (_t.isIdentifier(argPath.node) || _t.isMemberExpression(argPath.node)) {
          var effects = _collectDefinePropertyEffects(argPath, propName, depth, visited);
          results = results.concat(effects);
        }
      },
    }, _SKIP_NESTED_FUNCS));
  } catch (e) { _resolver.collectError(e, "definePropertyOnCallReturn"); }
  // Memoize for subsequent (callPath, propName) lookups.
  if (!memoSlot) { memoSlot = {}; _callReturnEffectMemo.set(memoKey, memoSlot); }
  memoSlot[propName] = results;
  return results;
  } finally { _resolver.unguard("DPCR:" + propName + ":", callPath.node); }
}

// Inter-procedural collector: find all `Object.defineProperty(_, propName, _)`
// effects on the heap object referenced by objPath, by walking direct
// defineProperty calls plus call sites that pass objPath as an argument
// into a function whose body installs the property on the corresponding
// parameter. Handles `.call`/`.apply` arg shifting and `for..in` over
// literal-keyed objects (the descriptor's get function is invoked once
// per known key, key-bound to the loop variable).
//
// objPath: NodePath to the receiver expression (Identifier / MemberExpression).
// propName: the property being read (e.g., "s").
// depth: recursion depth for _resolveAllValues.
// visited: Set of CallExpression nodes already processed to avoid cycles.
function _collectDefinePropertyEffects(objPath, propName, depth, visited) {
  if (!objPath || !objPath.node) return [];
  // Memoize per (entry node, propName). React-lib-style code installs
  // 24+ properties on a single object via `for(k in g) defineProperty`.
  // Without memoization every read of any such property re-iterates
  // ALL of g and re-walks each descriptor, producing O(reads × props)
  // work per call site. With shared receivers (e.g. `m.x`, `m.y`, `m.z`
  // all on the same `m`), the cost compounds across the bundle.
  var entryMemo = _callReturnEffectMemo.get(objPath.node);
  if (entryMemo && propName in entryMemo) return entryMemo[propName];
  // Iterative work queue. The "indirect" case (param passed to yet
  // another call whose callee installs the property) used to recurse
  // back into _collectDefinePropertyEffects, which on real bundles
  // (github 5.9MB) exceeded V8's call-stack depth (~13k frames) for
  // deeply-chained call graphs. Per CLAUDE.md "Convert recursive AST
  // walkers to iterative whenever they process chainable node types"
  // we maintain an explicit queue of (objPath) items to process; each
  // iteration extracts uses and either captures direct effects or
  // pushes the next-level objPath onto the queue. Cycles are still
  // prevented by the `visited` Set on each call site processed.
  var queue = [objPath];
  var queueSeen = new Set();  // node identity guard for queue entries — prevents the same objPath from being processed twice when chains of param-pass form cycles in the call graph
  if (objPath.node) queueSeen.add(objPath.node);
  var results = [];
  while (queue.length > 0) {
  var currentObjPath = queue.shift();
  if (!currentObjPath || !currentObjPath.node) continue;
  // Step 1: collect all "uses" of currentObjPath that we should
  // examine. For an Identifier currentObjPath, that's its binding's
  // referencePaths. For a MemberExpression like `c.exports`, we walk
  // the receiver `c`'s referencePaths and filter for accesses that
  // match the same property chain.
  var uses = _collectExprUses(currentObjPath);
  for (var ui = 0; ui < uses.length; ui++) {
    var useRef = uses[ui];
    var useParent = useRef.parentPath;
    if (!useParent) continue;
    // Direct: Object.defineProperty(USE, KEY, DESC)
    if (useParent.isCallExpression() && useParent.node.arguments[0] === useRef.node &&
        _isObjectDefineProperty(useParent)) {
      var d = _readDefinePropertyDescriptor(useParent, propName, depth);
      results = results.concat(d);
      continue;
    }
    // useRef as an argument to some other call: trace into callee body.
    if (useParent.isCallExpression()) {
      // Determine the param index this argument corresponds to. Handle
      // .call(thisArg, arg0, arg1, …) — arg0 inside the callee is at
      // index 0 (thisArg is consumed). .apply skipped (variadic shape).
      var argIdx = -1;
      var calleeNode = useParent.node.callee;
      var isCallForm = (_t.isMemberExpression(calleeNode) && !calleeNode.computed &&
                       _t.isIdentifier(calleeNode.property, { name: "call" }));
      var argOffset = isCallForm ? 1 : 0;
      for (var ai = 0; ai < useParent.node.arguments.length; ai++) {
        if (useParent.node.arguments[ai] === useRef.node) { argIdx = ai - argOffset; break; }
      }
      if (argIdx < 0) continue;
      if (visited.has(useParent.node)) continue;
      visited.add(useParent.node);
      // Resolve the callee's function body. For `.call`, the receiver IS
      // the function; otherwise resolve normally.
      var calleeFunc = isCallForm ?
        _resolveExprToFunctionPath(useParent.get("callee.object"), depth) :
        _resolveExprToFunctionPath(useParent.get("callee"), depth);
      if (!calleeFunc) continue;
      var fnParams = calleeFunc.node.params;
      if (!fnParams || argIdx >= fnParams.length) continue;
      var paramNode = fnParams[argIdx];
      if (!_t.isIdentifier(paramNode)) continue;
      // Inside the callee, the param's binding's referencePaths is where
      // the heap object is observable. Recurse: any defineProperty on
      // that param targets the SAME heap object the caller's arg points
      // to, so descriptors found there are descriptors on objPath.
      var paramBinding = calleeFunc.scope.getBinding(paramNode.name);
      if (!paramBinding) continue;
      // Use the param's referencePaths directly — each is an Identifier
      // path where the param is read. Object.defineProperty on the param
      // mutates the SAME heap object the caller passed in.
      var paramUses = paramBinding.referencePaths || [];
      for (var pi = 0; pi < paramUses.length; pi++) {
        var pUse = paramUses[pi];
        var pUseParent = pUse.parentPath;
        if (!pUseParent) continue;
        // Direct: Object.defineProperty(param, KEY, DESC) where KEY is a
        // string literal. (Loop-variable KEY falls through to for..in
        // unroll below — _readDefinePropertyDescriptor returns [] for
        // identifier keys, so the direct path naturally yields nothing
        // and we proceed.)
        if (pUseParent.isCallExpression() && pUseParent.node.arguments[0] === pUse.node &&
            _isObjectDefineProperty(pUseParent)) {
          var keyArgNode = pUseParent.node.arguments[1];
          var keyIsLiteral = _t.isStringLiteral(keyArgNode) ||
            (_t.isTemplateLiteral(keyArgNode) && keyArgNode.expressions.length === 0);
          if (keyIsLiteral) {
            var pd = _readDefinePropertyDescriptor(pUseParent, propName, depth);
            results = results.concat(pd);
            continue;
          }
          // KEY is identifier — fall through to for..in unroll path.
        }
        // Inside a for..in loop: `for (var k in DEFS) Object.defineProperty(param, k, …)`.
        // When DEFS is a literal-keyed object reachable from the param's
        // sibling parameter, unroll: for each key K of DEFS, propName
        // matches if K === propName, and the descriptor's get expression
        // uses DEFS[K] which resolves to the literal value at K.
        var inLoop = _findEnclosingForIn(pUseParent);
        if (inLoop && _isDefinePropertyInsideForIn(pUseParent, pUse, inLoop)) {
          var loopVarName = _getForInLoopVarName(inLoop);
          if (loopVarName) {
            var defsExprPath = inLoop.get("right");
            // Resolve defs to the actual ObjectExpression (literal at
            // caller). Try param chain → caller arg position → literal.
            var defsObj = _resolveDefsObjectFromForIn(defsExprPath, calleeFunc, useParent, isCallForm, depth);
            if (defsObj && defsObj._path) {
              for (var dpi = 0; dpi < defsObj.properties.length; dpi++) {
                var dp = defsObj.properties[dpi];
                if (!_t.isObjectProperty(dp) || dp.computed) continue;
                var dKey = _t.isIdentifier(dp.key) ? dp.key.name :
                  (_t.isStringLiteral(dp.key) ? dp.key.value : null);
                if (dKey !== propName) continue;
                // Descriptor in the loop body: Object.defineProperty(param, k, DESC).
                // DESC's get is `defs[k]` — we know defs[k] for this iteration.
                var defsValPath = defsObj._path.get("properties." + dpi + ".value");
                results = results.concat(_resolveDefinePropertyDescFromGetExpr(pUseParent, defsValPath, loopVarName, depth));
              }
            }
          }
          continue;
        }
        // Indirect: param is itself passed to another call. The callee
        // may reference OTHER params of calleeFunc (e.g. `n.d(exp, …)`
        // where `n` is also a param of calleeFunc). For pure JS scope
        // resolution to follow this, substitute calleeFunc's params with
        // the caller's arguments at useParent (with .call offset
        // applied).
        if (pUseParent.isCallExpression()) {
          var subCalleeFunc = _resolveCalleeWithParamSubst(
            pUseParent, calleeFunc, useParent, isCallForm, depth);
          if (subCalleeFunc) {
            // Found callee via substitution. Walk into ITS body looking
            // for Object.defineProperty effects on the corresponding
            // parameter.
            var subArgIdx = -1;
            var subIsCall = (_t.isMemberExpression(pUseParent.node.callee) && !pUseParent.node.callee.computed &&
                             _t.isIdentifier(pUseParent.node.callee.property, { name: "call" }));
            var subOff = subIsCall ? 1 : 0;
            for (var sai = 0; sai < pUseParent.node.arguments.length; sai++) {
              if (pUseParent.node.arguments[sai] === pUse.node) { subArgIdx = sai - subOff; break; }
            }
            if (subArgIdx >= 0 && subArgIdx < subCalleeFunc.node.params.length) {
              var subParamNode = subCalleeFunc.node.params[subArgIdx];
              if (_t.isIdentifier(subParamNode)) {
                var subParamBinding = subCalleeFunc.scope.getBinding(subParamNode.name);
                if (subParamBinding && subParamBinding.referencePaths) {
                  for (var spi = 0; spi < subParamBinding.referencePaths.length; spi++) {
                    var subUse = subParamBinding.referencePaths[spi];
                    var subUseParent = subUse.parentPath;
                    if (!subUseParent || !subUseParent.isCallExpression()) continue;
                    if (subUseParent.node.arguments[0] !== subUse.node) continue;
                    if (!_isObjectDefineProperty(subUseParent)) continue;
                    var keyArgN = subUseParent.node.arguments[1];
                    var subForIn = _findEnclosingForIn(subUseParent);
                    if (subForIn && _isDefinePropertyInsideForIn(subUseParent, subUse, subForIn)) {
                      var subLoopVar = _getForInLoopVarName(subForIn);
                      if (subLoopVar) {
                        var subDefsExpr = subForIn.get("right");
                        var subDefsObj = _resolveDefsObjectFromForIn(
                          subDefsExpr, subCalleeFunc, pUseParent, subIsCall, depth);
                        if (subDefsObj && subDefsObj._path) {
                          for (var sdp = 0; sdp < subDefsObj.properties.length; sdp++) {
                            var sdProp = subDefsObj.properties[sdp];
                            if (!_t.isObjectProperty(sdProp) || sdProp.computed) continue;
                            var sdK = _t.isIdentifier(sdProp.key) ? sdProp.key.name :
                              (_t.isStringLiteral(sdProp.key) ? sdProp.key.value : null);
                            if (sdK !== propName) continue;
                            var sdValPath = subDefsObj._path.get("properties." + sdp + ".value");
                            results = results.concat(
                              _resolveDefinePropertyDescFromGetExpr(subUseParent, sdValPath, subLoopVar, depth));
                          }
                        }
                      }
                    }
                  }
                }
              }
            }
          } else {
            // Push for iterative processing instead of recursing —
            // prevents stack overflow on deeply-chained call graphs.
            // Identity guard via queueSeen — same objPath node never
            // queued twice, bounding total iterations by # unique
            // referencePaths in the program.
            if (pUse && pUse.node && !queueSeen.has(pUse.node)) {
              queueSeen.add(pUse.node);
              queue.push(pUse);
            }
          }
        }
      }
    }
  }
  } // end while
  // Cache the entry-point result so future lookups for the same
  // (objPath.node, propName) return immediately.
  if (!entryMemo) { entryMemo = {}; _callReturnEffectMemo.set(objPath.node, entryMemo); }
  entryMemo[propName] = results;
  return results;
}

// Resolve a callee expression where the receiver is a param of an
// enclosing function (paramHostFunc) by substituting the caller's
// argument at that param position. Returns the resolved function path
// or null.
//
// Example: inside `function(mod, exp, n) { n.d(exp, …); }`, when called
// as `d[e].call(c.exports, c, c.exports, f)`, `n` corresponds to
// caller arg[3] (after the .call thisArg shift) which is `f`. So `n.d`
// resolves as `f.d` — and `f.d` may have an assignment we can find.
function _resolveCalleeWithParamSubst(callPath, paramHostFunc, callerCallPath, callerIsCallForm, depth) {
  var callee = callPath.node.callee;
  if (!_t.isMemberExpression(callee) || callee.computed) return null;
  if (!_t.isIdentifier(callee.object)) return null;
  if (!_t.isIdentifier(callee.property)) return null;
  var recvName = callee.object.name;
  var propName = callee.property.name;
  var binding = callPath.scope.getBinding(recvName);
  if (!binding || binding.kind !== "param") return null;
  if (binding.scope.path !== paramHostFunc) return null;
  // Find the param's index in paramHostFunc.
  var pIdx = -1;
  for (var i = 0; i < paramHostFunc.node.params.length; i++) {
    if (_t.isIdentifier(paramHostFunc.node.params[i], { name: recvName })) { pIdx = i; break; }
  }
  if (pIdx < 0) return null;
  var argOffset = callerIsCallForm ? 1 : 0;
  var callerArgIdx = pIdx + argOffset;
  if (callerArgIdx >= callerCallPath.node.arguments.length) return null;
  var callerArgPath = callerCallPath.get("arguments." + callerArgIdx);
  if (!_t.isIdentifier(callerArgPath.node)) return null;
  // The caller's arg is an Identifier — find the function assigned at
  // `<arg>.<propName>` within the arg's scope.
  return _probePropertyOnExpr(callerArgPath, propName, depth);
}

// Collect referencePaths for an identifier, or for a MemberExpression
// like `c.exports`, the receiver chain matching that member access.
function _collectExprUses(exprPath) {
  var node = exprPath.node;
  if (_t.isIdentifier(node)) {
    var b = exprPath.scope.getBinding(node.name);
    return b && b.referencePaths ? b.referencePaths : [];
  }
  if (_t.isMemberExpression(node) && !node.computed && _t.isIdentifier(node.object) &&
      _t.isIdentifier(node.property)) {
    // For `c.exports`, walk c's referencePaths and keep only the ones
    // that appear AS the .object of a `.exports` access — those refer
    // to the same heap value as the original c.exports expression
    // (same alias graph).
    var rb = exprPath.scope.getBinding(node.object.name);
    if (!rb || !rb.referencePaths) return [];
    var out = [];
    var propWanted = node.property.name;
    for (var i = 0; i < rb.referencePaths.length; i++) {
      var rp = rb.referencePaths[i];
      var pp = rp.parentPath;
      if (!pp || !pp.isMemberExpression() || pp.node.object !== rp.node || pp.node.computed) continue;
      if (!_t.isIdentifier(pp.node.property, { name: propWanted })) continue;
      out.push(pp);
    }
    return out;
  }
  return [];
}

function _isObjectDefineProperty(callPath) {
  var c = callPath.node.callee;
  if (!_t.isMemberExpression(c) || c.computed) return false;
  if (!_t.isIdentifier(c.property, { name: "defineProperty" })) return false;
  if (!_t.isIdentifier(c.object, { name: "Object" })) return false;
  if (callPath.scope.getBinding("Object")) return false;
  return callPath.node.arguments.length >= 3;
}

// Read the descriptor at arg[2] of an Object.defineProperty call. If the
// descriptor's key matches propName, return the resolved value(s) of its
// `get` (invoked) or `value` (direct).
function _readDefinePropertyDescriptor(callPath, propName, depth) {
  var keyArg = callPath.node.arguments[1];
  var key = null;
  if (_t.isStringLiteral(keyArg)) key = keyArg.value;
  else if (_t.isTemplateLiteral(keyArg) && keyArg.expressions.length === 0 && keyArg.quasis.length === 1) {
    key = keyArg.quasis[0].value.cooked;
  } else if (_t.isIdentifier(keyArg)) {
    // Loop variable case is handled separately by the for..in unroll;
    // a plain identifier here means we can't statically link.
    return [];
  }
  if (key !== propName) return [];
  var descArg = callPath.node.arguments[2];
  if (!_t.isObjectExpression(descArg)) return [];
  var out = [];
  for (var i = 0; i < descArg.properties.length; i++) {
    var dp = descArg.properties[i];
    if (!_t.isObjectProperty(dp) && !_t.isObjectMethod(dp)) continue;
    var dk = _t.isIdentifier(dp.key) ? dp.key.name :
      (_t.isStringLiteral(dp.key) ? dp.key.value : null);
    if (dk === "value") {
      out = out.concat(_resolveAllValues(callPath.get("arguments.2.properties." + i + ".value"), depth + 1));
    } else if (dk === "get") {
      var fnPath;
      if (_t.isObjectMethod(dp)) fnPath = callPath.get("arguments.2.properties." + i);
      else fnPath = callPath.get("arguments.2.properties." + i + ".value");
      out = out.concat(_invokeFunctionExprForReturn(fnPath, depth));
    }
  }
  return out;
}

function _invokeFunctionExprForReturn(fnPath, depth) {
  var fnNode = fnPath.node;
  if (_t.isArrowFunctionExpression(fnNode) && !_t.isBlockStatement(fnNode.body)) {
    return _resolveAllValues(fnPath.get("body"), depth + 1);
  }
  if ((_t.isFunctionExpression(fnNode) || _t.isArrowFunctionExpression(fnNode) || _t.isObjectMethod(fnNode)) &&
      _t.isBlockStatement(fnNode.body)) {
    var out = [];
    try {
      fnPath.get("body").traverse(Object.assign({
        ReturnStatement: function(retPath) {
          if (retPath.node.argument) {
            out = out.concat(_resolveAllValues(retPath.get("argument"), depth + 1));
          }
        },
      }, _SKIP_NESTED_FUNCS));
    } catch (e) { _resolver.collectError(e, "invokeFnExprReturn"); }
    return out;
  }
  return [];
}

function _findEnclosingForIn(path) {
  var p = path;
  while (p && !p.isProgram()) {
    if (p.isForInStatement()) return p;
    p = p.parentPath;
  }
  return null;
}

function _isDefinePropertyInsideForIn(callPath, calleeUseRef, forInPath) {
  // The Object.defineProperty call must be in the for..in body (not the
  // header), and the loop variable must appear in the call's args (key
  // position).
  if (!forInPath) return false;
  if (!_isObjectDefineProperty(callPath)) return false;
  var loopVarName = _getForInLoopVarName(forInPath);
  if (!loopVarName) return false;
  var keyArg = callPath.node.arguments[1];
  return _t.isIdentifier(keyArg, { name: loopVarName });
}

function _getForInLoopVarName(forInPath) {
  var left = forInPath.node.left;
  if (_t.isVariableDeclaration(left) && left.declarations.length === 1 &&
      _t.isVariableDeclarator(left.declarations[0]) &&
      _t.isIdentifier(left.declarations[0].id)) {
    return left.declarations[0].id.name;
  }
  if (_t.isIdentifier(left)) return left.name;
  return null;
}

// Resolve the for..in iterable to the actual ObjectExpression literal.
// Inside `f.d = (e, a) => { for (var c in a) ... }`, `a` is param[1].
// The caller of f.d passed an object literal (or identifier resolving to
// one) at arg[1]. Use the existing _resolveToObject machinery.
function _resolveDefsObjectFromForIn(defsExprPath, calleeFunc, callerCallPath, isCallForm, depth) {
  // Direct path: maybe defsExprPath already resolves to an object via
  // existing scope logic.
  var direct = _resolveToObject(defsExprPath, depth);
  if (direct) return direct;
  // If defsExpr is an Identifier bound to a function param of calleeFunc,
  // map to caller's corresponding arg.
  if (_t.isIdentifier(defsExprPath.node)) {
    var b = defsExprPath.scope.getBinding(defsExprPath.node.name);
    if (b && b.kind === "param" && b.scope.path === calleeFunc) {
      var pIdx = -1;
      for (var i = 0; i < calleeFunc.node.params.length; i++) {
        if (_t.isIdentifier(calleeFunc.node.params[i], { name: defsExprPath.node.name })) { pIdx = i; break; }
      }
      if (pIdx >= 0) {
        var argOffset = isCallForm ? 1 : 0;
        var callerArgIdx = pIdx + argOffset;
        if (callerArgIdx < callerCallPath.node.arguments.length) {
          return _resolveToObject(callerCallPath.get("arguments." + callerArgIdx), depth);
        }
      }
    }
  }
  return null;
}

// Inside a for..in loop body, the descriptor's `get` is typically `defs[k]`
// where `k` is the loop variable. For a specific iteration where k is bound
// to a known key K, defs[K] resolves to the literal at K in defsObj. The
// returned value is what reading the defined property invokes.
//
// callPath is the Object.defineProperty call. defsValPath is the path to
// the value at key K in the original defs ObjectExpression. loopVarName
// is `k`.
function _resolveDefinePropertyDescFromGetExpr(callPath, defsValPath, loopVarName, depth) {
  var descArg = callPath.node.arguments[2];
  if (!_t.isObjectExpression(descArg)) return [];
  for (var i = 0; i < descArg.properties.length; i++) {
    var dp = descArg.properties[i];
    if (!_t.isObjectProperty(dp) && !_t.isObjectMethod(dp)) continue;
    var dk = _t.isIdentifier(dp.key) ? dp.key.name :
      (_t.isStringLiteral(dp.key) ? dp.key.value : null);
    if (dk !== "get") continue;
    var getValueNode = _t.isObjectMethod(dp) ? null : dp.value;
    // Common shape: get: defs[k]  → desc.value is a MemberExpression
    // computed-keyed by the loopVar. The defsValPath we already have IS
    // the value at key K, so just resolve / invoke it.
    if (getValueNode && _t.isMemberExpression(getValueNode) && getValueNode.computed &&
        _t.isIdentifier(getValueNode.property, { name: loopVarName })) {
      // defsValPath is the value at key K (a function — getter). Invoke.
      return _invokeFunctionExprForReturn(defsValPath, depth);
    }
    // Otherwise resolve the get expression directly using existing
    // machinery — won't see the loop binding but may resolve constants.
    var fnPath;
    if (_t.isObjectMethod(dp)) fnPath = callPath.get("arguments.2.properties." + i);
    else fnPath = callPath.get("arguments.2.properties." + i + ".value");
    return _invokeFunctionExprForReturn(fnPath, depth);
  }
  return [];
}

// Resolve a call expression through the callee's return statements.
// Traces into function definitions to find what they return.
// Handles: getUrl() → "https://...", buildUrl(base, path) → base + "/" + path
// State IDs for _rcrvStep (state machine for _resolveCallReturnValues).
var _RCRV_INIT = 0;
var _RCRV_ARROW_AFTER = 100;
var _RCRV_LOOP = 110;

function _rcrvMakeFrame(path, node, depth, stepFn, shortCircuit, guardPrefix, defaultResult) {
  return {
    path: path, node: node, depth: depth || 0,
    state: _RCRV_INIT, L: {},
    result: defaultResult !== undefined ? defaultResult : [],
    stepFn: stepFn || _rcrvStep,
    makeFrame: _rcrvMakeFrame,
    shortCircuit: shortCircuit,
    guardPrefix: guardPrefix !== undefined ? guardPrefix : "R",
    defaultResult: defaultResult !== undefined ? defaultResult : [],
  };
}

function _rcrvStep(F) {
  var path = F.path, depth = F.depth, L = F.L;
  while (true) {
    switch (F.state) {
      case _RCRV_INIT: {
        var funcPath = _resolveCalleeFuncPath(path, depth);
        if (!funcPath) {
          var callee = path.node.callee;
          if (_t.isFunctionExpression(callee) || _t.isArrowFunctionExpression(callee)) {
            funcPath = path.get("callee");
          } else if (_t.isSequenceExpression(callee) && callee.expressions.length > 0) {
            var lastIdx = callee.expressions.length - 1;
            var last = callee.expressions[lastIdx];
            if (_t.isFunctionExpression(last) || _t.isArrowFunctionExpression(last)) {
              funcPath = path.get("callee.expressions." + lastIdx);
            } else if (_t.isIdentifier(last)) {
              var seqBinding = path.scope.getBinding(last.name);
              if (seqBinding) {
                if (_t.isFunctionDeclaration(seqBinding.path.node)) funcPath = seqBinding.path;
                else if (_t.isVariableDeclarator(seqBinding.path.node) && seqBinding.path.node.init &&
                    (_t.isFunctionExpression(seqBinding.path.node.init) || _t.isArrowFunctionExpression(seqBinding.path.node.init))) {
                  funcPath = seqBinding.path.get("init");
                }
              }
            }
          }
        }
        if (!funcPath) return { done: [] };

        // Arrow function with expression body — single sub-trace via _ravStep.
        if (_t.isArrowFunctionExpression(funcPath.node) && !_t.isBlockStatement(funcPath.node.body)) {
          return {
            trace: funcPath.get("body"),
            state: _RCRV_ARROW_AFTER,
            stepFn: _ravStep,
            makeFrame: _ravMakeFrame,
            shortCircuit: _ravShortCircuit,
            guardPrefix: "V",
            defaultResult: [],
          };
        }

        // Block body — pre-collect ReturnStatement arg paths via traverse,
        // then iterate via state-machine sub-traces.
        var retPaths = [];
        try {
          funcPath.traverse(Object.assign({
            ReturnStatement: function(retPath) {
              if (retPath.node.argument) retPaths.push(retPath.get("argument"));
            },
          }, _SKIP_NESTED_FUNCS));
        } catch (e) { _resolver.collectError(e, "rcrvCollect"); }
        if (retPaths.length === 0) return { done: [] };
        L.retPaths = retPaths;
        L.values = [];
        L.ri = 0;
        return {
          trace: retPaths[0],
          state: _RCRV_LOOP,
          stepFn: _ravStep,
          makeFrame: _ravMakeFrame,
          shortCircuit: _ravShortCircuit,
          guardPrefix: "V",
          defaultResult: [],
        };
      }

      case _RCRV_ARROW_AFTER: {
        return { done: F.result || [] };
      }

      case _RCRV_LOOP: {
        L.values = L.values.concat(F.result || []);
        L.ri++;
        if (L.ri < L.retPaths.length) {
          return {
            trace: L.retPaths[L.ri],
            state: _RCRV_LOOP,
            stepFn: _ravStep,
            makeFrame: _ravMakeFrame,
            shortCircuit: _ravShortCircuit,
            guardPrefix: "V",
            defaultResult: [],
          };
        }
        return { done: L.values };
      }

      default: return { done: [] };
    }
  }
}

function _resolveCallReturnValues(callPath, depth) {
  var node = callPath && callPath.node;
  if (!node) return [];
  if (!_resolver.guard("R", node)) return [];
  var initialFrame = _rcrvMakeFrame(callPath, node, depth || 0,
    _rcrvStep, null, "R", []);
  return _runResolverStack(initialFrame);
}

// Resolve a call expression to its returned ObjectExpression (if any)
// State IDs for _rcrtoStep (state machine for _resolveCallReturnToObject).
var _RCRTO_INIT = 0;
var _RCRTO_ARROW_AFTER = 100;
var _RCRTO_LOOP = 110;

function _rcrtoMakeFrame(path, node, depth, stepFn, shortCircuit, guardPrefix, defaultResult) {
  return {
    path: path, node: node, depth: depth || 0,
    state: _RCRTO_INIT, L: {},
    result: defaultResult !== undefined ? defaultResult : null,
    stepFn: stepFn || _rcrtoStep,
    makeFrame: _rcrtoMakeFrame,
    shortCircuit: shortCircuit,
    guardPrefix: guardPrefix !== undefined ? guardPrefix : "O",
    defaultResult: defaultResult !== undefined ? defaultResult : null,
  };
}

function _rcrtoStep(F) {
  var path = F.path, depth = F.depth, L = F.L;
  while (true) {
    switch (F.state) {
      case _RCRTO_INIT: {
        var funcPath = _resolveCalleeFuncPath(path, depth);
        if (!funcPath) return { done: null };

        // Arrow function with expression body
        if (_t.isArrowFunctionExpression(funcPath.node) && !_t.isBlockStatement(funcPath.node.body)) {
          var bodyPath = funcPath.get("body");
          if (_t.isObjectExpression(bodyPath.node)) {
            bodyPath.node._path = bodyPath;
            return { done: bodyPath.node };
          }
          // Dispatch via shared driver to _rtoiStep — keeps the JS call
          // stack flat across mutual _resolveCallReturnToObject ↔
          // _resolveToObject recursion.
          return {
            trace: bodyPath, state: _RCRTO_ARROW_AFTER,
            stepFn: _rtoiStep, makeFrame: _rtoiMakeFrame,
            shortCircuit: _rtoiShortCircuit, guardPrefix: "T", defaultResult: null,
          };
        }

        // Block body — pre-collect ReturnStatement arg paths and iterate
        // via the state machine. First non-null result wins (matches the
        // original `if (result) return;` early exit).
        var retPaths = [];
        try {
          funcPath.traverse(Object.assign({
            ReturnStatement: function(retPath) {
              if (retPath.node.argument) retPaths.push(retPath.get("argument"));
            },
          }, _SKIP_NESTED_FUNCS));
        } catch (e) { _resolver.collectError(e, "rcrtoCollect"); }
        if (retPaths.length === 0) return { done: null };

        L.retPaths = retPaths;
        L.ri = 0;
        F.state = _RCRTO_LOOP;
        continue;
      }

      case _RCRTO_ARROW_AFTER:
        return { done: F.result };

      case _RCRTO_LOOP: {
        // If just returned from a sub-trace, check result first.
        if (L.justTraced) {
          L.justTraced = false;
          if (F.result) return { done: F.result };
        }
        while (L.ri < L.retPaths.length) {
          var argPath = L.retPaths[L.ri];
          L.ri++;
          if (_t.isObjectExpression(argPath.node)) {
            argPath.node._path = argPath;
            return { done: argPath.node };
          }
          L.justTraced = true;
          return {
            trace: argPath, state: _RCRTO_LOOP,
            stepFn: _rtoiStep, makeFrame: _rtoiMakeFrame,
            shortCircuit: _rtoiShortCircuit, guardPrefix: "T", defaultResult: null,
          };
        }
        return { done: null };
      }

      default: return { done: null };
    }
  }
}

function _resolveCallReturnToObject(callPath, depth) {
  var node = callPath && callPath.node;
  if (!node) return null;
  if (!_resolver.guard("O", node)) return null;
  var initialFrame = _rcrtoMakeFrame(callPath, node, depth || 0,
    _rcrtoStep, null, "O", null);
  return _runResolverStack(initialFrame);
}

// Resolve an expression to its ObjectExpression node (if it's a variable pointing to one).
// Memoization: profiling on real github bundles (react-lib's 380KB
// module-exports object) showed the resolver invoked 11M+ times during
// one analysis (each `t.X` read triggers the chain). Cache by node
// identity — results are deterministic per-analysis. Cycles caught via
// _resolver.guard at frame push.
//
// State IDs for _rtoiStep (state machine for _resolveToObject).
var _RTOI_INIT = 0;
// Identifier branch
var _RTOI_ID_CALL_INIT_AFTER = 100;
var _RTOI_ID_ARR_ELEM_AFTER = 110;
var _RTOI_ID_ARR_RESOLVED_AFTER = 120;
var _RTOI_ID_OBJ_PROP_DIRECT_AFTER = 130;
var _RTOI_ID_OBJ_INIT_AFTER = 140;
var _RTOI_ID_OBJ_PROP_RESOLVED_AFTER = 150;
var _RTOI_ID_PARAM_LOOP = 160;
// Generic CallExpression branch
var _RTOI_GENERIC_CALL_AFTER = 210;
// MemberExpression branch
var _RTOI_MEMBER_THIS_RHS_AFTER = 300;
var _RTOI_MEMBER_PARENT_AFTER = 310;

function _rtoiMakeFrame(path, node, depth, stepFn, shortCircuit, guardPrefix, defaultResult) {
  return {
    path: path, node: node, depth: depth || 0,
    state: _RTOI_INIT, L: {}, result: null,
    stepFn: stepFn || _rtoiStep,
    makeFrame: _rtoiMakeFrame,
    shortCircuit: shortCircuit || _rtoiShortCircuit,
    guardPrefix: guardPrefix !== undefined ? guardPrefix : "T",
    defaultResult: defaultResult !== undefined ? defaultResult : null,
  };
}

// Short-circuit: literal ObjectExpression resolves directly without a
// frame, and memo-cached results bypass dispatch entirely.
function _rtoiShortCircuit(node, path) {
  if (_resolveToObjectMemo.has(node)) return _resolveToObjectMemo.get(node);
  if (_t.isObjectExpression(node)) {
    node._path = path;
    return node;
  }
  return undefined;
}

// Memoize on completion. The driver does not own per-resolver memo
// tables; centralizing the write keeps every `return _rtoiDone(F, X)`
// inside _rtoiStep consistent with the entry-point cache.
function _rtoiDone(F, value) {
  _resolveToObjectMemo.set(F.node, value);
  return { done: value };
}

function _resolveToObject(initialPath, initialDepth) {
  var initialNode = initialPath && initialPath.node;
  if (!initialNode) return null;
  var sc = _rtoiShortCircuit(initialNode, initialPath);
  if (sc !== undefined) return sc;
  if (!_resolver.guard("T", initialNode)) return null;
  var initialFrame = _rtoiMakeFrame(initialPath, initialNode, initialDepth || 0,
    _rtoiStep, _rtoiShortCircuit, "T", null);
  var result = _runResolverStack(initialFrame);
  _resolveToObjectMemo.set(initialNode, result);
  return result;
}

// Pure helper for the this.prop "_findThisAssignedParamRich" branch.
// Walks class binding refs and indirect ctor-arg sources without any
// _resolveToObject recursion — every value pulled is a literal object
// at the new ClassName(...) call site, so no sub-trace is needed.
function _rtoiResolveAssignedRich(F, path, depth) {
  var L = F.L;
  var ctorMethod = L.ctorMethod;
  var ctorMethodNode = L.ctorMethodNode;
  var classDecl = L.classDecl;
  var className = L.className;
  var propName = L.memberPropName;
  if (!ctorMethod || !ctorMethodNode || !classDecl) return null;
  var _assignedRich = _findThisAssignedParamRich(ctorMethod, propName);
  if (!_assignedRich) return null;
  var _ctrMatch = _findCtorParamOrDestr(ctorMethodNode.params, _assignedRich.paramName);
  if (!_ctrMatch) return null;
  var paramIdx = _ctrMatch.idx;
  var _destrKey = _assignedRich.propFromParam || _ctrMatch.key || null;
  function _pullObj(ctorArgPath) {
    if (!_destrKey) {
      if (_t.isObjectExpression(ctorArgPath.node)) {
        ctorArgPath.node._path = ctorArgPath;
        return ctorArgPath.node;
      }
      return null;
    }
    if (_t.isObjectExpression(ctorArgPath.node)) {
      for (var pi = 0; pi < ctorArgPath.node.properties.length; pi++) {
        var op = ctorArgPath.node.properties[pi];
        if (!_t.isObjectProperty(op) || op.computed) continue;
        var opKey = _t.isIdentifier(op.key) ? op.key.name :
          (_t.isStringLiteral(op.key) ? op.key.value : null);
        if (opKey === _destrKey && _t.isObjectExpression(op.value)) {
          op.value._path = ctorArgPath.get("properties." + pi + ".value");
          return op.value;
        }
      }
    }
    return null;
  }
  var classBinding = path.scope.getBinding(className) || classDecl.scope.getBinding(className);
  if (classBinding && classBinding.referencePaths) {
    for (var cri = 0; cri < classBinding.referencePaths.length; cri++) {
      var cref = classBinding.referencePaths[cri];
      if (cref.parent && _t.isNewExpression(cref.parent) && cref.parent.callee === cref.node &&
          paramIdx < cref.parent.arguments.length) {
        var ctorArgPath = cref.parentPath.get("arguments." + paramIdx);
        var _pulled = _pullObj(ctorArgPath);
        if (_pulled) return _pulled;
      }
    }
    var _indirect = _findClassIndirectCtorArgs(classBinding, classDecl, paramIdx, depth);
    for (var _ii = 0; _ii < _indirect.length; _ii++) {
      var _pulledI = _pullObj(_indirect[_ii]);
      if (_pulledI) return _pulledI;
    }
  }
  return null;
}

function _rtoiStep(F) {
  var path = F.path, node = F.node, depth = F.depth, L = F.L;
  while (true) {
    switch (F.state) {
      case _RTOI_INIT: {
        // Identifier branch
        if (_t.isIdentifier(node)) {
          var binding = path.scope.getBinding(node.name);
          if (!binding) {
            var globalDef = _globalAssignments[node.name];
            if (globalDef && _t.isObjectExpression(globalDef.valueNode)) {
              globalDef.valueNode._path = globalDef.valuePath;
              return _rtoiDone(F, globalDef.valueNode);
            }
            return _rtoiDone(F, null);
          }
          if (_t.isVariableDeclarator(binding.path.node) && binding.path.node.init) {
            if (_t.isObjectExpression(binding.path.node.init)) {
              binding.path.node.init._path = binding.path.get("init");
              return _rtoiDone(F, binding.path.node.init);
            }
            // var cfg = getConfig() — dispatch through _rcrtoStep on the
            // shared driver so the JS call stack stays flat across
            // mutual call/object resolution.
            if (_t.isCallExpression(binding.path.node.init)) {
              return {
                trace: binding.path.get("init"),
                state: _RTOI_ID_CALL_INIT_AFTER,
                stepFn: _rcrtoStep, makeFrame: _rcrtoMakeFrame,
                shortCircuit: null, guardPrefix: "O", defaultResult: null,
              };
            }
            // ArrayPattern destructuring: `var [a, modules] = data`.
            if (_t.isArrayPattern(binding.path.node.id)) {
              var arrPat2 = binding.path.node.id;
              var elemIdx2 = -1;
              for (var dei = 0; dei < arrPat2.elements.length; dei++) {
                var dEl = arrPat2.elements[dei];
                if (_t.isIdentifier(dEl, { name: node.name })) { elemIdx2 = dei; break; }
                if (_t.isAssignmentPattern(dEl) && _t.isIdentifier(dEl.left, { name: node.name })) { elemIdx2 = dei; break; }
              }
              if (elemIdx2 >= 0) {
                var initE = binding.path.node.init;
                if (_t.isArrayExpression(initE) && elemIdx2 < initE.elements.length && initE.elements[elemIdx2]) {
                  return {
                    trace: binding.path.get("init.elements." + elemIdx2),
                    state: _RTOI_ID_ARR_ELEM_AFTER,
                  };
                }
                // _resolveToArray is itself a state-machine driver; one
                // wrapper frame, no recursion through _rtoiStep.
                var arrI = _resolveToArray(binding.path.get("init"), depth);
                if (arrI && arrI._path && elemIdx2 < arrI.elements.length && arrI.elements[elemIdx2]) {
                  return {
                    trace: arrI._path.get("elements." + elemIdx2),
                    state: _RTOI_ID_ARR_RESOLVED_AFTER,
                  };
                }
              }
            }
            // ObjectPattern destructuring: `var {key} = obj`.
            if (_t.isObjectPattern(binding.path.node.id)) {
              var keyForName2 = _findDestructuredKey(binding.path.node.id, node.name);
              if (keyForName2) {
                L.opKey = keyForName2;
                var initO = binding.path.node.init;
                if (_t.isObjectExpression(initO)) {
                  for (var oki2 = 0; oki2 < initO.properties.length; oki2++) {
                    var okp2 = initO.properties[oki2];
                    if (_t.isObjectProperty(okp2) && !okp2.computed && _getKeyName(okp2.key) === keyForName2) {
                      return {
                        trace: binding.path.get("init.properties." + oki2 + ".value"),
                        state: _RTOI_ID_OBJ_PROP_DIRECT_AFTER,
                      };
                    }
                  }
                }
                // Init isn't a literal object — resolve it, then look up
                // keyForName2 in the result. Two-step sub-trace.
                return {
                  trace: binding.path.get("init"),
                  state: _RTOI_ID_OBJ_INIT_AFTER,
                };
              }
            }
          }
          // Param: resolve from caller's arg.
          if (binding.kind === "param" && binding.path.parentPath) {
            var paramFnPath = binding.path.parentPath;
            if (_t.isFunction(paramFnPath.node)) {
              var pIdxObj = -1;
              for (var pi = 0; pi < paramFnPath.node.params.length; pi++) {
                if (_t.isIdentifier(paramFnPath.node.params[pi], { name: node.name })) { pIdxObj = pi; break; }
              }
              if (pIdxObj >= 0) {
                var fbObj = _getFunctionBinding(paramFnPath);
                if (fbObj && fbObj.referencePaths) {
                  L.paramRefs = fbObj.referencePaths;
                  L.paramPIdx = pIdxObj;
                  L.paramRi = 0;
                  L.paramJustTraced = false;
                  F.state = _RTOI_ID_PARAM_LOOP;
                  continue;
                }
              }
            }
          }
        }
        // Object.assign({}, src1, src2, ...) — merge all object args.
        // The synthesized merged object can't have a single Babel _path
        // (no underlying node owns a .properties array matching the
        // merged set). Per-property _path is attached at synthesis time
        // so downstream consumers reading prop.value._path see the
        // actual source location; node-level _path is omitted to keep
        // navigation safe.
        if (_t.isCallExpression(node) && _t.isMemberExpression(node.callee) &&
            _t.isIdentifier(node.callee.object, { name: "Object" }) &&
            !path.scope.getBinding("Object") &&
            _t.isIdentifier(node.callee.property, { name: "assign" }) &&
            node.arguments.length >= 2) {
          var mergedProps = [];
          for (var oai = 0; oai < node.arguments.length; oai++) {
            var oaArg = node.arguments[oai];
            var oaObj = null;
            var oaObjPath = null;
            if (_t.isObjectExpression(oaArg)) {
              oaObj = oaArg;
              oaObjPath = path.get("arguments." + oai);
            } else if (_t.isIdentifier(oaArg)) {
              var oaBinding = path.scope.getBinding(oaArg.name);
              if (oaBinding && _t.isVariableDeclarator(oaBinding.path.node) && _t.isObjectExpression(oaBinding.path.node.init)) {
                oaObj = oaBinding.path.node.init;
                oaObjPath = oaBinding.path.get("init");
              }
            }
            if (oaObj && oaObjPath) {
              for (var oap = 0; oap < oaObj.properties.length; oap++) {
                var oaProp = oaObj.properties[oap];
                if (!_t.isObjectProperty(oaProp) || oaProp.computed) continue;
                var oaKey = _getKeyName(oaProp.key);
                if (oaKey) {
                  // _path is a shared mutable AST field; overwriting on
                  // every Object.assign visit causes pathological
                  // re-traversal in bundles where the same value node
                  // is reachable from many sources.
                  if (oaProp.value && _t.isObjectExpression(oaProp.value) && !oaProp.value._path) {
                    oaProp.value._path = oaObjPath.get("properties." + oap + ".value");
                  }
                  // Later args override earlier ones (Object.assign semantics).
                  mergedProps = mergedProps.filter(function(mp) {
                    var mpKey = _getKeyName(mp.key);
                    return mpKey !== oaKey;
                  });
                  mergedProps.push(oaProp);
                }
              }
            }
          }
          if (mergedProps.length > 0) {
            var synObj = { type: "ObjectExpression", properties: mergedProps };
            return _rtoiDone(F, synObj);
          }
        }
        // Generic CallExpression — resolve through the callee's return
        // value(s). Symmetric with the `var x = call()` Identifier
        // branch above. Placed AFTER Object.assign so its merge handling
        // runs first.
        if (_t.isCallExpression(node)) {
          return {
            trace: path,
            state: _RTOI_GENERIC_CALL_AFTER,
            stepFn: _rcrtoStep, makeFrame: _rcrtoMakeFrame,
            shortCircuit: null, guardPrefix: "O", defaultResult: null,
          };
        }
        // MemberExpression: obj.prop where prop's value is an ObjectExpression.
        if (_t.isMemberExpression(node) && !node.computed) {
          var propName = _t.isIdentifier(node.property) ? node.property.name : null;
          if (propName) {
            L.memberPropName = propName;
            // `this.prop` inside a class method: trace through the
            // constructor's `this.prop = expr` / `this.prop = param`
            // assignment and pick up the object literal at a
            // `new C({...})` call site.
            if (_t.isThisExpression(node.object)) {
              var thisFuncPath = path.getFunctionParent && path.getFunctionParent();
              if (thisFuncPath && _t.isClassMethod(thisFuncPath.node) && _t.isClassBody(thisFuncPath.parent)) {
                var classDecl = thisFuncPath.parentPath.parentPath;
                if (classDecl && (_t.isClassDeclaration(classDecl.node) || _t.isClassExpression(classDecl.node)) && classDecl.node.id) {
                  var className = classDecl.node.id.name;
                  var ctorMethod = null, ctorMethodNode = null;
                  for (var cmi = 0; cmi < classDecl.node.body.body.length; cmi++) {
                    if (_t.isClassMethod(classDecl.node.body.body[cmi]) && classDecl.node.body.body[cmi].kind === "constructor") {
                      ctorMethod = classDecl.get("body.body." + cmi);
                      ctorMethodNode = classDecl.node.body.body[cmi];
                      break;
                    }
                  }
                  if (ctorMethod && ctorMethodNode) {
                    L.ctorMethod = ctorMethod;
                    L.ctorMethodNode = ctorMethodNode;
                    L.classDecl = classDecl;
                    L.className = className;
                    // Fast path: this.X = expr where expr is a closure
                    // value. Dispatch the RHS through _rtoiStep on the
                    // shared driver.
                    var _ctorRhs = _findThisAssignmentRhsPath(ctorMethod, propName);
                    if (_ctorRhs) {
                      return {
                        trace: _ctorRhs,
                        state: _RTOI_MEMBER_THIS_RHS_AFTER,
                      };
                    }
                    // No this.X = expr; try the param-substitution branch
                    // inline (no recursion required).
                    var _assignedObj = _rtoiResolveAssignedRich(F, path, depth);
                    if (_assignedObj) return _rtoiDone(F, _assignedObj);
                  }
                }
              }
            }
            // Resolve parent object then look up propName in its properties.
            return {
              trace: path.get("object"),
              state: _RTOI_MEMBER_PARENT_AFTER,
            };
          }
        }
        return _rtoiDone(F, null);
      }

      case _RTOI_ID_CALL_INIT_AFTER:
      case _RTOI_ID_ARR_ELEM_AFTER:
      case _RTOI_ID_ARR_RESOLVED_AFTER:
      case _RTOI_ID_OBJ_PROP_DIRECT_AFTER:
      case _RTOI_ID_OBJ_PROP_RESOLVED_AFTER:
      case _RTOI_GENERIC_CALL_AFTER:
        return _rtoiDone(F, F.result);

      case _RTOI_ID_OBJ_INIT_AFTER: {
        var initOO = F.result;
        if (initOO && initOO._path) {
          for (var oki3 = 0; oki3 < initOO.properties.length; oki3++) {
            var okp3 = initOO.properties[oki3];
            if (_t.isObjectProperty(okp3) && !okp3.computed && _getKeyName(okp3.key) === L.opKey) {
              return {
                trace: initOO._path.get("properties." + oki3 + ".value"),
                state: _RTOI_ID_OBJ_PROP_RESOLVED_AFTER,
              };
            }
          }
        }
        return _rtoiDone(F, null);
      }

      case _RTOI_ID_PARAM_LOOP: {
        // First non-null caller-arg result wins (matches the original
        // `if (pObj) return pObj;` early exit).
        if (L.paramJustTraced) {
          L.paramJustTraced = false;
          if (F.result) return _rtoiDone(F, F.result);
        }
        while (L.paramRi < L.paramRefs.length) {
          var refO = L.paramRefs[L.paramRi];
          L.paramRi++;
          if (_t.isCallExpression(refO.parent) && refO.parent.callee === refO.node &&
              L.paramPIdx < refO.parent.arguments.length) {
            var argP = refO.parentPath.get("arguments." + L.paramPIdx);
            L.paramJustTraced = true;
            return {
              trace: argP,
              state: _RTOI_ID_PARAM_LOOP,
            };
          }
        }
        return _rtoiDone(F, null);
      }

      case _RTOI_MEMBER_THIS_RHS_AFTER: {
        if (F.result) return _rtoiDone(F, F.result);
        // RHS didn't resolve to an object — fall through to the param
        // substitution branch (no recursion required).
        var _assignedObj2 = _rtoiResolveAssignedRich(F, path, depth);
        if (_assignedObj2) return _rtoiDone(F, _assignedObj2);
        // Then fall through to parent lookup so `this.x.y` on a class
        // whose ctor doesn't initialize `x` still resolves through the
        // outer expression's parent.
        return {
          trace: path.get("object"),
          state: _RTOI_MEMBER_PARENT_AFTER,
        };
      }

      case _RTOI_MEMBER_PARENT_AFTER: {
        var parentObj = F.result;
        if (parentObj) {
          for (var i = 0; i < parentObj.properties.length; i++) {
            var prop = parentObj.properties[i];
            if (!_t.isObjectProperty(prop) || prop.computed) continue;
            var key = _t.isIdentifier(prop.key) ? prop.key.name :
              (_t.isStringLiteral(prop.key) ? prop.key.value : null);
            if (key === L.memberPropName && _t.isObjectExpression(prop.value)) {
              // Synthetic Object.assign-merged parentObj has no
              // node-level _path — per-property _path was set at
              // synthesis time, so don't overwrite. For real
              // ObjectExpression, derive child path from parent.
              if (parentObj._path) {
                prop.value._path = parentObj._path.get("properties." + i + ".value");
              }
              return _rtoiDone(F, prop.value);
            }
          }
        }
        return _rtoiDone(F, null);
      }

      default: return _rtoiDone(F, null);
    }
  }
}

// Resolve an expression to its ArrayExpression node (if it's a variable pointing to one)
// State IDs for _rtaStep (state machine for _resolveToArray).
var _RTA_INIT = 0;
var _RTA_PARAM_LOOP = 100;

function _rtaMakeFrame(path, node, depth) {
  return { path: path, node: node, depth: depth || 0, state: _RTA_INIT, L: {}, result: null };
}

// Explicit-frame state-machine driver for _resolveToArray.
// Returns ArrayExpression node (with _path attached) or null.
function _resolveToArray(initialPath, initialDepth) {
  var initialNode = initialPath && initialPath.node;
  if (!initialNode) return null;
  if (_t.isArrayExpression(initialNode)) {
    initialNode._path = initialPath;
    return initialNode;
  }
  if (!_resolver.guard("A", initialNode)) return null;

  var stack = [_rtaMakeFrame(initialPath, initialNode, initialDepth || 0)];
  var lastResult = null;
  try {
    while (stack.length > 0) {
      var top = stack[stack.length - 1];
      top.result = lastResult;
      lastResult = null;
      var step;
      try { step = _rtaStep(top); }
      catch (e) {
        if (e instanceof RangeError) {
          _resolver.collectError(e, "resolveToArray");
          _resolver.unguard("A", top.node);
          stack.pop();
          lastResult = null;
          continue;
        }
        throw e;
      }
      if (step.done !== undefined) {
        lastResult = step.done;
        _resolver.unguard("A", top.node);
        stack.pop();
        continue;
      }
      var subPath = step.trace;
      top.state = step.state;
      if (!subPath || !subPath.node) { lastResult = null; continue; }
      var subNode = subPath.node;
      // ArrayExpression short-circuit — no frame needed, no guard.
      if (_t.isArrayExpression(subNode)) {
        subNode._path = subPath;
        lastResult = subNode;
        continue;
      }
      if (!_resolver.guard("A", subNode)) { lastResult = null; continue; }
      stack.push(_rtaMakeFrame(subPath, subNode, top.depth + 1));
    }
    return lastResult;
  } finally {
    while (stack.length > 0) _resolver.unguard("A", stack.pop().node);
  }
}

function _rtaStep(F) {
  var path = F.path, node = F.node, L = F.L;
  while (true) {
    switch (F.state) {
      case _RTA_INIT: {
        if (!_t.isIdentifier(node)) return { done: null };
        var binding = path.scope.getBinding(node.name);
        if (!binding) return { done: null };
        if (_t.isVariableDeclarator(binding.path.node) && binding.path.node.init &&
            _t.isArrayExpression(binding.path.node.init)) {
          binding.path.node.init._path = binding.path.get("init");
          return { done: binding.path.node.init };
        }
        // Parameter: resolve from callers via state-machine loop.
        if (binding.kind === "param") {
          var paramFuncPath = binding.scope.path;
          if (_t.isFunction(paramFuncPath.node)) {
            var pIdx = _findParamIndex(paramFuncPath.node.params, node.name);
            if (pIdx >= 0) {
              var fb = null;
              if (paramFuncPath.node.id) fb = (paramFuncPath.scope.parent || paramFuncPath.scope).getBinding(paramFuncPath.node.id.name);
              if (!fb && _t.isVariableDeclarator(paramFuncPath.parent)) fb = (paramFuncPath.scope.parent || paramFuncPath.scope).getBinding(paramFuncPath.parent.id.name);
              if (fb && fb.referencePaths) {
                L.paramRefs = fb.referencePaths;
                L.paramPIdx = pIdx;
                L.paramRi = 0;
                L.paramJustTraced = false;
                F.state = _RTA_PARAM_LOOP;
                continue;
              }
            }
          }
        }
        return { done: null };
      }

      case _RTA_PARAM_LOOP: {
        // If just returned from a sub-trace, check result; non-null wins.
        if (L.paramJustTraced) {
          L.paramJustTraced = false;
          if (F.result) return { done: F.result };
        }
        while (L.paramRi < L.paramRefs.length) {
          var ref = L.paramRefs[L.paramRi];
          L.paramRi++;
          if (_t.isCallExpression(ref.parent) && ref.parent.callee === ref.node && L.paramPIdx < ref.parent.arguments.length) {
            var argPath = ref.parentPath.get("arguments." + L.paramPIdx);
            if (_t.isArrayExpression(argPath.node)) {
              argPath.node._path = argPath;
              return { done: argPath.node };
            }
            L.paramJustTraced = true;
            return { trace: argPath, state: _RTA_PARAM_LOOP };
          }
        }
        return { done: null };
      }

      default:
        return { done: null };
    }
  }
}

function _resolveParamFromCallers(binding, depth, propName) {
  // Per-analysis memo keyed by (binding identifier, propName). Same
  // query returns the cached array. Hot resolver paths repeatedly
  // ask the same caller chain. depth is NOT part of the key — the
  // resolved set is depth-independent (depth only affects guard
  // nesting), and including it would defeat the cache for any
  // caller that varies depth across calls to the same binding.
  var memoKey = propName || "_NULL_";
  var memoForBinding = _resolveParamFromCallersMemo.get(binding.identifier);
  if (memoForBinding && memoForBinding[memoKey] !== undefined) {
    return memoForBinding[memoKey];
  }
  var result = _resolveParamFromCallersUncached(binding, depth, propName);
  if (!memoForBinding) {
    memoForBinding = {};
    _resolveParamFromCallersMemo.set(binding.identifier, memoForBinding);
  }
  memoForBinding[memoKey] = result;
  return result;
}

function _resolveParamFromCallersUncached(binding, depth, propName) {
  if (!_resolver.guard("P", binding.identifier)) return [];
  try {

  // Find the function that has this parameter
  var funcPath = binding.scope.path;
  if (!_t.isFunction(funcPath.node)) return [];

  // Debug: describe the enclosing function context
  var _funcId = funcPath.node.id ? funcPath.node.id.name : "(anon)";
  var _funcParentType = funcPath.parent ? funcPath.parent.type : "none";
  var _funcParentDetail = "";
  if (_t.isObjectProperty(funcPath.parent)) {
    var _fpk = funcPath.parent.key;
    _funcParentDetail = " key=" + (_t.isIdentifier(_fpk) ? _fpk.name : (_t.isStringLiteral(_fpk) ? _fpk.value : "?"));
  } else if (_t.isCallExpression(funcPath.parent)) {
    var _fpc = funcPath.parent.callee;
    if (_t.isIdentifier(_fpc)) _funcParentDetail = " callee=" + _fpc.name;
    else if (_t.isMemberExpression(_fpc) && _t.isIdentifier(_fpc.object) && _t.isIdentifier(_fpc.property))
      _funcParentDetail = " callee=" + _fpc.object.name + "." + _fpc.property.name;
  } else if (_t.isReturnStatement(funcPath.parent)) {
    _funcParentDetail = " (returned)";
  } else if (_t.isAssignmentExpression(funcPath.parent)) {
    var _fpl = funcPath.parent.left;
    if (_t.isIdentifier(_fpl)) _funcParentDetail = " assigned=" + _fpl.name;
    else if (_t.isMemberExpression(_fpl) && _t.isIdentifier(_fpl.object) && _t.isIdentifier(_fpl.property))
      _funcParentDetail = " assigned=" + _fpl.object.name + "." + _fpl.property.name;
    else if (_t.isMemberExpression(_fpl)) _funcParentDetail = " assigned=MemberExpr";
  }
  var _paramNames = funcPath.node.params.map(function(pp) { return _t.isIdentifier(pp) ? pp.name : pp.type; }).join(", ");
  console.debug("[AST:trace]   _resolveParamFromCallers: param=%s prop=%s func=%s(%s) parent=%s%s",
    binding.identifier.name, propName || "none", _funcId, _paramNames, _funcParentType, _funcParentDetail);

  // Find parameter index
  var paramIdx = _findParamIndex(funcPath.node.params, binding.identifier.name);

  // Destructured parameter: function f({url, method}) { fetch(url); }
  // The binding "url" is inside an ObjectPattern at params[i].
  // Resolve by finding the property key, then extracting it from callers' object arguments.
  if (paramIdx === -1) {
    for (var di = 0; di < funcPath.node.params.length; di++) {
      var dParam = funcPath.node.params[di];
      // Direct destructuring: function f({url, method})
      if (_t.isObjectPattern(dParam)) {
        var dKey = _findDestructuredKey(dParam, binding.identifier.name);
        if (dKey) { paramIdx = di; propName = dKey; break; }
      }
      // Destructuring with default: function f({url, method} = {})
      if (_t.isAssignmentPattern(dParam) && _t.isObjectPattern(dParam.left)) {
        var dKey2 = _findDestructuredKey(dParam.left, binding.identifier.name);
        if (dKey2) { paramIdx = di; propName = dKey2; break; }
      }
    }
  }
  if (paramIdx === -1) { console.debug("[AST:trace]     → paramIdx not found, aborting"); return []; }
  console.debug("[AST:trace]     paramIdx=%d", paramIdx);

  // Find the function's binding (how it's referenced)
  var funcBinding = _getFunctionBinding(funcPath);
  // Also check VariableDeclarator above assignment chain: const Se = me = function()
  // When callers use Se() but funcBinding is me, we need Se's binding too
  var _altFuncBinding = null;
  if (funcBinding && _t.isAssignmentExpression(funcPath.parent)) {
    var _chain = funcPath.parentPath;
    while (_chain && _t.isAssignmentExpression(_chain.node)) _chain = _chain.parentPath;
    if (_chain && _t.isVariableDeclarator(_chain.node) && _t.isIdentifier(_chain.node.id)) {
      _altFuncBinding = _chain.scope.getBinding(_chain.node.id.name);
      if (_altFuncBinding === funcBinding) _altFuncBinding = null;
    }
  }
  if (!funcBinding && _t.isObjectProperty(funcPath.parent)) {
    // { method: function(url) { ... } } — trace via obj.method(...) call sites
    var methodKey = funcPath.parent.key;
    var methodName = _t.isIdentifier(methodKey) ? methodKey.name :
      (_t.isStringLiteral(methodKey) ? methodKey.value : null);
    if (methodName) {
      console.debug("[AST:trace]     → ObjectProperty route: methodName=%s", methodName);
      return _resolveParamFromObjectMethod(funcPath, paramIdx, methodName, depth, propName);
    }
    console.debug("[AST:trace]     → ObjectProperty but no method name, aborting");
    return [];
  }
  // ES6 shorthand: { method(url) { ... } } — funcPath IS an ObjectMethod
  // whose own .key is the method name, parent is the ObjectExpression.
  // Callers route the same way as the ObjectProperty branch.
  if (!funcBinding && funcPath.isObjectMethod && funcPath.isObjectMethod()) {
    var omKey = funcPath.node.key;
    var omName = _t.isIdentifier(omKey) && !funcPath.node.computed ? omKey.name :
      (_t.isStringLiteral(omKey) ? omKey.value : null);
    if (omName) {
      console.debug("[AST:trace]     → ObjectMethod route: methodName=%s", omName);
      return _resolveParamFromObjectMethod(funcPath, paramIdx, omName, depth, propName);
    }
    return [];
  }
  if (!funcBinding && _t.isAssignmentExpression(funcPath.parent) &&
      _t.isMemberExpression(funcPath.parent.left) && !funcPath.parent.left.computed) {
    // obj.method = function(url) { ... } — trace via obj.method(...) call sites
    var assignProp = funcPath.parent.left.property;
    var assignMethodName = _t.isIdentifier(assignProp) ? assignProp.name : null;
    var assignObj = funcPath.parent.left.object;
    if (assignMethodName && _t.isIdentifier(assignObj)) {
      var assignObjBinding = funcPath.scope.getBinding(assignObj.name);
      if (assignObjBinding) {
        return _resolveParamFromMethodCalls(assignObjBinding, assignMethodName, paramIdx, depth, propName);
      }
      // Global assignment: window.doFetch = function(url) { ... }
      // The function is called as doFetch(...) — a bare identifier call with no binding.
      // Use _globalAssignments to confirm this is a known global, then find callers
      // by scanning the AST for bare identifier calls matching the assigned name.
      var isGlobalTarget = _isGlobalObject(assignObj.name, funcPath.scope);
      if (isGlobalTarget && _globalAssignments[assignMethodName]) {
        return _resolveParamFromGlobalCallers(funcPath, assignMethodName, paramIdx, depth, propName);
      }
    }
    // Ctor.prototype.method = function(params) { ... } — trace via instance.method(...) call sites
    if (assignMethodName && _t.isMemberExpression(assignObj) && !assignObj.computed &&
        (_t.isIdentifier(assignObj.property, { name: "prototype" }) ||
         (_t.isStringLiteral(assignObj.property) && assignObj.property.value === "prototype")) &&
        _t.isIdentifier(assignObj.object)) {
      var protoCtorName = assignObj.object.name;
      return _resolveParamFromPrototypeMethodCallers(funcPath, protoCtorName, assignMethodName, paramIdx, depth, propName);
    }
    return [];
  }
  // Computed member assignment: obj[method] = function(url) { ... }
  // Resolve method to its string/numeric values and search for obj[…] /
  // obj.<key> call sites. Property may be an Identifier (e.g. method
  // name var), a StringLiteral, or a NumericLiteral (e.g. module IDs in
  // a registry table).
  if (!funcBinding && _t.isAssignmentExpression(funcPath.parent) &&
      _t.isMemberExpression(funcPath.parent.left) && funcPath.parent.left.computed) {
    var compObj = funcPath.parent.left.object;
    var compProp = funcPath.parent.left.property;
    if (_t.isIdentifier(compObj)) {
      var compObjBinding = funcPath.scope.getBinding(compObj.name);
      var compPropVals = _resolveAllValues(funcPath.parentPath.get("left.property"), 0);
      if (compObjBinding && compPropVals.length > 0) {
        var compValues = [];
        for (var cvi = 0; cvi < compPropVals.length; cvi++) {
          var compKeyStr = String(compPropVals[cvi]);
          console.debug("[AST:trace]     → computed-member route: %s[%s] → %s.%s()", compObj.name, compKeyStr, compObj.name, compKeyStr);
          var cmVals = _resolveParamFromMethodCalls(compObjBinding, compKeyStr, paramIdx, depth, propName);
          compValues = compValues.concat(cmVals);
        }
        // Also check global aliases
        for (var gn in _globalAssignments) {
          var ga = _globalAssignments[gn];
          if (!ga.valueNode) continue;
          var gaVal = ga.valueNode;
          while (_t.isAssignmentExpression(gaVal)) gaVal = gaVal.right;
          if (_t.isIdentifier(gaVal) && gaVal.name === compObjBinding.identifier.name) {
            for (var cvi2 = 0; cvi2 < compPropVals.length; cvi2++) {
              if (typeof compPropVals[cvi2] !== "string") continue;
              console.debug("[AST:trace]     → computed-member global: %s.%s()", gn, compPropVals[cvi2]);
              var cgVals = _resolveParamFromGlobalCallers(funcPath, gn, paramIdx, depth + 1, propName, compPropVals[cvi2]);
              compValues = compValues.concat(cgVals);
            }
          }
        }
        if (compValues.length > 0) return compValues;
      }
    }
    return [];
  }
  // Callback argument pattern: someFunc(function(param) { sink(param.prop) })
  // The function is an argument to a call expression. Trace into the called function
  // to find where it invokes the callback parameter with concrete arguments.
  if (!funcBinding && _t.isCallExpression(funcPath.parent)) {
    var cbCallExpr = funcPath.parentPath;
    var cbArgIdx = -1;
    for (var cbi = 0; cbi < funcPath.parent.arguments.length; cbi++) {
      if (funcPath.parent.arguments[cbi] === funcPath.node) { cbArgIdx = cbi; break; }
    }
    if (cbArgIdx >= 0) {
      console.debug("[AST:trace]     → callback-arg route: arg[%d] of call, tracing receiver", cbArgIdx);
      var cbValues = _resolveParamFromCallbackArg(cbCallExpr, cbArgIdx, paramIdx, depth, propName);
      if (cbValues.length > 0) return cbValues;
      // HOF-wrapper for anonymous fn: `var w = HOF(fn, …); w(tainted)` —
      // same pattern as the named-function branch but the fn is an inline
      // FunctionExpression/ArrowFunctionExpression with no binding. Only
      // apply when fn is at arg[0] (the wrapped-callable convention).
      if (cbArgIdx === 0) {
        var hofParent2 = cbCallExpr.parentPath;
        var wrapperBinding2 = null;
        if (hofParent2 && _t.isVariableDeclarator(hofParent2.node) && _t.isIdentifier(hofParent2.node.id)) {
          wrapperBinding2 = hofParent2.scope.getBinding(hofParent2.node.id.name);
        }
        if (wrapperBinding2 && wrapperBinding2.referencePaths) {
          var hofVals = [];
          for (var wi2 = 0; wi2 < wrapperBinding2.referencePaths.length; wi2++) {
            var wref2 = wrapperBinding2.referencePaths[wi2];
            if (wref2.parent && _t.isCallExpression(wref2.parent) && wref2.parent.callee === wref2.node &&
                paramIdx < wref2.parent.arguments.length) {
              var wArgPath2 = wref2.parentPath.get("arguments." + paramIdx);
              var wArgVals2 = propName ? _resolvePropertyFromArg(wArgPath2, propName, depth + 1) : _resolveAllValues(wArgPath2, depth + 1);
              hofVals = hofVals.concat(wArgVals2);
            }
          }
          if (hofVals.length > 0) return hofVals;
        }
      }
    }
  }

  // ReturnStatement: function is returned from an enclosing function (e.g., IIFE)
  // Trace up to the IIFE call, find what its result is assigned to, then find callers.
  if (!funcBinding && _t.isReturnStatement(funcPath.parent)) {
    var enclosingFunc = funcPath.findParent(function(p) { return p.isFunction() && p !== funcPath; });
    if (enclosingFunc) {
      var encParent = enclosingFunc.parentPath;
      // IIFE: (function(){...return fn...})() — enclosingFunc is the callee of a CallExpression
      if (encParent && _t.isCallExpression(encParent.node) && encParent.node.callee === enclosingFunc.node) {
        var iifeCallPath = encParent;
        var iifeParent = iifeCallPath.parentPath;
        // var x = IIFE() → find callers of x
        if (iifeParent && _t.isVariableDeclarator(iifeParent.node) && _t.isIdentifier(iifeParent.node.id)) {
          var iifeVarBinding = iifeParent.scope.getBinding(iifeParent.node.id.name);
          if (iifeVarBinding) {
            console.debug("[AST:trace]     → returned-from-IIFE route: var %s = IIFE()", iifeParent.node.id.name);
            funcBinding = iifeVarBinding;
          }
        }
        // (windowAlias).X = IIFE() → find bare callers of X via global callers
        if (!funcBinding && iifeParent && _t.isAssignmentExpression(iifeParent.node) &&
            _t.isMemberExpression(iifeParent.node.left)) {
          var iifeAssignProp = iifeParent.node.left.property;
          var iifeGlobalName = _t.isIdentifier(iifeAssignProp) ? iifeAssignProp.name : null;
          if (iifeGlobalName && _globalAssignments[iifeGlobalName]) {
            console.debug("[AST:trace]     → returned-from-IIFE route: global %s = IIFE()", iifeGlobalName);
            return _resolveParamFromGlobalCallers(funcPath, iifeGlobalName, paramIdx, depth, propName);
          }
        }
      }
      // UMD callback pattern: !function(p, factory){...factory()...}(this, function(){return innerFn})
      // The enclosing function is an ARGUMENT to an outer IIFE, not its callee.
      // Find which parameter it maps to, then check if that parameter's call result is a global.
      if (!funcBinding && encParent && _t.isCallExpression(encParent.node) &&
          encParent.node.callee !== enclosingFunc.node) {
        var outerIIFECall = encParent.node;
        var encArgIdx = -1;
        for (var eai = 0; eai < outerIIFECall.arguments.length; eai++) {
          if (outerIIFECall.arguments[eai] === enclosingFunc.node) { encArgIdx = eai; break; }
        }
        var outerIIFECallee = outerIIFECall.callee;
        // Handle !function(){}() — UnaryExpression wrapping the FunctionExpression
        if (_t.isUnaryExpression(outerIIFECallee)) outerIIFECallee = outerIIFECallee.argument;
        if (encArgIdx >= 0 && (_t.isFunctionExpression(outerIIFECallee) || _t.isArrowFunctionExpression(outerIIFECallee)) &&
            encArgIdx < outerIIFECallee.params.length) {
          var factoryParamName = _t.isIdentifier(outerIIFECallee.params[encArgIdx])
            ? outerIIFECallee.params[encArgIdx].name : null;
          if (factoryParamName) {
            // Scan global assignments for one whose value calls this factory parameter
            for (var gn in _globalAssignments) {
              var ga = _globalAssignments[gn];
              if (ga.valueNode && _t.isCallExpression(ga.valueNode) &&
                  _t.isIdentifier(ga.valueNode.callee) && ga.valueNode.callee.name === factoryParamName) {
                console.debug("[AST:trace]     → UMD-callback route: factory param=%s, global=%s", factoryParamName, gn);
                return _resolveParamFromGlobalCallers(funcPath, gn, paramIdx, depth, propName);
              }
            }
          }
        }
      }
      // Non-IIFE cases (e.g., function withAuth(){return fn}) are already handled
      // by existing _resolveCallReturnToFunction when resolving authedFetch = withAuth()
    }
  }

  // ES6 class method: class Foo { method(param) { ... } }
  // funcPath is ClassMethod, parent is ClassBody, grandparent is ClassDeclaration
  if (!funcBinding && _t.isClassMethod(funcPath.node) && _t.isClassBody(funcPath.parent)) {
    var classDecl = funcPath.parentPath.parentPath;
    if (classDecl && (_t.isClassDeclaration(classDecl.node) || _t.isClassExpression(classDecl.node))) {
      var className = classDecl.node.id ? classDecl.node.id.name : null;
      var methodName = _t.isIdentifier(funcPath.node.key) ? funcPath.node.key.name : null;
      if (className && methodName) {
        console.debug("[AST:trace]     → class-method route: %s.%s()", className, methodName);
        return _resolveParamFromPrototypeMethodCallers(funcPath, className, methodName, paramIdx, depth, propName);
      }
    }
  }

  if (!funcBinding) {
    console.debug("[AST:trace]     → no funcBinding found (parent=%s), aborting", _funcParentType + _funcParentDetail);
    return [];
  }
  console.debug("[AST:trace]     funcBinding found: %s (refs=%d)", funcBinding.identifier.name, funcBinding.referencePaths ? funcBinding.referencePaths.length : 0);

  // Collect values from all call sites
  var values = [];
  // Check primary binding and alternative binding (for const Se = me = function pattern)
  var _bindings = [funcBinding];
  if (_altFuncBinding) _bindings.push(_altFuncBinding);
  for (var bi = 0; bi < _bindings.length; bi++) {
    var refs = _bindings[bi].referencePaths;
    if (!refs) continue;
    for (var r = 0; r < refs.length; r++) {
      var refPath = refs[r];
      if (refPath.parent && _t.isCallExpression(refPath.parent) && refPath.parent.callee === refPath.node) {
        if (paramIdx < refPath.parent.arguments.length) {
          var argPath = refPath.parentPath.get("arguments." + paramIdx);
          var argVals = propName ? _resolvePropertyFromArg(argPath, propName, depth) : _resolveAllValues(argPath, depth + 1);
          values = values.concat(argVals);
        } else {
          values = values.concat(_resolveOverloadedArg(refPath.parentPath, paramIdx, depth, propName));
        }
      }
      // Function.prototype.bind — `f.bind(thisArg, ...preArgs)` returns
      // a function that calls f with preArgs prepended to caller args.
      // Pure ECMAScript spec semantics. When the bind result is assigned
      // to a variable or property and later called, that call's args
      // come AFTER the bound preArgs in f's param list.
      if (refPath.parent && _t.isMemberExpression(refPath.parent) &&
          refPath.parent.object === refPath.node && !refPath.parent.computed &&
          _t.isIdentifier(refPath.parent.property, { name: "bind" })) {
        var bindCallNode = refPath.parentPath ? refPath.parentPath.parent : null;
        var bindCallP = refPath.parentPath ? refPath.parentPath.parentPath : null;
        if (bindCallNode && bindCallP && _t.isCallExpression(bindCallNode) &&
            bindCallNode.callee === refPath.parent && bindCallNode.arguments.length >= 1) {
          var preArgsCount = bindCallNode.arguments.length - 1;  // skip thisArg
          if (paramIdx < preArgsCount) {
            var preArgPath = bindCallP.get("arguments." + (paramIdx + 1));
            var preArgVals = propName ? _resolvePropertyFromArg(preArgPath, propName, depth + 1) : _resolveAllValues(preArgPath, depth + 1);
            values = values.concat(preArgVals);
          } else {
            var callerArgIdx = paramIdx - preArgsCount;
            var bindOwner = bindCallP.parent;
            if (bindOwner && _t.isVariableDeclarator(bindOwner) && _t.isIdentifier(bindOwner.id) &&
                bindOwner.init === bindCallNode) {
              var aliasB = bindCallP.parentPath.scope.getBinding(bindOwner.id.name);
              if (aliasB && aliasB.referencePaths) {
                for (var aIdx = 0; aIdx < aliasB.referencePaths.length; aIdx++) {
                  var aRef2 = aliasB.referencePaths[aIdx];
                  if (_t.isCallExpression(aRef2.parent) && aRef2.parent.callee === aRef2.node &&
                      callerArgIdx < aRef2.parent.arguments.length) {
                    var bArgPath = aRef2.parentPath.get("arguments." + callerArgIdx);
                    var bArgVals = propName ? _resolvePropertyFromArg(bArgPath, propName, depth + 1) : _resolveAllValues(bArgPath, depth + 1);
                    values = values.concat(bArgVals);
                  }
                }
              }
            } else if (bindOwner && _t.isAssignmentExpression(bindOwner) && bindOwner.right === bindCallNode &&
                       _t.isMemberExpression(bindOwner.left) && _t.isIdentifier(bindOwner.left.object)) {
              var bL = bindOwner.left;
              var bKey = null;
              if (!bL.computed && _t.isIdentifier(bL.property)) bKey = bL.property.name;
              else if (bL.computed) {
                if (_t.isStringLiteral(bL.property)) bKey = bL.property.value;
                else if (_t.isNumericLiteral(bL.property)) bKey = String(bL.property.value);
              }
              var bObjBinding = bindCallP.parentPath.scope.getBinding(bL.object.name);
              if (bKey != null && bObjBinding && bObjBinding.referencePaths) {
                for (var boi = 0; boi < bObjBinding.referencePaths.length; boi++) {
                  var boRef = bObjBinding.referencePaths[boi];
                  var boMem = boRef.parentPath;
                  if (!boMem || !boMem.isMemberExpression() || boMem.node.object !== boRef.node) continue;
                  var boMatch = (!boMem.node.computed && _t.isIdentifier(boMem.node.property, { name: bKey })) ||
                    (boMem.node.computed && _t.isStringLiteral(boMem.node.property) && boMem.node.property.value === bKey) ||
                    (boMem.node.computed && _t.isNumericLiteral(boMem.node.property) && String(boMem.node.property.value) === bKey);
                  if (!boMatch) continue;
                  var boCall = boMem.parentPath;
                  if (boCall && boCall.isCallExpression() && boCall.node.callee === boMem.node &&
                      callerArgIdx < boCall.node.arguments.length) {
                    var boArgPath = boCall.get("arguments." + callerArgIdx);
                    var boArgVals = propName ? _resolvePropertyFromArg(boArgPath, propName, depth + 1) : _resolveAllValues(boArgPath, depth + 1);
                    values = values.concat(boArgVals);
                  }
                }
              }
            }
          }
        }
      }
      // HOF-wrapper: `var w = HOF(fn, opts); w(tainted)` — function
      // passed as arg to a HOF whose return is called. Conservatively
      // assume the wrapper forwards args to fn, so `w(...)` callers
      // contribute to fn's param values. Matches memoize/debounce/
      // throttle/once and similar utility HOFs without name matching.
      if (refPath.parent && _t.isCallExpression(refPath.parent) && refPath.parent.callee !== refPath.node) {
        // Find which arg position fn is at
        var hofArgIdx = -1;
        for (var hai = 0; hai < refPath.parent.arguments.length; hai++) {
          if (refPath.parent.arguments[hai] === refPath.node) { hofArgIdx = hai; break; }
        }
        if (hofArgIdx === 0) {
          // Only when fn is the FIRST arg — that's the wrapped-function
          // convention for memoize(fn)/debounce(fn)/wrap(fn)/etc. Other
          // positions are typically options/config and don't call fn
          // with forwarded args.
          var hofCall = refPath.parentPath;
          var hofParent = hofCall.parentPath;
          var wrapperBinding = null;
          if (hofParent && _t.isVariableDeclarator(hofParent.node) && _t.isIdentifier(hofParent.node.id)) {
            wrapperBinding = hofParent.scope.getBinding(hofParent.node.id.name);
          }
          if (wrapperBinding && wrapperBinding.referencePaths) {
            for (var wi = 0; wi < wrapperBinding.referencePaths.length; wi++) {
              var wref = wrapperBinding.referencePaths[wi];
              if (wref.parent && _t.isCallExpression(wref.parent) && wref.parent.callee === wref.node &&
                  paramIdx < wref.parent.arguments.length) {
                var wArgPath = wref.parentPath.get("arguments." + paramIdx);
                var wArgVals = propName ? _resolvePropertyFromArg(wArgPath, propName, depth + 1) : _resolveAllValues(wArgPath, depth + 1);
                values = values.concat(wArgVals);
              }
            }
          }
        }
      }
    }
  }
  return values;
  } catch (_rpce) {
    if (_rpce instanceof RangeError) { _resolver.collectError(_rpce, "resolveParamFromCallers"); return []; }
    throw _rpce;
  } finally { _resolver.unguard("P", binding.identifier); }
}

// Resolve param from obj.method(...) calls when the function is an object property value
function _resolveParamFromObjectMethod(funcPath, paramIdx, methodName, depth, propName) {
  // Walk up to the ObjectExpression. ObjectProperty shape:
  // `{method: function(p){}}` — funcPath.parent is ObjectProperty, its
  // parent is ObjectExpression (2 levels). ObjectMethod shorthand
  // `{method(p){}}` — funcPath IS the ObjectMethod whose parent is
  // directly the ObjectExpression (1 level). Handle both.
  var objExprPath = null;
  if (funcPath.isObjectMethod && funcPath.isObjectMethod()) {
    objExprPath = funcPath.parentPath;
  } else if (funcPath.parentPath) {
    objExprPath = funcPath.parentPath.parentPath;
  }
  if (!objExprPath || !_t.isObjectExpression(objExprPath.node)) return [];

  var declPath = objExprPath.parentPath;
  if (!declPath) return [];

  var objBinding = null;
  if (_t.isVariableDeclarator(declPath.node) && _t.isIdentifier(declPath.node.id)) {
    objBinding = declPath.scope.getBinding(declPath.node.id.name);
  }

  // Handle returned objects: the object is inside a ReturnStatement of a factory function.
  // e.g., function createClient(baseUrl) { return { get: function(path) { fetch(baseUrl + path); } }; }
  // var client = createClient("https://..."); client.get("/path");
  if (!objBinding && _t.isReturnStatement(declPath.node)) {
    var factoryFunc = declPath.getFunctionParent();
    if (factoryFunc) {
      var factoryBinding = _getFunctionBinding(factoryFunc);
      if (factoryBinding) {
        return _resolveParamFromFactoryCallers(factoryBinding, methodName, paramIdx, depth, propName);
      }
    }
  }

  // Handle extend pattern: X.extend({method: function(params) { ... }})
  // Properties get copied to X, so callers use X.method(...)
  if (!objBinding && _t.isCallExpression(declPath.node)) {
    var extCallee = declPath.node.callee;
    if (_t.isMemberExpression(extCallee) && !extCallee.computed && _t.isIdentifier(extCallee.object)) {
      var extObjBinding = declPath.scope.getBinding(extCallee.object.name);
      if (extObjBinding) {
        console.debug("[AST:trace]     extend-pattern: %s.%s({%s: fn}) → searching for %s.%s() calls",
          extCallee.object.name, extCallee.property.name || extCallee.property.value || "?",
          methodName, extCallee.object.name, methodName);
        var extVals = _resolveParamFromMethodCalls(extObjBinding, methodName, paramIdx, depth + 1, propName);
        // Always also check global aliases (external callers like $.ajax() outside the IIFE)
        // e.g., window.jQuery = lib; then jQuery.ajax(...) calls should be found
        for (var gn in _globalAssignments) {
          var ga = _globalAssignments[gn];
          if (!ga.valueNode) continue;
          var gaVal = ga.valueNode;
          while (_t.isAssignmentExpression(gaVal)) gaVal = gaVal.right;
          if (_t.isIdentifier(gaVal) && gaVal.name === extObjBinding.identifier.name) {
            console.debug("[AST:trace]     extend-pattern: %s aliased to global %s, searching for %s.%s() calls",
              extCallee.object.name, gn, gn, methodName);
            var globalVals = _resolveParamFromGlobalCallers(funcPath, gn, paramIdx, depth + 1, propName, methodName);
            extVals = extVals.concat(globalVals);
          }
        }
        return extVals;
      }
      // If the extend target has no binding (e.g., lib defined as {} at module level),
      // check _globalAssignments for it
      var globalDef = _globalAssignments[extCallee.object.name];
      if (globalDef && globalDef.valuePath) {
        var gBinding = globalDef.valuePath.scope.getBinding(extCallee.object.name);
        if (!gBinding) {
          // Try resolving through the global assignment's value
          var gVal = globalDef.valueNode;
          while (_t.isAssignmentExpression(gVal)) gVal = gVal.right;
          if (_t.isIdentifier(gVal)) {
            gBinding = globalDef.valuePath.scope.getBinding(gVal.name);
          }
        }
        if (gBinding) {
          console.debug("[AST:trace]     extend-pattern (global): %s.%s({%s: fn}) → searching for %s.%s() calls",
            extCallee.object.name, extCallee.property.name || "?",
            methodName, extCallee.object.name, methodName);
          return _resolveParamFromMethodCalls(gBinding, methodName, paramIdx, depth + 1, propName);
        }
      }
    }
  }

  if (!objBinding) return [];

  return _resolveParamFromMethodCalls(objBinding, methodName, paramIdx, depth, propName);
}

// Resolve param values when the function is a callback argument:
//   registerCallback(function(param) { sink(param.prop) })
// Traces into the called function to find where it invokes the callback and with what args.
function _resolveParamFromCallbackArg(callExprPath, cbArgIdx, paramIdx, depth, propName) {
  if (!_resolver.guard("C", callExprPath.node)) return [];
  try {
  var calleeNode = callExprPath.node.callee;

  // Resolve the called function
  var targetFuncPath = _resolveCalleeFuncPath(callExprPath, depth + 1);
  var targetFuncNode = targetFuncPath ? targetFuncPath.node : null;

  if (!targetFuncNode) {
    // Try resolving callee as a call return value (e.g. addToPrefiltersOrTransports(structure) returns a function)
    if (_t.isCallExpression(calleeNode)) {
      // Not tractable without deeper analysis
    }
    // Try identifier that resolves to a call expression returning a function
    if (_t.isIdentifier(calleeNode)) {
      var callerBinding = callExprPath.scope.getBinding(calleeNode.name);
      if (callerBinding && _t.isVariableDeclarator(callerBinding.path.node) && callerBinding.path.node.init &&
          _t.isCallExpression(callerBinding.path.node.init)) {
        var retFuncNode = _resolveCallReturnToFunction(callerBinding.path.get("init"), depth + 1);
        if (retFuncNode) { targetFuncNode = retFuncNode.node || retFuncNode; targetFuncPath = retFuncNode._path || null; }
      }
    }
    // For member expressions, resolve through property value being a call return
    if (!targetFuncNode && _t.isMemberExpression(calleeNode) && !calleeNode.computed) {
      var mProp = _t.isIdentifier(calleeNode.property) ? calleeNode.property.name : null;
      if (mProp && _t.isIdentifier(calleeNode.object)) {
        var objBinding = callExprPath.scope.getBinding(calleeNode.object.name);
        if (objBinding) {
          var refs = objBinding.referencePaths;
          for (var ri = 0; ri < refs.length && !targetFuncNode; ri++) {
            var refP = refs[ri].parent;
            // Pattern 1: obj.prop = someCallExpr() or obj.prop = function()
            if (_t.isMemberExpression(refP) && refP.object === refs[ri].node && !refP.computed &&
                _t.isIdentifier(refP.property, { name: mProp })) {
              var asgn = refs[ri].parentPath ? refs[ri].parentPath.parent : null;
              if (asgn && _t.isAssignmentExpression(asgn) && asgn.left === refP) {
                if (_t.isCallExpression(asgn.right)) {
                  var retFunc = _resolveCallReturnToFunction(refs[ri].parentPath.parentPath.get("right"), depth + 1);
                  if (retFunc) { targetFuncNode = retFunc.node || retFunc; targetFuncPath = retFunc._path || null; break; }
                }
                if (_t.isFunctionExpression(asgn.right) || _t.isArrowFunctionExpression(asgn.right)) {
                  targetFuncNode = asgn.right;
                  targetFuncPath = refs[ri].parentPath.parentPath.get("right");
                  break;
                }
              }
            }
            // (Removed `obj.extend({...}) / obj.mixin({...}) / obj.assign({...})`
            // framework-shape recognition per CLAUDE.md L29 ban on
            // framework-specific name matching. The same property-
            // assignment effect is reached spec-compliantly by
            // resolving `obj.<methodName>` through _resolveCalleeFuncPath
            // to the actual extend/mixin/assign function definition,
            // then trace-throughing its body via the inter-procedural
            // caller-arg pipeline. Object.assign — the spec global —
            // is recognised separately at scope-checked call sites.)
          }
        }
      }
    }
  }

  if (!targetFuncNode) {
    // Fallback: arr.forEach(fn) — resolve array element values per
    // ECMA-262 § 23.1.3.15 (Array.prototype.forEach). Receiver is the
    // array; callback is invoked with (element, index, array). The
    // jQuery / underscore / lodash `.each(arr, fn)` shape was removed
    // per CLAUDE.md L29 — those library helpers reach the analysed
    // callback when their bundle is present and the analyzer traces
    // through their own forEach-equivalent call.
    if (_t.isMemberExpression(calleeNode) && !calleeNode.computed) {
      var iterMethod = _t.isIdentifier(calleeNode.property) ? calleeNode.property.name : null;
      if (_ITERATION_METHODS[iterMethod]) {
        // Skip if callee object is a known non-iterable type — `.forEach`
        // also exists on Map/Set but the array-element semantics here
        // assume Array.prototype dispatch.
        var _cbObjType = _getTrackedType(callExprPath.get("callee.object"), calleeNode.object);
        if (_cbObjType && _NON_ITERABLE_TYPES[_cbObjType]) {
          // Known non-iterable — skip
        } else {
        var arrPath = callExprPath.get("callee.object");
        var elemParamIdx = 0;
        if (paramIdx === elemParamIdx) {
          var arrNode = _resolveToArray(arrPath, 0);
          if (arrNode && arrNode.elements && arrNode.elements.length > 0) {
            var iterValues = [];
            for (var avi = 0; avi < arrNode.elements.length; avi++) {
              if (_t.isStringLiteral(arrNode.elements[avi])) iterValues.push(arrNode.elements[avi].value);
            }
            console.debug("[AST:trace]     callback-arg: iteration values from array: [%s]", iterValues.join(", "));
            return iterValues;
          }
        }
        } // end V4 else (not non-iterable)
      }
    }
    console.debug("[AST:trace]     callback-arg: could not resolve callee function");
    return [];
  }

  // Determine which parameter of the target function receives our callback
  // Account for string argument shifting (jQuery pattern: if typeof arg0 !== "string", func = arg0)
  var targetParams = targetFuncNode.params || [];
  var cbParamIdx = cbArgIdx;
  // If there are fewer params than args, the function might shift arguments
  if (cbParamIdx >= targetParams.length) cbParamIdx = targetParams.length - 1;
  if (cbParamIdx < 0) {
    console.debug("[AST:trace]     callback-arg: no param for arg[%d]", cbArgIdx);
    return [];
  }

  var cbParamName = _t.isIdentifier(targetParams[cbParamIdx]) ? targetParams[cbParamIdx].name : null;
  if (!cbParamName) {
    console.debug("[AST:trace]     callback-arg: param[%d] not identifier", cbParamIdx);
    return [];
  }

  console.debug("[AST:trace]     callback-arg: callee param '%s' receives our callback, searching for calls", cbParamName);

  // Search inside the target function for calls to the callback parameter
  // or storage patterns (container.push(cbParam)) that indicate store-and-call-later.
  var values = [];
  if (targetFuncPath) {
    try {
      targetFuncPath.traverse({
        CallExpression: function(innerPath) {
          var ic = innerPath.node.callee;
          // Direct call: cbParam(arg0, arg1, ...)
          if (_t.isIdentifier(ic, { name: cbParamName })) {
            var innerBinding = innerPath.scope.getBinding(cbParamName);
            // Must be the same binding (same function's param)
            if (innerBinding && innerBinding.kind === "param" && innerBinding.scope.path === targetFuncPath) {
              if (paramIdx < innerPath.node.arguments.length) {
                var argPath = innerPath.get("arguments." + paramIdx);
                var argVals = propName ? _resolvePropertyFromArg(argPath, propName, depth + 1) : _resolveAllValues(argPath, depth + 1);
                console.debug("[AST:trace]     callback-arg: direct call found, arg[%d] → [%s]", paramIdx, argVals.join(", "));
                values = values.concat(argVals);
              }
            }
          }

          // .call()/.apply() invocation: cbParam.call(thisArg, arg0, arg1, ...)
          // or cbParam.apply(thisArg, [arg0, arg1, ...])
          // .call() shifts args by 1 (arg0 is at arguments[1])
          if (_t.isMemberExpression(ic) && !ic.computed &&
              _t.isIdentifier(ic.object, { name: cbParamName })) {
            var callMethod = _t.isIdentifier(ic.property) ? ic.property.name : null;
            if (callMethod === "call") {
              var icBinding = innerPath.scope.getBinding(cbParamName);
              if (icBinding && icBinding.kind === "param" && icBinding.scope.path === targetFuncPath) {
                // .call(thisArg, arg0, arg1, ...) — paramIdx+1 to skip thisArg
                var callArgIdx = paramIdx + 1;
                if (callArgIdx < innerPath.node.arguments.length) {
                  var callArgPath = innerPath.get("arguments." + callArgIdx);
                  var callArgVals = propName ? _resolvePropertyFromArg(callArgPath, propName, depth + 1) : _resolveAllValues(callArgPath, depth + 1);
                  console.debug("[AST:trace]     callback-arg: .call() found, arg[%d] → [%s]", callArgIdx, callArgVals.join(", "));
                  values = values.concat(callArgVals);
                }
              }
            }
          }

          // Store-and-call-later: container.push(cbParam), container.unshift(cbParam),
          // or container[key][cond ? "unshift" : "push"](cbParam)
          // When the callback is stored in an array rather than called directly, trace
          // the container to find where items are later retrieved and called.
          var isPushOrUnshift = false;
          if (_t.isMemberExpression(ic) && !ic.computed &&
              (_t.isIdentifier(ic.property, { name: "push" }) || _t.isIdentifier(ic.property, { name: "unshift" }))) {
            isPushOrUnshift = true;
          }
          // Computed conditional: container[cond ? "unshift" : "push"](cb)
          if (!isPushOrUnshift && _t.isMemberExpression(ic) && ic.computed && _t.isConditionalExpression(ic.property)) {
            var condCons = ic.property.consequent;
            var condAlt = ic.property.alternate;
            if ((_t.isStringLiteral(condCons) && (condCons.value === "push" || condCons.value === "unshift")) ||
                (_t.isStringLiteral(condAlt) && (condAlt.value === "push" || condAlt.value === "unshift"))) {
              isPushOrUnshift = true;
            }
          }
          if (isPushOrUnshift) {
            var pushArgs = innerPath.node.arguments;
            var storingCb = false;
            for (var pai = 0; pai < pushArgs.length; pai++) {
              if (_t.isIdentifier(pushArgs[pai], { name: cbParamName })) {
                var pushArgBinding = innerPath.scope.getBinding(cbParamName);
                if (pushArgBinding && pushArgBinding.kind === "param" && pushArgBinding.scope.path === targetFuncPath) {
                  storingCb = true;
                }
              }
              // Also check for derived variable: func = cbParam; container.push(func)
              if (!storingCb && _t.isIdentifier(pushArgs[pai])) {
                var derivedBinding = innerPath.scope.getBinding(pushArgs[pai].name);
                if (derivedBinding && derivedBinding.constantViolations) {
                  for (var dvi = 0; dvi < derivedBinding.constantViolations.length; dvi++) {
                    var dvNode = derivedBinding.constantViolations[dvi].node;
                    if (_t.isAssignmentExpression(dvNode) && _t.isIdentifier(dvNode.right, { name: cbParamName })) {
                      storingCb = true; break;
                    }
                  }
                }
              }
            }
            if (storingCb) {
              // Unwrap to find the container variable:
              // structure[key].push(cb) → structure
              // (structure[key] = structure[key] || []).push(cb) → structure (jQuery pattern)
              var containerNode = ic.object;
              if (_t.isAssignmentExpression(containerNode)) containerNode = containerNode.left;
              while (_t.isMemberExpression(containerNode) && containerNode.computed) {
                containerNode = containerNode.object;
              }
              if (_t.isIdentifier(containerNode)) {
                var containerBinding = innerPath.scope.getBinding(containerNode.name);
                if (containerBinding) {
                  console.debug("[AST:trace]     callback-arg: stored via %s.push(), tracing container", containerNode.name);
                  var storedVals = _resolveStoredCallbackArgs(containerBinding, paramIdx, depth + 1, propName);
                  values = values.concat(storedVals);
                }
              }
            }
          }
        },
        // Don't descend into nested function declarations (scope confusion)
        FunctionDeclaration: function(p) { p.skip(); },
      });
    } catch (e) { _resolver.collectError(e, "resolveParamFromCallers"); }
  }

  // Fallback: arr.forEach(fn) per ECMA-262 § 23.1.3.15 — fn invoked with
  // (element, index, array). The jQuery / underscore / lodash `.each`
  // shape was removed per CLAUDE.md L29.
  if (values.length === 0 && _t.isMemberExpression(calleeNode) && !calleeNode.computed) {
    var iterMethod = _t.isIdentifier(calleeNode.property) ? calleeNode.property.name : null;
    if (iterMethod === "forEach") {
      var _fb2ObjType = _getTrackedType(callExprPath.get("callee.object"), calleeNode.object);
      if (!(_fb2ObjType && _NON_ITERABLE_TYPES[_fb2ObjType])) {
        var arrPath = callExprPath.get("callee.object");
        var elemParamIdx = 0;
        if (paramIdx === elemParamIdx) {
          var arrNode = _resolveToArray(arrPath, 0);
          if (arrNode && arrNode.elements) {
            for (var avi = 0; avi < arrNode.elements.length; avi++) {
              if (_t.isStringLiteral(arrNode.elements[avi])) values.push(arrNode.elements[avi].value);
            }
            if (values.length > 0) console.debug("[AST:trace]     callback-arg: iteration values from array: [%s]", values.join(", "));
          }
        }
      }
    }
  }

  return values;
  } catch (_rce) {
    if (_rce instanceof RangeError) { _resolver.collectError(_rce, "resolvePropertyFromCall"); return []; }
    throw _rce;
  } finally { _resolver.unguard("C", callExprPath.node); }
}

// Trace a container variable (array) to find where its stored items are called.
// When a callback is stored via container.push(cb), this finds patterns like:
//   - container[i](args) — direct indexed call
//   - container.forEach(function(item) { item(args); }) — forEach
//   - someLib.each(container[key], function(_, item) { item(args); }) — library each
// If the container is a parameter, resolves from callers to find the actual variable.
function _resolveStoredCallbackArgs(initialContainerBinding, paramIdx, depth, propName) {
  // Iterative param-chain: when a container is a param, resolve through
  // callers to find the actual variable; when ANY actual variable IS a
  // local container, process it directly. Worklist of bindings; visited
  // set keyed on identifier identity prevents cycles.
  var bindingQueue = [initialContainerBinding];
  var bindingSeen = new Set();
  var values = [];
  while (bindingQueue.length > 0) {
    var containerBinding = bindingQueue.shift();
    if (!containerBinding || bindingSeen.has(containerBinding.identifier)) continue;
    bindingSeen.add(containerBinding.identifier);
    if (!_resolver.guard("S", containerBinding.identifier)) continue;
    try {

  // If the container is a parameter of its function, queue caller arg
  // bindings instead of recursing.
  if (containerBinding.kind === "param") {
    var enclosingFunc = containerBinding.scope.path;
    var containerParamIdx = -1;
    var params = enclosingFunc.node.params;
    for (var pi = 0; pi < params.length; pi++) {
      if (_t.isIdentifier(params[pi]) && params[pi].name === containerBinding.identifier.name) {
        containerParamIdx = pi; break;
      }
    }
    if (containerParamIdx < 0) { _resolver.unguard("S", containerBinding.identifier); continue; }

    var funcBinding = _getFunctionBinding(enclosingFunc);
    if (!funcBinding || !funcBinding.referencePaths) { _resolver.unguard("S", containerBinding.identifier); continue; }

    var callerRefs = funcBinding.referencePaths;
    for (var ri = 0; ri < callerRefs.length; ri++) {
      if (!callerRefs[ri].parent || !_t.isCallExpression(callerRefs[ri].parent) ||
          callerRefs[ri].parent.callee !== callerRefs[ri].node) continue;
      if (containerParamIdx >= callerRefs[ri].parent.arguments.length) continue;

      var actualArg = callerRefs[ri].parent.arguments[containerParamIdx];
      if (_t.isIdentifier(actualArg)) {
        var actualBinding = callerRefs[ri].parentPath.scope.getBinding(actualArg.name);
        if (actualBinding) {
          bindingQueue.push(actualBinding);
        }
      }
    }
    _resolver.unguard("S", containerBinding.identifier);
    continue;
  }

  // Container is a local/module-level variable. Search its references for patterns
  // where items are extracted and called.
  var refs = containerBinding.referencePaths;
  for (var ri = 0; ri < refs.length; ri++) {
    var refPath = refs[ri];

    // Pattern 1: container[i](args) — direct computed-member call
    // AST: CallExpression { callee: MemberExpression(container, i, computed) }
    if (_t.isMemberExpression(refPath.parent) && refPath.parent.object === refPath.node &&
        refPath.parent.computed) {
      var memberCallParent = refPath.parentPath ? refPath.parentPath.parent : null;
      if (memberCallParent && _t.isCallExpression(memberCallParent) &&
          memberCallParent.callee === refPath.parent) {
        // container[i](arg0, arg1, ...) — items are called with these args
        if (paramIdx < memberCallParent.arguments.length) {
          var argPath = refPath.parentPath.parentPath.get("arguments." + paramIdx);
          var argVals = propName ? _resolvePropertyFromArg(argPath, propName, depth + 1) : _resolveAllValues(argPath, depth + 1);
          console.debug("[AST:trace]     stored-callback: found %s[i](args), arg[%d] → [%s]", containerBinding.identifier.name, paramIdx, argVals.join(", "));
          values = values.concat(argVals);
        }
      }
    }

    // Pattern 2: container passed as argument to a function that iterates and calls items.
    if (_t.isCallExpression(refPath.parent) && refPath.parent.callee !== refPath.node) {
      var iterCallPath = refPath.parentPath;
      var iterArgIdx = -1;
      for (var iai = 0; iai < iterCallPath.node.arguments.length; iai++) {
        if (iterCallPath.node.arguments[iai] === refPath.node) { iterArgIdx = iai; break; }
        if (_containsNode(iterCallPath.node.arguments[iai], refPath.node)) { iterArgIdx = iai; break; }
      }
      if (iterArgIdx >= 0) {
        var iterVals = _resolveItemCallsInFunction(iterCallPath, iterArgIdx, paramIdx, depth + 1, propName);
        values = values.concat(iterVals);
      }
    }
  }
    } catch (_rse) {
      if (_rse instanceof RangeError) { _resolver.collectError(_rse, "resolveStoredCallbackArgs"); }
      else throw _rse;
    } finally { _resolver.unguard("S", containerBinding.identifier); }
  }
  return values;
}

// Check if a node tree contains a target node (shallow check for LogicalExpression/MemberExpression)
function _containsNode(node, target) {
  // Iterative: walk LogicalExpression/MemberExpression chains via explicit stack
  var stack = [node];
  while (stack.length > 0) {
    var n = stack.pop();
    if (n === target) return true;
    if (_t.isLogicalExpression(n)) { stack.push(n.left, n.right); }
    else if (_t.isMemberExpression(n)) { stack.push(n.object); }
  }
  return false;
}

// Given a function call where arg[iterArgIdx] contains a container, resolve the function
// and find where items from the corresponding parameter are called.
// Handles: forEach(function(item) { item(args); }), jQuery.each(container, function(_, item) { item(args); })
function _resolveItemCallsInFunction(callPath, iterArgIdx, paramIdx, depth, propName) {
  if (!_resolver.guard("I", callPath.node)) return [];
  try {

  // Check if another argument is a callback function that calls items
  var callArgs = callPath.node.arguments;
  for (var ai = 0; ai < callArgs.length; ai++) {
    if (ai === iterArgIdx) continue;
    if (!_t.isFunctionExpression(callArgs[ai]) && !_t.isArrowFunctionExpression(callArgs[ai])) continue;

    // This argument is a callback — check if any of its params are called
    var cbFuncPath = callPath.get("arguments." + ai);
    var values = [];

    try {
      cbFuncPath.traverse(Object.assign({
        CallExpression: function(innerPath) {
          var ic = innerPath.node.callee;
          if (!_t.isIdentifier(ic)) return;
          var icBinding = innerPath.scope.getBinding(ic.name);
          if (!icBinding || icBinding.kind !== "param") return;
          // Verify this param belongs to the callback function
          if (icBinding.scope.path !== cbFuncPath) return;

          // This param of the callback is called — it's an item from the container
          if (paramIdx < innerPath.node.arguments.length) {
            var argPath = innerPath.get("arguments." + paramIdx);
            var argVals = propName ? _resolvePropertyFromArg(argPath, propName, depth + 1) : _resolveAllValues(argPath, depth + 1);
            console.debug("[AST:trace]     stored-callback: found item call in iterator callback, arg[%d] → [%s]", paramIdx, argVals.join(", "));
            values = values.concat(argVals);
          }
        },
      }, _SKIP_NESTED_FUNCS));
    } catch (e) { _resolver.collectError(e, "storedCallbackArgs"); }

    if (values.length > 0) return values;
  }

  // The called function might not have an inline callback — resolve it and search inside
  // E.g., inspectPrefiltersOrTransports(transports, s, ...) — the function iterates
  // internally and calls items. This requires resolving the function and finding the pattern.
  var targetFuncPath = _resolveCalleeFuncPath(callPath, 0);
  if (targetFuncPath && iterArgIdx < targetFuncPath.node.params.length) {
    var containerParamName = _t.isIdentifier(targetFuncPath.node.params[iterArgIdx]) ? targetFuncPath.node.params[iterArgIdx].name : null;
    if (containerParamName) {
      // Search inside the resolved function for container[key][i](args) or
      // iteration patterns using the parameter
      var funcValues = _resolveItemCallsFromParam(targetFuncPath, containerParamName, paramIdx, depth, propName);
      if (funcValues.length > 0) return funcValues;
    }
  }

  return [];
  } catch (_rie) {
    if (_rie instanceof RangeError) { _resolver.collectError(_rie, "resolveItemCallsInFunction"); return []; }
    throw _rie;
  } finally { _resolver.unguard("I", callPath.node); }
}

// Search inside a resolved function for calls to items from a container parameter.
// Handles: param[key].forEach(fn), jQuery.each(param[key], fn), for loops
function _resolveItemCallsFromParam(funcPath, containerParamName, paramIdx, depth, propName) {
  // Traverse function body to find where items from containerParamName are called.
  // Handles: jQuery.each(param[key], fn), param[key].forEach(fn), param.forEach(fn),
  // for (var i; i < param.length; i++) param[i](args), for-in/for-of loops
  if (!funcPath) return [];

  var values = [];
  try {
    funcPath.traverse({
      CallExpression: function(innerPath) {
        var ic = innerPath.node.callee;

        // Pattern: containerParam[key].forEach(function(item) { item(args); })
        // or containerParam.forEach(function(item) { item(args); })
        // (`.each` shape removed per CLAUDE.md L29 — Array.prototype.forEach
        //  per ECMA § 23.1.3.15 is the only spec-defined match.)
        var iterContainer = null;
        var cbArgStartIdx = -1;

        if (_t.isMemberExpression(ic) && !ic.computed) {
          var methodName = _t.isIdentifier(ic.property) ? ic.property.name : null;
          if (methodName === "forEach") {
            // Check if object is containerParam or containerParam[key]
            var obj = ic.object;
            if (_t.isIdentifier(obj, { name: containerParamName })) {
              iterContainer = obj; cbArgStartIdx = 0;
            } else if (_t.isMemberExpression(obj) && _t.isIdentifier(obj.object, { name: containerParamName })) {
              iterContainer = obj; cbArgStartIdx = 0;
            }
            // Also: (containerParam[key] || []).forEach(fn) — unwrap LogicalExpression
            if (!iterContainer && _t.isLogicalExpression(obj)) {
              var left = obj.left;
              if (_t.isIdentifier(left, { name: containerParamName }) ||
                  (_t.isMemberExpression(left) && _t.isIdentifier(left.object, { name: containerParamName }))) {
                iterContainer = left; cbArgStartIdx = 0;
              }
            }
          }
        }

        // (Removed `.each` framework-name recognition per CLAUDE.md
        // L29 ban on framework-specific shape matching. jQuery /
        // Underscore / Lodash `.each` is just JavaScript — the inter-
        // procedural callback tracer (_traceCallbackArgToArgs) reaches
        // it by resolving someLib's `.each` property to its actual
        // function definition in the same bundle and walking that
        // body for callback invocations. Coverage for genuine `.each`
        // call sites depends on _resolveCalleeFuncPath being able to
        // resolve `someLib.each` to its function definition; if a
        // real-site bundle proves it can't, the gap is in
        // _resolveCalleeFuncPath and gets closed there.)

        if (iterContainer && cbArgStartIdx >= 0) {
          // Find the callback argument after the container
          var callArgs = innerPath.node.arguments;
          for (var cai = cbArgStartIdx; cai < callArgs.length; cai++) {
            if (!_t.isFunctionExpression(callArgs[cai]) && !_t.isArrowFunctionExpression(callArgs[cai])) continue;
            var cbPath = innerPath.get("arguments." + cai);
            try {
              cbPath.traverse(Object.assign({
                CallExpression: function(cbInner) {
                  var cbc = cbInner.node.callee;
                  if (!_t.isIdentifier(cbc)) return;
                  var cbBinding = cbInner.scope.getBinding(cbc.name);
                  if (!cbBinding || cbBinding.kind !== "param" || cbBinding.scope.path !== cbPath) return;
                  // This is a call to a callback param — it's an item from the container
                  if (paramIdx < cbInner.node.arguments.length) {
                    var argPath = cbInner.get("arguments." + paramIdx);
                    var argVals = propName ? _resolvePropertyFromArg(argPath, propName, depth + 1) : _resolveAllValues(argPath, depth + 1);
                    values = values.concat(argVals);
                  }
                },
              }, _SKIP_NESTED_FUNCS));
            } catch (e) { _resolver.collectError(e, "itemCallsCallbackTraverse"); }
          }
        }

        // Pattern: containerParam[key][i](args) — direct indexed call on sub-array
        if (_t.isMemberExpression(ic) && ic.computed) {
          var icObj = ic.object;
          // containerParam[key][i] — two levels of member access
          if (_t.isMemberExpression(icObj) && _t.isIdentifier(icObj.object, { name: containerParamName })) {
            if (paramIdx < innerPath.node.arguments.length) {
              var argPath2 = innerPath.get("arguments." + paramIdx);
              var argVals2 = propName ? _resolvePropertyFromArg(argPath2, propName, depth + 1) : _resolveAllValues(argPath2, depth + 1);
              values = values.concat(argVals2);
            }
          }
        }
      },
      // Do NOT skip nested functions — iteration patterns are often inside
      // helper functions (e.g., jQuery's inspect() inside inspectPrefiltersOrTransports)
    });
  } catch (e) { _resolver.collectError(e, "itemCallsFromParam"); }
  return values;
}

// Resolve a call expression to the function it returns (for callback-arg tracing)
function _resolveCallReturnToFunction(callPath, depth) {
  if (!_resolver.guard("F", callPath.node)) return null;
  try {
  var callee = callPath.node.callee;
  var funcPath = _resolveCalleeFuncPath(callPath, depth);
  // Param binding: callee is a function parameter (e.g., n() in IIFE)
  if (!funcPath && _t.isIdentifier(callee)) {
    var binding = callPath.scope.getBinding(callee.name);
    if (binding && binding.kind === "param") {
      var encFn = binding.path.findParent(function(p) { return p.isFunction(); });
      if (encFn) {
        var pIdx = -1;
        for (var pi = 0; pi < encFn.node.params.length; pi++) {
          if (encFn.node.params[pi] === binding.path.node) { pIdx = pi; break; }
          if (_t.isIdentifier(encFn.node.params[pi]) && encFn.node.params[pi].name === callee.name) { pIdx = pi; break; }
        }
        if (pIdx >= 0 && encFn.parentPath && _t.isCallExpression(encFn.parent) &&
            encFn.parent.callee === encFn.node && pIdx < encFn.parent.arguments.length) {
          var iifeArg = encFn.parent.arguments[pIdx];
          if (_t.isFunctionExpression(iifeArg) || _t.isArrowFunctionExpression(iifeArg)) {
            funcPath = encFn.parentPath.get("arguments." + pIdx);
          }
        }
      }
    }
  }
  if (!funcPath) return null;

  // Find return statements that return a function
  var result = null;
  try {
    funcPath.traverse(Object.assign({
      ReturnStatement: function(retPath) {
        if (result) return;
        var arg = retPath.node.argument;
        if (!arg) return;
        if (_t.isFunctionExpression(arg) || _t.isArrowFunctionExpression(arg)) {
          result = { node: arg, _path: retPath.get("argument") };
        }
      },
    }, _SKIP_NESTED_FUNCS));
  } catch (e) { _resolver.collectError(e, "resolveCallReturnToFunction"); }
  return result;
  } catch (_rfe) {
    if (_rfe instanceof RangeError) { _resolver.collectError(_rfe, "resolveCallReturnToFunction"); return null; }
    throw _rfe;
  } finally { _resolver.unguard("F", callPath.node); }
}

// Get the scope binding for a function (by name, variable declarator, or assignment)
function _getFunctionBinding(funcPath) {
  if (funcPath.node.id) {
    var binding = funcPath.scope.parent ? funcPath.scope.parent.getBinding(funcPath.node.id.name) : null;
    if (!binding) binding = funcPath.scope.getBinding(funcPath.node.id.name);
    return binding;
  }
  if (_t.isVariableDeclarator(funcPath.parent)) {
    return funcPath.scope.parent ? funcPath.scope.parent.getBinding(funcPath.parent.id.name) : null;
  }
  if (_t.isAssignmentExpression(funcPath.parent) && _t.isIdentifier(funcPath.parent.left)) {
    return funcPath.scope.getBinding(funcPath.parent.left.name);
  }
  return null;
}

// Resolve parameter values from factory pattern: var result = factory(...); result.method(arg)
function _resolveParamFromFactoryCallers(factoryBinding, methodName, paramIdx, depth, propName) {
  var values = [];
  var refs = factoryBinding.referencePaths;
  for (var r = 0; r < refs.length; r++) {
    var refPath = refs[r];
    // Factory call: var result = factory(...)
    if (refPath.parent && _t.isCallExpression(refPath.parent) && refPath.parent.callee === refPath.node) {
      var callExprPath = refPath.parentPath;
      var callParent = callExprPath.parentPath;
      if (callParent && _t.isVariableDeclarator(callParent.node) && callParent.node.init === callExprPath.node) {
        var resultName = _t.isIdentifier(callParent.node.id) ? callParent.node.id.name : null;
        if (resultName) {
          var resultBinding = callParent.scope.getBinding(resultName);
          if (resultBinding) {
            var methodVals = _resolveParamFromMethodCalls(resultBinding, methodName, paramIdx, depth, propName);
            values = values.concat(methodVals);
          }
        }
      }
    }
  }
  return values;
}

// Handle overloaded function signatures: when paramIdx >= arguments.length,
// try earlier argument positions. This handles patterns like jQuery.ajax(url, options)
// being called as jQuery.ajax(options) — options is paramIdx=1 but caller passes it at 0.
// Only applies when propName is set (looking for a property on an object argument).
function _resolveOverloadedArg(callExprPath, paramIdx, depth, propName) {
  if (!propName) return [];
  var args = callExprPath.node.arguments;
  if (paramIdx < args.length || args.length === 0) return [];
  for (var fi = args.length - 1; fi >= 0; fi--) {
    var argPath = callExprPath.get("arguments." + fi);
    var argVals = _resolvePropertyFromArg(argPath, propName, depth);
    if (argVals.length > 0) return argVals;
  }
  return [];
}

// ─── Correlated Multi-Property Resolution ───────────────────────────────────
// Traces a parameter binding through the full call chain (callback-arg, stored-callback,
// container iteration, extend-patterns, global aliases) and returns the concrete caller
// argument paths instead of resolved values. This enables the XHR/fetch handler to extract
// multiple properties (method + url) from each caller's argument in a correlated way.

function _resolveParamToCallerArgs(binding) {
  if (!_resolver.guard("Z", binding.identifier)) return [];
  try {
    return _traceParamToArgs(binding);
  } catch (_rze) {
    if (_rze instanceof RangeError) { _resolver.collectError(_rze, "resolveParamToCallerArgs"); return []; }
    throw _rze;
  } finally { _resolver.unguard("Z", binding.identifier); }
}

// Iterative entry-point. Replaces the prior synchronous recursion
// through _collectOrTraceArg → _traceParamToArgs that closed the
// caller-arg-tracing SCC. The driver maintains a worklist of
// bindings; for each, _traceParamToArgsRoutes runs the original 6+
// route dispatch (now passed the queue), and _collectOrTraceArg
// (also queue-aware) pushes any param-bound identifiers it
// encounters back onto the queue rather than recursing
// synchronously. The visited set keys on the binding identity to
// terminate cycles in the trace graph (e.g. mutually-recursive
// callers).
function _traceParamToArgs(rootBinding) {
  // Per-analysis memo. Same binding asked twice within one
  // analyzeJSBundle invocation returns the cached array — avoids
  // rewalking caller chains for hot bindings (e.g. a wrapper
  // function referenced by every fetch site in a request module).
  if (_traceParamToArgsMemo.has(rootBinding)) {
    return _traceParamToArgsMemo.get(rootBinding);
  }
  // LIFO worklist — the only observable side effect is concatenation
  // into allArgs, and downstream consumers iterate the array without
  // dependence on insertion order. pop() avoids the O(N) cost of
  // shift() that an Array-as-queue incurs.
  var stack = [rootBinding];
  var visited = new Set();
  var allArgs = [];
  while (stack.length > 0) {
    var binding = stack.pop();
    if (visited.has(binding)) continue;
    visited.add(binding);
    var routeArgs = _traceParamToArgsRoutes(binding, stack);
    if (routeArgs && routeArgs.length > 0) {
      for (var rai = 0; rai < routeArgs.length; rai++) allArgs.push(routeArgs[rai]);
    }
  }
  _traceParamToArgsMemo.set(rootBinding, allArgs);
  return allArgs;
}

function _traceParamToArgsRoutes(binding, queue) {
  var funcPath = binding.scope.path;
  if (!_t.isFunction(funcPath.node)) return [];

  var paramIdx = _findParamIndex(funcPath.node.params, binding.identifier.name);
  if (paramIdx === -1) return [];

  // Route 1: ObjectProperty — { method: function(param) {} }
  var funcBinding = _getFunctionBinding(funcPath);
  if (!funcBinding && _t.isObjectProperty(funcPath.parent)) {
    var methodKey = funcPath.parent.key;
    var methodName = _t.isIdentifier(methodKey) ? methodKey.name :
      (_t.isStringLiteral(methodKey) ? methodKey.value : null);
    if (methodName) {
      return _traceObjectMethodToArgs(funcPath, paramIdx, methodName, queue);
    }
    return [];
  }
  // Route 1b: obj.method = function(param) {} — trace via obj.method() call sites
  if (!funcBinding && _t.isAssignmentExpression(funcPath.parent) &&
      _t.isMemberExpression(funcPath.parent.left) && !funcPath.parent.left.computed) {
    var assignProp = funcPath.parent.left.property;
    var assignMethodName = _t.isIdentifier(assignProp) ? assignProp.name : null;
    var assignObj = funcPath.parent.left.object;
    if (assignMethodName && _t.isIdentifier(assignObj)) {
      var assignObjBinding = funcPath.scope.getBinding(assignObj.name);
      if (assignObjBinding) {
        return _collectMethodCallerArgs(assignObjBinding.referencePaths, assignMethodName, paramIdx, queue);
      }
      // Global: window.X = function() {} or known alias
      var isGlobalTarget = _isGlobalObject(assignObj.name, funcPath.scope);
      if (isGlobalTarget && _globalAssignments[assignMethodName]) {
        return _collectGlobalCallerArgs(funcPath, assignMethodName, paramIdx);
      }
    }
    return [];
  }
  // Route 1c: obj[method] = function(param) {} — computed member assignment
  if (!funcBinding && _t.isAssignmentExpression(funcPath.parent) &&
      _t.isMemberExpression(funcPath.parent.left) && funcPath.parent.left.computed) {
    var compObj = funcPath.parent.left.object;
    var compProp = funcPath.parent.left.property;
    if (_t.isIdentifier(compObj) && _t.isIdentifier(compProp)) {
      var compObjBinding = funcPath.scope.getBinding(compObj.name);
      var compPropVals = _resolveAllValues(funcPath.parentPath.get("left.property"), 0);
      if (compObjBinding && compPropVals.length > 0) {
        var compArgs = [];
        for (var cvi = 0; cvi < compPropVals.length; cvi++) {
          if (typeof compPropVals[cvi] !== "string") continue;
          var cmArgs = _collectMethodCallerArgs(compObjBinding.referencePaths, compPropVals[cvi], paramIdx, queue);
          compArgs = compArgs.concat(cmArgs);
        }
        for (var gn in _globalAssignments) {
          var ga = _globalAssignments[gn];
          if (!ga.valueNode) continue;
          var gaVal = ga.valueNode;
          while (_t.isAssignmentExpression(gaVal)) gaVal = gaVal.right;
          if (_t.isIdentifier(gaVal) && gaVal.name === compObjBinding.identifier.name) {
            for (var cvi2 = 0; cvi2 < compPropVals.length; cvi2++) {
              if (typeof compPropVals[cvi2] !== "string") continue;
              var cgArgs = _collectGlobalMethodCallerArgs(funcPath, gn, compPropVals[cvi2], paramIdx, queue);
              compArgs = compArgs.concat(cgArgs);
            }
          }
        }
        return compArgs;
      }
    }
    return [];
  }
  // Route 2: Callback argument — someFunc(function(param) {})
  if (!funcBinding && _t.isCallExpression(funcPath.parent)) {
    var cbCallExpr = funcPath.parentPath;
    var cbArgIdx = -1;
    for (var cbi = 0; cbi < funcPath.parent.arguments.length; cbi++) {
      if (funcPath.parent.arguments[cbi] === funcPath.node) { cbArgIdx = cbi; break; }
    }
    if (cbArgIdx >= 0) {
      return _traceCallbackArgToArgs(cbCallExpr, cbArgIdx, paramIdx, queue);
    }
  }
  // Route 3: Ctor.prototype.method = function(param) {} — trace via instance.method()
  if (!funcBinding && _t.isAssignmentExpression(funcPath.parent) &&
      _t.isMemberExpression(funcPath.parent.left) && !funcPath.parent.left.computed) {
    var ptAssignObj = funcPath.parent.left.object;
    var ptAssignProp = funcPath.parent.left.property;
    var ptMethodName = _t.isIdentifier(ptAssignProp) ? ptAssignProp.name : null;
    if (ptMethodName && _t.isMemberExpression(ptAssignObj) && !ptAssignObj.computed &&
        (_t.isIdentifier(ptAssignObj.property, { name: "prototype" }) ||
         (_t.isStringLiteral(ptAssignObj.property) && ptAssignObj.property.value === "prototype")) &&
        _t.isIdentifier(ptAssignObj.object)) {
      var ptCtorName = ptAssignObj.object.name;
      var ptProgramPath = funcPath.findParent(function(p) { return p.isProgram(); });
      if (ptProgramPath) {
        var ptArgs = [];
        try {
          ptProgramPath.traverse({
            VariableDeclarator: function(decPath) {
              var init = decPath.node.init;
              if (!init || !_t.isNewExpression(init) || !_t.isIdentifier(init.callee, { name: ptCtorName })) return;
              var instBinding = decPath.scope.getBinding(decPath.node.id.name);
              if (instBinding) {
                ptArgs = ptArgs.concat(_collectMethodCallerArgs(instBinding.referencePaths, ptMethodName, paramIdx, queue));
              }
            },
          });
        } catch (e) { _resolver.collectError(e, "protoMethodInstances"); }
        return ptArgs;
      }
    }
  }
  // Route 4: ReturnStatement — function returned from IIFE
  if (!funcBinding && _t.isReturnStatement(funcPath.parent)) {
    var encFunc = funcPath.findParent(function(p) { return p.isFunction() && p !== funcPath; });
    if (encFunc) {
      var encParent = encFunc.parentPath;
      if (encParent && _t.isCallExpression(encParent.node) && encParent.node.callee === encFunc.node) {
        var iifeParent = encParent.parentPath;
        if (iifeParent && _t.isVariableDeclarator(iifeParent.node) && _t.isIdentifier(iifeParent.node.id)) {
          var iifeVarBinding = iifeParent.scope.getBinding(iifeParent.node.id.name);
          if (iifeVarBinding) return _collectCallerArgs(iifeVarBinding.referencePaths, paramIdx, queue);
        }
        if (iifeParent && _t.isAssignmentExpression(iifeParent.node) && _t.isMemberExpression(iifeParent.node.left)) {
          var iifeGlobalProp = iifeParent.node.left.property;
          var iifeGlobalName = _t.isIdentifier(iifeGlobalProp) ? iifeGlobalProp.name : null;
          if (iifeGlobalName && _globalAssignments[iifeGlobalName]) {
            return _collectGlobalCallerArgs(funcPath, iifeGlobalName, paramIdx);
          }
        }
      }
    }
  }
  // Route 5: ES6 class method — class Foo { method(param) {} }
  if (!funcBinding && _t.isClassMethod(funcPath.node) && _t.isClassBody(funcPath.parent)) {
    var classDecl = funcPath.parentPath.parentPath;
    if (classDecl && (_t.isClassDeclaration(classDecl.node) || _t.isClassExpression(classDecl.node))) {
      var className = classDecl.node.id ? classDecl.node.id.name : null;
      var clMethodName = _t.isIdentifier(funcPath.node.key) ? funcPath.node.key.name : null;
      if (className && clMethodName) {
        var clProgramPath = funcPath.findParent(function(p) { return p.isProgram(); });
        if (clProgramPath) {
          var clArgs = [];
          try {
            clProgramPath.traverse({
              VariableDeclarator: function(decPath) {
                var init = decPath.node.init;
                if (!init || !_t.isNewExpression(init) || !_t.isIdentifier(init.callee, { name: className })) return;
                var instBinding = decPath.scope.getBinding(decPath.node.id.name);
                if (instBinding) {
                  clArgs = clArgs.concat(_collectMethodCallerArgs(instBinding.referencePaths, clMethodName, paramIdx, queue));
                }
              },
            });
          } catch (e) { _resolver.collectError(e, "classMethodInstances"); }
          return clArgs;
        }
      }
    }
  }
  // Route 6: Direct function binding — collect caller arguments
  if (!funcBinding) return [];
  return _collectCallerArgs(funcBinding.referencePaths, paramIdx, queue);
}

// Collect concrete caller argument paths from reference paths to a function
function _collectCallerArgs(refs, paramIdx, queue) {
  var args = [];
  if (!refs) return args;
  for (var r = 0; r < refs.length; r++) {
    var callPath = _matchDirectCallSite(refs[r]);
    if (!callPath) continue;
    // Overloaded-call clamp: when paramIdx exceeds the actual arg
    // count, fall back to the last arg. Common in jQuery-style
    // signatures `(method, url, opts?)` where some callers omit the
    // optional positional.
    var effectiveIdx = paramIdx < callPath.node.arguments.length ? paramIdx :
      (callPath.node.arguments.length > 0 ? callPath.node.arguments.length - 1 : -1);
    if (effectiveIdx >= 0) {
      _collectOrTraceArg(callPath.get("arguments." + effectiveIdx), args, queue);
    }
  }
  return args;
}

// Param-bound identifiers enqueue the binding onto the iterative
// driver's stack (owned by _traceParamToArgs) instead of recursing
// synchronously. Contract: every caller MUST pass a non-null queue.
// The single entry point _traceParamToArgs creates and owns the
// queue; all helpers in the caller-arg pipeline thread it through
// untouched. There is no defensive fallback — a missing queue is a
// bug, not a degraded path.
function _collectOrTraceArg(argPath, out, queue) {
  if (_t.isIdentifier(argPath.node)) {
    var binding = argPath.scope.getBinding(argPath.node.name);
    if (binding && binding.kind === "param") {
      queue.push(binding);
      return;
    }
    // Var initialized from function call: var s = merge(opts) — when
    // any arg of that init is a param, queue it; the driver follows
    // the chain.
    if (binding && binding.path.isVariableDeclarator && binding.path.isVariableDeclarator()) {
      var initNode = binding.path.node.init;
      if (initNode && _t.isCallExpression(initNode)) {
        var initArgs = initNode.arguments;
        for (var ai = 0; ai < initArgs.length; ai++) {
          if (_t.isIdentifier(initArgs[ai])) {
            var argBinding = binding.path.get("init").scope.getBinding(initArgs[ai].name);
            if (argBinding && argBinding.kind === "param") {
              queue.push(argBinding);
              return;
            }
          }
        }
      }
    }
  }
  out.push(argPath);
}

// Trace ObjectProperty method to caller args: { method: function(param) {} }
// Walks up to the object, then through extend-patterns and global aliases
function _traceObjectMethodToArgs(funcPath, paramIdx, methodName, queue) {
  var objExprPath = funcPath.parentPath ? funcPath.parentPath.parentPath : null;
  if (!objExprPath || !_t.isObjectExpression(objExprPath.node)) return [];
  var declPath = objExprPath.parentPath;
  if (!declPath) return [];

  var objBinding = null;
  if (_t.isVariableDeclarator(declPath.node) && _t.isIdentifier(declPath.node.id)) {
    objBinding = declPath.scope.getBinding(declPath.node.id.name);
  }

  // Extend pattern: X.extend({method: fn}) → callers use X.method(...)
  if (!objBinding && _t.isCallExpression(declPath.node)) {
    var extCallee = declPath.node.callee;
    if (_t.isMemberExpression(extCallee) && !extCallee.computed && _t.isIdentifier(extCallee.object)) {
      var args = [];
      var extObjBinding = declPath.scope.getBinding(extCallee.object.name);
      if (extObjBinding) {
        args = _collectMethodCallerArgs(extObjBinding.referencePaths, methodName, paramIdx, queue);
        for (var gn in _globalAssignments) {
          var ga = _globalAssignments[gn];
          if (!ga.valueNode) continue;
          var gaVal = ga.valueNode;
          while (_t.isAssignmentExpression(gaVal)) gaVal = gaVal.right;
          if (_t.isIdentifier(gaVal) && gaVal.name === extObjBinding.identifier.name) {
            var globalArgs = _collectGlobalMethodCallerArgs(funcPath, gn, methodName, paramIdx, queue);
            args = args.concat(globalArgs);
          }
        }
      }
      return args;
    }
  }
  if (!objBinding) return [];
  return _collectMethodCallerArgs(objBinding.referencePaths, methodName, paramIdx, queue);
}

// Collect caller args from obj.method(...) call sites
function _collectMethodCallerArgs(refs, methodName, paramIdx, queue) {
  var args = [];
  if (!refs) return args;
  for (var r = 0; r < refs.length; r++) {
    var callPath = _matchMethodCallSite(refs[r], methodName);
    if (!callPath) continue;
    var effectiveIdx = paramIdx < callPath.node.arguments.length ? paramIdx :
      (callPath.node.arguments.length > 0 ? callPath.node.arguments.length - 1 : -1);
    if (effectiveIdx >= 0) {
      _collectOrTraceArg(callPath.get("arguments." + effectiveIdx), args, queue);
    }
  }
  return args;
}

// Unified global caller traversal: finds all calls to globalName(...) or globalName.methodName(...)
// in the program scope and invokes onMatch(callPath) for each match.
function _traverseGlobalCallers(funcPath, globalName, methodName, onMatch) {
  var cacheKey = globalName + (methodName ? "." + methodName : "");
  var cached = _globalCallerCache[cacheKey];
  if (!cached) {
    cached = [];
    var scope = funcPath.scope;
    while (scope.parent) scope = scope.parent;
    var programPath = scope.path;
    if (!programPath) { _globalCallerCache[cacheKey] = cached; return; }
    try {
      programPath.traverse({
        CallExpression: function(innerPath) {
          var c = innerPath.node.callee;
          var isMatch = methodName
            ? (_t.isMemberExpression(c) && !c.computed &&
               _t.isIdentifier(c.object, { name: globalName }) &&
               _t.isIdentifier(c.property, { name: methodName }))
            : _t.isIdentifier(c, { name: globalName });
          if (isMatch && !innerPath.scope.getBinding(globalName)) cached.push(innerPath);
        },
      });
    } catch (e) { _resolver.collectError(e, "globalCallerTraversal"); }
    _globalCallerCache[cacheKey] = cached;
  }
  for (var _gci = 0; _gci < cached.length; _gci++) onMatch(cached[_gci]);
}

function _collectGlobalMethodCallerArgs(funcPath, globalName, methodName, paramIdx, queue) {
  var args = [];
  _traverseGlobalCallers(funcPath, globalName, methodName, function(innerPath) {
    var effectiveIdx = paramIdx < innerPath.node.arguments.length ? paramIdx :
      (innerPath.node.arguments.length > 0 ? innerPath.node.arguments.length - 1 : -1);
    if (effectiveIdx >= 0) _collectOrTraceArg(innerPath.get("arguments." + effectiveIdx), args, queue);
  });
  return args;
}

function _collectGlobalCallerArgs(funcPath, globalName, paramIdx) {
  var args = [];
  _traverseGlobalCallers(funcPath, globalName, null, function(innerPath) {
    if (paramIdx < innerPath.node.arguments.length) {
      args.push(innerPath.get("arguments." + paramIdx));
    } else if (innerPath.node.arguments.length > 0) {
      args.push(innerPath.get("arguments." + (innerPath.node.arguments.length - 1)));
    }
  });
  return args;
}

// Trace callback argument through callee to find where callback is invoked with args
function _traceCallbackArgToArgs(callExprPath, cbArgIdx, paramIdx, queue) {
  if (!_resolver.guard("ZC", callExprPath.node)) return [];
  try {
    var calleeNode = callExprPath.node.callee;
    // Resolve the callee function
    var targetFuncPath = _resolveCalleeFuncPath(callExprPath, 0);
    // Member expression — resolve through extend pattern
    if (!targetFuncPath && _t.isMemberExpression(calleeNode) && !calleeNode.computed) {
      var mProp = _t.isIdentifier(calleeNode.property) ? calleeNode.property.name : null;
      if (mProp && _t.isIdentifier(calleeNode.object)) {
        var objBinding = callExprPath.scope.getBinding(calleeNode.object.name);
        if (objBinding) {
          // (Removed `obj.extend({...}) / obj.mixin({...}) / obj.assign({...})`
          // framework-shape recognition per CLAUDE.md L29 — same
          // rationale as the matching block in _resolveParamFromCallbackArg.)
        }
      }
    }
    if (!targetFuncPath) return [];

    var targetParams = targetFuncPath.node.params || [];
    var cbParamIdx = cbArgIdx;
    if (cbParamIdx >= targetParams.length) cbParamIdx = targetParams.length - 1;
    if (cbParamIdx < 0) return [];
    var cbParamName = _t.isIdentifier(targetParams[cbParamIdx]) ? targetParams[cbParamIdx].name : null;
    if (!cbParamName) return [];

    // Search for stored-callback pattern: container.push(cbParam) → trace container
    var args = [];
    try {
      targetFuncPath.traverse({
        CallExpression: function(innerPath) {
          // Direct call: cbParam(arg0, ...)
          var ic = innerPath.node.callee;
          if (_t.isIdentifier(ic, { name: cbParamName })) {
            var innerBinding = innerPath.scope.getBinding(cbParamName);
            if (innerBinding && innerBinding.kind === "param" && innerBinding.scope.path === targetFuncPath) {
              if (paramIdx < innerPath.node.arguments.length) {
                args.push(innerPath.get("arguments." + paramIdx));
              }
            }
          }
          // Store-and-call-later: container.push(cbParam) → trace container
          var isPushOrUnshift = false;
          if (_t.isMemberExpression(ic) && !ic.computed &&
              (_t.isIdentifier(ic.property, { name: "push" }) || _t.isIdentifier(ic.property, { name: "unshift" })))
            isPushOrUnshift = true;
          if (!isPushOrUnshift && _t.isMemberExpression(ic) && ic.computed && _t.isConditionalExpression(ic.property)) {
            var cc = ic.property.consequent, ca = ic.property.alternate;
            if ((_t.isStringLiteral(cc) && (cc.value === "push" || cc.value === "unshift")) ||
                (_t.isStringLiteral(ca) && (ca.value === "push" || ca.value === "unshift")))
              isPushOrUnshift = true;
          }
          if (isPushOrUnshift) {
            var pushArgs = innerPath.node.arguments;
            var storingCb = false;
            for (var pai = 0; pai < pushArgs.length; pai++) {
              if (_t.isIdentifier(pushArgs[pai], { name: cbParamName })) {
                var pushB = innerPath.scope.getBinding(cbParamName);
                if (pushB && pushB.kind === "param" && pushB.scope.path === targetFuncPath) storingCb = true;
              }
              // Derived variable: func = cbParam; container.push(func)
              if (!storingCb && _t.isIdentifier(pushArgs[pai])) {
                var derivedB = innerPath.scope.getBinding(pushArgs[pai].name);
                if (derivedB && derivedB.constantViolations) {
                  for (var dvi = 0; dvi < derivedB.constantViolations.length; dvi++) {
                    var dvN = derivedB.constantViolations[dvi].node;
                    if (_t.isAssignmentExpression(dvN) && _t.isIdentifier(dvN.right, { name: cbParamName })) {
                      storingCb = true; break;
                    }
                  }
                }
              }
            }
            if (storingCb) {
              var containerNode = ic.object;
              if (_t.isAssignmentExpression(containerNode)) containerNode = containerNode.left;
              while (_t.isMemberExpression(containerNode) && containerNode.computed) containerNode = containerNode.object;
              if (_t.isIdentifier(containerNode)) {
                var containerBinding = innerPath.scope.getBinding(containerNode.name);
                if (containerBinding) {
                  var storedArgs = _traceStoredCallbackToArgs(containerBinding, paramIdx, queue);
                  args = args.concat(storedArgs);
                }
              }
            }
          }
        },
        FunctionDeclaration: function(p) { p.skip(); },
      });
    } catch (e) { _resolver.collectError(e, "traceCallbackArgToArgs"); }
    return args;
  } catch (_rzce) {
    if (_rzce instanceof RangeError) { _resolver.collectError(_rzce, "traceCallbackArgToArgs"); return []; }
    throw _rzce;
  } finally { _resolver.unguard("ZC", callExprPath.node); }
}

// Trace a stored-callback container to find where items are called and return their arg paths
// Iterative worklist over container bindings. Replaces tail-recursion
// when a container is a parameter (resolve through callers — push each
// resolved binding onto the worklist instead of recursing).
function _traceStoredCallbackToArgs(initialContainerBinding, paramIdx, queue) {
  var bindingQueue = [initialContainerBinding];
  var bindingSeen = new Set();
  var args = [];
  while (bindingQueue.length > 0) {
    var containerBinding = bindingQueue.shift();
    if (!containerBinding || bindingSeen.has(containerBinding.identifier)) continue;
    bindingSeen.add(containerBinding.identifier);
    if (!_resolver.guard("ZS", containerBinding.identifier)) continue;
    try {
      // If container is a param, queue caller bindings.
      if (containerBinding.kind === "param") {
        var enclosingFunc = containerBinding.scope.path;
        var containerParamIdx = -1;
        var params = enclosingFunc.node.params;
        for (var pi = 0; pi < params.length; pi++) {
          if (_t.isIdentifier(params[pi]) && params[pi].name === containerBinding.identifier.name) {
            containerParamIdx = pi; break;
          }
        }
        if (containerParamIdx < 0) continue;
        var funcBinding = _getFunctionBinding(enclosingFunc);
        if (!funcBinding || !funcBinding.referencePaths) continue;
        var callerRefs = funcBinding.referencePaths;
        for (var ri = 0; ri < callerRefs.length; ri++) {
          if (!callerRefs[ri].parent || !_t.isCallExpression(callerRefs[ri].parent) ||
              callerRefs[ri].parent.callee !== callerRefs[ri].node) continue;
          if (containerParamIdx >= callerRefs[ri].parent.arguments.length) continue;
          var actualArg = callerRefs[ri].parent.arguments[containerParamIdx];
          if (_t.isIdentifier(actualArg)) {
            var actualBinding = callerRefs[ri].parentPath.scope.getBinding(actualArg.name);
            if (actualBinding) bindingQueue.push(actualBinding);
          }
        }
        continue;
      }

      // Container is local — find where items are called.
      var refs = containerBinding.referencePaths;
      for (var ri2 = 0; ri2 < refs.length; ri2++) {
        var refPath = refs[ri2];
        // Pattern 1: container[i](args).
        if (_t.isMemberExpression(refPath.parent) && refPath.parent.object === refPath.node && refPath.parent.computed) {
          var memberCallParent = refPath.parentPath ? refPath.parentPath.parent : null;
          if (memberCallParent && _t.isCallExpression(memberCallParent) && memberCallParent.callee === refPath.parent) {
            if (paramIdx < memberCallParent.arguments.length) {
              var directArgPath = refPath.parentPath.parentPath.get("arguments." + paramIdx);
              if (_t.isIdentifier(directArgPath.node)) {
                var directArgBinding = directArgPath.scope.getBinding(directArgPath.node.name);
                if (directArgBinding && directArgBinding.kind === "param") {
                  // Defer to the iterative driver via the shared queue
                  // instead of recursing through _traceParamToArgs.
                  if (queue) queue.push(directArgBinding);
                  else args.push(directArgPath);
                } else {
                  args.push(directArgPath);
                }
              } else {
                args.push(directArgPath);
              }
            }
          }
        }
        // Pattern 2: container passed to function that iterates and calls items.
        if (_t.isCallExpression(refPath.parent) && refPath.parent.callee !== refPath.node) {
          var iterCallPath = refPath.parentPath;
          var iterArgIdx = -1;
          for (var iai = 0; iai < iterCallPath.node.arguments.length; iai++) {
            if (iterCallPath.node.arguments[iai] === refPath.node) { iterArgIdx = iai; break; }
            if (_containsNode(iterCallPath.node.arguments[iai], refPath.node)) { iterArgIdx = iai; break; }
          }
          if (iterArgIdx >= 0) {
            var iterArgs = _traceItemCallsToArgs(iterCallPath, iterArgIdx, paramIdx, queue);
            args = args.concat(iterArgs);
          }
        }
      }
    } catch (_rzse) {
      if (_rzse instanceof RangeError) { _resolver.collectError(_rzse, "traceStoredCallbackToArgs"); }
      else throw _rzse;
    } finally { _resolver.unguard("ZS", containerBinding.identifier); }
  }
  return args;
}

// Trace iteration pattern to find where item callbacks are called with args
function _traceItemCallsToArgs(callPath, iterArgIdx, paramIdx, queue) {
  if (!_resolver.guard("ZI", callPath.node)) return [];
  try {
    // Resolve the called function
    var funcPath = _resolveCalleeFuncPath(callPath, 0);
    if (!funcPath) return [];
    if (iterArgIdx >= funcPath.node.params.length) return [];
    var containerParamName = _t.isIdentifier(funcPath.node.params[iterArgIdx]) ? funcPath.node.params[iterArgIdx].name : null;
    if (!containerParamName) return [];

    // Spec-grounded inter-procedural trace: scan the called
    // function's body for invocations of items extracted from the
    // container parameter. Three concrete shapes per ECMA-262:
    //   • Direct indexed call: container[i](args), container[k](args)
    //     — MemberExpression with the container as base, called as a
    //       function (§ 13.3.5 CallExpression on MemberExpression).
    //   • Aliased item invocation: var item = container[i]; item(args)
    //     — Variable declared from container[i], then called.
    //   • Iterator/forEach callback invocation: a callback that
    //     received an item is invoked with args. Detected by scanning
    //     for any param-bound identifier called with args at paramIdx,
    //     where the param's binding came from a container iteration.
    // No framework-name recognition (.each / .forEach / etc.) — the
    // analyzer relies on Array.prototype.forEach / iterator protocols
    // being part of the function's actual implementation in the same
    // bundle, reachable via _resolveCalleeFuncPath when those methods
    // resolve via scope binding.
    var args = [];
    try {
      funcPath.traverse({
        CallExpression: function(innerPath) {
          var ic = innerPath.node.callee;
          if (paramIdx >= innerPath.node.arguments.length) return;

          // container[...](args)
          if (_t.isMemberExpression(ic) && ic.computed &&
              _t.isIdentifier(ic.object, { name: containerParamName })) {
            var icBinding = innerPath.scope.getBinding(containerParamName);
            if (icBinding && icBinding.kind === "param" && icBinding.scope.path === funcPath) {
              _collectOrTraceArg(innerPath.get("arguments." + paramIdx), args, queue);
              return;
            }
          }

          // <ident>(args) where ident's binding init is container[i] /
          // container[i][j] / container.method(...) — chase the var
          // assignment back to a container access.
          if (_t.isIdentifier(ic)) {
            var aliasBinding = innerPath.scope.getBinding(ic.name);
            if (!aliasBinding || aliasBinding.scope.path !== funcPath) return;
            var aliasInit = aliasBinding.path && _t.isVariableDeclarator(aliasBinding.path.node) ? aliasBinding.path.node.init : null;
            if (!aliasInit) return;
            // Walk member chain to find container at base.
            var chain = aliasInit;
            while (_t.isMemberExpression(chain) || _t.isOptionalMemberExpression(chain)) chain = chain.object;
            if (_t.isIdentifier(chain, { name: containerParamName })) {
              var chainBinding = aliasBinding.scope.getBinding(containerParamName);
              if (chainBinding && chainBinding.kind === "param" && chainBinding.scope.path === funcPath) {
                _collectOrTraceArg(innerPath.get("arguments." + paramIdx), args, queue);
              }
            }
          }
        },
      });
    } catch (e) { _resolver.collectError(e, "traceItemCallsToArgs"); }
    return args;
  } catch (_rzie) {
    if (_rzie instanceof RangeError) { _resolver.collectError(_rzie, "traceItemCallsToArgs"); return []; }
    throw _rzie;
  } finally { _resolver.unguard("ZI", callPath.node); }
}

// Resolve correlated properties from caller arguments.
// Given an argument path (from a caller), resolve multiple properties from it.
// Returns { prop1: [val1, ...], prop2: [val2, ...] }
// Iterative worklist version. The original recursed once per
// CallExpression-arg unwrap and once per Identifier-init-call-arg
// unwrap; both could chain through wrapper functions of arbitrary depth.
// Single result accumulator filled across iterations; only empty slots
// get filled by deferred work, matching original semantics.
function _resolvePropsFromArg(argPath, propNames) {
  var result = {};
  for (var i0 = 0; i0 < propNames.length; i0++) result[propNames[i0]] = [];
  var queue = [argPath];
  var queueSeen = new Set();
  while (queue.length > 0) {
    var ap = queue.shift();
    if (!ap || !ap.node) continue;
    if (queueSeen.has(ap.node)) continue;
    queueSeen.add(ap.node);

    // Per-property direct resolution at this arg path.
    for (var i = 0; i < propNames.length; i++) {
      if (result[propNames[i]].length > 0) continue;
      result[propNames[i]] = _resolvePropertyFromArg(ap, propNames[i], 0);
    }
    if (!allEmpty(result, propNames)) continue;

    // CallExpression arg — push each call arg.
    if (_t.isCallExpression(ap.node)) {
      var callArgs = ap.node.arguments;
      for (var cai = 0; cai < callArgs.length; cai++) {
        queue.push(ap.get("arguments." + cai));
      }
      continue;
    }

    // Identifier whose binding is initialized from a function call —
    // push each init call's args; also check property-assignment fallback.
    if (_t.isIdentifier(ap.node)) {
      var binding = ap.scope.getBinding(ap.node.name);
      if (binding && binding.path.isVariableDeclarator && binding.path.isVariableDeclarator()) {
        var initNode = binding.path.node.init;
        if (initNode && _t.isCallExpression(initNode)) {
          var initPath = binding.path.get("init");
          var initArgs = initNode.arguments;
          for (var iai = 0; iai < initArgs.length; iai++) {
            queue.push(initPath.get("arguments." + iai));
          }
        }
      }
      // Also check property assignments: s.url = ..., s.type = ...
      if (binding && binding.referencePaths) {
        for (var pi = 0; pi < propNames.length; pi++) {
          if (result[propNames[pi]].length > 0) continue;
          var propN = propNames[pi];
          var refs = binding.referencePaths;
          for (var ri = 0; ri < refs.length; ri++) {
            var refParent = refs[ri].parent;
            if (_t.isMemberExpression(refParent) && refParent.object === refs[ri].node &&
                !refParent.computed && _t.isIdentifier(refParent.property, { name: propN })) {
              var assignNode = refs[ri].parentPath ? refs[ri].parentPath.parent : null;
              if (assignNode && _t.isAssignmentExpression(assignNode) && assignNode.operator === "=" &&
                  assignNode.left === refParent) {
                var rhsVals = _resolveAllValues(refs[ri].parentPath.parentPath.get("right"), 0);
                result[propN] = result[propN].concat(rhsVals);
              }
            }
          }
        }
      }
    }
  }
  return result;
}

function allEmpty(obj, keys) {
  for (var i = 0; i < keys.length; i++) {
    if (obj[keys[i]] && obj[keys[i]].length > 0) return false;
  }
  return true;
}

// Search an object binding's references for obj.method(...) call sites
var _methodCallVisited = null;
function _resolveParamFromMethodCalls(initialObjBinding, methodName, paramIdx, depth, propName) {
  // Iterative factory-chain processing: a.create()→b, b.create()→c
  // walks bindings via worklist instead of recursion. Cycle protection
  // via _methodCallVisited (per-binding, per-method).
  var isRoot = !_methodCallVisited;
  if (isRoot) _methodCallVisited = new Set();
  var bindingQueue = [initialObjBinding];
  var values = [];
  try {
  while (bindingQueue.length > 0) {
  var objBinding = bindingQueue.shift();
  var bindKey = objBinding.identifier.start + ":" + objBinding.identifier.end + ":" + methodName;
  if (_methodCallVisited.has(bindKey)) continue;
  _methodCallVisited.add(bindKey);
  // Collect direct references + global-alias references. A `window.$ = ce`
  // assignment makes `$.method()` semantically equivalent to `ce.method()`;
  // the pre-pass index `_unboundIdRefs` records `$`'s member-access uses so
  // we can enumerate them here without a program-wide retraversal.
  var refs = objBinding.referencePaths.slice();
  if (_unboundIdRefs) {
    for (var pmGn in _globalAssignments) {
      var pmGa = _globalAssignments[pmGn];
      if (!pmGa || !pmGa.valueNode) continue;
      var pmGv = pmGa.valueNode;
      while (_t.isAssignmentExpression(pmGv)) pmGv = pmGv.right;
      if (_t.isIdentifier(pmGv) && pmGv.name === objBinding.identifier.name) {
        var pmAliasRefs = _unboundIdRefs[pmGn];
        if (pmAliasRefs) {
          for (var pmI = 0; pmI < pmAliasRefs.length; pmI++) refs.push(pmAliasRefs[pmI]);
        }
      }
    }
  }
  for (var r = 0; r < refs.length; r++) {
    var refPath = refs[r];
    // Looking for: obj.method(...) OR obj["method"](...) OR obj[N](...)
    // where method/N matches methodName (compared as string).
    if (refPath.parent && _t.isMemberExpression(refPath.parent) &&
        refPath.parent.object === refPath.node) {
      var rmProp = refPath.parent.property;
      var rmMatch = false;
      if (!refPath.parent.computed && _t.isIdentifier(rmProp, { name: methodName })) rmMatch = true;
      else if (refPath.parent.computed) {
        if (_t.isStringLiteral(rmProp) && rmProp.value === methodName) rmMatch = true;
        else if (_t.isNumericLiteral(rmProp) && String(rmProp.value) === methodName) rmMatch = true;
      }
      if (!rmMatch) continue;
      var callNode = refPath.parentPath ? refPath.parentPath.parent : null;
      if (callNode && _t.isCallExpression(callNode) && callNode.callee === refPath.parent) {
        if (paramIdx < callNode.arguments.length) {
          var argPath = refPath.parentPath.parentPath.get("arguments." + paramIdx);
          var argVals = propName ? _resolvePropertyFromArg(argPath, propName, depth) : _resolveAllValues(argPath, depth + 1);
          values = values.concat(argVals);
        } else {
          values = values.concat(_resolveOverloadedArg(refPath.parentPath.parentPath, paramIdx, depth, propName));
        }
      }
      // Also handle obj[K].call(thisArg, args) — Function.prototype.call form
      if (callNode && _t.isMemberExpression(callNode) && callNode.object === refPath.parent &&
          !callNode.computed && _t.isIdentifier(callNode.property, { name: "call" })) {
        var outerCall = refPath.parentPath.parentPath ? refPath.parentPath.parentPath.parent : null;
        if (outerCall && _t.isCallExpression(outerCall) && outerCall.callee === callNode) {
          // .call shifts: param[k] is at outerCall.arguments[k+1]
          var callArgIdx = paramIdx + 1;
          if (callArgIdx < outerCall.arguments.length) {
            var callArgPath = refPath.parentPath.parentPath.parentPath.get("arguments." + callArgIdx);
            var callArgVals = propName ? _resolvePropertyFromArg(callArgPath, propName, depth) : _resolveAllValues(callArgPath, depth + 1);
            values = values.concat(callArgVals);
          }
        }
      }
    }
  }
  // Factory clone tracking: var api = obj(...) or var api = obj.create(...)
  // The returned value has the same methods, so api.method() callers should be included.
  for (var fc = 0; fc < refs.length; fc++) {
    var fcRef = refs[fc];
    var fcCallPath = null;
    // Pattern 1: obj(...) → direct call where obj is callee
    if (fcRef.parent && _t.isCallExpression(fcRef.parent) && fcRef.parent.callee === fcRef.node) {
      fcCallPath = fcRef.parentPath;
    }
    // Pattern 2: obj.create(...) or obj.extend(...) → member method call
    else if (fcRef.parent && _t.isMemberExpression(fcRef.parent) && fcRef.parent.object === fcRef.node &&
             !fcRef.parent.computed && fcRef.parentPath && fcRef.parentPath.parent &&
             _t.isCallExpression(fcRef.parentPath.parent) && fcRef.parentPath.parent.callee === fcRef.parent) {
      fcCallPath = fcRef.parentPath.parentPath;
    }
    if (!fcCallPath) continue;
    // Check if the call result is assigned to a variable: var alias = obj.create(...)
    var fcAssignee = null;
    if (fcCallPath.parent && _t.isVariableDeclarator(fcCallPath.parent) && _t.isIdentifier(fcCallPath.parent.id)) {
      fcAssignee = fcCallPath.parentPath.scope.getBinding(fcCallPath.parent.id.name);
    } else if (fcCallPath.parent && _t.isAssignmentExpression(fcCallPath.parent) &&
               fcCallPath.parent.right === fcCallPath.node && _t.isIdentifier(fcCallPath.parent.left)) {
      fcAssignee = fcCallPath.parentPath.scope.getBinding(fcCallPath.parent.left.name);
    }
    if (fcAssignee && fcAssignee !== objBinding) {
      // Iterative: push factory clone onto worklist instead of recursing.
      // Its values are accumulated in the same `values` array on the next
      // loop iteration.
      bindingQueue.push(fcAssignee);
    }
  }
  // IIFE-return alias check: per-binding, only if THIS binding's local
  // processing produced no values for the current iteration. Note: with
  // the iterative worklist, `values` accumulates across all bindings, so
  // we can't easily check per-binding emptiness. Per-binding empty-check
  // is approximated by tracking values length BEFORE this binding's
  // iteration vs AFTER. Since this is a fall-back rather than the primary
  // resolution, the simplification is acceptable for real bundles where
  // IIFE-alias rarely combines with factory clones.
  if (values.length === 0) {
    var encFuncScope = objBinding.scope.path;
    if (encFuncScope && _t.isFunction(encFuncScope.node)) {
      var encParent = encFuncScope.parentPath;
      if (encParent && _t.isCallExpression(encParent.node) && encParent.node.callee === encFuncScope.node) {
        var iifeCallParent = encParent.parentPath;
        if (iifeCallParent && _t.isVariableDeclarator(iifeCallParent.node) && _t.isIdentifier(iifeCallParent.node.id)) {
          var outerBinding = iifeCallParent.scope.getBinding(iifeCallParent.node.id.name);
          // Push outer binding onto worklist instead of recursing.
          if (outerBinding) bindingQueue.push(outerBinding);
        }
        if (values.length === 0 && iifeCallParent && _t.isAssignmentExpression(iifeCallParent.node) &&
            _t.isMemberExpression(iifeCallParent.node.left)) {
          var gProp = iifeCallParent.node.left.property;
          var gName = _t.isIdentifier(gProp) ? gProp.name : null;
          if (gName && _globalAssignments[gName]) {
            values = values.concat(_resolveParamFromGlobalCallers(objBinding.scope.path, gName, paramIdx, depth, propName, methodName));
          }
        }
      }
      // Factory-argument pattern: !function(t){ win.X = t() }(factoryFunc)
      if (values.length === 0 && encParent && _t.isCallExpression(encParent.node) &&
          encParent.node.callee !== encFuncScope.node) {
        var _outerCallee = encParent.node.callee;
        if (_t.isFunctionExpression(_outerCallee) || _t.isArrowFunctionExpression(_outerCallee)) {
          var _argIdx = -1;
          for (var _fi = 0; _fi < encParent.node.arguments.length; _fi++) {
            if (encParent.node.arguments[_fi] === encFuncScope.node) { _argIdx = _fi; break; }
          }
          if (_argIdx >= 0 && _argIdx < _outerCallee.params.length && _t.isIdentifier(_outerCallee.params[_argIdx])) {
            var _factoryParam = _outerCallee.params[_argIdx].name;
            for (var _gn in _globalAssignments) {
              var _gv = _globalAssignments[_gn];
              if (_gv.valueNode && _t.isCallExpression(_gv.valueNode) &&
                  _t.isIdentifier(_gv.valueNode.callee, {name: _factoryParam})) {
                values = values.concat(_resolveParamFromGlobalCallers(objBinding.scope.path, _gn, paramIdx, depth, propName, methodName));
                break;
              }
            }
          }
        }
      }
    }
  }
  }  // end while (bindingQueue.length > 0)
  return values;
  } finally { if (isRoot) _methodCallVisited = null; }
}

// Resolve parameters of prototype methods: Ctor.prototype.method = function(params) { ... }
// Find new Ctor() instances, then find instance.method(args) call sites.
function _resolveParamFromPrototypeMethodCallers(funcPath, ctorName, methodName, paramIdx, depth, propName) {
  if (!_resolver.guard("M", funcPath.node)) return [];
  try {
  var ctorBinding = funcPath.scope.getBinding(ctorName);
  if (!ctorBinding || !ctorBinding.referencePaths) return [];

  var values = [];
  var refs = ctorBinding.referencePaths;
  for (var r = 0; r < refs.length; r++) {
    var ref = refs[r];
    // Find new Ctor(...) stored in a variable: var x = new Ctor(...)
    if (ref.parent && _t.isNewExpression(ref.parent) && ref.parent.callee === ref.node) {
      var newExprPath = ref.parentPath;
      var newParent = newExprPath.parentPath;
      if (newParent && _t.isVariableDeclarator(newParent.node) && newParent.node.init === newExprPath.node &&
          _t.isIdentifier(newParent.node.id)) {
        var instanceName = newParent.node.id.name;
        var instanceBinding = newParent.scope.getBinding(instanceName);
        if (instanceBinding) {
          var methodVals = _resolveParamFromMethodCalls(instanceBinding, methodName, paramIdx, depth + 1, propName);
          values = values.concat(methodVals);
        }
      }
    }
  }
  return values;
  } catch (_rme) {
    if (_rme instanceof RangeError) { _resolver.collectError(_rme, "resolveMethodCallValues"); return []; }
    throw _rme;
  } finally { _resolver.unguard("M", funcPath.node); }
}

// Correlated this.prop XHR resolution for prototype methods.
// Traces this.method and this.url through constructor → new Ctor() callers,
// keeping method/URL paired per-caller to avoid cross-contamination.
function _resolveThisPropXhrCorrelated(fromPath, ctorName, methodProp, urlProp, headers, bodyParams) {
  var sites = [];
  var ctorBinding = fromPath.scope.getBinding(ctorName);
  if (!ctorBinding && _lastIIFEFuncPath) {
    try { ctorBinding = _lastIIFEFuncPath.scope.getBinding(ctorName); } catch(e) { _resolver.collectError(e, "xhrCtorIIFEScope"); }
  }
  if (!ctorBinding) return sites;
  var ctorNode = null, ctorPath = null;
  if (_t.isFunctionDeclaration(ctorBinding.path.node)) { ctorNode = ctorBinding.path.node; ctorPath = ctorBinding.path; }
  else if (_t.isVariableDeclarator(ctorBinding.path.node) && ctorBinding.path.node.init &&
           (_t.isFunctionExpression(ctorBinding.path.node.init) || _t.isArrowFunctionExpression(ctorBinding.path.node.init))) {
    ctorNode = ctorBinding.path.node.init; ctorPath = ctorBinding.path.get("init");
  }
  if (!ctorNode || !ctorNode.params) return sites;
  // Find this.methodProp = param and this.urlProp = param assignments
  var methodParamIdx = -1, urlParamIdx = -1;
  var paramNames = {};
  for (var pi = 0; pi < ctorNode.params.length; pi++) {
    var p = ctorNode.params[pi];
    if (_t.isIdentifier(p)) paramNames[p.name] = pi;
  }
  if (ctorNode.body && ctorNode.body.body) {
    var _checkAssign = function(expr) {
      if (_t.isAssignmentExpression(expr) && expr.operator === "=") {
        var aL = expr.left, aR = expr.right;
        if (_t.isMemberExpression(aL) && _t.isThisExpression(aL.object) && _t.isIdentifier(aL.property) &&
            _t.isIdentifier(aR) && paramNames[aR.name] !== undefined) {
          if (aL.property.name === methodProp) methodParamIdx = paramNames[aR.name];
          if (aL.property.name === urlProp) urlParamIdx = paramNames[aR.name];
        }
      }
    };
    for (var si = 0; si < ctorNode.body.body.length; si++) {
      var stmt = ctorNode.body.body[si];
      if (_t.isExpressionStatement(stmt)) {
        if (_t.isAssignmentExpression(stmt.expression)) _checkAssign(stmt.expression);
        else if (_t.isSequenceExpression(stmt.expression)) {
          for (var sei = 0; sei < stmt.expression.expressions.length; sei++) _checkAssign(stmt.expression.expressions[sei]);
        }
      }
    }
  }
  if (methodParamIdx < 0 && urlParamIdx < 0) return sites;
  // Find new Ctor() callers (direct + aliased)
  var newCallers = []; // [{argPaths: [path...]}]
  if (ctorBinding.referencePaths) {
    for (var ri = 0; ri < ctorBinding.referencePaths.length; ri++) {
      var ref = ctorBinding.referencePaths[ri];
      if (ref.parent && _t.isNewExpression(ref.parent) && ref.parent.callee === ref.node) {
        var args = [];
        for (var ai = 0; ai < ref.parent.arguments.length; ai++) args.push(ref.parentPath.get("arguments." + ai));
        newCallers.push(args);
      }
      // Aliased: obj.prop = Ctor → new obj.prop(...)
      if (ref.parent && _t.isAssignmentExpression(ref.parent) && ref.parent.right === ref.node &&
          _t.isMemberExpression(ref.parent.left) && !ref.parent.left.computed) {
        var _ao = ref.parent.left.object, _ap = ref.parent.left.property;
        if (_t.isIdentifier(_ao) && _t.isIdentifier(_ap)) {
          var _ab = ref.parentPath.scope.getBinding(_ao.name);
          if (_ab && _ab.referencePaths) {
            for (var ari = 0; ari < _ab.referencePaths.length; ari++) {
              var aRef = _ab.referencePaths[ari];
              if (_t.isMemberExpression(aRef.parent) && aRef.parent.object === aRef.node &&
                  _t.isIdentifier(aRef.parent.property, {name: _ap.name}) &&
                  aRef.parentPath && _t.isNewExpression(aRef.parentPath.parent) &&
                  aRef.parentPath.parent.callee === aRef.parent) {
                var args2 = [];
                for (var ai2 = 0; ai2 < aRef.parentPath.parent.arguments.length; ai2++)
                  args2.push(aRef.parentPath.parentPath.get("arguments." + ai2));
                newCallers.push(args2);
              }
            }
          }
        }
      }
    }
  }
  // For each new Ctor() caller, resolve method and URL with per-caller correlation
  for (var ci = 0; ci < newCallers.length; ci++) {
    var cArgs = newCallers[ci];
    var mArg = methodParamIdx >= 0 && methodParamIdx < cArgs.length ? cArgs[methodParamIdx] : null;
    var uArg = urlParamIdx >= 0 && urlParamIdx < cArgs.length ? cArgs[urlParamIdx] : null;
    if (!mArg && !uArg) continue;
    // Classify each arg: literal string, param of enclosing function, or other
    var mLiteral = mArg && _t.isStringLiteral(mArg.node) ? mArg.node.value : null;
    var uLiteral = uArg && _t.isStringLiteral(uArg.node) ? uArg.node.value : null;
    var mParamBinding = mArg && _t.isIdentifier(mArg.node) ? mArg.scope.getBinding(mArg.node.name) : null;
    var uParamBinding = uArg && _t.isIdentifier(uArg.node) ? uArg.scope.getBinding(uArg.node.name) : null;
    var mIsParam = mParamBinding && mParamBinding.kind === "param";
    var uIsParam = uParamBinding && uParamBinding.kind === "param";
    // When BOTH args are params of the SAME function, do per-caller correlated resolution.
    // Skip mixed literal+param callers — they're conditional branches where the param
    // carries wrong-type values (e.g. method strings in a URL slot).
    if (mIsParam && uIsParam && mParamBinding.scope === uParamBinding.scope) {
      var _funcP = mParamBinding.scope.path;
      var _mIdx = -1, _uIdx = -1;
      for (var _pi = 0; _pi < _funcP.node.params.length; _pi++) {
        if (_t.isIdentifier(_funcP.node.params[_pi]) && _funcP.node.params[_pi].name === mArg.node.name) _mIdx = _pi;
        if (_t.isIdentifier(_funcP.node.params[_pi]) && _funcP.node.params[_pi].name === uArg.node.name) _uIdx = _pi;
      }
      if (_mIdx >= 0 && _uIdx >= 0) {
        var _fCallers = _findFunctionCallerArgs(_funcP);
        for (var _fci = 0; _fci < _fCallers.length; _fci++) {
          var _fArgs = _fCallers[_fci];
          var _fm = [], _fu = [];
          if (_mIdx < _fArgs.length) _fm = _resolveAllValues(_fArgs[_mIdx], 2);
          if (_uIdx < _fArgs.length) _fu = _resolveAllValues(_fArgs[_uIdx], 2);
          _fm = _fm.filter(function(m) { return typeof m === "string" && _HTTP_METHODS_LC[m.toLowerCase()]; });
          for (var _fui = 0; _fui < _fu.length; _fui++) {
            if (typeof _fu[_fui] !== "string") continue;
            var _fMethod = _fm.length > 0 ? _fm[0].toUpperCase() : "GET";
            sites.push({ url: _fu[_fui], method: _fMethod, headers: headers || {}, type: "xhr" });
          }
        }
      }
      continue;
    }
    // Skip mixed: one literal + one param (conditional branch — param meaning is ambiguous)
    if ((mLiteral && uIsParam) || (uLiteral && mIsParam)) continue;
    // Both args are literals — emit directly
    if (mLiteral || uLiteral) {
      var _m = mLiteral && _HTTP_METHODS_LC[mLiteral.toLowerCase()] ? mLiteral.toUpperCase() : "GET";
      if (uLiteral) sites.push({ url: uLiteral, method: _m, headers: headers || {}, type: "xhr" });
    }
  }
  // Global method caller sweep: find globalName.httpMethod(url) callers directly.
  // This handles chained .send()/.set() and works when factory uses loop variables.
  // Prefer these results over constructor-based ones (more complete: has chaining info).
  {
    var _globalSites = [];
    var _progPath = fromPath.findParent(function(p) { return p.isProgram(); });
    if (_progPath) {
      try {
        _progPath.traverse({
          CallExpression: function(_gcPath) {
            var _gc = _gcPath.node.callee;
            if (!_t.isMemberExpression(_gc) || _gc.computed) return;
            if (!_t.isIdentifier(_gc.object) || !_t.isIdentifier(_gc.property)) return;
            var _gn = _gc.object.name, _mn = _gc.property.name;
            if (!_globalAssignments[_gn] || !_HTTP_METHODS_LC[_mn]) return;
            if (_gcPath.scope.getBinding(_gn)) return;
            if (_gcPath.node.arguments.length < 1) return;
            var _gUrls = _resolveAllValues(_gcPath.get("arguments.0"), 1);
            for (var _gui = 0; _gui < _gUrls.length; _gui++) {
              if (typeof _gUrls[_gui] === "string") {
                var _gSite = { url: _gUrls[_gui], method: _mn.toUpperCase(), headers: headers || {}, type: "xhr" };
                // Extract body params from .send({...}) chains
                var _chainNode = _gcPath;
                while (_chainNode.parentPath && _t.isMemberExpression(_chainNode.parent) &&
                       _chainNode.parent.object === _chainNode.node &&
                       _chainNode.parentPath.parentPath && _t.isCallExpression(_chainNode.parentPath.parent) &&
                       _chainNode.parentPath.parent.callee === _chainNode.parent) {
                  var _chainCall = _chainNode.parentPath.parentPath;
                  var _chainProp = _chainNode.parent.property;
                  if (_t.isIdentifier(_chainProp)) {
                    if (_chainProp.name === "send" && _chainCall.node.arguments.length > 0) {
                      var _sendArg = _chainCall.node.arguments[0];
                      if (_t.isObjectExpression(_sendArg)) {
                        _gSite.params = _extractObjectProperties(_sendArg);
                        for (var _spi = 0; _spi < _gSite.params.length; _spi++) _gSite.params[_spi].location = "body";
                      }
                    } else if (_chainProp.name === "set" && _chainCall.node.arguments.length >= 2) {
                      var _hKey = _chainCall.node.arguments[0], _hVal = _chainCall.node.arguments[1];
                      if (_t.isStringLiteral(_hKey) && _t.isStringLiteral(_hVal)) {
                        if (!_gSite.headers) _gSite.headers = {};
                        _gSite.headers[_hKey.value] = _hVal.value;
                      }
                    }
                  }
                  _chainNode = _chainCall;
                }
                _globalSites.push(_gSite);
              }
            }
          }
        });
      } catch (e) { _resolver.collectError(e, "globalMethodCallerSweep"); }
    }
    if (_globalSites.length > 0) sites = _globalSites;
  }
  return sites;
}

// Resolve this.prop through constructor: find SomeClass constructor, look for this.prop = param,
// then find new SomeClass(value) call sites and extract the corresponding argument.
function _resolveConstructorProperty(fromPath, ctorName, propName, depth) {
  if (!_resolver.guard("X", fromPath.node)) return [];
  try {
  var ctorBinding = fromPath.scope.getBinding(ctorName);
  if (!ctorBinding) return [];
  var ctorPath = null;
  if (_t.isFunctionDeclaration(ctorBinding.path.node)) ctorPath = ctorBinding.path;
  else if (_t.isVariableDeclarator(ctorBinding.path.node) && ctorBinding.path.node.init) {
    var init = ctorBinding.path.node.init;
    if (_t.isFunctionExpression(init) || _t.isArrowFunctionExpression(init))
      ctorPath = ctorBinding.path.get("init");
  }
  if (!ctorPath) return [];

  var rich = _findThisAssignedParamRich(ctorPath, propName);
  if (!rich) return [];

  var match = _findCtorParamOrDestr(ctorPath.node.params, rich.paramName);
  if (!match) return [];
  var paramIdx = match.idx;
  var extractKey = rich.propFromParam || match.key || null;

  function extractArg(argPath) {
    if (extractKey) return _resolveCtorArgDestrProp(argPath, extractKey, depth);
    return _resolveAllValues(argPath, depth + 1);
  }

  // Find new CtorName(args) callers (direct and aliased)
  var values = [];
  if (ctorBinding.referencePaths) {
    for (var r = 0; r < ctorBinding.referencePaths.length; r++) {
      var ref = ctorBinding.referencePaths[r];
      // Direct: new CtorName(args)
      if (ref.parent && _t.isNewExpression(ref.parent) && ref.parent.callee === ref.node &&
          paramIdx < ref.parent.arguments.length) {
        values = values.concat(extractArg(ref.parentPath.get("arguments." + paramIdx)));
      }
      // Aliased: obj.prop = CtorName → new obj.prop(args)
      if (ref.parent && _t.isAssignmentExpression(ref.parent) && ref.parent.right === ref.node &&
          _t.isMemberExpression(ref.parent.left) && !ref.parent.left.computed) {
        var _aliasObj = ref.parent.left.object;
        var _aliasProp = ref.parent.left.property;
        if (_t.isIdentifier(_aliasObj) && _t.isIdentifier(_aliasProp)) {
          var _aliasBinding = ref.parentPath.scope.getBinding(_aliasObj.name);
          if (_aliasBinding && _aliasBinding.referencePaths) {
            for (var ari = 0; ari < _aliasBinding.referencePaths.length; ari++) {
              var aRef = _aliasBinding.referencePaths[ari];
              // Check: new aliasObj.aliasProp(args) — aRef is Identifier(aliasObj), parent is MemberExpression
              if (_t.isMemberExpression(aRef.parent) && aRef.parent.object === aRef.node &&
                  _t.isIdentifier(aRef.parent.property, {name: _aliasProp.name}) &&
                  aRef.parentPath && _t.isNewExpression(aRef.parentPath.parent) &&
                  aRef.parentPath.parent.callee === aRef.parent &&
                  paramIdx < aRef.parentPath.parent.arguments.length) {
                values = values.concat(extractArg(aRef.parentPath.parentPath.get("arguments." + paramIdx)));
              }
            }
          }
        }
      }
    }
  }
  return values;
  } catch (_rxe) {
    if (_rxe instanceof RangeError) { _resolver.collectError(_rxe, "resolveXhrOpenValues"); return []; }
    throw _rxe;
  } finally { _resolver.unguard("X", fromPath.node); }
}

// Resolve this.prop in an ES6 class method by finding the constructor's this.prop = param assignment,
// then tracing the param value from new ClassName(...) call sites.
function _resolveClassConstructorProperty(fromPath, classDecl, className, propName, depth) {
  if (!_resolver.guard("CC", fromPath.node)) return [];
  try {
  // Find the constructor ClassMethod in the class body
  var classBody = classDecl.node.body;
  var ctorMethod = null;
  var ctorMethodPath = null;
  for (var ci = 0; ci < classBody.body.length; ci++) {
    var member = classBody.body[ci];
    if (_t.isClassMethod(member) && member.kind === "constructor") {
      ctorMethod = member;
      ctorMethodPath = classDecl.get("body.body." + ci);
      break;
    }
  }
  if (!ctorMethod) return [];

  var rich = _findThisAssignedParamRich(ctorMethodPath, propName);
  if (!rich) {
    // Constructor assigns `this.X = expr` where expr is neither a bare
    // ident nor an ident.Y member. Resolve expr directly in the
    // constructor's scope — no caller substitution needed because expr
    // is closed-over in the constructor body.
    var rhsPath = _findThisAssignmentRhsPath(ctorMethodPath, propName);
    if (rhsPath) return _resolveAllValues(rhsPath, depth + 1);
    return [];
  }

  // Resolve the param identifier (including destructure patterns) to an idx
  // and optional destructured-key. When the assignment is `this.X = p.Y`,
  // `rich.propFromParam` takes precedence over any destructured-key logic —
  // the param is a plain identifier `p` and we want to extract `.Y` from
  // the caller's arg.
  var match = _findCtorParamOrDestr(ctorMethod.params, rich.paramName);
  if (!match) return [];
  var paramIdx = match.idx;
  // When both destr-key AND propFromParam are present, prefer propFromParam
  // because `rich` reflects the actual in-body access pattern. In practice
  // they can't both be set: a destructured param binds an identifier in
  // scope; it doesn't carry a property access in the RHS.
  var extractKey = rich.propFromParam || match.key || null;

  // Find new ClassName(args) callers and extract the corresponding argument
  var classBinding = fromPath.scope.getBinding(className);
  if (!classBinding) classBinding = classDecl.scope.getBinding(className);
  if (!classBinding || !classBinding.referencePaths) return [];

  var values = [];
  for (var r = 0; r < classBinding.referencePaths.length; r++) {
    var ref = classBinding.referencePaths[r];
    if (ref.parent && _t.isNewExpression(ref.parent) && ref.parent.callee === ref.node &&
        paramIdx < ref.parent.arguments.length) {
      var argPath = ref.parentPath.get("arguments." + paramIdx);
      if (extractKey) values = values.concat(_resolveCtorArgDestrProp(argPath, extractKey, depth));
      else values = values.concat(_resolveAllValues(argPath, depth + 1));
    }
  }
  // Indirect callers via Object.defineProperty getter exposure: pure-JS
  // pattern where a class is exposed through `{get: () => CLASS}` on
  // some object, and consumers do `new <obj>.<key>(args)`. Reading
  // `<obj>.<key>` invokes the getter and returns the class; constructing
  // it invokes the class's constructor with `args`. To find these
  // callers we walk the class's referencePaths for arrow-thunk shapes
  // `() => CLASS`, locate the descriptor, and recursively find consumers
  // that construct from `<aliasOf(target)>.<key>`.
  if (values.length === 0) {
    var indirect = _findClassIndirectCtorArgs(classBinding, classDecl, paramIdx, depth);
    for (var ii = 0; ii < indirect.length; ii++) {
      if (extractKey) values = values.concat(_resolveCtorArgDestrProp(indirect[ii], extractKey, depth));
      else values = values.concat(_resolveAllValues(indirect[ii], depth + 1));
    }
  }
  return values;
  } catch (_rcce) {
    if (_rcce instanceof RangeError) { _resolver.collectError(_rcce, "resolveCallbackChainValues"); return []; }
    throw _rcce;
  } finally { _resolver.unguard("CC", fromPath.node); }
}

// Resolve parameter values from callers of a global function (window.X = function).
// Since there's no scope binding, walk up to the program scope and find bare
// identifier calls matching the global name.
function _resolveParamFromGlobalCallers(funcPath, globalName, paramIdx, depth, propName, methodName) {
  var values = [];
  _traverseGlobalCallers(funcPath, globalName, methodName || null, function(innerPath) {
    if (paramIdx < innerPath.node.arguments.length) {
      var argPath = innerPath.get("arguments." + paramIdx);
      var argVals = propName ? _resolvePropertyFromArg(argPath, propName, depth) : _resolveAllValues(argPath, depth + 1);
      values = values.concat(argVals);
    } else {
      values = values.concat(_resolveOverloadedArg(innerPath, paramIdx, depth, propName));
    }
  });
  return values;
}


// Module-level call-site matchers. Single source of truth for "is
// this referencePath the object of a direct/method invocation, and
// if so, what's the CallExpression NodePath?". Pure structural
// matching against AST shapes per ECMA-262 § 13.3.6 (MemberExpression)
// and § 13.3.5 (CallExpression). Higher-level helpers (scanners,
// collectors) compose these matchers; downstream behavior (collect
// arg path, enqueue binding, resolve value, handle overload) lives
// at the higher layer where it belongs.

// `<ref>(...args)` invocation site. Returns the CallExpression
// NodePath, or null if refPath isn't the callee of a CallExpression.
function _matchDirectCallSite(refPath) {
  if (!refPath || !refPath.parent) return null;
  if (!_t.isCallExpression(refPath.parent) || refPath.parent.callee !== refPath.node) return null;
  return refPath.parentPath;
}

// `<ref>.<methodName>(...args)` invocation site. Returns the
// CallExpression NodePath, or null. The method-name match is a
// structural property check (ECMA-262 § 13.3.2 MemberExpression with
// non-computed string-named property), not a name-resolution lookup.
function _matchMethodCallSite(refPath, methodName) {
  if (!refPath || !refPath.parent) return null;
  var mem = refPath.parent;
  if (!_t.isMemberExpression(mem) || mem.object !== refPath.node || mem.computed) return null;
  if (!_t.isIdentifier(mem.property, { name: methodName })) return null;
  var callNode = refPath.parentPath ? refPath.parentPath.parent : null;
  if (!callNode || !_t.isCallExpression(callNode) || callNode.callee !== mem) return null;
  return refPath.parentPath.parentPath;
}

// Module-level scanners — each takes a referencePaths array and a
// paramIdx and returns arg paths at the specific call shape. Both
// _enumerateCallerArgPathsForParam and other resolvers compose these.
// They are strict (paramIdx must be < arguments.length); the
// _collect* helpers handle the overloaded-call clamp separately.

function _scanDirectCallers(refs, paramIdx) {
  var out = [];
  if (!refs) return out;
  for (var r = 0; r < refs.length; r++) {
    var callPath = _matchDirectCallSite(refs[r]);
    if (callPath && paramIdx < callPath.node.arguments.length) {
      out.push(callPath.get("arguments." + paramIdx));
    }
  }
  return out;
}

function _scanMethodCallers(refs, methodName, paramIdx) {
  var out = [];
  if (!refs) return out;
  for (var r = 0; r < refs.length; r++) {
    var callPath = _matchMethodCallSite(refs[r], methodName);
    if (callPath && paramIdx < callPath.node.arguments.length) {
      out.push(callPath.get("arguments." + paramIdx));
    }
  }
  return out;
}

// `<ref>.bind(thisArg, ...preArgs)` invocation site (Function.prototype
// .bind per ECMA-262 § 20.2.3.2). The bound `preArgs` are spec-prepended
// to subsequent caller args, so paramIdx in the original function maps
// to either:
//   • paramIdx < preArgsCount   → the bind call's `arguments[paramIdx+1]`
//                                 (skipping thisArg).
//   • paramIdx >= preArgsCount  → the bind result's callers' arg at
//                                 `(paramIdx - preArgsCount)`. Resolved
//                                 by walking the bind result's variable
//                                 binding's referencePaths.
function _scanBindCallers(refs, paramIdx) {
  var out = [];
  if (!refs) return out;
  for (var r = 0; r < refs.length; r++) {
    var bindCallPath = _matchMethodCallSite(refs[r], "bind");
    if (!bindCallPath) continue;
    var bindCallNode = bindCallPath.node;
    if (bindCallNode.arguments.length < 1) continue;
    var preArgsCount = bindCallNode.arguments.length - 1;
    if (paramIdx < preArgsCount) {
      out.push(bindCallPath.get("arguments." + (paramIdx + 1)));
      continue;
    }
    var callerArgIdx = paramIdx - preArgsCount;
    var bindOwner = bindCallPath.parent;
    if (!bindOwner || !_t.isVariableDeclarator(bindOwner) || !_t.isIdentifier(bindOwner.id) ||
        bindOwner.init !== bindCallNode) continue;
    var aliasB = bindCallPath.parentPath.scope.getBinding(bindOwner.id.name);
    if (!aliasB || !aliasB.referencePaths) continue;
    var aliasArgs = _scanDirectCallers(aliasB.referencePaths, callerArgIdx);
    for (var ai = 0; ai < aliasArgs.length; ai++) out.push(aliasArgs[ai]);
  }
  return out;
}

// Enumerate every caller-arg path that a function-parameter binding
// could receive. Covers the same definition-shape routes as
// _resolveParamFromCallers (direct funcBinding, ObjectProperty,
// obj.method assignment, computed obj[method] assignment, callback
// arg, prototype-method, IIFE return, ES6 class method), but does
// NOT call any resolver function — only walks bindings and pushes
// arg paths. Used by _resolvePropertyFromArg's worklist to follow
// param identifiers without recursing into _resolveParamFromCallers
// (which would close the resolver SCC). Pure spec-grounded
// scope/binding navigation. Composes _scanDirectCallers /
// _scanMethodCallers as building blocks.
function _enumerateCallerArgPathsForParam(binding) {
  var out = [];
  var funcPath = binding.scope.path;
  if (!_t.isFunction(funcPath.node)) return out;
  var paramIdx = _findParamIndex(funcPath.node.params, binding.identifier.name);
  if (paramIdx === -1) return out;
  var funcBinding = _getFunctionBinding(funcPath);
  function pushDirectCallerArgs(refs) {
    var args = _scanDirectCallers(refs, paramIdx);
    for (var i = 0; i < args.length; i++) out.push(args[i]);
  }
  function pushMethodCallerArgs(refs, methodName) {
    var args = _scanMethodCallers(refs, methodName, paramIdx);
    for (var i = 0; i < args.length; i++) out.push(args[i]);
  }

  // Route 6: direct funcBinding callers + Function.prototype.bind.
  if (funcBinding && funcBinding.referencePaths) {
    pushDirectCallerArgs(funcBinding.referencePaths);
    var bindArgs = _scanBindCallers(funcBinding.referencePaths, paramIdx);
    for (var bi = 0; bi < bindArgs.length; bi++) out.push(bindArgs[bi]);
    return out;
  }
  // Route 1: ObjectProperty — { method: function(param) {} }
  if (_t.isObjectProperty(funcPath.parent)) {
    var opKey = funcPath.parent.key;
    var opName = _t.isIdentifier(opKey) ? opKey.name :
      (_t.isStringLiteral(opKey) ? opKey.value : null);
    if (opName) {
      var objExprPath = funcPath.parentPath ? funcPath.parentPath.parentPath : null;
      if (objExprPath && _t.isObjectExpression(objExprPath.node)) {
        var declPath = objExprPath.parentPath;
        if (declPath && _t.isVariableDeclarator(declPath.node) && _t.isIdentifier(declPath.node.id)) {
          var opObjBinding = declPath.scope.getBinding(declPath.node.id.name);
          if (opObjBinding && opObjBinding.referencePaths) pushMethodCallerArgs(opObjBinding.referencePaths, opName);
        }
      }
    }
    return out;
  }
  // ES6 shorthand: { method(param) {} } — funcPath IS an ObjectMethod.
  if (funcPath.isObjectMethod && funcPath.isObjectMethod()) {
    var omKey = funcPath.node.key;
    var omName = _t.isIdentifier(omKey) && !funcPath.node.computed ? omKey.name :
      (_t.isStringLiteral(omKey) ? omKey.value : null);
    if (omName) {
      var omObjPath = funcPath.parentPath;
      if (omObjPath && _t.isObjectExpression(omObjPath.node)) {
        var omDecl = omObjPath.parentPath;
        if (omDecl && _t.isVariableDeclarator(omDecl.node) && _t.isIdentifier(omDecl.node.id)) {
          var omObjBinding = omDecl.scope.getBinding(omDecl.node.id.name);
          if (omObjBinding && omObjBinding.referencePaths) pushMethodCallerArgs(omObjBinding.referencePaths, omName);
        }
      }
    }
    return out;
  }
  // Route 1b: obj.method = function(param) {} — non-computed assignment.
  if (_t.isAssignmentExpression(funcPath.parent) &&
      _t.isMemberExpression(funcPath.parent.left) && !funcPath.parent.left.computed) {
    var aProp = funcPath.parent.left.property;
    var aObj = funcPath.parent.left.object;
    var aMethodName = _t.isIdentifier(aProp) ? aProp.name : null;
    if (aMethodName) {
      // Plain identifier object: obj.method = ...
      if (_t.isIdentifier(aObj)) {
        var aObjBinding = funcPath.scope.getBinding(aObj.name);
        if (aObjBinding && aObjBinding.referencePaths) pushMethodCallerArgs(aObjBinding.referencePaths, aMethodName);
        return out;
      }
      // Route 3: Ctor.prototype.method = function(param) {} — trace via instances.
      if (_t.isMemberExpression(aObj) && !aObj.computed &&
          (_t.isIdentifier(aObj.property, { name: "prototype" }) ||
           (_t.isStringLiteral(aObj.property) && aObj.property.value === "prototype")) &&
          _t.isIdentifier(aObj.object)) {
        var ctorName = aObj.object.name;
        var programPath = funcPath.findParent(function(p) { return p.isProgram(); });
        if (programPath) {
          try {
            programPath.traverse({
              VariableDeclarator: function(decPath) {
                var init = decPath.node.init;
                if (!init || !_t.isNewExpression(init) || !_t.isIdentifier(init.callee, { name: ctorName })) return;
                var instBinding = decPath.scope.getBinding(decPath.node.id.name);
                if (instBinding && instBinding.referencePaths) pushMethodCallerArgs(instBinding.referencePaths, aMethodName);
              },
            });
          } catch (e) { _resolver.collectError(e, "enumeratePrototypeMethodCallerArgs"); }
        }
        return out;
      }
    }
  }
  // Route 1c: obj[method] = function(param) {} — computed assignment.
  if (_t.isAssignmentExpression(funcPath.parent) &&
      _t.isMemberExpression(funcPath.parent.left) && funcPath.parent.left.computed) {
    var caObj = funcPath.parent.left.object;
    var caProp = funcPath.parent.left.property;
    if (_t.isIdentifier(caObj) && _t.isIdentifier(caProp)) {
      var caObjBinding = funcPath.scope.getBinding(caObj.name);
      var caPropVals = _resolveAllValues(funcPath.parentPath.get("left.property"), 0);
      if (caObjBinding && caPropVals.length > 0) {
        for (var cvi = 0; cvi < caPropVals.length; cvi++) {
          if (typeof caPropVals[cvi] !== "string") continue;
          pushMethodCallerArgs(caObjBinding.referencePaths, caPropVals[cvi]);
        }
      }
    }
    return out;
  }
  // Route 2: Callback argument — someFunc(function(param) {})
  // The callback is invoked inside the callee. Resolve the callee,
  // find which of its params receives our callback, then traverse
  // the callee body for direct calls / .call() / .apply() on that
  // param. Each such invocation's arg at paramIdx is a caller-arg
  // path. _resolveCalleeFuncPath is outside the SCC so calling it
  // here doesn't reintroduce the cycle.
  if (_t.isCallExpression(funcPath.parent)) {
    var cbCallExpr = funcPath.parentPath;
    var cbArgIdx = -1;
    for (var cbi = 0; cbi < funcPath.parent.arguments.length; cbi++) {
      if (funcPath.parent.arguments[cbi] === funcPath.node) { cbArgIdx = cbi; break; }
    }
    if (cbArgIdx >= 0) {
      var targetFuncPath = _resolveCalleeFuncPath(cbCallExpr, 0);
      if (targetFuncPath && targetFuncPath.node) {
        var targetParams = targetFuncPath.node.params || [];
        var cbParamIdx = cbArgIdx;
        if (cbParamIdx >= targetParams.length) cbParamIdx = targetParams.length - 1;
        if (cbParamIdx >= 0) {
          var cbParamName = _t.isIdentifier(targetParams[cbParamIdx]) ? targetParams[cbParamIdx].name : null;
          if (cbParamName) {
            try {
              targetFuncPath.traverse({
                CallExpression: function(innerPath) {
                  var ic = innerPath.node.callee;
                  // Direct: cbParam(arg0, arg1, ...)
                  if (_t.isIdentifier(ic, { name: cbParamName })) {
                    var innerBinding = innerPath.scope.getBinding(cbParamName);
                    if (innerBinding && innerBinding.kind === "param" && innerBinding.scope.path === targetFuncPath &&
                        paramIdx < innerPath.node.arguments.length) {
                      out.push(innerPath.get("arguments." + paramIdx));
                    }
                  }
                  // .call(thisArg, arg0, ...): paramIdx shifts by 1.
                  if (_t.isMemberExpression(ic) && !ic.computed &&
                      _t.isIdentifier(ic.object, { name: cbParamName }) &&
                      _t.isIdentifier(ic.property, { name: "call" })) {
                    var callBinding = innerPath.scope.getBinding(cbParamName);
                    if (callBinding && callBinding.kind === "param" && callBinding.scope.path === targetFuncPath) {
                      var shifted = paramIdx + 1;
                      if (shifted < innerPath.node.arguments.length) {
                        out.push(innerPath.get("arguments." + shifted));
                      }
                    }
                  }
                  // .apply(thisArg, [arg0, ...]) — push the literal-array element at paramIdx.
                  if (_t.isMemberExpression(ic) && !ic.computed &&
                      _t.isIdentifier(ic.object, { name: cbParamName }) &&
                      _t.isIdentifier(ic.property, { name: "apply" })) {
                    var applyBinding = innerPath.scope.getBinding(cbParamName);
                    if (applyBinding && applyBinding.kind === "param" && applyBinding.scope.path === targetFuncPath &&
                        innerPath.node.arguments.length >= 2 &&
                        _t.isArrayExpression(innerPath.node.arguments[1]) &&
                        paramIdx < innerPath.node.arguments[1].elements.length &&
                        innerPath.node.arguments[1].elements[paramIdx]) {
                      out.push(innerPath.get("arguments.1.elements." + paramIdx));
                    }
                  }
                },
              });
            } catch (e) { _resolver.collectError(e, "enumerateCallbackArgInvocations"); }
          }
        }
      }
      return out;
    }
  }
  // Route 5: ES6 class method — class Foo { method(param) {} }
  if (_t.isClassMethod(funcPath.node) && _t.isClassBody(funcPath.parent)) {
    var classDecl = funcPath.parentPath.parentPath;
    if (classDecl && (_t.isClassDeclaration(classDecl.node) || _t.isClassExpression(classDecl.node))) {
      var className = classDecl.node.id ? classDecl.node.id.name : null;
      var clMethodName = _t.isIdentifier(funcPath.node.key) ? funcPath.node.key.name : null;
      if (className && clMethodName) {
        var clProgramPath = funcPath.findParent(function(p) { return p.isProgram(); });
        if (clProgramPath) {
          try {
            clProgramPath.traverse({
              VariableDeclarator: function(decPath) {
                var init = decPath.node.init;
                if (!init || !_t.isNewExpression(init) || !_t.isIdentifier(init.callee, { name: className })) return;
                var instBinding = decPath.scope.getBinding(decPath.node.id.name);
                if (instBinding && instBinding.referencePaths) pushMethodCallerArgs(instBinding.referencePaths, clMethodName);
              },
            });
          } catch (e) { _resolver.collectError(e, "enumerateClassMethodCallerArgs"); }
        }
      }
    }
    return out;
  }
  // Route 4: ReturnStatement — function returned from IIFE.
  if (_t.isReturnStatement(funcPath.parent)) {
    var encFunc = funcPath.findParent(function(p) { return p.isFunction() && p !== funcPath; });
    if (encFunc) {
      var encParent = encFunc.parentPath;
      if (encParent && _t.isCallExpression(encParent.node) && encParent.node.callee === encFunc.node) {
        var iifeParent = encParent.parentPath;
        if (iifeParent && _t.isVariableDeclarator(iifeParent.node) && _t.isIdentifier(iifeParent.node.id)) {
          var iifeVarBinding = iifeParent.scope.getBinding(iifeParent.node.id.name);
          if (iifeVarBinding && iifeVarBinding.referencePaths) pushDirectCallerArgs(iifeVarBinding.referencePaths);
        }
      }
    }
  }
  return out;
}

// ─── Object Property Resolution from Caller Arguments ───────────────────────
// When a function parameter is used as obj.prop (MemberExpression), resolve the
// property value by finding callers, getting their object literal arguments,
// and extracting the named property.

// Iterative worklist version: a queue of (argPath, depth) tuples to try.
// First non-empty resolution wins. Replaces recursion via initArgs which
// could grow JS stack linearly with wrapper-call chain depth.
function _resolvePropertyFromArg(argPath, propName, depth) {
  var queue = [{ p: argPath, d: depth }];
  var queueSeen = new Set();
  while (queue.length > 0) {
    var item = queue.shift();
    var ap = item.p;
    var dp = item.d;
    if (!ap || !ap.node) continue;
    if (queueSeen.has(ap.node)) continue;
    queueSeen.add(ap.node);

    var objNode = _resolveToObject(ap, dp + 1);
    if (objNode && objNode._path) {
      for (var i = 0; i < objNode.properties.length; i++) {
        var prop = objNode.properties[i];
        if (!_t.isObjectProperty(prop) || prop.computed) continue;
        if (_getKeyName(prop.key) === propName) {
          return _resolveAllValues(objNode._path.get("properties." + i + ".value"), dp + 1);
        }
      }
      continue;
    }

    // Fallbacks for Identifier args.
    if (_t.isIdentifier(ap.node)) {
      var paramBinding = ap.scope.getBinding(ap.node.name);
      // Fallback 1: param → enumerate every caller-arg path the
      // param could receive (across all definition-shape routes —
      // direct funcBinding, ObjectProperty, obj.method assignment,
      // computed assignment, callback arg, prototype, IIFE return,
      // ES6 class method) and push them onto our own worklist. The
      // queue's existing logic extracts propName / chains through
      // further fallbacks. Avoids a synchronous
      // _resolveParamFromCallers call that would close the
      // _resolvePropertyFromArg ↔ _resolveParamFromCallers SCC.
      if (paramBinding && paramBinding.kind === "param") {
        var callerArgPaths = _enumerateCallerArgPathsForParam(paramBinding);
        for (var cap = 0; cap < callerArgPaths.length; cap++) {
          queue.push({ p: callerArgPaths[cap], d: dp + 1 });
        }
        continue;
      }
      // Fallback 2: local variable — look for obj.prop = value assignments.
      if (paramBinding && paramBinding.referencePaths) {
        var assignVals = [];
        var refs = paramBinding.referencePaths;
        for (var ri = 0; ri < refs.length; ri++) {
          var refParent = refs[ri].parent;
          if (_t.isMemberExpression(refParent) && refParent.object === refs[ri].node &&
              !refParent.computed && _t.isIdentifier(refParent.property, { name: propName })) {
            var assignNode = refs[ri].parentPath ? refs[ri].parentPath.parent : null;
            if (assignNode && _t.isAssignmentExpression(assignNode) && assignNode.operator === "=" &&
                assignNode.left === refParent) {
              var rhsVals = _resolveAllValues(refs[ri].parentPath.parentPath.get("right"), dp + 1);
              assignVals = assignVals.concat(rhsVals);
            }
          }
        }
        if (assignVals.length > 0) return assignVals;
      }
      // Fallback 3: variable initialized from function call — push each
      // call arg onto the work queue (iterative replacement for the
      // original tail-recursive trace through initArgs).
      if (paramBinding && paramBinding.path.isVariableDeclarator && paramBinding.path.isVariableDeclarator()) {
        var initNode = paramBinding.path.node.init;
        if (initNode && _t.isCallExpression(initNode)) {
          var initPath = paramBinding.path.get("init");
          for (var iai = 0; iai < initNode.arguments.length; iai++) {
            queue.push({ p: initPath.get("arguments." + iai), d: dp + 1 });
          }
        }
      }
    }
  }
  return [];
}

// ─── Data Extraction Helpers ────────────────────────────────────────────────

// Resolve a header value node to a string using parameter bindings (for closure variables)
// Iterative version: explicit work-stack of "compose" frames + a single
// dispatch loop. Each frame carries either a leaf node to resolve or a
// pending compose (BinaryExpression + or TemplateLiteral) waiting for
// sub-results. JS call stack stays at depth 1 regardless of expression depth.
function _resolveHeaderValue(rootNode, bindings) {
  // Quick path for common cases — avoids work-stack allocation.
  if (_t.isStringLiteral(rootNode)) return rootNode.value;
  if (_t.isIdentifier(rootNode) && bindings[rootNode.name] && bindings[rootNode.name].length > 0 &&
      typeof bindings[rootNode.name][0] === "string") return bindings[rootNode.name][0];

  // Resolve a leaf node directly (returns string or null).
  function resolveLeaf(n) {
    if (_t.isStringLiteral(n)) return n.value;
    if (_t.isIdentifier(n) && bindings[n.name] && bindings[n.name].length > 0 &&
        typeof bindings[n.name][0] === "string") return bindings[n.name][0];
    return null;
  }

  // Work stack of frames. Each frame: { type, node, parts, idx }
  //   type "BIN_LEFT": expecting to produce left value of node, then move to BIN_RIGHT
  //   type "BIN_RIGHT": left already on results stack
  //   type "TL": iterating template quasis/expressions; idx tracks position
  // Results stack carries computed substrings.
  var stack = [{ type: "DISPATCH", node: rootNode }];
  var results = [];

  while (stack.length > 0) {
    var f = stack[stack.length - 1];

    if (f.type === "DISPATCH") {
      // Decide what kind of node this is.
      stack.pop();
      var n = f.node;
      if (_t.isStringLiteral(n) || _t.isIdentifier(n)) {
        var v = resolveLeaf(n);
        if (v === null) return null;
        results.push(v);
      } else if (_t.isBinaryExpression(n) && n.operator === "+") {
        // Iterative left-spine collection — handles N-term concats with
        // O(N) work-stack memory and O(1) JS call-stack depth.
        var binParts = [];
        var binCur = n;
        while (_t.isBinaryExpression(binCur) && binCur.operator === "+") {
          binParts.push(binCur.right);
          binCur = binCur.left;
        }
        binParts.push(binCur);
        binParts.reverse();
        // Push a JOIN frame marking how many parts to combine, then push
        // each part as a DISPATCH frame in REVERSE (so leftmost part is
        // processed first when popped LIFO).
        stack.push({ type: "JOIN", count: binParts.length });
        for (var bi = binParts.length - 1; bi >= 0; bi--) {
          stack.push({ type: "DISPATCH", node: binParts[bi] });
        }
      } else if (_t.isTemplateLiteral(n)) {
        // Push a TL_JOIN frame, then push each expression as DISPATCH.
        // Quasis are interleaved at JOIN time using the original node.
        stack.push({ type: "TL_JOIN", node: n });
        for (var ei = n.expressions.length - 1; ei >= 0; ei--) {
          stack.push({ type: "DISPATCH", node: n.expressions[ei] });
        }
      } else {
        return null;
      }
      continue;
    }

    if (f.type === "JOIN") {
      stack.pop();
      // Combine the last `count` results into one concatenated string.
      var combined = "";
      var startIdx = results.length - f.count;
      for (var ji = startIdx; ji < results.length; ji++) {
        combined += results[ji];
      }
      results.length = startIdx;
      results.push(combined);
      continue;
    }

    if (f.type === "TL_JOIN") {
      stack.pop();
      // Interleave quasis with the resolved expression values at the top
      // of the results stack. Pull `node.expressions.length` from results.
      var tn = f.node;
      var exprCount = tn.expressions.length;
      var exprStartIdx = results.length - exprCount;
      var exprVals = results.slice(exprStartIdx);
      results.length = exprStartIdx;
      var parts = [];
      for (var qi = 0; qi < tn.quasis.length; qi++) {
        parts.push(tn.quasis[qi].value.cooked || tn.quasis[qi].value.raw);
        if (qi < exprCount) parts.push(exprVals[qi]);
      }
      results.push(parts.join(""));
      continue;
    }

    // Should not happen — defensive.
    return null;
  }
  return results.length === 1 ? results[0] : null;
}

function _extractHeaders(objNode) {
  var headers = {};
  for (var i = 0; i < objNode.properties.length; i++) {
    var prop = objNode.properties[i];
    if (!_t.isObjectProperty(prop) || prop.computed) continue;
    var name = _getKeyName(prop.key);
    if (name && _t.isStringLiteral(prop.value)) {
      headers[name] = prop.value.value;
    }
  }
  return headers;
}

// State IDs for the iterative body-param extractor's frame state
// machine. INIT dispatches on node type; each AFTER-state receives a
// child frame's result via F.result and either bubbles it to this
// frame's caller (REPLACE) or accumulates it (param-loop) or
// short-circuits on first non-empty (opts.data).
// State IDs use semantic ranges matching the rest of the file's
// convention (_RAV_INIT = 0, group AFTER states by hundreds). 0 is
// the entry; 100/200/300/400/500 are the per-route AFTER groups.
var _EBP_INIT = 0;
var _EBP_ALIAS_AFTER = 100;       // alias-chain recursion result — REPLACE
var _EBP_PARAM_LOOP = 200;        // caller loop for param-bound identifiers
var _EBP_PARAM_AFTER = 201;       // caller-arg result accumulator
var _EBP_JSON_AFTER = 300;        // JSON.stringify(ident) recursion result — REPLACE
var _EBP_TOSTRING_AFTER = 400;    // .toString() unwrap recursion result — REPLACE
var _EBP_OPTS_LOOP = 500;         // opts.data candidate × property loop
var _EBP_OPTS_AFTER = 501;        // first non-empty wins

function _extractBodyParams(rootNode, rootScope) {
  // Iterative driver replacing the prior wrapper/inner mutual
  // recursion with a state-machine stack. Each frame represents one
  // extraction task; sub-frames (for alias chains, JSON.stringify on
  // identifiers, .toString() unwraps, function-param caller loops,
  // and opts.data property walks) are pushed onto the same stack.
  // The visited set is per-call (lives on the driver, not the
  // module) so concurrent calls don't share state.
  if (!rootNode) return [];
  var visited = new Set();
  var stack = [{ state: _EBP_INIT, node: rootNode, scope: rootScope,
                  L: {}, result: undefined }];
  var lastResult = [];
  while (stack.length > 0) {
    var top = stack[stack.length - 1];
    top.result = lastResult;
    lastResult = [];
    var step = _ebpStep(top);
    if (step.done !== undefined) {
      lastResult = step.done;
      stack.pop();
      continue;
    }
    top.state = step.state;
    var subNode = step.trace.node;
    var subScope = step.trace.scope;
    var subKey = (subNode.start != null && subNode.end != null) ? "B" + subNode.start + ":" + subNode.end : null;
    if (subKey && visited.has(subKey)) {
      lastResult = [];
      continue;
    }
    if (subKey) visited.add(subKey);
    stack.push({ state: _EBP_INIT, node: subNode, scope: subScope,
                  L: {}, result: undefined });
  }
  return lastResult;
}

function _ebpStep(F) {
  var node = F.node, scopePath = F.scope, L = F.L;
  while (true) {
    switch (F.state) {
      case _EBP_INIT: {
        // CASE A: Identifier with scope binding — FormData/URLSearchParams
        // accumulator walk, alias chain, or function-param caller loop.
        if (_t.isIdentifier(node) && scopePath && scopePath.scope) {
          var bpBinding = scopePath.scope.getBinding(node.name);
          if (bpBinding) {
            var bpInit = _t.isVariableDeclarator(bpBinding.path.node) ? bpBinding.path.node.init : null;
            var bpType = bpInit && _t.isNewExpression(bpInit) && _t.isIdentifier(bpInit.callee) ? bpInit.callee.name : null;
            if ((bpType === "FormData" || bpType === "URLSearchParams") &&
                !bpBinding.path.scope.getBinding(bpType) && bpBinding.referencePaths) {
              var fdParams = [];
              for (var fdri = 0; fdri < bpBinding.referencePaths.length; fdri++) {
                var fdRef = bpBinding.referencePaths[fdri];
                var fdMem = fdRef.parentPath;
                if (!fdMem || !fdMem.isMemberExpression() || fdMem.node.object !== fdRef.node || fdMem.node.computed) continue;
                var fdMethod = _t.isIdentifier(fdMem.node.property) ? fdMem.node.property.name : null;
                if (fdMethod !== "append" && fdMethod !== "set") continue;
                var fdCall = fdMem.parentPath;
                if (!fdCall || !fdCall.isCallExpression() || fdCall.node.callee !== fdMem.node ||
                    fdCall.node.arguments.length < 1) continue;
                var fdNameVals = _resolveAllValues(fdCall.get("arguments.0"), 0);
                for (var fdni = 0; fdni < fdNameVals.length; fdni++) {
                  if (typeof fdNameVals[fdni] !== "string" || !fdNameVals[fdni]) continue;
                  var fdAlreadyHave = false;
                  for (var fdpi = 0; fdpi < fdParams.length; fdpi++) {
                    if (fdParams[fdpi].name === fdNameVals[fdni]) { fdAlreadyHave = true; break; }
                  }
                  if (fdAlreadyHave) continue;
                  var fdField = { name: fdNameVals[fdni], required: true, location: "body" };
                  if (fdCall.node.arguments.length >= 2) {
                    var fdValVals = _resolveAllValues(fdCall.get("arguments.1"), 0);
                    if (fdValVals.length > 0 && typeof fdValVals[0] === "string") {
                      fdField.defaultValue = fdValVals[0];
                      fdField.type = "string";
                    }
                  }
                  fdParams.push(fdField);
                }
              }
              if (fdParams.length > 0) return { done: fdParams };
            }
            if (_t.isVariableDeclarator(bpBinding.path.node) && bpBinding.path.node.init) {
              return { trace: { node: bpBinding.path.node.init, scope: bpBinding.path },
                        state: _EBP_ALIAS_AFTER };
            }
            if (bpBinding.kind === "param") {
              var bpFuncPath = bpBinding.scope.path;
              var bpFuncB = null;
              if (bpFuncPath.node.id) bpFuncB = bpFuncPath.scope.parent ? bpFuncPath.scope.parent.getBinding(bpFuncPath.node.id.name) : null;
              if (!bpFuncB && _t.isVariableDeclarator(bpFuncPath.parent)) bpFuncB = bpFuncPath.scope.parent ? bpFuncPath.scope.parent.getBinding(bpFuncPath.parent.id.name) : null;
              if (bpFuncB && bpFuncB.referencePaths) {
                var bpIdx = _findParamIndex(bpFuncPath.node.params, node.name);
                if (bpIdx >= 0) {
                  L.bpRefs = bpFuncB.referencePaths;
                  L.bpArgIdx = bpIdx;
                  L.bri = 0;
                  L.params = [];
                  F.state = _EBP_PARAM_LOOP;
                  continue;
                }
              }
            }
          }
        }
        // CASE B: JSON.stringify
        if (_isJsonStringify(node, scopePath)) {
          if (node.arguments[0] && _t.isObjectExpression(node.arguments[0])) {
            var jsonParams = _extractObjectProperties(node.arguments[0]);
            for (var i = 0; i < jsonParams.length; i++) jsonParams[i].location = "body";
            return { done: jsonParams };
          }
          if (node.arguments[0] && _t.isIdentifier(node.arguments[0]) && scopePath) {
            return { trace: { node: node.arguments[0], scope: scopePath },
                      state: _EBP_JSON_AFTER };
          }
          return { done: [] };
        }
        // CASE C: new URLSearchParams({...})
        if (_t.isNewExpression(node) && _t.isIdentifier(node.callee, { name: "URLSearchParams" }) &&
            (!scopePath || !scopePath.scope.getBinding("URLSearchParams")) &&
            node.arguments[0] && _t.isObjectExpression(node.arguments[0])) {
          var uspParams = _extractObjectProperties(node.arguments[0]);
          for (var j = 0; j < uspParams.length; j++) uspParams[j].location = "body";
          return { done: uspParams };
        }
        // CASE D: .toString() unwrap
        if (_t.isCallExpression(node) && _t.isMemberExpression(node.callee) &&
            !node.callee.computed && _t.isIdentifier(node.callee.property, { name: "toString" }) &&
            scopePath) {
          return { trace: { node: node.callee.object, scope: scopePath },
                    state: _EBP_TOSTRING_AFTER };
        }
        // CASE E: ObjectExpression
        if (_t.isObjectExpression(node)) {
          var oeParams = _extractObjectProperties(node);
          for (var k = 0; k < oeParams.length; k++) oeParams[k].location = "body";
          return { done: oeParams };
        }
        // CASE F: opts.data MemberExpression
        if (_t.isMemberExpression(node) && !node.computed &&
            _t.isIdentifier(node.object) && _t.isIdentifier(node.property) &&
            scopePath && scopePath.scope) {
          var objName = node.object.name;
          var propName = node.property.name;
          var objBinding = scopePath.scope.getBinding(objName);
          if (objBinding) {
            var objCandidates = [];
            if (_t.isVariableDeclarator(objBinding.path.node) && objBinding.path.node.init) {
              var localObj = _resolveToObject(objBinding.path.get("init"), 0);
              if (localObj) objCandidates.push({ obj: localObj, path: objBinding.path.get("init") });
            } else if (objBinding.kind === "param") {
              var pFnPath = objBinding.scope.path;
              var pIdx = _findParamIndex(pFnPath.node.params, objName);
              if (pIdx >= 0) {
                var callerArgPaths = _findFunctionCallerArgs(pFnPath);
                for (var pri = 0; pri < callerArgPaths.length; pri++) {
                  if (pIdx < callerArgPaths[pri].length) {
                    var argPath = callerArgPaths[pri][pIdx];
                    var callerObj = _resolveToObject(argPath, 0);
                    if (callerObj) objCandidates.push({ obj: callerObj, path: argPath });
                  }
                }
              }
            }
            if (objCandidates.length > 0) {
              L.objCandidates = objCandidates;
              L.propName = propName;
              L.fci = 0;
              L.fpi = 0;
              F.state = _EBP_OPTS_LOOP;
              continue;
            }
          }
        }
        return { done: [] };
      }

      case _EBP_ALIAS_AFTER:
      case _EBP_JSON_AFTER:
      case _EBP_TOSTRING_AFTER: {
        // REPLACE — child's result becomes this frame's result.
        return { done: F.result || [] };
      }

      case _EBP_PARAM_LOOP: {
        while (L.bri < L.bpRefs.length) {
          var bRef = L.bpRefs[L.bri++];
          if (_t.isCallExpression(bRef.parent) && bRef.parent.callee === bRef.node &&
              L.bpArgIdx < bRef.parent.arguments.length) {
            return {
              trace: { node: bRef.parent.arguments[L.bpArgIdx], scope: bRef.parentPath },
              state: _EBP_PARAM_AFTER
            };
          }
        }
        return { done: L.params };
      }

      case _EBP_PARAM_AFTER: {
        // Accumulate caller's result; loop to next caller.
        var pSub = F.result || [];
        for (var pci = 0; pci < pSub.length; pci++) L.params.push(pSub[pci]);
        F.state = _EBP_PARAM_LOOP;
        continue;
      }

      case _EBP_OPTS_LOOP: {
        // Iterate candidates × matching properties; first non-empty wins.
        while (L.fci < L.objCandidates.length) {
          var cur = L.objCandidates[L.fci];
          if (!cur.obj.properties) { L.fci++; L.fpi = 0; continue; }
          while (L.fpi < cur.obj.properties.length) {
            var pp = cur.obj.properties[L.fpi];
            var ppi = L.fpi;
            L.fpi++;
            if (!_t.isObjectProperty(pp) || pp.computed) continue;
            var pk = _t.isIdentifier(pp.key) ? pp.key.name :
              (_t.isStringLiteral(pp.key) ? pp.key.value : null);
            if (pk !== L.propName) continue;
            var subScope = cur.obj._path ? cur.obj._path.get("properties." + ppi + ".value") : cur.path;
            return { trace: { node: pp.value, scope: subScope },
                      state: _EBP_OPTS_AFTER };
          }
          L.fci++;
          L.fpi = 0;
        }
        return { done: [] };
      }

      case _EBP_OPTS_AFTER: {
        var oSub = F.result || [];
        if (oSub.length > 0) return { done: oSub };
        F.state = _EBP_OPTS_LOOP;
        continue;
      }
    }
    return { done: [] };
  }
}

function _extractObjectProperties(node, objExprPath) {
  if (!node || !_t.isObjectExpression(node)) return [];
  var props = [];
  for (var i = 0; i < node.properties.length; i++) {
    var p = node.properties[i];
    if (_t.isSpreadElement(p)) {
      var spreadName = _t.isIdentifier(p.argument) ? p.argument.name : null;
      if (spreadName) props.push({ name: "..." + spreadName, spread: true, required: false });
      continue;
    }
    if (!_t.isObjectProperty(p) || p.computed) continue;
    var keyName = _getKeyName(p.key);
    if (!keyName) continue;

    var prop = { name: keyName, required: true };
    var val = p.value;

    if (p.shorthand && _t.isIdentifier(val)) {
      prop.source = val.name;
    } else if (_t.isIdentifier(val)) {
      prop.source = val.name;
    }

    if (_t.isLogicalExpression(val) && (val.operator === "||" || val.operator === "??")) {
      prop.required = false;
      if (_t.isStringLiteral(val.right) || _t.isNumericLiteral(val.right)) prop.defaultValue = val.right.value;
      if (_t.isIdentifier(val.left)) prop.source = val.left.name;
    }
    if (_t.isConditionalExpression(val)) {
      prop.required = false;
      var alt = val.alternate;
      if (_t.isStringLiteral(alt) || _t.isNumericLiteral(alt)) prop.defaultValue = alt.value;
    }
    if (_t.isStringLiteral(val) || _t.isNumericLiteral(val)) {
      prop.defaultValue = val.value;
      prop.type = typeof val.value;
    }
    if (_t.isBooleanLiteral(val)) {
      prop.defaultValue = val.value;
      prop.type = "boolean";
    }

    // When a path is available and the value didn't resolve to a
    // direct literal, run _resolveAllValues on the value path. This
    // closes the multi-hop indirection gap (e.g. `var s1 = "X"; s2 = s1;
    // body: {token: s2}` resolves token to "X" via the value chain).
    if (objExprPath && prop.defaultValue === undefined && !_t.isStringLiteral(val) &&
        !_t.isNumericLiteral(val) && !_t.isBooleanLiteral(val)) {
      try {
        var valPath = objExprPath.get("properties." + i + ".value");
        if (valPath && valPath.node) {
          var resolved = _resolveAllValues(valPath, 0);
          if (resolved && resolved.length > 0) {
            var primOnly = [];
            for (var ri = 0; ri < resolved.length; ri++) {
              var rv = resolved[ri];
              if (typeof rv === "string" || typeof rv === "number" || typeof rv === "boolean") {
                primOnly.push(rv);
              }
            }
            if (primOnly.length > 0) {
              prop.defaultValue = primOnly[0];
              if (typeof primOnly[0] === "string") prop.type = "string";
              else if (typeof primOnly[0] === "number") prop.type = "number";
              else if (typeof primOnly[0] === "boolean") prop.type = "boolean";
              if (primOnly.length > 1) prop.validValues = primOnly;
            }
          }
        }
      } catch (e) { _resolver.collectError(e, "extractObjectPropertiesValueResolve"); }
    }

    props.push(prop);
  }
  return props;
}

function _extractTemplateParams(node) {
  if (!_t.isTemplateLiteral(node)) return [];
  var params = [];
  for (var i = 0; i < node.expressions.length; i++) {
    var expr = node.expressions[i];
    if (_t.isIdentifier(expr)) params.push(expr.name);
    else if (_t.isMemberExpression(expr)) {
      var _tmplKey = _memberChainKey(expr);
      if (_tmplKey) params.push(_tmplKey);
    }
    else if (_t.isCallExpression(expr) && expr.arguments.length > 0 && _t.isIdentifier(expr.arguments[0])) {
      params.push(expr.arguments[0].name);
    }
  }
  return params;
}

function _templateToUrl(node) {
  if (!_t.isTemplateLiteral(node)) return null;
  var parts = [];
  for (var i = 0; i < node.quasis.length; i++) {
    parts.push(node.quasis[i].value.raw || node.quasis[i].value.cooked || "");
    if (i < node.expressions.length) {
      var expr = node.expressions[i];
      var name = _t.isIdentifier(expr) ? expr.name : "param" + i;
      parts.push("{" + name + "}");
    }
  }
  return parts.join("");
}

function _extractFuncParams(funcNode) {
  if (!funcNode || !funcNode.params) return null;
  var params = [];
  for (var i = 0; i < funcNode.params.length; i++) {
    var p = funcNode.params[i];
    if (_t.isIdentifier(p)) {
      params.push({ name: p.name, required: true });
    } else if (_t.isAssignmentPattern(p)) {
      var pName = _t.isIdentifier(p.left) ? p.left.name : null;
      var defVal = _t.isStringLiteral(p.right) || _t.isNumericLiteral(p.right) ? p.right.value : undefined;
      if (pName) params.push({ name: pName, required: false, defaultValue: defVal });
    } else if (_t.isObjectPattern(p)) {
      for (var j = 0; j < p.properties.length; j++) {
        var dp = p.properties[j];
        if (_t.isRestElement(dp)) {
          params.push({ name: _t.isIdentifier(dp.argument) ? dp.argument.name : "rest", required: false, rest: true });
        } else if (_t.isObjectProperty(dp)) {
          var dpName = _t.isIdentifier(dp.key) ? dp.key.name : null;
          var dpReq = true, dpDef;
          if (_t.isAssignmentPattern(dp.value)) {
            dpReq = false;
            dpDef = (_t.isStringLiteral(dp.value.right) || _t.isNumericLiteral(dp.value.right)) ? dp.value.right.value : undefined;
          }
          if (dpName) params.push({ name: dpName, required: dpReq, defaultValue: dpDef });
        }
      }
    } else if (_t.isRestElement(p)) {
      params.push({ name: _t.isIdentifier(p.argument) ? p.argument.name : "rest", required: false, rest: true });
    }
  }
  var name = funcNode.id && _t.isIdentifier(funcNode.id) ? funcNode.id.name : null;
  return { name: name, params: params };
}

function _detectResponseParsing(funcPath) {
  var found = null;
  try {
    funcPath.traverse(Object.assign({
      CallExpression: function(innerPath) {
        if (found) { innerPath.stop(); return; }
        var c = innerPath.node.callee;
        if (!_t.isMemberExpression(c)) return;
        var mName = _t.isIdentifier(c.property) ? c.property.name : null;
        if (mName === "json") found = "json";
        else if (mName === "arrayBuffer" && !found) found = "arrayBuffer";
        else if (mName === "blob" && !found) found = "blob";
      },
    }, _SKIP_NESTED_FUNCS));
  } catch (e) { _resolver.collectError(e, "detectResponseParsing"); }
  return found;
}

// ─── Value Constraint Collection ────────────────────────────────────────────

// Identify which function parameter an expression traces to.
// Returns { funcPath, paramIdx, propName, defaultValue } or null.
// Handles: identifier params, member expressions (param.prop), LogicalExpression defaults (param.prop || "default")
function _identifyParamSource(node, path) {
  // Unwrap LogicalExpression: n.method || "get" → n.method with default "get"
  var defaultValue = null;
  var inner = node;
  if (_t.isLogicalExpression(node) && node.operator === "||") {
    if (_t.isStringLiteral(node.right)) { defaultValue = node.right.value; inner = node.left; }
    else if (_t.isStringLiteral(node.left)) { defaultValue = node.left.value; inner = node.right; }
  }
  // MemberExpression: param.prop
  if (_t.isMemberExpression(inner) && !inner.computed && _t.isIdentifier(inner.object)) {
    var binding = path.scope.getBinding(inner.object.name);
    if (binding && binding.kind === "param") {
      var funcPath = binding.scope.path;
      var pIdx = _findParamIndex(funcPath.node.params, inner.object.name);
      if (pIdx >= 0) {
        var propName = _t.isIdentifier(inner.property) ? inner.property.name : null;
        return { funcPath: funcPath, paramIdx: pIdx, propName: propName, defaultValue: defaultValue };
      }
    }
  }
  // Plain identifier: param
  if (_t.isIdentifier(inner)) {
    var binding2 = path.scope.getBinding(inner.name);
    if (binding2 && binding2.kind === "param") {
      var funcPath2 = binding2.scope.path;
      var pIdx2 = -1;
      for (var j = 0; j < funcPath2.node.params.length; j++) {
        var p2 = funcPath2.node.params[j];
        if (_t.isIdentifier(p2) && p2.name === inner.name) { pIdx2 = j; break; }
      }
      if (pIdx2 >= 0) return { funcPath: funcPath2, paramIdx: pIdx2, propName: null, defaultValue: defaultValue };
    }
  }
  return null;
}

// Find all caller argument arrays for a function.
// Returns array of arrays of paths: [[arg0Path, arg1Path, ...], ...]
function _findFunctionCallerArgs(funcPath) {
  var results = [];
  var funcBinding = null;
  if (funcPath.node.id) {
    funcBinding = funcPath.scope.parent ? funcPath.scope.parent.getBinding(funcPath.node.id.name) : null;
    if (!funcBinding) funcBinding = funcPath.scope.getBinding(funcPath.node.id.name);
  }
  if (!funcBinding && _t.isVariableDeclarator(funcPath.parent))
    funcBinding = funcPath.scope.parent ? funcPath.scope.parent.getBinding(funcPath.parent.id.name) : null;
  // Handle assignment chains: const X = Y = function(){} — walk up AssignmentExpressions to VariableDeclarator
  if (!funcBinding && _t.isAssignmentExpression(funcPath.parent)) {
    var _chain = funcPath.parentPath;
    while (_chain && _t.isAssignmentExpression(_chain.node)) _chain = _chain.parentPath;
    if (_chain && _t.isVariableDeclarator(_chain.node) && _t.isIdentifier(_chain.node.id)) {
      funcBinding = _chain.scope.getBinding(_chain.node.id.name);
    }
    // Also try the direct assignment target: Y = function(){} → get binding for Y
    if (!funcBinding && _t.isIdentifier(funcPath.parent.left)) {
      funcBinding = funcPath.parentPath.scope.getBinding(funcPath.parent.left.name);
    }
    // Prototype method: Ctor.prototype.method = function(params) { ... }
    // Find callers: instance.method(args) where instance = new Ctor()
    if (!funcBinding && _t.isMemberExpression(funcPath.parent.left)) {
      var protoLeft = funcPath.parent.left;
      // Match X.prototype.Y or X.prototype["Y"]
      if (_t.isMemberExpression(protoLeft.object) && !protoLeft.object.computed &&
          _t.isIdentifier(protoLeft.object.property, { name: "prototype" }) &&
          _t.isIdentifier(protoLeft.object.object)) {
        var protoCtorName2 = protoLeft.object.object.name;
        var protoMethodName = _t.isIdentifier(protoLeft.property) ? protoLeft.property.name :
          (_t.isStringLiteral(protoLeft.property) ? protoLeft.property.value : null);
        if (protoCtorName2 && protoMethodName) {
          var protoResults = _findPrototypeMethodCallerArgs(funcPath, protoCtorName2, protoMethodName);
          if (protoResults.length > 0) return protoResults;
        }
      }
    }
    // Indexed-property assignment: `obj[K] = function(params) { ... }` or
    // `obj.K = function(params) { ... }`. Callers reach the function via
    // `obj[K](args)` / `obj.K(args)` / `obj[K].call(thisArg, args)`.
    // Walk obj's referencePaths for matching member-access calls.
    if (!funcBinding && _t.isMemberExpression(funcPath.parent.left)) {
      var ipLeft = funcPath.parent.left;
      if (_t.isIdentifier(ipLeft.object)) {
        var ipKey = null;
        if (!ipLeft.computed && _t.isIdentifier(ipLeft.property)) ipKey = ipLeft.property.name;
        else if (ipLeft.computed) {
          if (_t.isStringLiteral(ipLeft.property)) ipKey = ipLeft.property.value;
          else if (_t.isNumericLiteral(ipLeft.property)) ipKey = String(ipLeft.property.value);
        }
        if (ipKey != null) {
          var ipObjBinding = funcPath.parentPath.scope.getBinding(ipLeft.object.name);
          if (ipObjBinding && ipObjBinding.referencePaths) {
            // Collect references for this object plus any global aliases.
            // `var ce = {}; ce.post = fn; window.$ = ce;` — `$.post(args)`
            // reaches ce.post but `$` is a separate global. Walk
            // _globalAssignments for any global whose value is this
            // identifier; pull its references from the pre-pass index
            // (no program-traverse-during-traverse).
            var ipAllRefs = ipObjBinding.referencePaths.slice();
            if (_unboundIdRefs) {
              for (var ipGn in _globalAssignments) {
                var ipGa = _globalAssignments[ipGn];
                if (!ipGa || !ipGa.valueNode) continue;
                var ipGv = ipGa.valueNode;
                while (_t.isAssignmentExpression(ipGv)) ipGv = ipGv.right;
                if (_t.isIdentifier(ipGv) && ipGv.name === ipLeft.object.name) {
                  var aliasRefs = _unboundIdRefs[ipGn];
                  if (aliasRefs) {
                    for (var ari2 = 0; ari2 < aliasRefs.length; ari2++) ipAllRefs.push(aliasRefs[ari2]);
                  }
                }
              }
            }
            var ipResults = [];
            for (var iri = 0; iri < ipAllRefs.length; iri++) {
              var iRef = ipAllRefs[iri];
              var iMem = iRef.parentPath;
              if (!iMem || !iMem.isMemberExpression() || iMem.node.object !== iRef.node) continue;
              var iMatch = (!iMem.node.computed && _t.isIdentifier(iMem.node.property, { name: ipKey })) ||
                (iMem.node.computed && _t.isStringLiteral(iMem.node.property) && iMem.node.property.value === ipKey) ||
                (iMem.node.computed && _t.isNumericLiteral(iMem.node.property) && String(iMem.node.property.value) === ipKey);
              if (!iMatch) continue;
              // obj.K(args) — direct call
              var iCall = iMem.parentPath;
              if (iCall && iCall.isCallExpression() && iCall.node.callee === iMem.node) {
                var iArgs = [];
                for (var iai = 0; iai < iCall.node.arguments.length; iai++) iArgs.push(iCall.get("arguments." + iai));
                ipResults.push(iArgs);
                continue;
              }
              // obj.K.call(thisArg, args) — Function.prototype.call form
              if (iCall && iCall.isMemberExpression() && iCall.node.object === iMem.node &&
                  !iCall.node.computed && _t.isIdentifier(iCall.node.property, { name: "call" })) {
                var iCall2 = iCall.parentPath;
                if (iCall2 && iCall2.isCallExpression() && iCall2.node.callee === iCall.node) {
                  var iArgs2 = [];
                  // Skip thisArg (arg[0]); shift remaining into params
                  for (var iaj = 1; iaj < iCall2.node.arguments.length; iaj++) iArgs2.push(iCall2.get("arguments." + iaj));
                  ipResults.push(iArgs2);
                }
              }
            }
            if (ipResults.length > 0) return ipResults;
          }
        }
      }
    }
  }
  if (funcBinding && funcBinding.referencePaths) {
    for (var i = 0; i < funcBinding.referencePaths.length; i++) {
      var ref = funcBinding.referencePaths[i];
      if (_t.isCallExpression(ref.parent) && ref.parent.callee === ref.node) {
        var argPaths = [];
        for (var j = 0; j < ref.parent.arguments.length; j++) argPaths.push(ref.parentPath.get("arguments." + j));
        results.push(argPaths);
      }
      // Function.prototype.bind: `f.bind(thisArg, …preArgs)` returns a
      // function. Effective callee args at runtime = preArgs prepended
      // to whatever the bound function is called with. Find:
      //   var alias = f.bind(null, X, Y);
      //   alias(Z, W);          // → f(X, Y, Z, W)
      //   obj.k = f.bind(null, X);
      //   obj.k(Z);             // → f(X, Z)
      // Walk one .bind hop and collect aggregated arg lists for the
      // bound aliases' call sites.
      if (_t.isMemberExpression(ref.parent) && ref.parent.object === ref.node && !ref.parent.computed &&
          _t.isIdentifier(ref.parent.property, { name: "bind" })) {
        var bindCall = ref.parentPath ? ref.parentPath.parent : null;
        var bindCallPath = ref.parentPath ? ref.parentPath.parentPath : null;
        if (bindCall && bindCallPath && _t.isCallExpression(bindCall) && bindCall.callee === ref.parent &&
            bindCall.arguments.length >= 1) {
          // Pre-args (everything after thisArg).
          var preArgs = [];
          for (var bi = 1; bi < bindCall.arguments.length; bi++) preArgs.push(bindCallPath.get("arguments." + bi));
          // Find what the bind result is assigned to.
          var bindParent = bindCallPath.parent;
          if (bindParent && _t.isVariableDeclarator(bindParent) && _t.isIdentifier(bindParent.id) &&
              bindParent.init === bindCall) {
            var aliasBinding = bindCallPath.parentPath.scope.getBinding(bindParent.id.name);
            if (aliasBinding && aliasBinding.referencePaths) {
              for (var ari = 0; ari < aliasBinding.referencePaths.length; ari++) {
                var aRef = aliasBinding.referencePaths[ari];
                if (_t.isCallExpression(aRef.parent) && aRef.parent.callee === aRef.node) {
                  var combined = preArgs.slice();
                  for (var aci = 0; aci < aRef.parent.arguments.length; aci++) {
                    combined.push(aRef.parentPath.get("arguments." + aci));
                  }
                  results.push(combined);
                }
              }
            }
          } else if (bindParent && _t.isAssignmentExpression(bindParent) && bindParent.right === bindCall &&
                     _t.isMemberExpression(bindParent.left) && _t.isIdentifier(bindParent.left.object)) {
            // obj.k = f.bind(...) — find obj.k call sites.
            var asgnLeft = bindParent.left;
            var asgnObj = asgnLeft.object;
            var asgnKey = null;
            if (!asgnLeft.computed && _t.isIdentifier(asgnLeft.property)) asgnKey = asgnLeft.property.name;
            else if (asgnLeft.computed) {
              if (_t.isStringLiteral(asgnLeft.property)) asgnKey = asgnLeft.property.value;
              else if (_t.isNumericLiteral(asgnLeft.property)) asgnKey = String(asgnLeft.property.value);
            }
            var asgnObjBinding = bindCallPath.parentPath.scope.getBinding(asgnObj.name);
            if (asgnKey != null && asgnObjBinding && asgnObjBinding.referencePaths) {
              for (var aori = 0; aori < asgnObjBinding.referencePaths.length; aori++) {
                var aoRef = asgnObjBinding.referencePaths[aori];
                var aoMem = aoRef.parentPath;
                if (!aoMem || !aoMem.isMemberExpression() || aoMem.node.object !== aoRef.node) continue;
                var aoMatch = (!aoMem.node.computed && _t.isIdentifier(aoMem.node.property, { name: asgnKey })) ||
                  (aoMem.node.computed && _t.isStringLiteral(aoMem.node.property) && aoMem.node.property.value === asgnKey) ||
                  (aoMem.node.computed && _t.isNumericLiteral(aoMem.node.property) && String(aoMem.node.property.value) === asgnKey);
                if (!aoMatch) continue;
                var aoCall = aoMem.parentPath;
                if (aoCall && aoCall.isCallExpression() && aoCall.node.callee === aoMem.node) {
                  var combined2 = preArgs.slice();
                  for (var aoaci = 0; aoaci < aoCall.node.arguments.length; aoaci++) {
                    combined2.push(aoCall.get("arguments." + aoaci));
                  }
                  results.push(combined2);
                }
              }
            }
          }
        }
      }
    }
    return results;
  }
  // Object-literal method: `var u = {m(e){}}; u.m(tainted);` or
  // `{m: function(e){}}; `. The function itself has no named binding —
  // it's reached via `u.m(args)`. Walk up to the ObjectExpression, find
  // its VariableDeclarator binding, then filter ref usages to calls
  // through `obj.methodName`.
  if (!funcBinding) {
    var objMethodName = null;
    var objExprPath2 = null;
    if (funcPath.isObjectMethod && funcPath.isObjectMethod()) {
      var omK = funcPath.node.key;
      if (!funcPath.node.computed) {
        if (_t.isIdentifier(omK)) objMethodName = omK.name;
        else if (_t.isStringLiteral(omK)) objMethodName = omK.value;
      }
      objExprPath2 = funcPath.parentPath;
    } else if (funcPath.parentPath && _t.isObjectProperty(funcPath.parent)) {
      var opK = funcPath.parent.key;
      if (!funcPath.parent.computed) {
        if (_t.isIdentifier(opK)) objMethodName = opK.name;
        else if (_t.isStringLiteral(opK)) objMethodName = opK.value;
      }
      objExprPath2 = funcPath.parentPath.parentPath;
    }
    if (objMethodName && objExprPath2 && objExprPath2.isObjectExpression() &&
        objExprPath2.parentPath && _t.isVariableDeclarator(objExprPath2.parentPath.node) &&
        _t.isIdentifier(objExprPath2.parentPath.node.id)) {
      var objName = objExprPath2.parentPath.node.id.name;
      var objBind = objExprPath2.scope.getBinding(objName);
      if (objBind && objBind.referencePaths) {
        for (var omi = 0; omi < objBind.referencePaths.length; omi++) {
          var oref = objBind.referencePaths[omi];
          var mem = oref.parentPath;
          if (!mem || !mem.isMemberExpression() || mem.node.object !== oref.node || mem.node.computed) continue;
          var accessName = _t.isIdentifier(mem.node.property) ? mem.node.property.name : null;
          if (accessName !== objMethodName) continue;
          var callExpr = mem.parentPath;
          if (!callExpr || !callExpr.isCallExpression() || callExpr.node.callee !== mem.node) continue;
          var omArgPaths = [];
          for (var omj = 0; omj < callExpr.node.arguments.length; omj++) {
            omArgPaths.push(callExpr.get("arguments." + omj));
          }
          results.push(omArgPaths);
        }
        if (results.length > 0) return results;
      }
    }
  }
  // Function passed as argument: makeHandler(function(data) { sink(data); })
  // If our function is an argument to a call, find the corresponding parameter in the
  // called function, then find all call sites of that parameter within the function body.
  if (!funcBinding && funcPath.parentPath && _t.isCallExpression(funcPath.parent) &&
      funcPath.parent.callee !== funcPath.node) {
    var _fpaCallNode = funcPath.parent;
    var _fpaArgIdx = -1;
    for (var _fpai = 0; _fpai < _fpaCallNode.arguments.length; _fpai++) {
      if (_fpaCallNode.arguments[_fpai] === funcPath.node) { _fpaArgIdx = _fpai; break; }
    }
    if (_fpaArgIdx >= 0) {
      // Resolve the called function
      var _fpaCallee = _fpaCallNode.callee;
      var _fpaCalleeFn = null;
      var _fpaCalleePath = null;
      if (_t.isIdentifier(_fpaCallee)) {
        var _fpaBind = funcPath.scope.getBinding(_fpaCallee.name);
        if (_fpaBind) {
          if (_t.isFunctionDeclaration(_fpaBind.path.node)) { _fpaCalleeFn = _fpaBind.path.node; _fpaCalleePath = _fpaBind.path; }
          else if (_fpaBind.path.isVariableDeclarator() && _fpaBind.path.node.init && _t.isFunction(_fpaBind.path.node.init)) {
            _fpaCalleeFn = _fpaBind.path.node.init; _fpaCalleePath = _fpaBind.path.get("init");
          }
        }
      } else if (_t.isFunctionExpression(_fpaCallee) || _t.isArrowFunctionExpression(_fpaCallee)) {
        _fpaCalleeFn = _fpaCallee; _fpaCalleePath = funcPath.parentPath.get("callee");
      }
      if (_fpaCalleeFn && _fpaCalleeFn.params && _fpaArgIdx < _fpaCalleeFn.params.length &&
          _t.isIdentifier(_fpaCalleeFn.params[_fpaArgIdx]) && _fpaCalleePath) {
        var _fpaParamName = _fpaCalleeFn.params[_fpaArgIdx].name;
        // Find all call sites of this parameter within the enclosing function body
        try {
          _fpaCalleePath.traverse({
            CallExpression: function(callP) {
              if (_t.isIdentifier(callP.node.callee, { name: _fpaParamName }) &&
                  callP.scope.getBinding(_fpaParamName) &&
                  callP.scope.getBinding(_fpaParamName).scope === _fpaCalleePath.scope) {
                var _fpaArgPaths = [];
                for (var _fpaj = 0; _fpaj < callP.node.arguments.length; _fpaj++) _fpaArgPaths.push(callP.get("arguments." + _fpaj));
                results.push(_fpaArgPaths);
              }
            }
          });
        } catch (e) { _resolver.collectError(e, "funcParamCallSites"); }
        if (results.length > 0) return results;
      }
    }
  }

  // IIFE return / UMD: function returned from enclosing function
  if (_t.isReturnStatement(funcPath.parent)) {
    var encFunc = funcPath.findParent(function(p) { return p.isFunction() && p !== funcPath; });
    if (encFunc) {
      var encParent = encFunc.parentPath;
      // UMD: encFunc is arg to outer IIFE
      if (encParent && _t.isCallExpression(encParent.node) && encParent.node.callee !== encFunc.node) {
        var outerCallee = encParent.node.callee;
        if (_t.isUnaryExpression(outerCallee)) outerCallee = outerCallee.argument;
        var argIdx = -1;
        for (var ai = 0; ai < encParent.node.arguments.length; ai++) {
          if (encParent.node.arguments[ai] === encFunc.node) { argIdx = ai; break; }
        }
        if (argIdx >= 0 && (_t.isFunctionExpression(outerCallee) || _t.isArrowFunctionExpression(outerCallee)) &&
            argIdx < outerCallee.params.length) {
          var factoryName = _t.isIdentifier(outerCallee.params[argIdx]) ? outerCallee.params[argIdx].name : null;
          if (factoryName) {
            // Find global that calls the factory: (win).X = factory()
            for (var gn in _globalAssignments) {
              var ga = _globalAssignments[gn];
              if (ga.valueNode && _t.isCallExpression(ga.valueNode) &&
                  _t.isIdentifier(ga.valueNode.callee) && ga.valueNode.callee.name === factoryName) {
                // Find all global callers: X(url, opts)
                return _collectGlobalCallerArgArrays(funcPath, gn);
              }
            }
          }
        }
      }
    }
  }
  return results;
}

// Find callers of prototype methods: instance.method(args) where instance = new Ctor()
function _findPrototypeMethodCallerArgs(funcPath, ctorName, methodName) {
  // Memo per (funcPath.node, ctorName, methodName). Same triple returns
  // the same call-site set within one analysis.
  var memoKey = ctorName + "|" + methodName;
  var fnMemo = _findPrototypeMethodCallerArgsMemo.get(funcPath.node);
  if (fnMemo && memoKey in fnMemo) return fnMemo[memoKey];
  var results = [];
  // Cache: ctorName instances found via full-program traversal
  var instCacheKey = "inst:" + ctorName;
  var instanceEntries = _globalCallerCache[instCacheKey];
  if (!instanceEntries) {
    instanceEntries = [];
    var programPath = funcPath.findParent(function(p) { return p.isProgram(); });
    if (!programPath) return results;
    try {
      programPath.traverse({
        VariableDeclarator: function(decPath) {
          var init = decPath.node.init;
          if (!init || !_t.isNewExpression(init) || !_t.isIdentifier(init.callee, { name: ctorName })) return;
          var ib = decPath.scope.getBinding(decPath.node.id.name);
          if (ib) instanceEntries.push(ib);
        },
      });
    } catch(e) { _resolver.collectError(e, "protoInstanceCache"); }
    _globalCallerCache[instCacheKey] = instanceEntries;
  }
  for (var _iei = 0; _iei < instanceEntries.length; _iei++) {
    var instanceBinding = instanceEntries[_iei];
    if (!instanceBinding.referencePaths) continue;
    for (var ri = 0; ri < instanceBinding.referencePaths.length; ri++) {
      var ref = instanceBinding.referencePaths[ri];
      if (_t.isMemberExpression(ref.parent) && !ref.parent.computed &&
          ref.parent.object === ref.node &&
          _t.isIdentifier(ref.parent.property, { name: methodName }) &&
          _t.isCallExpression(ref.parentPath.parent) &&
          ref.parentPath.parent.callee === ref.parent) {
        var callNode = ref.parentPath.parent;
        var callPath2 = ref.parentPath.parentPath;
        var argPaths = [];
        for (var ai = 0; ai < callNode.arguments.length; ai++) argPaths.push(callPath2.get("arguments." + ai));
        results.push(argPaths);
      }
    }
  }
  if (!fnMemo) { fnMemo = {}; _findPrototypeMethodCallerArgsMemo.set(funcPath.node, fnMemo); }
  fnMemo[memoKey] = results;
  return results;
}

// Collect caller argument arrays for global function calls
function _collectGlobalCallerArgArrays(funcPath, globalName) {
  var results = [];
  _traverseGlobalCallers(funcPath, globalName, null, function(callPath) {
    var argPaths = [];
    for (var j = 0; j < callPath.node.arguments.length; j++) argPaths.push(callPath.get("arguments." + j));
    results.push(argPaths);
  });
  return results;
}

// Structural member chain key: walks MemberExpression chain to produce a
// deterministic key like "options.type". Returns null for computed or complex expressions.
// Iterative MemberExpression chain key builder — walks property chain
// leaf→root collecting names, then joins. Avoids recursion on deeply
// chained `a.b.c.d.e...` patterns common in minified module-table reads.
function _memberChainKey(node) {
  if (_t.isIdentifier(node)) return node.name;
  if (!_t.isMemberExpression(node) || node.computed) return null;
  var parts = [];
  var cur = node;
  while (_t.isMemberExpression(cur) && !cur.computed) {
    var prop = _t.isIdentifier(cur.property) ? cur.property.name :
      (_t.isStringLiteral(cur.property) ? cur.property.value : null);
    if (!prop) return null;
    parts.push(prop);
    cur = cur.object;
  }
  if (!_t.isIdentifier(cur)) return null;
  parts.push(cur.name);
  parts.reverse();
  return parts.join(".");
}

function _addConstraint(path, varName, values, source) {
  if (!varName || !values || values.length === 0) return;
  var meaningful = values.filter(function(v) {
    if (v === true || v === false || v === null || v === undefined) return false;
    if (typeof v === "string" && v.length === 0) return false;
    return true;
  });
  if (meaningful.length === 0) return;

  var scopeUid = path.scope.uid;
  var key = scopeUid + ":" + varName;
  if (!_constraints[key]) _constraints[key] = { varName: varName, values: new Set(), sources: [] };
  for (var i = 0; i < meaningful.length; i++) _constraints[key].values.add(meaningful[i]);
  _constraints[key].sources.push(source);
}

function _getConstraint(path, varName) {
  var scope = path.scope;
  while (scope) {
    var key = scope.uid + ":" + varName;
    if (_constraints[key]) return _constraints[key];
    scope = scope.parent;
  }
  return null;
}

function _collectSwitchConstraints(path) {
  var disc = path.node.discriminant;
  var varName = _t.isIdentifier(disc) ? disc.name :
    (_t.isMemberExpression(disc) ? _memberChainKey(disc) : null);
  if (!varName) return;

  var values = [];
  var cases = path.node.cases;
  for (var i = 0; i < cases.length; i++) {
    var test = cases[i].test;
    if (!test) continue;
    if (_t.isStringLiteral(test) || _t.isNumericLiteral(test)) values.push(test.value);
  }
  if (values.length >= 1) {
    _addConstraint(path, varName, values, "switch");
  }
}

function _collectIncludesConstraints(path) {
  var node = path.node;
  if (!_t.isMemberExpression(node.callee)) return;
  if (!_t.isIdentifier(node.callee.property, { name: "includes" })) return;
  if (node.arguments.length < 1) return;

  var testedArg = node.arguments[0];
  var testedVar = _t.isIdentifier(testedArg) ? testedArg.name :
    (_t.isMemberExpression(testedArg) ? _memberChainKey(testedArg) : null);
  if (!testedVar) return;

  var obj = node.callee.object;

  // Inline array: ["json", "xml"].includes(format)
  if (_t.isArrayExpression(obj)) {
    var values = _extractLiteralArray(obj);
    if (values.length >= 1) _addConstraint(path, testedVar, values, "includes_inline");
    return;
  }

  // Named array: FORMATS.includes(type) — resolve through scope
  if (_t.isIdentifier(obj)) {
    var binding = path.scope.getBinding(obj.name);
    if (binding && binding.path.node.init && _t.isArrayExpression(binding.path.node.init)) {
      var arrValues = _extractLiteralArray(binding.path.node.init);
      if (arrValues.length >= 1) _addConstraint(path, testedVar, arrValues, "includes_ref");
    }
  }
}

function _collectEqualityConstraints(path) {
  var node = path.node;
  if (node.operator !== "||" && node.operator !== "&&") return;

  var comparisons = [];
  _flattenLogicalChain(node, comparisons);

  var byVar = {};
  for (var i = 0; i < comparisons.length; i++) {
    var c = comparisons[i];
    if (!byVar[c.varName]) byVar[c.varName] = [];
    byVar[c.varName].push(c.value);
  }

  for (var varName in byVar) {
    if (byVar[varName].length >= 1) {
      _addConstraint(path, varName, byVar[varName], "equality_chain");
    }
  }
}

// Detect iteration constraints from spec-defined Array iterators per
// ECMA-262 § 23.1.3.15 / § 23.1.3.21 — `arr.forEach(fn)` / `arr.map(fn)`.
// The callback parameter is constrained to the array's element values.
// The jQuery / underscore / lodash `X.each(arr, fn)` shape was removed
// per CLAUDE.md L29 — those library helpers reach the analyser when
// their bundle is present and the analyser traces through their own
// forEach-equivalent call.
function _collectIterationConstraints(path) {
  var node = path.node;
  if (!_t.isMemberExpression(node.callee)) return;
  var methodName = _t.isIdentifier(node.callee.property) ? node.callee.property.name : null;
  if (!methodName) return;

  var arrNode = null, callbackNode = null;

  if ((methodName === "forEach" || methodName === "map") && node.arguments.length >= 1) {
    var _icObjType = _getTrackedType(path.get("callee.object"), node.callee.object);
    if (_icObjType && _NON_ITERABLE_TYPES[_icObjType]) return;
    var arrPath = path.get("callee.object");
    arrNode = _resolveToArray(arrPath, 0);
    callbackNode = node.arguments[0];
  }

  if (!arrNode || !callbackNode) return;
  if (!_t.isFunctionExpression(callbackNode) && !_t.isArrowFunctionExpression(callbackNode)) return;

  // Extract array element values
  var elemValues = [];
  for (var ei = 0; ei < arrNode.elements.length; ei++) {
    var elem = arrNode.elements[ei];
    if (_t.isStringLiteral(elem) || _t.isNumericLiteral(elem)) elemValues.push(elem.value);
  }
  if (elemValues.length < 1) return;

  // forEach / map per ECMA § 23.1.3.15 / § 23.1.3.21 invoke the callback
  // with (element, index, array) — element is param 0.
  var elemParamIdx = 0;
  if (callbackNode.params.length <= elemParamIdx) return;
  var elemParam = callbackNode.params[elemParamIdx];
  var elemParamName = _t.isIdentifier(elemParam) ? elemParam.name : null;
  if (elemParamName) {
    _addConstraint(path, elemParamName, elemValues, "iteration");
  }
}

// Detect constraints from object literal structure:
// 1. Array properties: {statusCodes: [408,413,429,...], methods: ["get","put",...]} → emit array values
// 2. String-valued objects: {json: "application/json", text: "text/*"} → emit string values
function _collectObjectLiteralConstraints(path) {
  var node = path.node;
  if (!node.properties || node.properties.length < 1) return;

  var stringVals = [];
  for (var i = 0; i < node.properties.length; i++) {
    var prop = node.properties[i];
    if (!_t.isObjectProperty(prop) && !(_t.isProperty && _t.isProperty(prop))) continue;
    var keyName = _t.isIdentifier(prop.key) ? prop.key.name :
      (_t.isStringLiteral(prop.key) ? prop.key.value : null);
    if (!keyName) continue;
    var val = prop.value;

    // Array property: {methods: ["get","put",...], statusCodes: [408,...]}
    if (_t.isArrayExpression(val)) {
      var arrVals = _extractLiteralArray(val);
      if (arrVals.length >= 1) {
        _addConstraint(path, keyName, arrVals, "object_array_prop");
      }
    }

    // Collect string values for the object-values constraint
    if (_t.isStringLiteral(val)) stringVals.push(val.value);
  }

  // Object with 3+ string literal values: {json:"application/json", text:"text/*",...}
  if (stringVals.length >= 1) {
    // Use the variable name if available, else a generic key
    var objVarName = null;
    if (path.parent && _t.isVariableDeclarator(path.parent) && _t.isIdentifier(path.parent.id)) {
      objVarName = path.parent.id.name;
    } else if (path.parent && _t.isAssignmentExpression(path.parent) && _t.isIdentifier(path.parent.left)) {
      objVarName = path.parent.left.name;
    }
    if (objVarName) {
      _addConstraint(path, objVarName, stringVals, "object_string_values");
    }
  }
}

function _flattenLogicalChain(node, out) {
  // Iterative: walk LogicalExpression chains via explicit stack
  var stack = [node];
  while (stack.length > 0) {
    var n = stack.pop();
    if (_t.isLogicalExpression(n)) {
      stack.push(n.left, n.right);
      continue;
    }
    if (_t.isBinaryExpression(n) &&
        (n.operator === "===" || n.operator === "==" || n.operator === "!==" || n.operator === "!=")) {
      var varName = null, value = null;
      if (_t.isIdentifier(n.left) && (_t.isStringLiteral(n.right) || _t.isNumericLiteral(n.right))) {
        varName = n.left.name; value = n.right.value;
      } else if (_t.isIdentifier(n.right) && (_t.isStringLiteral(n.left) || _t.isNumericLiteral(n.left))) {
        varName = n.right.name; value = n.left.value;
      } else if (_t.isMemberExpression(n.left) && (_t.isStringLiteral(n.right) || _t.isNumericLiteral(n.right))) {
        varName = _memberChainKey(n.left); value = n.right.value;
      } else if (_t.isMemberExpression(n.right) && (_t.isStringLiteral(n.left) || _t.isNumericLiteral(n.left))) {
        varName = _memberChainKey(n.right); value = n.left.value;
      }
      if (varName !== null && value !== null) out.push({ varName: varName, value: value });
    }
  }
}

// ─── Security Analysis: Code Context Extraction ─────────────────────────────

// Extract a line range from the source code by line numbers (1-based).
// Returns trimmed lines joined by newline, each capped at 120 chars.
// Cut a ~140-char window centred on (line, column) from _sourceCode,
// whitespace-collapsed and elided with "…" on both sides when truncated.
// Attached to each taint-path hop so UI consumers (popup, harness) can
// display inline code at every step without needing to re-fetch and
// seek the source. Minified bundles have one giant line each — the
// column is what matters, not the line number.
function _snippetAtLoc(loc) {
  if (!_sourceCode || !loc) return null;
  if (!_sourceLines) _sourceLines = _sourceCode.split("\n");
  var lineIdx = (loc.line || 1) - 1;
  if (lineIdx < 0 || lineIdx >= _sourceLines.length) return null;
  // Compute absolute offset for this (line, column) so we can slice
  // across newlines cleanly without special-casing.
  var offset = 0;
  for (var i = 0; i < lineIdx; i++) offset += _sourceLines[i].length + 1;
  offset += (loc.column || 0);
  if (offset < 0 || offset >= _sourceCode.length) return null;
  var start = Math.max(0, offset - 70);
  var end = Math.min(_sourceCode.length, offset + 70);
  var snip = _sourceCode.slice(start, end).replace(/\s+/g, " ");
  if (start > 0) snip = "\u2026" + snip;
  if (end < _sourceCode.length) snip = snip + "\u2026";
  return snip;
}

function _extractLines(fromLine, toLine, column) {
  if (!_sourceCode || !fromLine || !toLine) return null;
  if (!_sourceLines) {
    _sourceLines = _sourceCode.split("\n");
  }
  var start = Math.max(0, fromLine - 1);
  var end = Math.min(_sourceLines.length, toLine);
  var out = [];
  for (var i = start; i < end; i++) {
    var line = _sourceLines[i].trim();
    if (line.length > 120) {
      // For very long lines (minified code), extract around the column of interest
      // Bias toward showing more code after the sink (the relevant part)
      if (column != null && column > 20) {
        var from = Math.max(0, column - 20);
        var to = Math.min(line.length, column + 100);
        line = (from > 0 ? "\u2026" : "") + line.substring(from, to) + (to < line.length ? "\u2026" : "");
      } else {
        line = line.substring(0, 120) + "\u2026";
      }
    }
    if (line) out.push(line);
  }
  return out.length > 0 ? out.join("\n") : null;
}

// Build source-to-sink code context.
// sinkNode: the AST node of the dangerous sink (always present)
// valueSource: the taint trace result { sourceType, source, sourceLoc? }
// Returns a multi-line string showing the data flow.
function _extractCodeContext(sinkNode, valueSource) {
  if (!_sourceCode || !sinkNode || !sinkNode.loc) return null;
  var sinkLine = sinkNode.loc.start.line;
  var sinkCol = sinkNode.loc.start.column;

  // If we have a source location on a different line, show source→sink range
  if (valueSource && valueSource.sourceLoc && valueSource.sourceLoc.line !== sinkLine) {
    var srcLine = valueSource.sourceLoc.line;
    var srcCol = valueSource.sourceLoc.column;
    var fromLine = Math.min(srcLine, sinkLine);
    var toLine = Math.max(srcLine, sinkLine);
    // Cap at 10 lines — if the flow spans more, show source + sink with gap
    if (toLine - fromLine + 1 <= 10) {
      return _extractLines(fromLine, toLine, sinkCol);
    }
    // Too far apart — show source line, ellipsis, sink line
    var srcText = _extractLines(srcLine, srcLine, srcCol);
    var sinkText = _extractLines(sinkLine, sinkLine, sinkCol);
    if (srcText && sinkText) return srcText + "\n  \u2026\n" + sinkText;
    return sinkText;
  }

  // No source location or same line — show the sink line only
  return _extractLines(sinkLine, sinkLine, sinkCol);
}

var _sourceLines = null; // lazily split from _sourceCode

// ─── Security Analysis: Taint Source Tracking ───────────────────────────────

// Structural taint source patterns — matched via AST nodes, not strings.
// Each pattern: { obj: base object name, props: { propName: 1, ... } }
// Roots: these object names must be unbound globals (verified via scope).
// "window" and "self" prefixes are normalized (stripped) before matching.
// Taint sources the attacker can influence:
// - location.hash/search/href/pathname: attacker controls these by choosing
//   the link the victim clicks or the URL they navigate to.
// - location.hostname/origin/protocol: server-controlled (scheme+host+port is
//   fixed per page). Treating them as user-controlled produced widespread
//   FPs (e.g. `new URL(x, location.origin)` for same-origin fetch).
// - document.title/domain: writable by page JS, but their source is the page
//   itself — not directly an attacker vector unless the title has already
//   been set from a tainted source (which the tracer handles transitively).
var _TAINT_PATTERNS = [
  { obj: "location", props: { "hash":1, "search":1, "href":1, "pathname":1 } },
  { obj: "document", props: { "referrer":1, "URL":1, "documentURI":1, "baseURI":1, "URLUnencoded":1, "cookie":1, "domain":1, "title":1 } },
  { obj: "window", props: { "name":1, "location":1 } },
  { obj: "event", props: { "data":1 } },
];

// Properties of `location` that are server-set and NOT attacker-controlled.
// Static access to these (`location.origin`) must not be tainted, but
// computed access (`location[x]`) is still suspicious because `x` may select
// a tainted prop.
var _LOCATION_SAFE_PROPS = { "origin":1, "hostname":1, "host":1, "protocol":1, "port":1, "ancestorOrigins":1 };
// WHATWG URL instance properties — read-only string projections of the
// parsed URL. When `(new URL(input, base)).<prop>` resolves with literal
// input/base, we can extract these directly via Node's URL parser.
var _URL_INSTANCE_PROPS = {
  "href":1, "origin":1, "protocol":1, "username":1, "password":1,
  "host":1, "hostname":1, "port":1, "pathname":1, "search":1, "hash":1,
};

// Scope-aware taint source classification using structural AST matching.
// Replaces _describeNode() + _TAINT_SOURCES string lookup.
// Returns taint source string (e.g., "location.hash") if matched, or null.
function _matchTaintSource(path, node) {
  // Accept both `.` and optional-chained `?.` access — they're
  // semantically equivalent as taint sources; the only difference is
  // short-circuit on nullish.
  if ((!_t.isMemberExpression(node) && !_t.isOptionalMemberExpression(node)) || node.computed) return null;

  // Collect the member chain as AST property names: [root, prop1, prop2, ...]
  var chain = [];
  var current = node;
  while ((_t.isMemberExpression(current) || _t.isOptionalMemberExpression(current)) &&
         !current.computed && _t.isIdentifier(current.property)) {
    chain.unshift(current.property.name);
    current = current.object;
  }
  if (!_t.isIdentifier(current)) return null;
  var rootName = current.name;

  // Verify root identifier is unbound (not shadowed by local binding)
  if (path.scope.getBinding(rootName)) return null;

  // Normalize: strip "window." or "self." prefix if root is the global window/self
  // Canonicalize so `window.location.href`, `self.location.href`, and
  // `location.href` all produce the SAME source name. This removes any
  // need for callers (e.g. `_dimsForSource`) to carry aliases for each
  // alternative root. Without normalization, `window.location` would
  // produce obj="window", prop="location" — a different source name
  // than bare `location`, breaking downstream dim lookups.
  var objName, propName;
  if ((rootName === "window" || rootName === "self") && chain.length >= 2) {
    objName = chain[0];
    propName = chain[1];
  } else if ((rootName === "window" || rootName === "self") && chain.length === 1 && chain[0] === "location") {
    // `window.location` or `self.location` (no further property) — same
    // runtime object as bare `location`; canonicalize source name.
    return "location";
  } else if (chain.length >= 1) {
    objName = rootName;
    propName = chain[0];
  } else {
    return null;
  }

  // Match against patterns
  for (var pi = 0; pi < _TAINT_PATTERNS.length; pi++) {
    var pat = _TAINT_PATTERNS[pi];
    if (pat.obj === objName && pat.props[propName]) {
      return objName + "." + propName;
    }
  }
  return null;
}

// Lightweight taint tracker: classifies where a value originates.
// Returns { sourceType: "user-controlled"|"dynamic"|"literal", source: string|null,
//          sourceLoc?: { line, column }, taintPath?: [{ kind, desc, at }],
//          dimensions?: { origin, path, query, hash, content } }
// taintPath records each hop from the sink back to the taint source — e.g.
//   [ {kind:"source",       desc:"location.hash", at:{line,column}},
//     {kind:"method-call",  desc:".split",        at:{line,column}},
//     {kind:"binding",      desc:"parts",         at:{line,column}},
//     {kind:"call-arg",     desc:"arg 0",         at:{line,column}} ]
// so a reviewer can judge WHY the classifier thinks a value is user-controlled
// without re-parsing the minified script by hand.
//
// DIMENSIONS — which parts of a URL-like value are attacker-controllable.
// Set at the source (location.hash → hash+content; location.href → all) and
// refined at URL operations (new URL, .origin, .pathname, .search, .hash).
// Sinks ask dimension-specific questions: request-forgery:fetch cares about
// `origin` (attacker redirecting the request), redirect:href cares about any
// URL dim (attacker picks where the user goes), xss:innerHTML cares about
// `content` (string reaches the HTML parser). This distinguishes the common
// `new URL(src, location.href)` FP — location.href contributes origin:true
// via the BASE, but the resulting URL's origin is still the PAGE'S own
// origin (base.origin) unless the input is absolute. When input isn't
// user-controlled, origin-attacker-control can't happen.
//
// sourceLoc records where the taint source was found (for source-to-sink context).
// Uses a visited-node set instead of depth limits to prevent infinite recursion while
// allowing unlimited tracing depth through variable chains, function params, and object properties.
var _taintVisited = null;

// Dimension-set constructors used across the module so the shape stays
// consistent. Omitted properties are treated as false — we never rely on
// boolean-undefined differentiation.
function _tvsDims(o) {
  return {
    origin: !!(o && o.origin),
    path: !!(o && o.path),
    query: !!(o && o.query),
    hash: !!(o && o.hash),
    content: !!(o && o.content),
  };
}
function _tvsDimsAll() { return _tvsDims({ origin: true, path: true, query: true, hash: true, content: true }); }
function _tvsDimsContent() { return _tvsDims({ content: true }); }
function _tvsDimsUnion(a, b) {
  if (!a) return b ? _tvsDims(b) : _tvsDimsContent();
  if (!b) return _tvsDims(a);
  return _tvsDims({
    origin: a.origin || b.origin,
    path: a.path || b.path,
    query: a.query || b.query,
    hash: a.hash || b.hash,
    content: a.content || b.content,
  });
}

// Build a user-controlled result with a single-hop taintPath rooted at the
// taint source. Every direct taint-source emission in _traceValueSourceInner
// goes through this, so downstream reviewers see where the taint starts.
// `dims` declares which parts of the value are attacker-controllable; when
// omitted, defaults to content-only (conservative for non-URL sources).
function _tvsSource(source, loc, dims) {
  var d = dims ? _tvsDims(dims) : _tvsDimsContent();
  var hop = { kind: "source", desc: String(source), at: loc || null, dims: d };
  var code = _snippetAtLoc(loc);
  if (code) hop.code = code;
  return {
    sourceType: "user-controlled",
    source: source,
    sourceLoc: loc || null,
    taintPath: [hop],
    dimensions: d,
  };
}

// Propagate a sub-trace one hop outward. `kind` classifies the hop
// (binding | member | call-arg | template-expr | concat | ...), `desc` is a
// short human label (variable name, property accessed, etc.), `at` is where
// the hop node itself sits. The sub-trace's existing taintPath is preserved
// (innermost hops already recorded) and the new hop is appended at the end,
// so the final path reads source → ... → sink (outermost).
// Dimensions pass through by default; URL-aware sites call _tvsHopDims to
// restrict or transform them. Each hop records its dimensions-out so a
// reviewer reading the taint chain can see WHERE origin flipped from
// false to true (e.g. `.slice(1)` on location.hash strips the "#"
// structural prefix). When the change is non-trivial, the hop also
// records `dimsBefore` for side-by-side comparison.
function _tvsHop(sub, kind, desc, at) {
  if (!sub || sub.sourceType !== "user-controlled") return sub;
  var prior = sub.taintPath || [];
  var newPath = new Array(prior.length + 1);
  for (var _tpi = 0; _tpi < prior.length; _tpi++) newPath[_tpi] = prior[_tpi];
  var outDims = sub.dimensions ? _tvsDims(sub.dimensions) : _tvsDimsContent();
  var hop = {
    kind: kind,
    desc: desc == null ? "" : String(desc),
    at: at || null,
    dims: outDims,
  };
  // Attach a code snippet captured from the bundle at this hop's position
  // so UI consumers (popup taint-path accordion, harness finding command)
  // can display what the code actually does at every step without needing
  // to re-fetch the source. The sink-level codeContext only shows the
  // innermost sink; intermediate transforms are where the dim transitions
  // happen and are the hardest to judge without inline code.
  var code = _snippetAtLoc(at);
  if (code) hop.code = code;
  newPath[prior.length] = hop;
  return {
    sourceType: sub.sourceType,
    source: sub.source,
    sourceLoc: sub.sourceLoc,
    taintPath: newPath,
    dimensions: outDims,
  };
}

// Same as _tvsHop but replaces the dimensions on the way out. Records
// both before and after on the hop so reviewers see the transition.
function _tvsHopDims(sub, kind, desc, at, dims) {
  var h = _tvsHop(sub, kind, desc, at);
  if (!h || h.sourceType !== "user-controlled") return h;
  var newDims = _tvsDims(dims || { content: true });
  var oldDims = sub && sub.dimensions ? _tvsDims(sub.dimensions) : null;
  h.dimensions = newDims;
  var lastHop = h.taintPath[h.taintPath.length - 1];
  lastHop.dims = newDims;
  // Record `dimsBefore` only when it differs — keeps small cases clean.
  if (oldDims && _dimsDiffer(oldDims, newDims)) lastHop.dimsBefore = oldDims;
  return h;
}

function _dimsDiffer(a, b) {
  if (!a || !b) return !!(a || b);
  return a.origin !== b.origin || a.path !== b.path || a.query !== b.query ||
         a.hash !== b.hash || a.content !== b.content;
}

// Source-name → dimensions. These are facts about what each browser
// taint source intrinsically carries. The taint source's name comes
// from _matchTaintSource's pattern table and is the canonical form
// (`location.href`, `window.name`, `event.data`, etc.).
//
// The `origin` dim captures: attacker can cause a fetch() of this value
// to resolve to a CROSS-origin target. It is NOT the same as "attacker
// picked the URL the victim visits" — location.href is always the
// current origin, and fetch(location.href) is a same-origin request
// regardless of which URL the attacker lured the victim to.
function _dimsForSource(srcName) {
  switch (srcName) {
    case "location":
    case "location.href":
    case "document.URL":
    case "document.documentURI":
      // Full URL of the current page. Its origin portion IS the current
      // origin (browser guarantee) — fetch(location.href) is same-origin.
      // Path/query/hash portions are attacker-influenced (user's URL).
      return { path: true, query: true, hash: true, content: true };
    case "location.origin":
    case "location.host":
    case "location.hostname":
    case "location.port":
    case "location.protocol":
    case "document.domain":
      // These are the current origin/parts-of-origin. fetch(location.origin)
      // is same-origin. Value is content for concatenation purposes.
      return { content: true };
    case "location.pathname":
      return { path: true, content: true };
    case "location.search":
      return { query: true, content: true };
    case "location.hash":
      return { hash: true, content: true };
    case "document.referrer":
      // Referring page's URL. Attacker can host a site with a link here,
      // so the entire URL (including origin) is attacker-controlled. Browsers
      // strip the fragment from Referer per RFC — no hash dim.
      return { origin: true, path: true, query: true, content: true };
    case "document.baseURI":
      // Controlled by <base href="…">. Attacker can inject/set this tag
      // if they have any DOM write; origin is attacker-controllable.
      return { origin: true, path: true, content: true };
    default:
      // event.data, window.name, document.cookie, storage.getItem, etc.
      // Free-form attacker-supplied string. When used as a fetch URL,
      // the attacker can specify ANY URL including any origin/scheme.
      return { origin: true, content: true };
  }
}
// State IDs for the _tvsStep state machine. INIT (0) is the entry state
// for every new frame; from there, the dispatch falls through type-checks
// in source order. Each case either:
//   • returns {done: value}          — frame complete, value bubbles to caller
//   • returns {trace: subPath, state: AFTER_ID}  — push sub-frame; resume here
//   • sets F.state = NEXT and `continue`s the while loop  — internal goto
// AFTER states read F.result (driver populates from sub-frame's done value),
// then either resolve, request another sub-trace, or fall through via the
// internal-goto pattern. State IDs are grouped by AST node type for
// readability — gaps are intentional and reserved for future sub-states.
var _TVS_INIT = 0;
// Non-recursive early-return checks all live within INIT.
// Object property access (MemberExpression with Identifier object).
var _TVS_OBJ_EXPR_PROP_AFTER = 100;
var _TVS_OBJ_ASSIGN_OUTER = 110;       // outer arg loop (idx _oaPI)
var _TVS_OBJ_ASSIGN_SRC_AFTER = 112;
var _TVS_OBJ_ASSIGN_INL_AFTER = 114;
// Non-computed MemberExpression chain leaf trace.
var _TVS_NCMC_AFTER_LEAF = 200;
// Computed MemberExpression chain leaf trace.
var _TVS_CMC_AFTER_LEAF = 300;
// Identifier branches.
var _TVS_IDENT_AFTER_INIT = 400;
var _TVS_IDENT_CV_LOOP = 401;
var _TVS_IDENT_CV_AFTER = 402;
var _TVS_IDENT_UCV_LOOP = 410;
var _TVS_IDENT_UCV_AFTER = 411;
var _TVS_IDENT_FOR_ITER_AFTER = 420;
var _TVS_IDENT_DESTR_OBJ_AFTER = 421;
var _TVS_IDENT_DESTR_OBJ_FOR_AFTER = 422;
var _TVS_IDENT_DESTR_ARR_AFTER = 423;
var _TVS_IDENT_DESTR_ARR_FOR_AFTER = 424;
var _TVS_IDENT_PARAM_IIFE_AFTER = 425;
var _TVS_IDENT_PARAM_CALLERS_LOOP = 430;
var _TVS_IDENT_PARAM_CALLERS_AFTER = 431;
var _TVS_IDENT_PARAM_ITER_REF_LOOP = 440;
var _TVS_IDENT_PARAM_ITER_REF_LOOP_INNER = 441;
var _TVS_IDENT_PARAM_ITER_AFTER = 442;
var _TVS_IDENT_PARAM_ITER_REF_AFTER = 443;
var _TVS_IDENT_PARAM_REDUCE_AFTER = 444;
// Fall-through dispatcher used by UCV_LOOP exhaust → for-iter / destructure /
// param checks (matches original generator's "if (X) … if (Y) …" sequence).
var _TVS_IDENT_TRY_OTHER = 445;
// After all param recursive paths fail — runs msg-handler check (no recursion).
var _TVS_IDENT_PARAM_TRY_MSG = 446;
// Template literal expressions loop.
var _TVS_TL_AFTER = 501;
// Tagged template expressions loop.
var _TVS_TT_AFTER = 511;
// Binary expression iterative chain-walk.
var _TVS_BIN_AFTER_LEAF = 600;
var _TVS_BIN_RIGHTS_AFTER = 602;
// Conditional expression.
var _TVS_COND_AFTER_CONS = 700;
var _TVS_COND_AFTER_ALT = 701;
// Logical expression iterative chain-walk.
var _TVS_LOG_AFTER_LEAF = 800;
var _TVS_LOG_RIGHTS_AFTER = 802;
// Single-trace branches (sequence, await, spread, assignment-rhs).
var _TVS_SEQ_AFTER = 900;
var _TVS_AWAIT_AFTER = 901;
var _TVS_SPREAD_AFTER = 902;
var _TVS_ASSIGN_RHS_AFTER = 903;
// Call expression branches.
var _TVS_CALL_METHOD_AFTER_OBJ = 1000;
var _TVS_CALL_OA_LOOP = 1010;
var _TVS_CALL_OA_AFTER = 1011;
var _TVS_CALL_GETHAS_AFTER = 1020;
var _TVS_CALL_GENERIC_AFTER = 1021;
var _TVS_CALL_GENERIC_AFTER_DISPATCH = 1030;
var _TVS_CALL_GENERIC_AFTER_DISPATCH_FN_RETURN = 1031;
// new URL(input, base?).
var _TVS_NEW_URL_AFTER_INPUT = 1100;
var _TVS_NEW_URL_AFTER_BASE = 1101;
var _TVS_NEW_URL_RESOLVE = 1102;
// NewExpression args loop.
var _TVS_NEW_AFTER = 1201;
// ArrayExpression elements loop.
var _TVS_ARR_AFTER = 1301;
// ObjectExpression properties loop.
var _TVS_OBJ_LOOP = 1400;
var _TVS_OBJ_PROP_AFTER = 1401;
var _TVS_OBJ_SPREAD_AFTER = 1402;

// Function-return resolution: walk a callee's body for ReturnStatement
// nodes, then dispatch each return-arg as a TVS sub-frame. Replaces the
// prior synchronous call to _traceReturnsInBlock from inside _tvsStep,
// which created a static call cycle TVS ↔ _traceReturnsInBlock.
var _TVS_FN_RETURN_LOOP = 1500;
var _TVS_FN_RETURN_AFTER = 1501;

function _tvsMakeFrame(path, node) {
  return {
    path: path,
    node: node,
    nodeLoc: node && node.loc ? { line: node.loc.start.line, column: node.loc.start.column } : null,
    state: _TVS_INIT,
    L: {},        // saved locals between sub-traces
    result: undefined,
  };
}

// Explicit-frame state-machine driver. Each frame is a plain object whose
// `state` field selects which step-handler runs next. Recursive descent in
// _traceValueSourceInner was replaced by _tvsStep returning {trace: subPath,
// state: AFTER_ID}; the driver pushes a new frame for subPath and resumes
// the saved state after the sub-frame produces a result. The JS call stack
// stays at depth 1 regardless of AST depth, and frame state is fully
// inspectable for future optimizations (memoization, batched processing,
// work coalescing) — a generator's internal state isn't.
function _traceValueSource(path, _unused) {
  if (!path || !path.node) return { sourceType: "dynamic", source: null };
  var node = path.node;

  // Cycle detection via visited set keyed on AST node position
  var isRoot = !_taintVisited;
  if (isRoot) _taintVisited = new Set();
  var rootKey = (node.start != null && node.end != null) ? node.start + ":" + node.end : null;
  if (rootKey) {
    if (_taintVisited.has(rootKey)) {
      if (isRoot) _taintVisited = null;
      return { sourceType: "dynamic", source: null };
    }
    _taintVisited.add(rootKey);
  }

  var DYN = { sourceType: "dynamic", source: null };
  var stack = [_tvsMakeFrame(path, node)];
  var lastResult = undefined;

  try {
    while (stack.length > 0) {
      var top = stack[stack.length - 1];
      top.result = lastResult;
      lastResult = undefined;

      var step;
      try {
        step = _tvsStep(top);
      } catch (_tvse) {
        if (_tvse instanceof RangeError) {
          _resolver.collectError(_tvse, "traceValueSource");
          stack.pop();
          lastResult = DYN;
          continue;
        }
        throw _tvse;
      }

      if (step.done !== undefined) {
        lastResult = step.done;
        stack.pop();
        continue;
      }

      // step.trace is a sub-path; step.state is the AFTER state ID to resume
      var subPath = step.trace;
      top.state = step.state;
      if (!subPath || !subPath.node) {
        lastResult = DYN;
        continue;
      }
      var subNode = subPath.node;
      var subKey = (subNode.start != null && subNode.end != null) ? subNode.start + ":" + subNode.end : null;
      if (subKey) {
        if (_taintVisited.has(subKey)) {
          lastResult = DYN;
          continue;
        }
        _taintVisited.add(subKey);
      }
      stack.push(_tvsMakeFrame(subPath, subNode));
    }
    return lastResult === undefined ? DYN : lastResult;
  } finally {
    if (isRoot) _taintVisited = null;
  }
}

// Pure AST walk — collect every ReturnStatement.argument path inside
// a function body, including nested blocks (if/else chains, switch
// cases, try/catch, for/while bodies). No taint analysis here; just
// path enumeration. Used by _tvsStep's function-return resolution
// state in lieu of a synchronous _traceReturnsInBlock call (which
// would create a static call cycle TVS ↔ _traceReturnsInBlock).
function _collectReturnArgPaths(blockPath) {
  if (!blockPath || !blockPath.node) return [];
  var argPaths = [];
  var worklist = [blockPath];
  while (worklist.length > 0) {
    var curBlock = worklist.shift();
    if (!curBlock || !curBlock.node) continue;
    var stmts = curBlock.node.body;
    if (!stmts) continue;
    for (var i = 0; i < stmts.length; i++) {
      var stmt = stmts[i];
      var stmtPath = curBlock.get("body." + i);
      if (_t.isReturnStatement(stmt) && stmt.argument) {
        argPaths.push(stmtPath.get("argument"));
      } else if (_t.isIfStatement(stmt)) {
        if (stmt.consequent && _t.isBlockStatement(stmt.consequent)) {
          worklist.push(stmtPath.get("consequent"));
        }
        if (stmt.alternate) {
          if (_t.isBlockStatement(stmt.alternate)) {
            worklist.push(stmtPath.get("alternate"));
          } else if (_t.isIfStatement(stmt.alternate)) {
            var elseIfPath = stmtPath.get("alternate");
            while (elseIfPath && elseIfPath.node && _t.isIfStatement(elseIfPath.node)) {
              if (elseIfPath.node.consequent && _t.isBlockStatement(elseIfPath.node.consequent)) {
                worklist.push(elseIfPath.get("consequent"));
              }
              if (!elseIfPath.node.alternate) break;
              if (_t.isBlockStatement(elseIfPath.node.alternate)) {
                worklist.push(elseIfPath.get("alternate"));
                break;
              }
              if (_t.isIfStatement(elseIfPath.node.alternate)) {
                elseIfPath = elseIfPath.get("alternate");
                continue;
              }
              break;
            }
          }
        }
      } else if (_t.isSwitchStatement(stmt)) {
        for (var ci = 0; ci < stmt.cases.length; ci++) {
          var c = stmt.cases[ci];
          for (var cj = 0; cj < c.consequent.length; cj++) {
            var cs = c.consequent[cj];
            if (_t.isReturnStatement(cs) && cs.argument) {
              argPaths.push(stmtPath.get("cases." + ci + ".consequent." + cj + ".argument"));
            } else if (_t.isBlockStatement(cs)) {
              worklist.push(stmtPath.get("cases." + ci + ".consequent." + cj));
            }
          }
        }
      } else if (_t.isTryStatement(stmt)) {
        if (stmt.block) worklist.push(stmtPath.get("block"));
        if (stmt.handler && stmt.handler.body) worklist.push(stmtPath.get("handler.body"));
      } else if (_t.isBlockStatement(stmt)) {
        worklist.push(stmtPath);
      } else if (stmt.body && _t.isBlockStatement(stmt.body)) {
        worklist.push(stmtPath.get("body"));
      }
    }
  }
  return argPaths;
}

// Recursively walk a block statement to find all ReturnStatement nodes,
// including those inside if/else, switch/case, try/catch, for/while, etc.
// Returns the first user-controlled return source found, or null.
function _traceReturnsInBlock(blockPath) {
  // Iterative walker: explicit stack of BlockStatement paths to scan.
  // Adversarial / deeply-nested code (long if/else-if chains, deep
  // try/catch nesting, switch with many cases each holding a block) can
  // produce arbitrary block-nesting depth — recursive descent could
  // stack-overflow. The native call stack stays at depth 1 here; each
  // nested block is pushed onto `worklist` and processed in the same
  // function frame.
  if (!blockPath || !blockPath.node) return null;
  var worklist = [blockPath];
  while (worklist.length > 0) {
    var curBlock = worklist.shift();
    if (!curBlock || !curBlock.node) continue;
    var stmts = curBlock.node.body;
    if (!stmts) continue;
    for (var _rbi = 0; _rbi < stmts.length; _rbi++) {
      var stmt = stmts[_rbi];
      var stmtPath = curBlock.get("body." + _rbi);
      if (_t.isReturnStatement(stmt) && stmt.argument) {
        var _rs = _traceValueSource(stmtPath.get("argument"));
        if (_rs.sourceType === "user-controlled") return _rs;
      } else if (_t.isIfStatement(stmt)) {
        if (stmt.consequent && _t.isBlockStatement(stmt.consequent)) {
          worklist.push(stmtPath.get("consequent"));
        }
        if (stmt.alternate) {
          if (_t.isBlockStatement(stmt.alternate)) {
            worklist.push(stmtPath.get("alternate"));
          } else if (_t.isIfStatement(stmt.alternate)) {
            // else if — walk the chain iteratively, pushing each
            // consequent block onto the worklist.
            var elseIfPath = stmtPath.get("alternate");
            while (elseIfPath && elseIfPath.node && _t.isIfStatement(elseIfPath.node)) {
              if (elseIfPath.node.consequent && _t.isBlockStatement(elseIfPath.node.consequent)) {
                worklist.push(elseIfPath.get("consequent"));
              }
              if (!elseIfPath.node.alternate) break;
              if (_t.isBlockStatement(elseIfPath.node.alternate)) {
                worklist.push(elseIfPath.get("alternate"));
                break;
              }
              if (_t.isIfStatement(elseIfPath.node.alternate)) {
                elseIfPath = elseIfPath.get("alternate");
                continue;
              }
              break;
            }
          }
        }
      } else if (_t.isSwitchStatement(stmt)) {
        for (var _sci = 0; _sci < stmt.cases.length; _sci++) {
          var _case = stmt.cases[_sci];
          for (var _csj = 0; _csj < _case.consequent.length; _csj++) {
            var _csStmt = _case.consequent[_csj];
            if (_t.isReturnStatement(_csStmt) && _csStmt.argument) {
              var _csR = _traceValueSource(stmtPath.get("cases." + _sci + ".consequent." + _csj + ".argument"));
              if (_csR && _csR.sourceType === "user-controlled") return _csR;
            } else if (_t.isBlockStatement(_csStmt)) {
              worklist.push(stmtPath.get("cases." + _sci + ".consequent." + _csj));
            }
          }
        }
      } else if (_t.isTryStatement(stmt)) {
        if (stmt.block) worklist.push(stmtPath.get("block"));
        if (stmt.handler && stmt.handler.body) worklist.push(stmtPath.get("handler.body"));
      } else if (_t.isBlockStatement(stmt)) {
        worklist.push(stmtPath);
      } else if (stmt.body && _t.isBlockStatement(stmt.body)) {
        // for, while, do-while, etc.
        worklist.push(stmtPath.get("body"));
      }
    }
  }
  return null;
}
function _traceReturnsInIfChain(ifPath) {
  // Iterative: walk else-if chains without recursion
  var cur = ifPath;
  while (cur && cur.node && _t.isIfStatement(cur.node)) {
    var stmt = cur.node;
    if (stmt.consequent && _t.isBlockStatement(stmt.consequent)) {
      var _r = _traceReturnsInBlock(cur.get("consequent"));
      if (_r && _r.sourceType === "user-controlled") return _r;
    }
    if (!stmt.alternate) break;
    if (_t.isBlockStatement(stmt.alternate)) {
      var _r2 = _traceReturnsInBlock(cur.get("alternate"));
      if (_r2 && _r2.sourceType === "user-controlled") return _r2;
      break;
    }
    if (_t.isIfStatement(stmt.alternate)) {
      cur = cur.get("alternate");
      continue;
    }
    break;
  }
  return null;
}

// Fully iterative step function for the _traceValueSource state machine.
// Each AST branch from the original recursive form is now a `case` block
// (or a chain of cases for branches with internal recursion). Sub-traces
// are requested by returning {trace: subPath, state: AFTER_ID}; the
// driver pushes a new frame and resumes at AFTER_ID with F.result set.
//
// Fall-through between branches uses F.L.branchSkip — a monotonically
// increasing ordinal that tells INIT to skip already-tried branches when
// re-entered after a recursive sub-frame returned a non-tainted result.
// This preserves the original generator's "if (X) {…} if (Y) {…}"
// fall-through ordering without re-checking branches we already passed.
//
// All ~40 former `_traceValueSource(X)` recursion sites are now explicit
// state transitions; the JS call stack stays at depth 1 regardless of
// AST depth, and frame state (F.L, F.state, F.result) is fully visible
// for future optimizations.
function _tvsStep(F) {
  var path = F.path, node = F.node, nodeLoc = F.nodeLoc, L = F.L;
  var DYN = { sourceType: "dynamic", source: null };
  var LIT = { sourceType: "literal", source: null };

  while (true) {
    switch (F.state) {
      case _TVS_INIT: {
        var skip = L.branchSkip || 0;

        // ── Branch 1: Literals ────────────────────────────────────────
        if (skip < 1 && (_t.isStringLiteral(node) || _t.isNumericLiteral(node) ||
            _t.isBooleanLiteral(node) || _t.isNullLiteral(node) ||
            (_t.isTemplateLiteral(node) && node.expressions.length === 0))) {
          return { done: LIT };
        }

        // ── Branch 2: MemberExpression — known taint sources ───────────
        if (skip < 2 && (_t.isMemberExpression(node) || _t.isOptionalMemberExpression(node)) && !node.computed) {
          var taintMatch = _matchTaintSource(path, node);
          if (taintMatch) {
            return { done: _tvsSource(taintMatch, nodeLoc, _dimsForSource(taintMatch)) };
          }
          if (_t.isIdentifier(node.object, { name: "location" }) && !path.scope.getBinding("location") &&
              _t.isIdentifier(node.property) && _LOCATION_SAFE_PROPS[node.property.name]) {
            return { done: LIT };
          }
          if (_t.isMemberExpression(node.object) && !node.object.computed &&
              _t.isIdentifier(node.object.object) &&
              (node.object.object.name === "window" || node.object.object.name === "self") &&
              !path.scope.getBinding(node.object.object.name) &&
              _t.isIdentifier(node.object.property, { name: "location" }) &&
              _t.isIdentifier(node.property) && _LOCATION_SAFE_PROPS[node.property.name]) {
            return { done: LIT };
          }
        }

        // ── Branch 3: Computed location access ────────────────────────
        if (skip < 3 && _t.isMemberExpression(node) && node.computed &&
            _t.isIdentifier(node.object, { name: "location" }) && !path.scope.getBinding("location")) {
          return { done: DYN };
        }

        // ── Branch 4: Object property access (cfg.redirectUrl) ─────────
        if (skip < 4 && _t.isMemberExpression(node) && !node.computed && _t.isIdentifier(node.object)) {
          var objPropName = _t.isIdentifier(node.property) ? node.property.name : null;
          if (objPropName) {
            var objBinding = path.scope.getBinding(node.object.name);
            if (objBinding && objBinding.path.isVariableDeclarator() && objBinding.path.node.init) {
              var _objInit = objBinding.path.node.init;
              if (_t.isObjectExpression(_objInit)) {
                for (var oi = 0; oi < _objInit.properties.length; oi++) {
                  var op = _objInit.properties[oi];
                  if (_t.isObjectProperty(op) &&
                      ((_t.isIdentifier(op.key) && op.key.name === objPropName) ||
                       (_t.isStringLiteral(op.key) && op.key.value === objPropName))) {
                    L.objBinding = objBinding;
                    L.objPropName = objPropName;
                    L.objProps = _objInit.properties;
                    L.oi = oi;
                    L.branchSkip = 4;
                    return {
                      trace: objBinding.path.get("init.properties." + oi + ".value"),
                      state: _TVS_OBJ_EXPR_PROP_AFTER,
                    };
                  }
                }
              }
              if (_t.isCallExpression(_objInit) && _t.isMemberExpression(_objInit.callee) &&
                  !_objInit.callee.computed && _t.isIdentifier(_objInit.callee.property, { name: "assign" }) &&
                  _t.isIdentifier(_objInit.callee.object, { name: "Object" }) &&
                  !path.scope.getBinding("Object") && _objInit.arguments.length >= 2) {
                L.objBinding = objBinding;
                L.objPropName = objPropName;
                L.objInit = _objInit;
                L.oaPI = 1;
                L.branchSkip = 4;
                F.state = _TVS_OBJ_ASSIGN_OUTER;
                continue;
              }
            }
          }
        }

        // ── Branch 5: Non-computed MemberExpression chain ─────────────
        if (skip < 5 && (_t.isMemberExpression(node) || _t.isOptionalMemberExpression(node)) && !node.computed) {
          var _chainProps = [];
          var _chainPath = path;
          var _chainNode = node;
          while ((_t.isMemberExpression(_chainNode) || _t.isOptionalMemberExpression(_chainNode)) &&
                 !_chainNode.computed) {
            _chainProps.push({
              name: _t.isIdentifier(_chainNode.property) ? _chainNode.property.name : "?",
              loc: _chainNode.loc ? { line: _chainNode.loc.start.line, column: _chainNode.loc.start.column } : null,
            });
            _chainPath = _chainPath.get("object");
            _chainNode = _chainPath.node;
          }
          L.chainProps = _chainProps;
          L.branchSkip = 5;
          return { trace: _chainPath, state: _TVS_NCMC_AFTER_LEAF };
        }

        // ── Branch 6: Computed MemberExpression chain ─────────────────
        if (skip < 6 && (_t.isMemberExpression(node) || _t.isOptionalMemberExpression(node)) && node.computed) {
          var _compHops = 0;
          var _compPath = path;
          var _compNode = node;
          while ((_t.isMemberExpression(_compNode) || _t.isOptionalMemberExpression(_compNode)) && _compNode.computed) {
            _compHops++;
            _compPath = _compPath.get("object");
            _compNode = _compPath.node;
          }
          L.compHops = _compHops;
          L.branchSkip = 6;
          return { trace: _compPath, state: _TVS_CMC_AFTER_LEAF };
        }

        // ── Branch 7: Identifier (multi-recursive) ─────────────────────
        if (skip < 7 && _t.isIdentifier(node)) {
          var binding = path.scope.getBinding(node.name);
          if (binding) {
            // Variable initializer + reassignments
            if (binding.path.isVariableDeclarator() && binding.path.node.init) {
              L.identBinding = binding;
              L.identName = node.name;
              L.branchSkip = 7;
              return { trace: binding.path.get("init"), state: _TVS_IDENT_AFTER_INIT };
            }
            // Uninitialized declaration with reassignments
            if (binding.path.isVariableDeclarator() && !binding.path.node.init &&
                binding.constantViolations && binding.constantViolations.length > 0) {
              L.identBinding = binding;
              L.identName = node.name;
              L.identUcvI = 0;
              L.branchSkip = 7;
              F.state = _TVS_IDENT_UCV_LOOP;
              continue;
            }
            // For-in/for-of loop variable
            if (binding.path.isVariableDeclarator() && !binding.path.node.init) {
              var _forParent = binding.path.parentPath && binding.path.parentPath.parentPath;
              if (_forParent && (_t.isForInStatement(_forParent.node) || _t.isForOfStatement(_forParent.node)) &&
                  _forParent.node.left === binding.path.parent) {
                L.identName = node.name;
                L.branchSkip = 7;
                return { trace: _forParent.get("right"), state: _TVS_IDENT_FOR_ITER_AFTER };
              }
            }
            // Destructured ObjectProperty/RestElement
            if (_t.isObjectProperty(binding.path.node) || _t.isRestElement(binding.path.node)) {
              var _destrParent = binding.path.parentPath;
              while (_destrParent && !_destrParent.isVariableDeclarator()) {
                _destrParent = _destrParent.parentPath;
              }
              if (_destrParent && _destrParent.node.init) {
                L.identName = node.name;
                L.branchSkip = 7;
                return { trace: _destrParent.get("init"), state: _TVS_IDENT_DESTR_OBJ_AFTER };
              }
              if (_destrParent && !_destrParent.node.init) {
                var _forOwner = _destrParent.parentPath && _destrParent.parentPath.parentPath;
                if (_forOwner && (_t.isForOfStatement(_forOwner.node) || _t.isForInStatement(_forOwner.node))) {
                  L.identName = node.name;
                  L.branchSkip = 7;
                  return { trace: _forOwner.get("right"), state: _TVS_IDENT_DESTR_OBJ_FOR_AFTER };
                }
              }
            }
            // Destructured ArrayPattern element
            if (_t.isArrayPattern(binding.path.parent)) {
              var _arrDestrParent = binding.path.parentPath;
              while (_arrDestrParent && !_arrDestrParent.isVariableDeclarator()) {
                _arrDestrParent = _arrDestrParent.parentPath;
              }
              if (_arrDestrParent && _arrDestrParent.node.init) {
                L.identName = node.name;
                L.branchSkip = 7;
                return { trace: _arrDestrParent.get("init"), state: _TVS_IDENT_DESTR_ARR_AFTER };
              }
              if (_arrDestrParent && !_arrDestrParent.node.init) {
                var _forOwner2 = _arrDestrParent.parentPath && _arrDestrParent.parentPath.parentPath;
                if (_forOwner2 && (_t.isForOfStatement(_forOwner2.node) || _t.isForInStatement(_forOwner2.node))) {
                  L.identName = node.name;
                  L.branchSkip = 7;
                  return { trace: _forOwner2.get("right"), state: _TVS_IDENT_DESTR_ARR_FOR_AFTER };
                }
              }
            }
            // Function parameter
            if (binding.kind === "param") {
              var paramIdx = -1;
              var funcPath = binding.scope.path;
              if (funcPath && funcPath.node.params) {
                for (var pi = 0; pi < funcPath.node.params.length; pi++) {
                  if (_t.isIdentifier(funcPath.node.params[pi], { name: node.name })) { paramIdx = pi; break; }
                }
                if (paramIdx === -1) {
                  for (var _dpi = 0; _dpi < funcPath.node.params.length; _dpi++) {
                    var _dpParam = funcPath.node.params[_dpi];
                    if (_t.isObjectPattern(_dpParam)) {
                      if (_findDestructuredKey(_dpParam, node.name)) { paramIdx = _dpi; break; }
                    }
                    if (_t.isAssignmentPattern(_dpParam) && _t.isObjectPattern(_dpParam.left)) {
                      if (_findDestructuredKey(_dpParam.left, node.name)) { paramIdx = _dpi; break; }
                    }
                  }
                }
              }
              if (paramIdx >= 0 && funcPath) {
                var _iifeParent = funcPath.parentPath;
                if (_iifeParent && _t.isCallExpression(_iifeParent.node) && _iifeParent.node.callee === funcPath.node &&
                    paramIdx < _iifeParent.node.arguments.length) {
                  L.identName = node.name;
                  L.paramIdx = paramIdx;
                  L.funcPath = funcPath;
                  L.branchSkip = 7;
                  return {
                    trace: _iifeParent.get("arguments." + paramIdx),
                    state: _TVS_IDENT_PARAM_IIFE_AFTER,
                  };
                }
                L.identName = node.name;
                L.paramIdx = paramIdx;
                L.funcPath = funcPath;
                L.callerArgs = _findFunctionCallerArgs(funcPath);
                L.callerArgsCi = 0;
                L.branchSkip = 7;
                F.state = _TVS_IDENT_PARAM_CALLERS_LOOP;
                continue;
              }
              // Message handler detection (no recursion — entirely structural)
              if (paramIdx === 0 && funcPath) {
                var _isMsgHandler = false;
                var _mhParent = funcPath.parentPath;
                if (_mhParent) {
                  if (_mhParent.isCallExpression()) {
                    var _aeNode = _mhParent.node;
                    if (_aeNode.arguments.length >= 2 && _aeNode.arguments[1] === funcPath.node &&
                        _t.isStringLiteral(_aeNode.arguments[0], { value: "message" })) {
                      var _aeCal = _aeNode.callee;
                      if ((_t.isMemberExpression(_aeCal) && !_aeCal.computed &&
                           _t.isIdentifier(_aeCal.property, { name: "addEventListener" })) ||
                          (_t.isIdentifier(_aeCal, { name: "addEventListener" }) &&
                           !funcPath.scope.getBinding("addEventListener"))) {
                        if (_isCrossOriginMsgReceiver(_aeCal, _mhParent)) _isMsgHandler = true;
                      }
                    }
                  }
                  if (_mhParent.isAssignmentExpression() && _mhParent.node.right === funcPath.node) {
                    var _omLeft = _mhParent.node.left;
                    if (_t.isMemberExpression(_omLeft) && !_omLeft.computed &&
                        _t.isIdentifier(_omLeft.property, { name: "onmessage" })) {
                      if (_isCrossOriginMsgReceiver(_omLeft, _mhParent)) _isMsgHandler = true;
                    }
                  }
                  if (!_isMsgHandler && _t.isReturnStatement(_mhParent.node)) {
                    var _mhEncFunc = funcPath.findParent(function(p) { return p.isFunction() && p !== funcPath; });
                    if (_mhEncFunc) {
                      var _mhEncBinding = null;
                      if (_t.isFunctionDeclaration(_mhEncFunc.node) && _t.isIdentifier(_mhEncFunc.node.id)) {
                        _mhEncBinding = _mhEncFunc.parentPath.scope.getBinding(_mhEncFunc.node.id.name);
                      } else if (_mhEncFunc.parentPath && _mhEncFunc.parentPath.isVariableDeclarator()) {
                        _mhEncBinding = _mhEncFunc.parentPath.scope.getBinding(_mhEncFunc.parentPath.node.id.name);
                      }
                      if (_mhEncBinding) {
                        var _mhRefs = _mhEncBinding.referencePaths || [];
                        for (var _mhri = 0; _mhri < _mhRefs.length && !_isMsgHandler; _mhri++) {
                          var _mhRefParent = _mhRefs[_mhri].parentPath;
                          if (_mhRefParent && _mhRefParent.isCallExpression() && _mhRefParent.node.callee === _mhRefs[_mhri].node) {
                            var _mhCallParent = _mhRefParent.parentPath;
                            if (_mhCallParent && _mhCallParent.isCallExpression()) {
                              var _mhOuterCall = _mhCallParent.node;
                              if (_mhOuterCall.arguments.length >= 2 && _mhOuterCall.arguments[1] === _mhRefParent.node &&
                                  _t.isStringLiteral(_mhOuterCall.arguments[0], { value: "message" })) {
                                var _mhOuterCallee = _mhOuterCall.callee;
                                if ((_t.isMemberExpression(_mhOuterCallee) && !_mhOuterCallee.computed &&
                                     _t.isIdentifier(_mhOuterCallee.property, { name: "addEventListener" })) ||
                                    (_t.isIdentifier(_mhOuterCallee, { name: "addEventListener" }) &&
                                     !_mhCallParent.scope.getBinding("addEventListener"))) {
                                  if (_isCrossOriginMsgReceiver(_mhOuterCallee, _mhCallParent)) _isMsgHandler = true;
                                }
                              }
                            }
                            if (_mhCallParent && _mhCallParent.isAssignmentExpression() && _mhCallParent.node.right === _mhRefParent.node) {
                              var _mhOmLeft2 = _mhCallParent.node.left;
                              if (_t.isMemberExpression(_mhOmLeft2) && !_mhOmLeft2.computed &&
                                  _t.isIdentifier(_mhOmLeft2.property, { name: "onmessage" })) {
                                _isMsgHandler = true;
                              }
                            }
                          }
                        }
                      }
                    }
                  }
                }
                if (!_isMsgHandler && _mhParent) {
                  var _namedBinding = null;
                  if (_mhParent.isVariableDeclarator() && _t.isIdentifier(_mhParent.node.id)) {
                    _namedBinding = _mhParent.scope.getBinding(_mhParent.node.id.name);
                  } else if (_mhParent.isAssignmentExpression() && _t.isIdentifier(_mhParent.node.left) &&
                             _mhParent.node.right === funcPath.node) {
                    _namedBinding = _mhParent.scope.getBinding(_mhParent.node.left.name);
                  }
                  if (_namedBinding) {
                    var _nbRefs = _namedBinding.referencePaths || [];
                    for (var _nbri = 0; _nbri < _nbRefs.length && !_isMsgHandler; _nbri++) {
                      var _nbRef = _nbRefs[_nbri];
                      var _nbRefP = _nbRef.parentPath;
                      if (!_nbRefP) continue;
                      if (_nbRefP.isCallExpression() && _nbRefP.node.arguments.length >= 2 &&
                          _nbRefP.node.arguments[1] === _nbRef.node &&
                          _t.isStringLiteral(_nbRefP.node.arguments[0], { value: "message" })) {
                        var _nbCal = _nbRefP.node.callee;
                        if ((_t.isMemberExpression(_nbCal) && !_nbCal.computed &&
                             _t.isIdentifier(_nbCal.property, { name: "addEventListener" })) ||
                            (_t.isIdentifier(_nbCal, { name: "addEventListener" }) &&
                             !_nbRefP.scope.getBinding("addEventListener"))) {
                          if (_isCrossOriginMsgReceiver(_nbCal, _nbRefP)) _isMsgHandler = true;
                        }
                      }
                      if (_nbRefP.isAssignmentExpression() && _nbRefP.node.right === _nbRef.node) {
                        var _nbOmL = _nbRefP.node.left;
                        if (_t.isMemberExpression(_nbOmL) && !_nbOmL.computed &&
                            _t.isIdentifier(_nbOmL.property, { name: "onmessage" })) {
                          _isMsgHandler = true;
                        }
                      }
                    }
                  }
                }
                if (_isMsgHandler) {
                  return { done: _tvsSource("event.data", nodeLoc, _dimsForSource("event.data")) };
                }
              }
              // No caller args found — return DYN
              return { done: DYN };
            }
          }
          // Bare `location` identifier (unbound)
          if (node.name === "location") return { done: _tvsSource("location", nodeLoc, _dimsForSource("location")) };
          return { done: DYN };
        }

        // ── Branch 8: TemplateLiteral with expressions ─────────────────
        if (skip < 8 && _t.isTemplateLiteral(node)) {
          var _tmplStripsOrigin = false;
          var _q0 = node.quasis && node.quasis[0];
          var _q0Raw = _q0 && _q0.value ? _q0.value.raw : "";
          if (_q0Raw && !/^https?:/i.test(_q0Raw) && !_q0Raw.startsWith("//") &&
              (_q0Raw.startsWith("/") || _q0Raw.startsWith("./") || _q0Raw.startsWith("../"))) {
            _tmplStripsOrigin = true;
          }
          if (node.expressions.length === 0) return { done: LIT };
          L.tmplStripsOrigin = _tmplStripsOrigin;
          L.tlTi = 0;
          L.tlExprCount = node.expressions.length;
          L.branchSkip = 8;
          return { trace: path.get("expressions.0"), state: _TVS_TL_AFTER };
        }

        // ── Branch 9: TaggedTemplateExpression ─────────────────────────
        if (skip < 9 && _t.isTaggedTemplateExpression(node)) {
          if (node.quasi.expressions.length > 0) {
            L.ttQuasiPath = path.get("quasi");
            L.ttTi = 0;
            L.ttExprCount = node.quasi.expressions.length;
            L.branchSkip = 9;
            return { trace: L.ttQuasiPath.get("expressions.0"), state: _TVS_TT_AFTER };
          }
        }

        // ── Branch 10: Binary expression (iterative left-chain walk) ───
        if (skip < 10 && _t.isBinaryExpression(node) && node.operator === "+") {
          var _binRights = [];
          var _binPath = path;
          var _binNode = node;
          while (_t.isBinaryExpression(_binNode) && _binNode.operator === "+") {
            _binRights.push({
              path: _binPath.get("right"),
              loc: _binNode.loc ? { line: _binNode.loc.start.line, column: _binNode.loc.start.column } : nodeLoc,
            });
            _binPath = _binPath.get("left");
            _binNode = _binPath.node;
          }
          L.binRights = _binRights;
          L.concatStripsOrigin = _leftmostLiteralIsSameOriginPrefix(path);
          L.branchSkip = 10;
          return { trace: _binPath, state: _TVS_BIN_AFTER_LEAF };
        }

        // ── Branch 11: Conditional expression ──────────────────────────
        if (skip < 11 && _t.isConditionalExpression(node)) {
          L.branchSkip = 11;
          return { trace: path.get("consequent"), state: _TVS_COND_AFTER_CONS };
        }

        // ── Branch 12: Logical expression (iterative left-chain walk) ──
        if (skip < 12 && _t.isLogicalExpression(node)) {
          var _logRights = [];
          var _logPath = path;
          var _logNode = node;
          while (_t.isLogicalExpression(_logNode)) {
            _logRights.push({
              path: _logPath.get("right"),
              op: _logNode.operator,
              loc: _logNode.loc ? { line: _logNode.loc.start.line, column: _logNode.loc.start.column } : nodeLoc,
            });
            _logPath = _logPath.get("left");
            _logNode = _logPath.node;
          }
          L.logRights = _logRights;
          L.branchSkip = 12;
          return { trace: _logPath, state: _TVS_LOG_AFTER_LEAF };
        }

        // ── Branch 13: Sequence expression (last expr) ─────────────────
        if (skip < 13 && _t.isSequenceExpression(node) && node.expressions.length > 0) {
          var _lastIdx = node.expressions.length - 1;
          L.branchSkip = 13;
          return { trace: path.get("expressions." + _lastIdx), state: _TVS_SEQ_AFTER };
        }

        // ── Branch 14: Await ───────────────────────────────────────────
        if (skip < 14 && _t.isAwaitExpression(node)) {
          L.branchSkip = 14;
          return { trace: path.get("argument"), state: _TVS_AWAIT_AFTER };
        }

        // ── Branch 15: Spread element ──────────────────────────────────
        if (skip < 15 && _t.isSpreadElement(node)) {
          L.branchSkip = 15;
          return { trace: path.get("argument"), state: _TVS_SPREAD_AFTER };
        }

        // ── Branch 16: Call expression ─────────────────────────────────
        if (skip < 16 && (_t.isCallExpression(node) || _t.isOptionalCallExpression(node))) {
          // localStorage.getItem / sessionStorage.getItem
          if (_t.isMemberExpression(node.callee) && !node.callee.computed &&
              _t.isIdentifier(node.callee.property, { name: "getItem" })) {
            var _storObj = node.callee.object;
            if ((_t.isIdentifier(_storObj, { name: "localStorage" }) && !path.scope.getBinding("localStorage")) ||
                (_t.isIdentifier(_storObj, { name: "sessionStorage" }) && !path.scope.getBinding("sessionStorage"))) {
              return { done: _tvsSource(_storObj.name + ".getItem", nodeLoc, _dimsForSource(_storObj.name + ".getItem")) };
            }
          }
          // Method call on tainted object (callee.object trace)
          if ((_t.isMemberExpression(node.callee) || _t.isOptionalMemberExpression(node.callee)) && !node.callee.computed) {
            L.branchSkip = 16;
            // Stash method-call context for the AFTER state
            L.callMethodName = _t.isIdentifier(node.callee.property) ? node.callee.property.name : "?";
            var _ca0 = null;
            if (node.arguments.length > 0) {
              var _ca0n = node.arguments[0];
              if (_t.isStringLiteral(_ca0n)) _ca0 = _ca0n.value;
              else if (_t.isTemplateLiteral(_ca0n) && _ca0n.expressions.length === 0 && _ca0n.quasis.length === 1) {
                _ca0 = _ca0n.quasis[0].value.cooked;
              }
            }
            L.callArgLit = _ca0;
            return { trace: path.get("callee.object"), state: _TVS_CALL_METHOD_AFTER_OBJ };
          }
          // (Object.assign / call-arg / function-return are all covered by
          // the call-arg branch since the method-call branch above didn't
          // match for non-MemberExpression callees. Set up Object.assign
          // and call-arg processing through state transitions.)
          L.callNode = node;
          L.branchSkip = 16;
          F.state = _TVS_CALL_OA_LOOP;
          continue;
        }

        // ── Branch 17: new URL(input, base?) ───────────────────────────
        if (skip < 17 && _t.isNewExpression(node) && _t.isIdentifier(node.callee, { name: "URL" }) &&
            !path.scope.getBinding("URL") && node.arguments.length >= 1) {
          L.urlInputPath = path.get("arguments.0");
          L.urlInputNode = node.arguments[0];
          L.urlBasePath = node.arguments.length >= 2 ? path.get("arguments.1") : null;
          L.branchSkip = 17;
          return { trace: L.urlInputPath, state: _TVS_NEW_URL_AFTER_INPUT };
        }

        // ── Branch 18: NewExpression args loop ─────────────────────────
        if (skip < 18 && _t.isNewExpression(node) && node.arguments.length > 0) {
          L.newCalleeName = _t.isIdentifier(node.callee) ? node.callee.name : "ctor";
          L.newNi = 0;
          L.newArgCount = node.arguments.length;
          L.branchSkip = 18;
          return { trace: path.get("arguments.0"), state: _TVS_NEW_AFTER };
        }

        // ── Branch 19: ArrayExpression elements loop ───────────────────
        if (skip < 19 && _t.isArrayExpression(node) && node.elements.length > 0) {
          L.arrAi = 0;
          L.arrElCount = node.elements.length;
          L.branchSkip = 19;
          // Skip null elements
          while (L.arrAi < L.arrElCount && !node.elements[L.arrAi]) L.arrAi++;
          if (L.arrAi >= L.arrElCount) {
            L.branchSkip = 20;
            F.state = _TVS_INIT;
            continue;
          }
          return { trace: path.get("elements." + L.arrAi), state: _TVS_ARR_AFTER };
        }

        // ── Branch 20: ObjectExpression properties loop ────────────────
        if (skip < 20 && _t.isObjectExpression(node) && node.properties.length > 0) {
          L.objExprOpi = 0;
          L.objExprPropsCount = node.properties.length;
          L.branchSkip = 20;
          F.state = _TVS_OBJ_LOOP;
          continue;
        }

        // ── Branch 21: AssignmentExpression right side ─────────────────
        if (skip < 21 && _t.isAssignmentExpression(node)) {
          L.branchSkip = 21;
          return { trace: path.get("right"), state: _TVS_ASSIGN_RHS_AFTER };
        }

        // No branch matched
        return { done: DYN };
      }

      // ── AFTER states ────────────────────────────────────────────────

      case _TVS_OBJ_EXPR_PROP_AFTER: {
        var src = F.result;
        if (src && src.sourceType === "user-controlled") {
          return { done: _tvsHop(src, "obj-prop", "." + L.objPropName, nodeLoc) };
        }
        // Continue scanning for next matching prop
        L.oi++;
        while (L.oi < L.objProps.length) {
          var pp = L.objProps[L.oi];
          if (_t.isObjectProperty(pp) &&
              ((_t.isIdentifier(pp.key) && pp.key.name === L.objPropName) ||
               (_t.isStringLiteral(pp.key) && pp.key.value === L.objPropName))) {
            return {
              trace: L.objBinding.path.get("init.properties." + L.oi + ".value"),
              state: _TVS_OBJ_EXPR_PROP_AFTER,
            };
          }
          L.oi++;
        }
        // Fall through to next branch — Object.assign init? Member chain?
        // Check if the object property branch's Object.assign sub-branch applies
        var _fallObjBinding = path.scope.getBinding(node.object.name);
        if (_fallObjBinding && _fallObjBinding.path.isVariableDeclarator() && _fallObjBinding.path.node.init) {
          var _fObjInit = _fallObjBinding.path.node.init;
          if (_t.isCallExpression(_fObjInit) && _t.isMemberExpression(_fObjInit.callee) &&
              !_fObjInit.callee.computed && _t.isIdentifier(_fObjInit.callee.property, { name: "assign" }) &&
              _t.isIdentifier(_fObjInit.callee.object, { name: "Object" }) &&
              !path.scope.getBinding("Object") && _fObjInit.arguments.length >= 2) {
            L.objBinding = _fallObjBinding;
            L.objInit = _fObjInit;
            L.oaPI = 1;
            F.state = _TVS_OBJ_ASSIGN_OUTER;
            continue;
          }
        }
        L.branchSkip = 5;  // skip past obj prop access (4)
        F.state = _TVS_INIT;
        continue;
      }

      case _TVS_OBJ_ASSIGN_OUTER: {
        // Outer arg loop: idx L.oaPI from 1 to length-1.
        // For each arg, check Identifier-source or inline-ObjectExpression sub-cases.
        if (L.oaPI >= L.objInit.arguments.length) {
          // Done — fall through past Branch 4
          L.branchSkip = 5;
          F.state = _TVS_INIT;
          continue;
        }
        var _oaSrcArg = L.objInit.arguments[L.oaPI];
        // Identifier-source sub-case
        if (_t.isIdentifier(_oaSrcArg)) {
          var _oaSrcBind = path.scope.getBinding(_oaSrcArg.name);
          if (_oaSrcBind && _oaSrcBind.path.isVariableDeclarator() && _oaSrcBind.path.node.init &&
              _t.isObjectExpression(_oaSrcBind.path.node.init)) {
            var _oaSrcProps = _oaSrcBind.path.node.init.properties;
            for (var _oaSPI = 0; _oaSPI < _oaSrcProps.length; _oaSPI++) {
              if (_t.isObjectProperty(_oaSrcProps[_oaSPI]) &&
                  ((_t.isIdentifier(_oaSrcProps[_oaSPI].key) && _oaSrcProps[_oaSPI].key.name === L.objPropName) ||
                   (_t.isStringLiteral(_oaSrcProps[_oaSPI].key) && _oaSrcProps[_oaSPI].key.value === L.objPropName))) {
                L.oaSrcBind = _oaSrcBind;
                L.oaSrcProps = _oaSrcProps;
                L.oaSPI = _oaSPI;
                return {
                  trace: _oaSrcBind.path.get("init.properties." + _oaSPI + ".value"),
                  state: _TVS_OBJ_ASSIGN_SRC_AFTER,
                };
              }
            }
          }
        }
        // Inline ObjectExpression sub-case
        if (_t.isObjectExpression(_oaSrcArg)) {
          for (var _oaInlI = 0; _oaInlI < _oaSrcArg.properties.length; _oaInlI++) {
            if (_t.isObjectProperty(_oaSrcArg.properties[_oaInlI]) &&
                ((_t.isIdentifier(_oaSrcArg.properties[_oaInlI].key) && _oaSrcArg.properties[_oaInlI].key.name === L.objPropName) ||
                 (_t.isStringLiteral(_oaSrcArg.properties[_oaInlI].key) && _oaSrcArg.properties[_oaInlI].key.value === L.objPropName))) {
              L.oaInlI = _oaInlI;
              return {
                trace: L.objBinding.path.get("init.arguments." + L.oaPI + ".properties." + _oaInlI + ".value"),
                state: _TVS_OBJ_ASSIGN_INL_AFTER,
              };
            }
          }
        }
        // Neither matched — advance outer
        L.oaPI++;
        F.state = _TVS_OBJ_ASSIGN_OUTER;
        continue;
      }

      case _TVS_OBJ_ASSIGN_SRC_AFTER: {
        var _oaPropSrc = F.result;
        if (_oaPropSrc && _oaPropSrc.sourceType === "user-controlled") {
          return { done: _tvsHop(_oaPropSrc, "obj-assign-prop", "." + L.objPropName + " (via Object.assign)", nodeLoc) };
        }
        // Continue scanning the same source's remaining props
        L.oaSPI++;
        while (L.oaSPI < L.oaSrcProps.length) {
          if (_t.isObjectProperty(L.oaSrcProps[L.oaSPI]) &&
              ((_t.isIdentifier(L.oaSrcProps[L.oaSPI].key) && L.oaSrcProps[L.oaSPI].key.name === L.objPropName) ||
               (_t.isStringLiteral(L.oaSrcProps[L.oaSPI].key) && L.oaSrcProps[L.oaSPI].key.value === L.objPropName))) {
            return {
              trace: L.oaSrcBind.path.get("init.properties." + L.oaSPI + ".value"),
              state: _TVS_OBJ_ASSIGN_SRC_AFTER,
            };
          }
          L.oaSPI++;
        }
        // Done with this source's props — also try inline sub-case for current arg
        var _oaSrcArg2 = L.objInit.arguments[L.oaPI];
        if (_t.isObjectExpression(_oaSrcArg2)) {
          for (var _oaInlI2 = 0; _oaInlI2 < _oaSrcArg2.properties.length; _oaInlI2++) {
            if (_t.isObjectProperty(_oaSrcArg2.properties[_oaInlI2]) &&
                ((_t.isIdentifier(_oaSrcArg2.properties[_oaInlI2].key) && _oaSrcArg2.properties[_oaInlI2].key.name === L.objPropName) ||
                 (_t.isStringLiteral(_oaSrcArg2.properties[_oaInlI2].key) && _oaSrcArg2.properties[_oaInlI2].key.value === L.objPropName))) {
              L.oaInlI = _oaInlI2;
              return {
                trace: L.objBinding.path.get("init.arguments." + L.oaPI + ".properties." + _oaInlI2 + ".value"),
                state: _TVS_OBJ_ASSIGN_INL_AFTER,
              };
            }
          }
        }
        // Advance outer
        L.oaPI++;
        F.state = _TVS_OBJ_ASSIGN_OUTER;
        continue;
      }

      case _TVS_OBJ_ASSIGN_INL_AFTER: {
        var _oaInlSrc = F.result;
        if (_oaInlSrc && _oaInlSrc.sourceType === "user-controlled") {
          return { done: _tvsHop(_oaInlSrc, "obj-assign-prop", "." + L.objPropName + " (via Object.assign inline)", nodeLoc) };
        }
        // Continue scanning inline props of current arg
        var _oaSrcArg3 = L.objInit.arguments[L.oaPI];
        L.oaInlI++;
        while (L.oaInlI < _oaSrcArg3.properties.length) {
          if (_t.isObjectProperty(_oaSrcArg3.properties[L.oaInlI]) &&
              ((_t.isIdentifier(_oaSrcArg3.properties[L.oaInlI].key) && _oaSrcArg3.properties[L.oaInlI].key.name === L.objPropName) ||
               (_t.isStringLiteral(_oaSrcArg3.properties[L.oaInlI].key) && _oaSrcArg3.properties[L.oaInlI].key.value === L.objPropName))) {
            return {
              trace: L.objBinding.path.get("init.arguments." + L.oaPI + ".properties." + L.oaInlI + ".value"),
              state: _TVS_OBJ_ASSIGN_INL_AFTER,
            };
          }
          L.oaInlI++;
        }
        // Advance outer
        L.oaPI++;
        F.state = _TVS_OBJ_ASSIGN_OUTER;
        continue;
      }

      case _TVS_NCMC_AFTER_LEAF: {
        var _deepObjSource = F.result;
        if (_deepObjSource && _deepObjSource.sourceType === "user-controlled" && L.chainProps.length > 0) {
          var _curSource = _deepObjSource;
          for (var _cpi = L.chainProps.length - 1; _cpi >= 0; _cpi--) {
            var _deepPropName = L.chainProps[_cpi].name;
            var _propLoc = L.chainProps[_cpi].loc || nodeLoc;
            var _srcDims = _curSource.dimensions || _tvsDimsContent();
            var _projDims = null;
            switch (_deepPropName) {
              case "origin": case "host": case "hostname": case "protocol": case "port":
                _projDims = { origin: _srcDims.origin, content: _srcDims.origin }; break;
              case "pathname":
                _projDims = { path: _srcDims.path, content: _srcDims.path }; break;
              case "search": case "searchParams":
                _projDims = { query: _srcDims.query, content: _srcDims.query }; break;
              case "hash":
                _projDims = { hash: _srcDims.hash, content: _srcDims.hash }; break;
              case "href":
                _projDims = _srcDims; break;
            }
            if (_projDims) _curSource = _tvsHopDims(_curSource, "member", "." + _deepPropName, _propLoc, _projDims);
            else _curSource = _tvsHop(_curSource, "member", "." + _deepPropName, _propLoc);
          }
          return { done: _curSource };
        }
        // Fall through to next branch
        L.branchSkip = 6;
        F.state = _TVS_INIT;
        continue;
      }

      case _TVS_CMC_AFTER_LEAF: {
        var compObjSource = F.result;
        if (compObjSource && compObjSource.sourceType === "user-controlled") {
          var _compRes = compObjSource;
          for (var _chi = 0; _chi < L.compHops; _chi++) {
            _compRes = _tvsHop(_compRes, "member-computed", "[…]", nodeLoc);
          }
          return { done: _compRes };
        }
        L.branchSkip = 7;
        F.state = _TVS_INIT;
        continue;
      }

      case _TVS_IDENT_AFTER_INIT: {
        var initSrc = F.result;
        if (initSrc && initSrc.sourceType === "user-controlled") {
          return { done: _tvsHop(initSrc, "binding", L.identName, nodeLoc) };
        }
        // Process constantViolations if present
        if (L.identBinding.constantViolations && L.identBinding.constantViolations.length > 0) {
          L.identInitSrc = initSrc;
          L.identCvi = 0;
          F.state = _TVS_IDENT_CV_LOOP;
          continue;
        }
        // No CV — return initSrc as final
        return { done: initSrc };
      }

      case _TVS_IDENT_CV_LOOP: {
        // Find next AssignmentExpression CV to trace
        while (L.identCvi < L.identBinding.constantViolations.length) {
          var _cvPath = L.identBinding.constantViolations[L.identCvi];
          if (_cvPath && _cvPath.isAssignmentExpression()) {
            return { trace: _cvPath.get("right"), state: _TVS_IDENT_CV_AFTER };
          }
          L.identCvi++;
        }
        // Done — return initSrc
        return { done: L.identInitSrc };
      }

      case _TVS_IDENT_CV_AFTER: {
        var _cvSrc = F.result;
        if (_cvSrc && _cvSrc.sourceType === "user-controlled") {
          return { done: _tvsHop(_cvSrc, "reassign", L.identName, nodeLoc) };
        }
        L.identCvi++;
        F.state = _TVS_IDENT_CV_LOOP;
        continue;
      }

      case _TVS_IDENT_UCV_LOOP: {
        while (L.identUcvI < L.identBinding.constantViolations.length) {
          var _cvnPath = L.identBinding.constantViolations[L.identUcvI];
          if (_cvnPath && _cvnPath.isAssignmentExpression()) {
            return { trace: _cvnPath.get("right"), state: _TVS_IDENT_UCV_AFTER };
          }
          L.identUcvI++;
        }
        // CVs exhausted without user-controlled — fall through to for-iter /
        // destructure / param checks (matches original generator's sequence).
        F.state = _TVS_IDENT_TRY_OTHER;
        continue;
      }

      case _TVS_IDENT_UCV_AFTER: {
        var _cvnSrc = F.result;
        if (_cvnSrc && _cvnSrc.sourceType === "user-controlled") {
          return { done: _tvsHop(_cvnSrc, "reassign-uninit", L.identName, nodeLoc) };
        }
        L.identUcvI++;
        F.state = _TVS_IDENT_UCV_LOOP;
        continue;
      }

      case _TVS_IDENT_FOR_ITER_AFTER: {
        return { done: _tvsHop(F.result, "for-iter", "iterates " + L.identName, nodeLoc) };
      }

      case _TVS_IDENT_DESTR_OBJ_AFTER: {
        return { done: _tvsHop(F.result, "destructure-obj", "{" + L.identName + "}", nodeLoc) };
      }

      case _TVS_IDENT_DESTR_OBJ_FOR_AFTER: {
        return { done: _tvsHop(F.result, "destructure-obj-for", "{" + L.identName + "} of …", nodeLoc) };
      }

      case _TVS_IDENT_DESTR_ARR_AFTER: {
        return { done: _tvsHop(F.result, "destructure-arr", "[…" + L.identName + "…]", nodeLoc) };
      }

      case _TVS_IDENT_DESTR_ARR_FOR_AFTER: {
        return { done: _tvsHop(F.result, "destructure-arr-for", "[…" + L.identName + "…] of …", nodeLoc) };
      }

      case _TVS_IDENT_PARAM_IIFE_AFTER: {
        var _iifeArgSrc = F.result;
        if (_iifeArgSrc && _iifeArgSrc.sourceType === "user-controlled") {
          return { done: _tvsHop(_iifeArgSrc, "param-iife", "param " + L.identName + " @ arg " + L.paramIdx, nodeLoc) };
        }
        // Continue with caller args loop
        L.callerArgs = _findFunctionCallerArgs(L.funcPath);
        L.callerArgsCi = 0;
        F.state = _TVS_IDENT_PARAM_CALLERS_LOOP;
        continue;
      }

      case _TVS_IDENT_PARAM_CALLERS_LOOP: {
        while (L.callerArgsCi < L.callerArgs.length) {
          if (L.paramIdx < L.callerArgs[L.callerArgsCi].length) {
            return {
              trace: L.callerArgs[L.callerArgsCi][L.paramIdx],
              state: _TVS_IDENT_PARAM_CALLERS_AFTER,
            };
          }
          L.callerArgsCi++;
        }
        // Done — try iter-callback (paramIdx === 0) or reduce (paramIdx === 1)
        // For simplicity, dispatch to an additional state:
        F.state = _TVS_IDENT_PARAM_ITER_REF_LOOP;
        continue;
      }

      case _TVS_IDENT_PARAM_CALLERS_AFTER: {
        var argSource = F.result;
        if (argSource && argSource.sourceType === "user-controlled") {
          return { done: _tvsHop(argSource, "param-caller", "param " + L.identName + " @ arg " + L.paramIdx, nodeLoc) };
        }
        L.callerArgsCi++;
        F.state = _TVS_IDENT_PARAM_CALLERS_LOOP;
        continue;
      }

      case _TVS_IDENT_PARAM_ITER_REF_LOOP: {
        // Cover the iter-callback / iter-callback-ref / reduce-element paths
        // from the original. These traced the array (callee.object) when
        // paramIdx satisfied the iter pattern. Without these, deeply
        // wrapped iteration callbacks lose taint propagation.
        if (L.paramIdx === 0) {
          var _iterParent = L.funcPath.parentPath;
          if (_iterParent && _iterParent.isCallExpression() &&
              _t.isMemberExpression(_iterParent.node.callee) && !_iterParent.node.callee.computed &&
              _iterParent.node.arguments.length >= 1 && _iterParent.node.arguments[0] === L.funcPath.node) {
            var _iterMethod = _t.isIdentifier(_iterParent.node.callee.property)
              ? _iterParent.node.callee.property.name : null;
            if (_ITERATION_METHODS[_iterMethod] || _iterMethod === "then" || _iterMethod === "catch") {
              var _iterObjType = _getTrackedType(_iterParent.get("callee.object"), _iterParent.node.callee.object);
              if ((_iterMethod === "then" || _iterMethod === "catch") &&
                  _isNetworkProducingCall(_iterParent.get("callee.object"))) {
                // skip
              } else if (!(_ITERATION_METHODS[_iterMethod] && _iterObjType && _NON_ITERABLE_TYPES[_iterObjType])) {
                L.iterMethod = _iterMethod;
                return { trace: _iterParent.get("callee.object"), state: _TVS_IDENT_PARAM_ITER_AFTER };
              }
            }
          }
          // Iter-callback via reference: find function binding's reference paths
          var _iterBinding = _getFunctionBinding(L.funcPath);
          if (_iterBinding && _iterBinding.referencePaths) {
            L.iterRefBinding = _iterBinding;
            L.iterRefIri = 0;
            F.state = _TVS_IDENT_PARAM_ITER_REF_LOOP_INNER;
            continue;
          }
        }
        // reduce(fn, init): paramIdx === 1
        if (L.paramIdx === 1) {
          var _redParent = L.funcPath.parentPath;
          if (_redParent && _redParent.isCallExpression() &&
              _t.isMemberExpression(_redParent.node.callee) && !_redParent.node.callee.computed &&
              _redParent.node.arguments.length >= 1 && _redParent.node.arguments[0] === L.funcPath.node) {
            var _redMethod = _t.isIdentifier(_redParent.node.callee.property)
              ? _redParent.node.callee.property.name : null;
            if (_redMethod === "reduce" || _redMethod === "reduceRight") {
              var _redObjType = _getTrackedType(_redParent.get("callee.object"), _redParent.node.callee.object);
              if (!(_redObjType && _NON_ITERABLE_TYPES[_redObjType])) {
                L.iterMethod = _redMethod;
                return { trace: _redParent.get("callee.object"), state: _TVS_IDENT_PARAM_REDUCE_AFTER };
              }
            }
          }
        }
        // No iter / reduce match — fall through to msg-handler check.
        F.state = _TVS_IDENT_PARAM_TRY_MSG;
        continue;
      }

      case _TVS_IDENT_PARAM_ITER_REF_LOOP_INNER: {
        while (L.iterRefIri < L.iterRefBinding.referencePaths.length) {
          var _irRef = L.iterRefBinding.referencePaths[L.iterRefIri];
          var _irParent = _irRef.parentPath;
          if (_irParent && _irParent.isCallExpression() &&
              _t.isMemberExpression(_irParent.node.callee) && !_irParent.node.callee.computed &&
              _irParent.node.arguments.length >= 1 && _irParent.node.arguments[0] === _irRef.node) {
            var _irMethod = _t.isIdentifier(_irParent.node.callee.property)
              ? _irParent.node.callee.property.name : null;
            if (_ITERATION_METHODS[_irMethod] || _irMethod === "then" || _irMethod === "catch") {
              var _irObjType = _getTrackedType(_irParent.get("callee.object"), _irParent.node.callee.object);
              if ((_irMethod === "then" || _irMethod === "catch") &&
                  _isNetworkProducingCall(_irParent.get("callee.object"))) {
                // skip
              } else if (!(_ITERATION_METHODS[_irMethod] && _irObjType && _NON_ITERABLE_TYPES[_irObjType])) {
                L.iterMethod = _irMethod;
                return { trace: _irParent.get("callee.object"), state: _TVS_IDENT_PARAM_ITER_REF_AFTER };
              }
            }
          }
          L.iterRefIri++;
        }
        F.state = _TVS_IDENT_PARAM_TRY_MSG;
        continue;
      }

      case _TVS_IDENT_PARAM_ITER_AFTER: {
        var _iterObjSrc = F.result;
        if (_iterObjSrc && _iterObjSrc.sourceType === "user-controlled") {
          return { done: _tvsHop(_iterObjSrc, "iter-callback", "." + L.iterMethod + " element", nodeLoc) };
        }
        // Continue with iter-callback-ref loop
        var _iterBindingP = _getFunctionBinding(L.funcPath);
        if (_iterBindingP && _iterBindingP.referencePaths) {
          L.iterRefBinding = _iterBindingP;
          L.iterRefIri = 0;
          F.state = _TVS_IDENT_PARAM_ITER_REF_LOOP_INNER;
          continue;
        }
        F.state = _TVS_IDENT_PARAM_TRY_MSG;
        continue;
      }

      case _TVS_IDENT_PARAM_ITER_REF_AFTER: {
        var _irObjSrc = F.result;
        if (_irObjSrc && _irObjSrc.sourceType === "user-controlled") {
          return { done: _tvsHop(_irObjSrc, "iter-callback-ref", "." + L.iterMethod + " element (via ref)", nodeLoc) };
        }
        L.iterRefIri++;
        F.state = _TVS_IDENT_PARAM_ITER_REF_LOOP_INNER;
        continue;
      }

      case _TVS_IDENT_PARAM_REDUCE_AFTER: {
        var _redObjSrc = F.result;
        if (_redObjSrc && _redObjSrc.sourceType === "user-controlled") {
          return { done: _tvsHop(_redObjSrc, "reduce-element", "." + L.iterMethod + " element", nodeLoc) };
        }
        F.state = _TVS_IDENT_PARAM_TRY_MSG;
        continue;
      }

      case _TVS_IDENT_TRY_OTHER: {
        // Fall-through dispatcher: tries for-iter, destructure-obj/rest,
        // destructure-arr, and param branches in source order. Entered
        // after UCV_LOOP exhausted without finding a user-controlled
        // reassignment. Each sub-branch either returns trace + state or
        // falls through to the next via the local `if` chain below.
        var binding = L.identBinding;
        // For-in/for-of loop variable: for (var key in obj) → key inherits taint of obj.
        if (binding.path.isVariableDeclarator() && !binding.path.node.init) {
          var _forParent = binding.path.parentPath && binding.path.parentPath.parentPath;
          if (_forParent && (_t.isForInStatement(_forParent.node) || _t.isForOfStatement(_forParent.node)) &&
              _forParent.node.left === binding.path.parent) {
            L.identName = node.name;
            return { trace: _forParent.get("right"), state: _TVS_IDENT_FOR_ITER_AFTER };
          }
        }
        // Destructured ObjectProperty / RestElement
        if (_t.isObjectProperty(binding.path.node) || _t.isRestElement(binding.path.node)) {
          var _destrParent = binding.path.parentPath;
          while (_destrParent && !_destrParent.isVariableDeclarator()) {
            _destrParent = _destrParent.parentPath;
          }
          if (_destrParent && _destrParent.node.init) {
            L.identName = node.name;
            return { trace: _destrParent.get("init"), state: _TVS_IDENT_DESTR_OBJ_AFTER };
          }
          if (_destrParent && !_destrParent.node.init) {
            var _forOwner = _destrParent.parentPath && _destrParent.parentPath.parentPath;
            if (_forOwner && (_t.isForOfStatement(_forOwner.node) || _t.isForInStatement(_forOwner.node))) {
              L.identName = node.name;
              return { trace: _forOwner.get("right"), state: _TVS_IDENT_DESTR_OBJ_FOR_AFTER };
            }
          }
        }
        // Destructured ArrayPattern element
        if (_t.isArrayPattern(binding.path.parent)) {
          var _arrDestrParent = binding.path.parentPath;
          while (_arrDestrParent && !_arrDestrParent.isVariableDeclarator()) {
            _arrDestrParent = _arrDestrParent.parentPath;
          }
          if (_arrDestrParent && _arrDestrParent.node.init) {
            L.identName = node.name;
            return { trace: _arrDestrParent.get("init"), state: _TVS_IDENT_DESTR_ARR_AFTER };
          }
          if (_arrDestrParent && !_arrDestrParent.node.init) {
            var _forOwner2 = _arrDestrParent.parentPath && _arrDestrParent.parentPath.parentPath;
            if (_forOwner2 && (_t.isForOfStatement(_forOwner2.node) || _t.isForInStatement(_forOwner2.node))) {
              L.identName = node.name;
              return { trace: _forOwner2.get("right"), state: _TVS_IDENT_DESTR_ARR_FOR_AFTER };
            }
          }
        }
        // Function parameter
        if (binding.kind === "param") {
          var paramIdx = -1;
          var funcPath = binding.scope.path;
          if (funcPath && funcPath.node.params) {
            for (var pi = 0; pi < funcPath.node.params.length; pi++) {
              if (_t.isIdentifier(funcPath.node.params[pi], { name: node.name })) { paramIdx = pi; break; }
            }
            if (paramIdx === -1) {
              for (var _dpi = 0; _dpi < funcPath.node.params.length; _dpi++) {
                var _dpParam = funcPath.node.params[_dpi];
                if (_t.isObjectPattern(_dpParam)) {
                  if (_findDestructuredKey(_dpParam, node.name)) { paramIdx = _dpi; break; }
                }
                if (_t.isAssignmentPattern(_dpParam) && _t.isObjectPattern(_dpParam.left)) {
                  if (_findDestructuredKey(_dpParam.left, node.name)) { paramIdx = _dpi; break; }
                }
              }
            }
          }
          if (paramIdx >= 0 && funcPath) {
            L.identName = node.name;
            L.paramIdx = paramIdx;
            L.funcPath = funcPath;
            var _iifeParent = funcPath.parentPath;
            if (_iifeParent && _t.isCallExpression(_iifeParent.node) && _iifeParent.node.callee === funcPath.node &&
                paramIdx < _iifeParent.node.arguments.length) {
              return {
                trace: _iifeParent.get("arguments." + paramIdx),
                state: _TVS_IDENT_PARAM_IIFE_AFTER,
              };
            }
            L.callerArgs = _findFunctionCallerArgs(funcPath);
            L.callerArgsCi = 0;
            F.state = _TVS_IDENT_PARAM_CALLERS_LOOP;
            continue;
          }
          // No paramIdx — only msg-handler check applies if paramIdx === 0,
          // which it isn't here. Return DYN.
          return { done: DYN };
        }
        // Bare `location` identifier (unbound) — already handled in INIT
        return { done: DYN };
      }

      case _TVS_IDENT_PARAM_TRY_MSG: {
        // Final fallback for param-typed identifiers: cross-origin message
        // handler detection. paramIdx must be 0 and the function must be
        // attached as a "message" event handler. No recursion — purely
        // structural via _isCrossOriginMsgReceiver.
        if (L.paramIdx === 0 && L.funcPath) {
          var _isMsgHandler = false;
          var _mhParent = L.funcPath.parentPath;
          if (_mhParent) {
            if (_mhParent.isCallExpression()) {
              var _aeNode = _mhParent.node;
              if (_aeNode.arguments.length >= 2 && _aeNode.arguments[1] === L.funcPath.node &&
                  _t.isStringLiteral(_aeNode.arguments[0], { value: "message" })) {
                var _aeCal = _aeNode.callee;
                if ((_t.isMemberExpression(_aeCal) && !_aeCal.computed &&
                     _t.isIdentifier(_aeCal.property, { name: "addEventListener" })) ||
                    (_t.isIdentifier(_aeCal, { name: "addEventListener" }) &&
                     !L.funcPath.scope.getBinding("addEventListener"))) {
                  if (_isCrossOriginMsgReceiver(_aeCal, _mhParent)) _isMsgHandler = true;
                }
              }
            }
            if (_mhParent.isAssignmentExpression() && _mhParent.node.right === L.funcPath.node) {
              var _omLeft = _mhParent.node.left;
              if (_t.isMemberExpression(_omLeft) && !_omLeft.computed &&
                  _t.isIdentifier(_omLeft.property, { name: "onmessage" })) {
                if (_isCrossOriginMsgReceiver(_omLeft, _mhParent)) _isMsgHandler = true;
              }
            }
            if (!_isMsgHandler && _t.isReturnStatement(_mhParent.node)) {
              var _mhEncFunc = L.funcPath.findParent(function(p) { return p.isFunction() && p !== L.funcPath; });
              if (_mhEncFunc) {
                var _mhEncBinding = null;
                if (_t.isFunctionDeclaration(_mhEncFunc.node) && _t.isIdentifier(_mhEncFunc.node.id)) {
                  _mhEncBinding = _mhEncFunc.parentPath.scope.getBinding(_mhEncFunc.node.id.name);
                } else if (_mhEncFunc.parentPath && _mhEncFunc.parentPath.isVariableDeclarator()) {
                  _mhEncBinding = _mhEncFunc.parentPath.scope.getBinding(_mhEncFunc.parentPath.node.id.name);
                }
                if (_mhEncBinding) {
                  var _mhRefs = _mhEncBinding.referencePaths || [];
                  for (var _mhri = 0; _mhri < _mhRefs.length && !_isMsgHandler; _mhri++) {
                    var _mhRefParent = _mhRefs[_mhri].parentPath;
                    if (_mhRefParent && _mhRefParent.isCallExpression() && _mhRefParent.node.callee === _mhRefs[_mhri].node) {
                      var _mhCallParent = _mhRefParent.parentPath;
                      if (_mhCallParent && _mhCallParent.isCallExpression()) {
                        var _mhOuterCall = _mhCallParent.node;
                        if (_mhOuterCall.arguments.length >= 2 && _mhOuterCall.arguments[1] === _mhRefParent.node &&
                            _t.isStringLiteral(_mhOuterCall.arguments[0], { value: "message" })) {
                          var _mhOuterCallee = _mhOuterCall.callee;
                          if ((_t.isMemberExpression(_mhOuterCallee) && !_mhOuterCallee.computed &&
                               _t.isIdentifier(_mhOuterCallee.property, { name: "addEventListener" })) ||
                              (_t.isIdentifier(_mhOuterCallee, { name: "addEventListener" }) &&
                               !_mhCallParent.scope.getBinding("addEventListener"))) {
                            if (_isCrossOriginMsgReceiver(_mhOuterCallee, _mhCallParent)) _isMsgHandler = true;
                          }
                        }
                      }
                      if (_mhCallParent && _mhCallParent.isAssignmentExpression() && _mhCallParent.node.right === _mhRefParent.node) {
                        var _mhOmLeft2 = _mhCallParent.node.left;
                        if (_t.isMemberExpression(_mhOmLeft2) && !_mhOmLeft2.computed &&
                            _t.isIdentifier(_mhOmLeft2.property, { name: "onmessage" })) {
                          _isMsgHandler = true;
                        }
                      }
                    }
                  }
                }
              }
            }
          }
          if (!_isMsgHandler && _mhParent) {
            var _namedBinding = null;
            if (_mhParent.isVariableDeclarator() && _t.isIdentifier(_mhParent.node.id)) {
              _namedBinding = _mhParent.scope.getBinding(_mhParent.node.id.name);
            } else if (_mhParent.isAssignmentExpression() && _t.isIdentifier(_mhParent.node.left) &&
                       _mhParent.node.right === L.funcPath.node) {
              _namedBinding = _mhParent.scope.getBinding(_mhParent.node.left.name);
            }
            if (_namedBinding) {
              var _nbRefs = _namedBinding.referencePaths || [];
              for (var _nbri = 0; _nbri < _nbRefs.length && !_isMsgHandler; _nbri++) {
                var _nbRef = _nbRefs[_nbri];
                var _nbRefP = _nbRef.parentPath;
                if (!_nbRefP) continue;
                if (_nbRefP.isCallExpression() && _nbRefP.node.arguments.length >= 2 &&
                    _nbRefP.node.arguments[1] === _nbRef.node &&
                    _t.isStringLiteral(_nbRefP.node.arguments[0], { value: "message" })) {
                  var _nbCal = _nbRefP.node.callee;
                  if ((_t.isMemberExpression(_nbCal) && !_nbCal.computed &&
                       _t.isIdentifier(_nbCal.property, { name: "addEventListener" })) ||
                      (_t.isIdentifier(_nbCal, { name: "addEventListener" }) &&
                       !_nbRefP.scope.getBinding("addEventListener"))) {
                    if (_isCrossOriginMsgReceiver(_nbCal, _nbRefP)) _isMsgHandler = true;
                  }
                }
                if (_nbRefP.isAssignmentExpression() && _nbRefP.node.right === _nbRef.node) {
                  var _nbOmL = _nbRefP.node.left;
                  if (_t.isMemberExpression(_nbOmL) && !_nbOmL.computed &&
                      _t.isIdentifier(_nbOmL.property, { name: "onmessage" })) {
                    _isMsgHandler = true;
                  }
                }
              }
            }
          }
          if (_isMsgHandler) {
            return { done: _tvsSource("event.data", nodeLoc, _dimsForSource("event.data")) };
          }
        }
        return { done: DYN };
      }

      case _TVS_TL_AFTER: {
        var exprSource = F.result;
        if (exprSource && exprSource.sourceType === "user-controlled") {
          var _hopT = _tvsHop(exprSource, "template-interp", "`…${" + L.tlTi + "}…`", nodeLoc);
          if (L.tmplStripsOrigin && _hopT && _hopT.dimensions) {
            _hopT.dimensions = _tvsDims({
              path: _hopT.dimensions.path, query: _hopT.dimensions.query, hash: _hopT.dimensions.hash, content: true,
            });
          }
          return { done: _hopT };
        }
        L.tlTi++;
        if (L.tlTi < L.tlExprCount) {
          return { trace: path.get("expressions." + L.tlTi), state: _TVS_TL_AFTER };
        }
        return { done: L.tlExprCount > 0 ? DYN : LIT };
      }

      case _TVS_TT_AFTER: {
        var _ttExprSrc = F.result;
        if (_ttExprSrc && _ttExprSrc.sourceType === "user-controlled") {
          return { done: _tvsHop(_ttExprSrc, "tagged-template", "tag`…${" + L.ttTi + "}…`", nodeLoc) };
        }
        L.ttTi++;
        if (L.ttTi < L.ttExprCount) {
          return { trace: L.ttQuasiPath.get("expressions." + L.ttTi), state: _TVS_TT_AFTER };
        }
        // Fall through to next branch
        L.branchSkip = 10;
        F.state = _TVS_INIT;
        continue;
      }

      case _TVS_BIN_AFTER_LEAF: {
        var _binLeafSrc = F.result;
        var _binStrip = function(_h) {
          if (L.concatStripsOrigin && _h && _h.dimensions) {
            _h.dimensions = _tvsDims({
              path: _h.dimensions.path, query: _h.dimensions.query, hash: _h.dimensions.hash, content: true,
            });
          }
          return _h;
        };
        if (_binLeafSrc && _binLeafSrc.sourceType === "user-controlled") {
          var _binRes = _binLeafSrc;
          for (var _bli = L.binRights.length - 1; _bli >= 0; _bli--) {
            _binRes = _binStrip(_tvsHop(_binRes, "concat-left", "… + …", L.binRights[_bli].loc));
          }
          return { done: _binRes };
        }
        L.binBrj = L.binRights.length - 1;
        if (L.binBrj < 0) return { done: DYN };
        return { trace: L.binRights[L.binBrj].path, state: _TVS_BIN_RIGHTS_AFTER };
      }

      case _TVS_BIN_RIGHTS_AFTER: {
        var _brSrc = F.result;
        var _binStrip2 = function(_h) {
          if (L.concatStripsOrigin && _h && _h.dimensions) {
            _h.dimensions = _tvsDims({
              path: _h.dimensions.path, query: _h.dimensions.query, hash: _h.dimensions.hash, content: true,
            });
          }
          return _h;
        };
        if (_brSrc && _brSrc.sourceType === "user-controlled") {
          var _brRes = _binStrip2(_tvsHop(_brSrc, "concat-right", "… + …", L.binRights[L.binBrj].loc));
          for (var _blk = L.binBrj - 1; _blk >= 0; _blk--) {
            _brRes = _binStrip2(_tvsHop(_brRes, "concat-left", "… + …", L.binRights[_blk].loc));
          }
          return { done: _brRes };
        }
        L.binBrj--;
        if (L.binBrj >= 0) {
          return { trace: L.binRights[L.binBrj].path, state: _TVS_BIN_RIGHTS_AFTER };
        }
        return { done: DYN };
      }

      case _TVS_COND_AFTER_CONS: {
        var consSource = F.result;
        if (consSource && consSource.sourceType === "user-controlled") {
          return { done: _tvsHop(consSource, "ternary-then", "? … : _", nodeLoc) };
        }
        return { trace: path.get("alternate"), state: _TVS_COND_AFTER_ALT };
      }

      case _TVS_COND_AFTER_ALT: {
        var altSource = F.result;
        if (altSource && altSource.sourceType === "user-controlled") {
          return { done: _tvsHop(altSource, "ternary-else", "? _ : …", nodeLoc) };
        }
        // Fall through to next branch
        L.branchSkip = 12;
        F.state = _TVS_INIT;
        continue;
      }

      case _TVS_LOG_AFTER_LEAF: {
        var _logLeafSrc = F.result;
        if (_logLeafSrc && _logLeafSrc.sourceType === "user-controlled") {
          var _logRes = _logLeafSrc;
          for (var _lli = L.logRights.length - 1; _lli >= 0; _lli--) {
            _logRes = _tvsHop(_logRes, "logical-left", "… " + L.logRights[_lli].op + " …", L.logRights[_lli].loc);
          }
          return { done: _logRes };
        }
        L.logLrj = L.logRights.length - 1;
        if (L.logLrj < 0) {
          L.branchSkip = 13;
          F.state = _TVS_INIT;
          continue;
        }
        return { trace: L.logRights[L.logLrj].path, state: _TVS_LOG_RIGHTS_AFTER };
      }

      case _TVS_LOG_RIGHTS_AFTER: {
        var _lrSrc = F.result;
        if (_lrSrc && _lrSrc.sourceType === "user-controlled") {
          var _lrRes = _tvsHop(_lrSrc, "logical-right", "… " + L.logRights[L.logLrj].op + " …", L.logRights[L.logLrj].loc);
          for (var _llk = L.logLrj - 1; _llk >= 0; _llk--) {
            _lrRes = _tvsHop(_lrRes, "logical-left", "… " + L.logRights[_llk].op + " …", L.logRights[_llk].loc);
          }
          return { done: _lrRes };
        }
        L.logLrj--;
        if (L.logLrj >= 0) {
          return { trace: L.logRights[L.logLrj].path, state: _TVS_LOG_RIGHTS_AFTER };
        }
        // Fall through
        L.branchSkip = 13;
        F.state = _TVS_INIT;
        continue;
      }

      case _TVS_SEQ_AFTER: {
        return { done: _tvsHop(F.result, "sequence", "(_, …)", nodeLoc) };
      }

      case _TVS_AWAIT_AFTER: {
        return { done: _tvsHop(F.result, "await", "await …", nodeLoc) };
      }

      case _TVS_SPREAD_AFTER: {
        return { done: _tvsHop(F.result, "spread", "…value", nodeLoc) };
      }

      case _TVS_ASSIGN_RHS_AFTER: {
        return { done: _tvsHop(F.result, "assignment-rhs", "lhs = …", nodeLoc) };
      }

      case _TVS_CALL_METHOD_AFTER_OBJ: {
        var callObjSource = F.result;
        if (callObjSource && callObjSource.sourceType === "user-controlled") {
          var _callMethodName = L.callMethodName;
          var _argLit = L.callArgLit;
          var _hopDesc = "." + _callMethodName + "(" + (_argLit !== null ? JSON.stringify(_argLit) : "") + ")";
          var _sdM = callObjSource.dimensions || _tvsDimsContent();
          var _hasUrlPartDim = !!(_sdM.hash || _sdM.query || _sdM.path);
          var _mcProjDims;
          var _isPrefixPreservingName =
            _callMethodName === "toString" || _callMethodName === "valueOf" ||
            _callMethodName === "toLowerCase" || _callMethodName === "toUpperCase" ||
            _callMethodName === "normalize" ||
            _callMethodName === "trim" || _callMethodName === "trimStart" || _callMethodName === "trimEnd";
          var _isSliceFamily = _callMethodName === "slice" || _callMethodName === "substring" || _callMethodName === "substr";
          var _sliceStartZero = _isSliceFamily && node.arguments.length >= 1 &&
            _t.isNumericLiteral(node.arguments[0]) && node.arguments[0].value === 0;
          var _hasAnyAttackerDim = !!(_sdM.origin || _sdM.path || _sdM.query || _sdM.hash || _sdM.content);
          if (!_hasAnyAttackerDim) {
            _mcProjDims = { origin: false, content: false };
          } else if (_isPrefixPreservingName || _sliceStartZero) {
            _mcProjDims = {
              origin: _sdM.origin, path: _sdM.path, query: _sdM.query, hash: _sdM.hash,
              content: _sdM.origin || _sdM.path || _sdM.query || _sdM.hash || _sdM.content,
            };
          } else if (_callMethodName === "get" || _callMethodName === "getAll" || _callMethodName === "has") {
            _mcProjDims = { origin: _sdM.origin || _hasUrlPartDim, content: true };
          } else {
            _mcProjDims = { origin: _sdM.origin || _hasUrlPartDim, content: true };
          }
          var _hop = _mcProjDims
            ? _tvsHopDims(callObjSource, "method-call", _hopDesc, nodeLoc, _mcProjDims)
            : _tvsHop(callObjSource, "method-call", _hopDesc, nodeLoc);
          if (_argLit !== null && _hop && _hop.taintPath && _hop.taintPath.length) {
            _hop.taintPath[_hop.taintPath.length - 1].arg = _argLit;
            _hop.taintPath[_hop.taintPath.length - 1].method = _callMethodName;
          }
          return { done: _hop };
        }
        // Fall through to Object.assign / call-arg / function-return
        L.callNode = node;
        F.state = _TVS_CALL_OA_LOOP;
        continue;
      }

      case _TVS_CALL_OA_LOOP: {
        // Object.assign(target, ...sources): trace each arg
        if (_t.isMemberExpression(node.callee) && !node.callee.computed &&
            _t.isIdentifier(node.callee.property, { name: "assign" }) &&
            _t.isIdentifier(node.callee.object, { name: "Object" }) && !path.scope.getBinding("Object") &&
            node.arguments.length >= 2) {
          if (L.callOaIdx === undefined) L.callOaIdx = 0;
          if (L.callOaIdx < node.arguments.length) {
            return { trace: path.get("arguments." + L.callOaIdx), state: _TVS_CALL_OA_AFTER };
          }
        }
        // Call-arg processing
        F.state = _TVS_CALL_GENERIC_AFTER_DISPATCH;
        continue;
      }

      case _TVS_CALL_OA_AFTER: {
        var _oaArgSrc = F.result;
        if (_oaArgSrc && _oaArgSrc.sourceType === "user-controlled") {
          return { done: _tvsHop(_oaArgSrc, "object-assign-arg", "Object.assign arg " + L.callOaIdx, nodeLoc) };
        }
        L.callOaIdx++;
        F.state = _TVS_CALL_OA_LOOP;
        continue;
      }

      case _TVS_CALL_GENERIC_AFTER_DISPATCH: {
        // arg0 trace, with .get/.has gating
        if (node.arguments.length > 0) {
          if (_t.isMemberExpression(node.callee) && !node.callee.computed &&
              _t.isIdentifier(node.callee.property) &&
              (node.callee.property.name === "get" || node.callee.property.name === "has")) {
            var _ccmRecvType = _getTrackedType(path, node.callee.object);
            var _isUrlLikeLookup = _ccmRecvType === "URLSearchParams" ||
              _ccmRecvType === "Headers" || _ccmRecvType === "Request" || _ccmRecvType === "Response";
            if (_isUrlLikeLookup) {
              return { trace: path.get("arguments.0"), state: _TVS_CALL_GETHAS_AFTER };
            }
            // else fall through to function-return
          } else {
            return { trace: path.get("arguments.0"), state: _TVS_CALL_GENERIC_AFTER };
          }
        }
        // Function-return resolution (no recursion via _traceValueSource — uses _traceReturnsInBlock)
        var _calleeNode = node.callee;
        var _calleeFuncPath = null;
        if (_t.isIdentifier(_calleeNode)) {
          var _calleeBinding = path.scope.getBinding(_calleeNode.name);
          if (_calleeBinding) {
            if (_t.isFunctionDeclaration(_calleeBinding.path.node)) {
              _calleeFuncPath = _calleeBinding.path;
            } else if (_calleeBinding.path.isVariableDeclarator() && _calleeBinding.path.node.init &&
                       _t.isFunction(_calleeBinding.path.node.init)) {
              _calleeFuncPath = _calleeBinding.path.get("init");
            }
          }
        }
        if (_calleeFuncPath && _calleeFuncPath.node.body && _t.isBlockStatement(_calleeFuncPath.node.body)) {
          // Defer the function-return scan to the FN_RETURN_LOOP
          // state machine — collects return-arg paths and dispatches
          // each as a TVS sub-frame. Replaces a synchronous
          // _traceReturnsInBlock call that would close a static
          // recursion cycle TVS ↔ _traceReturnsInBlock.
          L.fnReturnArgPaths = _collectReturnArgPaths(_calleeFuncPath.get("body"));
          L.fnReturnAi = 0;
          L.fnReturnCalleeName = _t.isIdentifier(_calleeNode) ? _calleeNode.name + "()" : "call()";
          F.state = _TVS_FN_RETURN_LOOP;
          continue;
        }
        // Done with call branch — fall through to next branch
        L.branchSkip = 17;
        F.state = _TVS_INIT;
        continue;
      }

      case _TVS_CALL_GETHAS_AFTER: {
        var argSourceGH = F.result;
        if (argSourceGH && argSourceGH.sourceType === "user-controlled") {
          var _callCalleeNameGH = "." + node.callee.property.name + "()";
          return { done: _tvsHop(argSourceGH, "call-arg", _callCalleeNameGH + " arg 0", nodeLoc) };
        }
        // Fall through to function-return
        L.callOaIdx = node.arguments.length;  // skip OA loop
        F.state = _TVS_CALL_GENERIC_AFTER_DISPATCH_FN_RETURN;
        continue;
      }

      case _TVS_CALL_GENERIC_AFTER: {
        var argSourceG = F.result;
        if (argSourceG && argSourceG.sourceType === "user-controlled") {
          var _callCalleeNameG = _t.isIdentifier(node.callee) ? node.callee.name + "()" :
            (_t.isMemberExpression(node.callee) && _t.isIdentifier(node.callee.property) ? "." + node.callee.property.name + "()" : "call()");
          return { done: _tvsHop(argSourceG, "call-arg", _callCalleeNameG + " arg 0", nodeLoc) };
        }
        // Fall through to function-return
        F.state = _TVS_CALL_GENERIC_AFTER_DISPATCH_FN_RETURN;
        continue;
      }

      case _TVS_CALL_GENERIC_AFTER_DISPATCH_FN_RETURN: {
        var _calleeNodeF = node.callee;
        var _calleeFuncPathF = null;
        if (_t.isIdentifier(_calleeNodeF)) {
          var _calleeBindingF = path.scope.getBinding(_calleeNodeF.name);
          if (_calleeBindingF) {
            if (_t.isFunctionDeclaration(_calleeBindingF.path.node)) {
              _calleeFuncPathF = _calleeBindingF.path;
            } else if (_calleeBindingF.path.isVariableDeclarator() && _calleeBindingF.path.node.init &&
                       _t.isFunction(_calleeBindingF.path.node.init)) {
              _calleeFuncPathF = _calleeBindingF.path.get("init");
            }
          }
        }
        if (_calleeFuncPathF && _calleeFuncPathF.node.body && _t.isBlockStatement(_calleeFuncPathF.node.body)) {
          L.fnReturnArgPaths = _collectReturnArgPaths(_calleeFuncPathF.get("body"));
          L.fnReturnAi = 0;
          L.fnReturnCalleeName = _t.isIdentifier(_calleeNodeF) ? _calleeNodeF.name + "()" : "call()";
          F.state = _TVS_FN_RETURN_LOOP;
          continue;
        }
        L.branchSkip = 17;
        F.state = _TVS_INIT;
        continue;
      }

      case _TVS_FN_RETURN_LOOP: {
        if (L.fnReturnAi < L.fnReturnArgPaths.length) {
          return { trace: L.fnReturnArgPaths[L.fnReturnAi++], state: _TVS_FN_RETURN_AFTER };
        }
        // No tainted return found — fall through to next branch.
        L.branchSkip = 17;
        F.state = _TVS_INIT;
        continue;
      }

      case _TVS_FN_RETURN_AFTER: {
        var _retArgSrc = F.result;
        if (_retArgSrc && _retArgSrc.sourceType === "user-controlled") {
          return { done: _tvsHop(_retArgSrc, "function-return", "return of " + L.fnReturnCalleeName, nodeLoc) };
        }
        F.state = _TVS_FN_RETURN_LOOP;
        continue;
      }

      case _TVS_NEW_URL_AFTER_INPUT: {
        L.urlInputSrc = F.result;
        if (L.urlBasePath) {
          return { trace: L.urlBasePath, state: _TVS_NEW_URL_AFTER_BASE };
        }
        L.urlBaseSrc = null;
        F.state = _TVS_NEW_URL_RESOLVE;
        continue;
      }

      case _TVS_NEW_URL_AFTER_BASE: {
        L.urlBaseSrc = F.result;
        F.state = _TVS_NEW_URL_RESOLVE;
        continue;
      }

      case _TVS_NEW_URL_RESOLVE: {
        var _urlInputNode = L.urlInputNode;
        var _urlInputSrc = L.urlInputSrc;
        var _urlBaseSrc = L.urlBaseSrc;
        var _urlInputIsRelative =
          (_t.isStringLiteral(_urlInputNode) &&
            !/^https?:/i.test(_urlInputNode.value) &&
            !_urlInputNode.value.startsWith("//")) ||
          (_t.isTemplateLiteral(_urlInputNode) && (function () {
            var q = _urlInputNode.quasis;
            var h = (q[0] && q[0].value && q[0].value.raw) || "";
            return h !== "" && !/^https?:/i.test(h) && !h.startsWith("//");
          })());
        var _inputDims = _urlInputSrc && _urlInputSrc.dimensions ? _urlInputSrc.dimensions : null;
        var _baseDims = _urlBaseSrc && _urlBaseSrc.dimensions ? _urlBaseSrc.dimensions : null;
        var _outDims = {
          origin: _urlInputIsRelative
            ? !!(_baseDims && _baseDims.origin)
            : !!((_inputDims && _inputDims.origin) || (_baseDims && _baseDims.origin && !_urlInputIsRelative && !_urlInputSrc)),
          path: !!(_inputDims && _inputDims.path),
          query: !!(_inputDims && _inputDims.query),
          hash: !!(_inputDims && _inputDims.hash),
          content: !!((_inputDims && _inputDims.content) || (_baseDims && _baseDims.content && _urlInputIsRelative)),
        };
        if (_urlInputSrc && _urlInputSrc.sourceType === "user-controlled") {
          var h1 = _tvsHop(_urlInputSrc, "new-url-input", "new URL(input, …)", nodeLoc);
          if (h1) h1.dimensions = _tvsDims(_outDims);
          return { done: h1 };
        }
        if (_urlBaseSrc && _urlBaseSrc.sourceType === "user-controlled") {
          var h2 = _tvsHop(_urlBaseSrc, "new-url-base", "new URL(_, base)", nodeLoc);
          if (h2) h2.dimensions = _tvsDims(_outDims);
          return { done: h2 };
        }
        L.branchSkip = 18;
        F.state = _TVS_INIT;
        continue;
      }

      case _TVS_NEW_AFTER: {
        var _nArgSrc = F.result;
        if (_nArgSrc && _nArgSrc.sourceType === "user-controlled") {
          return { done: _tvsHop(_nArgSrc, "new-arg", "new " + L.newCalleeName + " arg " + L.newNi, nodeLoc) };
        }
        L.newNi++;
        if (L.newNi < L.newArgCount) {
          return { trace: path.get("arguments." + L.newNi), state: _TVS_NEW_AFTER };
        }
        L.branchSkip = 19;
        F.state = _TVS_INIT;
        continue;
      }

      case _TVS_ARR_AFTER: {
        var _aElSrc = F.result;
        if (_aElSrc && _aElSrc.sourceType === "user-controlled") {
          return { done: _tvsHop(_aElSrc, "array-elem", "[…" + L.arrAi + "…]", nodeLoc) };
        }
        L.arrAi++;
        while (L.arrAi < L.arrElCount && !node.elements[L.arrAi]) L.arrAi++;
        if (L.arrAi < L.arrElCount) {
          return { trace: path.get("elements." + L.arrAi), state: _TVS_ARR_AFTER };
        }
        L.branchSkip = 20;
        F.state = _TVS_INIT;
        continue;
      }

      case _TVS_OBJ_LOOP: {
        // Find next ObjectProperty with traceable value (or SpreadElement)
        while (L.objExprOpi < L.objExprPropsCount) {
          var _opProp = node.properties[L.objExprOpi];
          if (_t.isObjectProperty(_opProp)) {
            return {
              trace: path.get("properties." + L.objExprOpi + ".value"),
              state: _TVS_OBJ_PROP_AFTER,
            };
          }
          if (_t.isSpreadElement(_opProp)) {
            return {
              trace: path.get("properties." + L.objExprOpi + ".argument"),
              state: _TVS_OBJ_SPREAD_AFTER,
            };
          }
          L.objExprOpi++;
        }
        L.branchSkip = 21;
        F.state = _TVS_INIT;
        continue;
      }

      case _TVS_OBJ_PROP_AFTER: {
        var _opValSrc = F.result;
        if (_opValSrc && _opValSrc.sourceType === "user-controlled") {
          var _opProp2 = node.properties[L.objExprOpi];
          var _opPropKey = _t.isIdentifier(_opProp2.key) ? _opProp2.key.name :
            _t.isStringLiteral(_opProp2.key) ? _opProp2.key.value : "?";
          return { done: _tvsHop(_opValSrc, "obj-prop-value", "{" + _opPropKey + ": …}", nodeLoc) };
        }
        L.objExprOpi++;
        F.state = _TVS_OBJ_LOOP;
        continue;
      }

      case _TVS_OBJ_SPREAD_AFTER: {
        var _opSpreadSrc = F.result;
        if (_opSpreadSrc && _opSpreadSrc.sourceType === "user-controlled") {
          return { done: _tvsHop(_opSpreadSrc, "obj-spread", "{...…}", nodeLoc) };
        }
        L.objExprOpi++;
        F.state = _TVS_OBJ_LOOP;
        continue;
      }

      default:
        return { done: DYN };
    }
  }
}

// ─── Security Analysis: Helpers ──────────────────────────────────────────────

function _nodeLoc(n) {
  return { line: n.loc ? n.loc.start.line : 0, column: n.loc ? n.loc.start.column : 0 };
}

// Does this right-hand-side expression guarantee that any taint flowing
// through it only reaches the URL's query or fragment portion (never the
// scheme, host, or path)? Two common patterns:
//
//   location.href = `https://example.com/path?${taintedQuery}`
//   window.location.href = `${untaintedPermalink}?${taintedQuery}`
//
// The URL parser locks the scheme+authority+path as soon as it sees the
// first `?` or `#`; characters after can't upgrade the result to a
// `javascript:` URL or redirect to a different origin. Walk the template
// literal: track whether we've crossed a `?` or `#` delimiter in the
// static text, and for every interpolation BEFORE the delimiter require
// that its source is NOT user-controlled. Interpolations AFTER the
// delimiter can be freely tainted.
function _taintFlowsOnlyIntoUrlQueryOrHash(valuePath) {
  if (!valuePath || !valuePath.node) return false;
  var node = valuePath.node;
  if (!_t.isTemplateLiteral(node)) return false;
  if (!node.quasis || node.quasis.length === 0) return false;
  // A template literal has N quasis interleaved with N-1 expressions:
  //   `q0${e0}q1${e1}q2` → [q0, q1, q2] and [e0, e1]
  var crossedDelim = /[?#]/.test((node.quasis[0].value && node.quasis[0].value.raw) || "");
  for (var i = 0; i < (node.expressions || []).length; i++) {
    if (!crossedDelim) {
      // This interpolation lands BEFORE any `?`/`#` — if it's tainted the
      // attacker can reach scheme/host/path. Refuse the downgrade.
      var exprSrc = _traceValueSource(valuePath.get("expressions." + i), 0);
      if (exprSrc.sourceType === "user-controlled") return false;
    }
    var quasiRaw = (node.quasis[i + 1] && node.quasis[i + 1].value && node.quasis[i + 1].value.raw) || "";
    if (!crossedDelim && /[?#]/.test(quasiRaw)) crossedDelim = true;
  }
  // Only honour the downgrade if a delimiter was eventually seen — a
  // template that never hits `?`/`#` could be a full-URL taint sink.
  return crossedDelim;
}

// For a Binary+ node, walk the leftmost chain to find the leftmost
// leaf. If it's a same-origin-relative string literal (starts with "/"
// but not "//", or "./" / "../"), the concatenation's origin dim is
// LOCKED to same-origin by URL-parser structure. Used by the Binary+
// dim-propagation rule to strip origin from propagated dims.
function _leftmostLiteralIsSameOriginPrefix(binPath) {
  if (!binPath || !binPath.node) return false;
  var n = binPath.node;
  // Iteratively walk down the `left` chain while it stays a Binary+.
  while (_t.isBinaryExpression(n) && n.operator === "+") { n = n.left; }
  if (!_t.isStringLiteral(n)) return false;
  var v = n.value;
  if (!v) return false;
  if (/^https?:/i.test(v)) return false;            // absolute URL literal — different origin possible
  if (v.startsWith("//")) return false;             // protocol-relative — different origin possible
  if (v.startsWith("/") || v.startsWith("./") || v.startsWith("../")) return true;
  return false;                                     // arbitrary text prefix — can't prove same-origin
}

// Dim-based downgrade check: a taint whose dims still carry a URL-part
// marker (`hash`/`query`/`path`) AND has `origin=false` is structurally
// confined to that URL part. Returns true for sources like
// `location.hash` used without `.slice(1)` — still carries its "#"
// delimiter — but false for `location.hash.slice(1)` (origin upgraded by
// prefix-strip) or plain content (hash/query/path dims all false).
function _dimsRetainUrlPartNoOrigin(src) {
  if (!src || !src.dimensions) return false;
  var d = src.dimensions;
  if (d.origin) return false;
  return !!(d.hash || d.query || d.path);
}

// Walk a value expression looking for a "current-origin lock" prefix in
// LEFTMOST position of a Binary+ chain. The URL parser consumes `+`
// concatenations left-to-right; if the leftmost leaf resolves to a
// browser-validated whole-URL read (`location.origin`,
// `window.location.href`, `document.URL`, etc.), the assembled string's
// scheme+host are pinned to the current page regardless of what's
// appended on the right.
//
// Templates are handled by the separate `_taintFlowsOnlyIntoUrlQueryOrHash`
// downgrade path (string-literal prefix → MEDIUM, not suppress), so this
// walker intentionally stops at Binary+ chains and identifier bindings —
// it doesn't recurse into TemplateLiteral quasis.
// Iterative loop unwrapping `+` left-spine and Identifier alias chain.
// Both branches were tail-recursive; now both update valuePath via
// `continue` to re-enter the loop without growing the JS call stack.
function _valueHasSameOriginPrefix(valuePath, visited) {
  if (!valuePath || !valuePath.node) return false;
  visited = visited || new Set();
  while (true) {
    if (!valuePath || !valuePath.node) return false;
    var node = valuePath.node;
    if (visited.has(node)) return false;
    visited.add(node);
    var scope = valuePath.scope;
    if (_t.isBinaryExpression(node) && node.operator === "+") {
      valuePath = valuePath.get("left");
      continue;
    }
    if (_t.isIdentifier(node)) {
      var binding = scope.getBinding(node.name);
      if (binding && binding.path && binding.path.isVariableDeclarator() && binding.path.node.init) {
        valuePath = binding.path.get("init");
        continue;
      }
      return false;
    }
    return _isSameOriginBaseExpr(node, scope);
  }
}

function _pushSink(result, node, type, sink, src, path, options) {
  // Dimensional gate for sinks whose attack vector requires attacker
  // control of the URL scheme. If the taint's `origin` dim is false
  // (e.g. `new URL(x, location.href).toString()` locks the scheme
  // to same-origin), the attacker cannot inject `javascript:` and
  // the finding is a false positive.
  //   request-forgery:*           — cross-origin fetch/beacon/XHR
  //   xss:setAttribute:href/src/action/formaction  — javascript: attr
  //   xss:href / xss:src / xss:action   — direct property assignment
  // Redirect sinks retain the separate `_taintFlowsOnlyIntoUrlQueryOrHash`
  // severity-downgrade path (they report medium rather than suppress
  // because reflected query content on a same-origin redirect is still
  // a signal worth surfacing).
  if (src && src.dimensions && !src.dimensions.origin) {
    const _schemeSink =
      type === "request-forgery" ||
      (type === "xss" && (
        sink === "setAttribute:href" || sink === "setAttribute:src" ||
        sink === "setAttribute:action" || sink === "setAttribute:formaction" ||
        sink === "href" || sink === "src" || sink === "action"
      ));
    if (_schemeSink) return;
  }
  // All-dims-false: the attacker-reach label survived but EVERY dim
  // got stripped along the way (e.g. `new URL(location.href).protocol`
  // reads a static scheme string — no user-content at all). A value
  // with no dims carries no attacker control; flagging it produces
  // a sink finding no reviewer can act on. Real-world FP: react-router
  // RSCErrorHandler 20-hop chain through `.protocol`/`.pathname`.
  if (src && src.dimensions) {
    var _d = src.dimensions;
    if (!_d.origin && !_d.path && !_d.query && !_d.hash && !_d.content) return;
  }
  var severity = "high";
  var sanitized = false;
  var sanitizerReport = null;
  if (path) {
    try {
      sanitized = _checkSanitization(path);
      if (sanitized) severity = "info";
    } catch (e) { _resolver.collectError(e, "checkSanitization"); }
    try {
      sanitizerReport = _buildSanitizerReport(path);
    } catch (e) { _resolver.collectError(e, "sanitizerReport"); }
  }
  // An explicit severity override (e.g. redirect where taint provably
  // flows only into the URL's query/hash portion — scheme locked to an
  // untainted prefix) wins as long as the call isn't already sanitized.
  // Sanitised paths stay "info" since that signals a verified mitigation.
  var notes = null;
  if (!sanitized && options) {
    if (options.severity) severity = options.severity;
    if (options.notes) notes = options.notes;
  }
  var entry = {
    type: type, sink: sink, location: _nodeLoc(node),
    sourceType: src.sourceType, source: src.source, severity: severity,
    sanitized: sanitized,
    codeContext: _extractCodeContext(node, src),
  };
  // Taint dims at the point the value reaches the sink — surfaced so
  // a reviewer inspecting a finding can see at a glance which dimensions
  // the attacker still controls (e.g. `{query:true, content:true}` vs
  // `{path:true}` only). All-dims-false would have suppressed above.
  if (src && src.dimensions) {
    var _trueDims = [];
    if (src.dimensions.origin) _trueDims.push("origin");
    if (src.dimensions.path) _trueDims.push("path");
    if (src.dimensions.query) _trueDims.push("query");
    if (src.dimensions.hash) _trueDims.push("hash");
    if (src.dimensions.content) _trueDims.push("content");
    entry.sinkDims = _trueDims;
  }
  // Receiver type at the sink: helps reviewers tell `d.innerHTML = html`
  // on a `document.getElementById()` Element (real XSS) from the same
  // shape on an unknown object. Derived structurally from the sink's
  // AST node — AssignmentExpression's left.object or CallExpression's
  // callee.object — using the same `_getTrackedType` used by sink-
  // detection skips. `null` means no tracked type was inferred (most
  // common; reviewer still needs to check the binding).
  if (path) {
    try {
      var _rcvNode = null;
      var _sinkNode = path.node;
      if (_t.isAssignmentExpression(_sinkNode) && _t.isMemberExpression(_sinkNode.left)) {
        _rcvNode = _sinkNode.left.object;
      } else if (_t.isCallExpression(_sinkNode) && _t.isMemberExpression(_sinkNode.callee)) {
        _rcvNode = _sinkNode.callee.object;
      }
      if (_rcvNode) {
        var _rcvType = _getTrackedType(path, _rcvNode);
        if (_rcvType) entry.receiverType = _rcvType;
      }
    } catch (e) { _resolver.collectError(e, "receiverType"); }
  }
  if (src.taintPath && src.taintPath.length > 0) entry.taintPath = src.taintPath;
  if (sanitizerReport && sanitizerReport.candidates.length > 0) entry.sanitizerReport = sanitizerReport;
  if (notes) entry.notes = notes;
  if (path && src.sourceType === "user-controlled") {
    try {
      var precs = _collectSinkPreconditions(path, src);
      if (precs.length) entry.preconditions = precs;
    } catch (e) { _resolver.collectError(e, "collectSinkPreconditions"); }
  }
  result.securitySinks.push(entry);
}

// Walk up from the sink path looking for gating guards. Three guard
// shapes are recognised — all express "the attacker's value at path X
// must equal literal Y for execution to reach the sink":
//   1. `if (<guard>) { … sink … }`  — guard in test, sink in consequent.
//   2. `switch (<disc>) { case Y: … sink … }`  — sink in that case arm.
//   3. Early-return guard: within the sink's enclosing function, a
//      preceding `if (<disc> !== Y) return` (or `throw`) equivalently
//      pins <disc> to Y on the flow that reaches the sink.
// The recorded path is {path, op, value}; path is the property chain
// from the taint root (matching the harness's fieldPath convention).
function _collectSinkPreconditions(sinkPath, sinkSrc) {
  var out = [];
  // Walk all ancestors in the current function AND cross function
  // boundaries via caller sites (inter-procedural). Performance comes
  // from memoising `_memberChainFromSameSource` — which re-runs
  // `_traceValueSource` on each guard's test LHS — inside a single
  // sink-level cache, not from truncating the walk.
  var gateCache = _taintGateCacheForSink();
  _walkGuardsFromPath(sinkPath, sinkSrc, out, gateCache, new Set());
  return out;
}

function _taintGateCacheForSink() {
  // {nodeKey: {chain|null}}  cache for _memberChainFromSameSource.
  // Skipped-result is `null` (node traced but wasn't from same source);
  // `undefined` = not-yet-computed. The explicit null check avoids
  // recomputing on every precondition scan within one sink.
  return Object.create(null);
}

// Walk ancestors of `start`, collecting gates. When we hit a function
// boundary, use binding.referencePaths to find ALL call sites and
// recurse the walk from each call site. Cycle-detection via
// `visitedFns` (set of Function path nodes we've already expanded).
// Iterative worklist of starting paths. The original recursed at each
// function-boundary crossing (once per caller). Worklist accumulates
// caller starts; outer loop processes one start at a time, walking up
// its ancestor chain and pushing more starts when crossing functions.
function _walkGuardsFromPath(start, sinkSrc, out, gateCache, visitedFns) {
  var workStarts = [start];
  while (workStarts.length > 0) {
    var ancestor = workStarts.shift();
    while (ancestor) {
      var parent = ancestor.parentPath;
      if (!parent) break;
      // Shape 1: IfStatement / Conditional whose consequent contains the sink.
      if (parent.isIfStatement() && parent.node.consequent &&
          (parent.node.consequent === ancestor.node ||
           _nodeContains(parent.node.consequent, ancestor.node))) {
        _collectEqualities(parent.get("test"), sinkSrc, out, gateCache);
      } else if (parent.isConditionalExpression() && parent.node.consequent === ancestor.node) {
        _collectEqualities(parent.get("test"), sinkSrc, out, gateCache);
      }
      // Shape 2: SwitchCase.
      if (parent.isSwitchCase() && parent.node.test) {
        var caseLit = _asLiteralValue(parent.node.test);
        if (caseLit !== undefined && caseLit !== null) {
          var sw = parent.parentPath;
          if (sw && sw.isSwitchStatement()) {
            var chain = _memberChainFromSameSourceCached(sw.get("discriminant"), sinkSrc, gateCache);
            if (chain) out.push({ path: chain, op: "===", value: caseLit });
          }
        }
      }
      // Shape 3: early-return gates in enclosing block.
      if (parent.isBlockStatement() && Array.isArray(parent.node.body)) {
        for (var ri = 0; ri < parent.node.body.length; ri++) {
          var stmt = parent.node.body[ri];
          if (stmt === ancestor.node) break;
          _extractEarlyReturnGate(parent.get("body." + ri), sinkSrc, out, gateCache);
        }
      }
      // Inter-procedural: function boundary — push each caller onto the
      // worklist instead of recursing. Stops the current ancestor walk
      // since callers handle everything above the function.
      if (parent.isFunction()) {
        var fnKey = parent.node.start + ":" + parent.node.end;
        if (visitedFns.has(fnKey)) break;
        visitedFns.add(fnKey);
        var refs = _callerReferencePaths(parent);
        for (var ci = 0; ci < refs.length; ci++) {
          var callPath = refs[ci].findParent(function(p) { return p.isCallExpression(); });
          workStarts.push(callPath || refs[ci]);
        }
        break;
      }
      ancestor = parent;
    }
  }
}

// Resolve the callers of a function path. Handles FunctionDeclaration,
// and function expressions assigned to `var/let/const` or to object
// properties. Returns an array of Identifier reference paths.
function _callerReferencePaths(fnPath) {
  var refs = [];
  if (!fnPath) return refs;
  var binding = null;
  var n = fnPath.node;
  if (_t.isFunctionDeclaration(n) && _t.isIdentifier(n.id)) {
    binding = fnPath.scope.getBinding(n.id.name);
  } else if (fnPath.parentPath) {
    var pp = fnPath.parentPath;
    if (pp.isVariableDeclarator() && _t.isIdentifier(pp.node.id)) {
      binding = pp.scope.getBinding(pp.node.id.name);
    } else if (pp.isAssignmentExpression() && _t.isIdentifier(pp.node.left)) {
      binding = pp.scope.getBinding(pp.node.left.name);
    }
  }
  if (binding && Array.isArray(binding.referencePaths)) {
    for (var i = 0; i < binding.referencePaths.length; i++) refs.push(binding.referencePaths[i]);
  }
  return refs;
}

// `if (<lhs> !== 'Y') return;`  pins <lhs> to 'Y' on the fall-through.
// Also handles throw / continue / break as terminators.
function _extractEarlyReturnGate(stmtPath, sinkSrc, out, gateCache) {
  if (!stmtPath || !stmtPath.isIfStatement()) return;
  var node = stmtPath.node;
  // The consequent must be a hard exit (return/throw) — otherwise the
  // fall-through doesn't pin anything.
  var cons = node.consequent;
  if (!cons) return;
  var consIsExit = false;
  if (_t.isReturnStatement(cons) || _t.isThrowStatement(cons)) consIsExit = true;
  else if (_t.isBlockStatement(cons) && cons.body.length >= 1) {
    var last = cons.body[cons.body.length - 1];
    if (_t.isReturnStatement(last) || _t.isThrowStatement(last)) consIsExit = true;
  }
  if (!consIsExit) return;
  var test = node.test;
  if (!test) return;
  // Invert the test — `!== 'Y'` on the exit branch ↔ `=== 'Y'` on fall-through.
  if (_t.isBinaryExpression(test) && (test.operator === "!==" || test.operator === "!=")) {
    var leftLit = _asLiteralValue(test.left);
    var rightLit = _asLiteralValue(test.right);
    var memberPath = null; var literalValue;
    if (leftLit === undefined && rightLit !== undefined) { memberPath = stmtPath.get("test.left"); literalValue = rightLit; }
    else if (rightLit === undefined && leftLit !== undefined) { memberPath = stmtPath.get("test.right"); literalValue = leftLit; }
    else return;
    var chain = _memberChainFromSameSourceCached(memberPath, sinkSrc, gateCache);
    if (chain) out.push({ path: chain, op: "===", value: literalValue });
  }
  // `if (!X) return` — only tells us X is truthy, no specific value.
  // Skip: a truthy precondition doesn't pin the probe payload.
}

// Cache wrapper. The uncached variant re-runs _traceValueSource on
// every guard's test expression — one sink inside nested blocks with
// many preceding-sibling gates can generate 100s of redundant traces.
// Keyed by start:end position of the guard LHS + the source name.
function _memberChainFromSameSourceCached(memPath, sinkSrc, gateCache) {
  if (!gateCache || !memPath || !memPath.node) return _memberChainFromSameSource(memPath, sinkSrc);
  var n = memPath.node;
  if (n.start == null || n.end == null) return _memberChainFromSameSource(memPath, sinkSrc);
  var key = n.start + ":" + n.end + "|" + (sinkSrc && sinkSrc.source ? sinkSrc.source : "");
  if (key in gateCache) return gateCache[key];
  var chain = _memberChainFromSameSource(memPath, sinkSrc);
  gateCache[key] = chain;
  return chain;
}

function _nodeContains(outer, target) {
  if (!outer || !target) return false;
  if (outer === target) return true;
  if (target.start == null || outer.start == null) return false;
  return target.start >= outer.start && target.end <= outer.end;
}

// test can be BinaryExpression (`===`, `==`, `!==`, `!=`) or
// LogicalExpression (`&&`). Recurse into && to collect conjuncts.
// Iterative collect: walk the `&&` chain via explicit work stack
// (a && b && c && d nests N levels deep — recursion would grow JS call
// stack linearly with chain length).
function _collectEqualities(testPath, sinkSrc, out, gateCache) {
  if (!testPath || !testPath.node) return;
  var stack = [testPath];
  while (stack.length > 0) {
    var p = stack.pop();
    if (!p || !p.node) continue;
    var n = p.node;
    if (_t.isLogicalExpression(n) && n.operator === "&&") {
      // Push right then left so left is processed first (LIFO).
      stack.push(p.get("right"));
      stack.push(p.get("left"));
      continue;
    }
    if (_t.isBinaryExpression(n) && (n.operator === "===" || n.operator === "==" ||
        n.operator === "!==" || n.operator === "!=")) {
      if (n.operator !== "===" && n.operator !== "==") continue;
      var leftLit = _asLiteralValue(n.left);
      var rightLit = _asLiteralValue(n.right);
      var memberPath = null; var literalValue;
      if (leftLit === undefined && rightLit !== undefined) { memberPath = p.get("left"); literalValue = rightLit; }
      else if (rightLit === undefined && leftLit !== undefined) { memberPath = p.get("right"); literalValue = leftLit; }
      else continue;
      var memChain = _memberChainFromSameSourceCached(memberPath, sinkSrc, gateCache);
      if (memChain) out.push({ path: memChain, op: "===", value: literalValue });
    }
  }
}

// Returns the literal value, or `undefined` if the node isn't a literal.
// (Can't use `null` as sentinel because `null` is a valid literal value.)
function _asLiteralValue(node) {
  if (!node) return undefined;
  if (_t.isStringLiteral(node) || _t.isNumericLiteral(node) || _t.isBooleanLiteral(node)) return node.value;
  if (_t.isNullLiteral(node)) return null;
  if (_t.isTemplateLiteral(node) && node.expressions.length === 0 && node.quasis.length === 1) {
    return node.quasis[0].value.cooked;
  }
  return undefined;
}

// Resolve a member-expression chain like `msg.data.action` back to the
// SAME source as sinkSrc (source name match). Returns the property
// name chain after stripping the `.data` hop for MessageEvent sources,
// so callers can merge it with the probe's fieldPath-style wrapping.
function _memberChainFromSameSource(memPath, sinkSrc) {
  if (!memPath || !memPath.node) return null;
  // Follow Identifier bindings first: `let action = msg.data.action` →
  // trace via _traceValueSource so we get the full original chain.
  var tvs;
  try { tvs = _traceValueSource(memPath); }
  catch (_) { return null; }
  if (!tvs || tvs.sourceType !== "user-controlled") return null;
  if (tvs.source !== sinkSrc.source) return null;
  // Walk the taintPath's member hops to reconstruct the chain.
  var chain = [];
  var tp = tvs.taintPath || [];
  for (var i = 0; i < tp.length; i++) {
    if (tp[i].kind !== "member") continue;
    var nm = String(tp[i].desc || "").replace(/^\./, "");
    if (!nm || nm === "?") continue;
    // Drop the implicit `.data` hop on MessageEvent sources — matches
    // the probe's field-path stripping in the harness extractor.
    if (chain.length === 0 && nm === "data" && /event\.data/.test(sinkSrc.source || "")) continue;
    chain.push(nm);
  }
  return chain.length ? chain : null;
}

function _pushDangerous(result, node, type, description, severity, src) {
  // All-dims-false gate: same as _pushSink. The "user-controlled"
  // label can survive a chain that strips every dim along the way
  // (e.g. `Object.entries(reactRouterResult)` where the result object
  // came from a server-side function whose output is just `{type:
  // "error", error: e}` and dims got reset). Flagging produces an
  // unactionable HIGH that the reviewer can't trace back to anything
  // attacker-controllable. Real-world FP: 110-hop react-router proto-
  // pollution chains where dims=={none} at the sink.
  if (src && src.dimensions) {
    var _d = src.dimensions;
    if (!_d.origin && !_d.path && !_d.query && !_d.hash && !_d.content) return;
  }
  var entry = {
    type: type, description: description, location: _nodeLoc(node),
    severity: severity, codeContext: _extractCodeContext(node, src),
  };
  // Surface source/sourceType like _pushSink does. Without these,
  // finding.exploit can't pick a probe strategy for prototype-pollution
  // and other dangerousPattern findings that ARE routed from a user-
  // controlled source (location.hash, event.data, etc.) — it falls
  // through to "no probe strategy yet for source null" even though the
  // source is known.
  if (src && src.sourceType) entry.sourceType = src.sourceType;
  if (src && src.source) entry.source = src.source;
  if (src && src.dimensions) {
    var _dngDims = [];
    if (src.dimensions.origin) _dngDims.push("origin");
    if (src.dimensions.path) _dngDims.push("path");
    if (src.dimensions.query) _dngDims.push("query");
    if (src.dimensions.hash) _dngDims.push("hash");
    if (src.dimensions.content) _dngDims.push("content");
    entry.sinkDims = _dngDims;
  }
  if (src && src.taintPath && src.taintPath.length > 0) entry.taintPath = src.taintPath;
  result.dangerousPatterns.push(entry);
}

// Does this CallExpression path produce a Promise whose resolved value is a
// server-controlled response where the SERVER'S ORIGIN cannot be chosen
// by an attacker? If yes, taint doesn't propagate from the URL argument
// to the `.then(response)` callback — the response bytes come from YOUR
// server (reflected XSS is a server-side bug, not a client DOM sink).
//
// If NO — i.e. the URL's origin could be attacker-controlled (e.g.
// `fetch(location.hash)`, `fetch(attackerInput)`) — taint MUST propagate
// because the attacker can host the response on their own server.
//
// Conservative default: return false (keep taint) unless we can prove the
// fetch target is same-origin.
// Iterative .then/.catch/.finally chain unwind. The original recursed
// once per chain link; minified Promise pipelines can be deep.
function _isNetworkProducingCall(callPath) {
  if (!callPath || !callPath.node) return false;
  while (true) {
    if (!callPath || !callPath.node) return false;
    var node = callPath.node;
    if (!_t.isCallExpression(node)) return false;
    var callee = node.callee;

    // Direct fetch.
    var isBareFetch = _t.isIdentifier(callee, { name: "fetch" }) && !callPath.scope.getBinding("fetch");
    var isScopedFetch = _t.isMemberExpression(callee) && !callee.computed &&
      _t.isIdentifier(callee.property, { name: "fetch" }) &&
      _t.isIdentifier(callee.object) &&
      (callee.object.name === "window" || callee.object.name === "self" || callee.object.name === "globalThis") &&
      !callPath.scope.getBinding(callee.object.name);
    if (isBareFetch || isScopedFetch) {
      if (node.arguments.length === 0) return false;
      return _isSameOriginFetchTarget(callPath.get("arguments.0"));
    }

    // Promise chain: unwrap to receiver via loop continuation.
    if (_t.isMemberExpression(callee) && !callee.computed && _t.isIdentifier(callee.property)) {
      var m = callee.property.name;
      if (m === "then" || m === "catch" || m === "finally") {
        callPath = callPath.get("callee.object");
        continue;
      }
      if (m === "json" || m === "text" || m === "blob" || m === "arrayBuffer" || m === "formData") {
        return false;
      }
    }
    return false;
  }
}

// Does the URL argument to fetch() provably target a same-origin resource?
// Returns true only when we can statically determine the target origin is
// the page's own origin (so the response is under the page's trust). Any
// uncertainty → return false so taint keeps propagating (safe default).
//
// Recognized same-origin shapes:
//   "/absolute/path" or "./relative" or "relative" string literals without scheme
//   `/path-with-${tainted-segments}` — path is relative
//   new URL(x, location.origin) / new URL(x, window.location.origin) / new URL(x, self.location.origin)
//   new URL(x, document.baseURI) where base is same-origin (rely on server-set base tag)
//   URL identifier that resolves to one of the above
// Iterative: tail-recursive unwrap loop. The original recursed twice
// (Identifier→init alias chain, .toString() receiver). Loop walks both
// patterns; argPath/node updated each iteration until a terminal shape is
// reached. JS call stack stays at depth 1.
function _isSameOriginFetchTarget(argPath) {
  if (!argPath || !argPath.node) return false;
  var _isofVisited = new Set();
  while (true) {
    if (!argPath || !argPath.node) return false;
    if (_isofVisited.has(argPath.node)) return false;
    _isofVisited.add(argPath.node);
    var node = argPath.node;

  // String literal — same-origin only if NOT absolute and NOT protocol-relative.
  if (_t.isStringLiteral(node)) {
    var v = node.value;
    if (/^https?:/i.test(v)) return false;              // absolute URL → foreign origin possible
    if (v.startsWith("//")) return false;               // protocol-relative → foreign origin
    return true;                                        // path-only
  }

  // Template literal: first quasi determines the prefix.
  if (_t.isTemplateLiteral(node)) {
    var quasis = node.quasis;
    if (!quasis.length) return false;
    var head = (quasis[0] && quasis[0].value && quasis[0].value.raw) || "";
    if (/^https?:/i.test(head)) return false;
    if (head.startsWith("//")) return false;
    if (head.startsWith("/") || head.startsWith("./") || head.startsWith("../")) return true;
    // Starts with `${expr}...` — can't tell what expr resolves to.
    if (head === "") return false;
    // Non-absolute relative path
    return true;
  }

  // `new URL(input, base)` — same-origin iff the BASE is a known same-origin
  // AND the INPUT doesn't override the base origin (absolute / protocol-
  // relative strings override). Only recognize the input-is-safe case
  // when we can see it's a relative literal or relative-prefixed template.
  // Deeper question of "can attacker control origin" is answered by the
  // dimensional taint model at the sink layer, not here.
  if (_t.isNewExpression(node) && _t.isIdentifier(node.callee, { name: "URL" }) &&
      !argPath.scope.getBinding("URL") && node.arguments.length >= 2) {
    var baseArg = node.arguments[1];
    if (_isSameOriginBaseExpr(baseArg, argPath.scope)) {
      var inputArg = node.arguments[0];
      if (_t.isStringLiteral(inputArg) && !/^https?:/i.test(inputArg.value) && !inputArg.value.startsWith("//")) {
        return true;
      }
      if (_t.isTemplateLiteral(inputArg)) {
        var qs = inputArg.quasis;
        var ihead = (qs[0] && qs[0].value && qs[0].value.raw) || "";
        if (!/^https?:/i.test(ihead) && !ihead.startsWith("//") && ihead !== "") return true;
      }
      return false;
    }
  }

  // Identifier — unwrap to init via loop continuation.
  if (_t.isIdentifier(node)) {
    var b = argPath.scope.getBinding(node.name);
    if (b && b.path.isVariableDeclarator() && b.path.node.init) {
      argPath = b.path.get("init");
      continue;
    }
    return false;
  }

  // Binary `+` concatenation: `"/api?" + tainted` — walk left-most descendant
  // and check its string prefix.
  if (_t.isBinaryExpression(node) && node.operator === "+") {
    var leftmost = node;
    while (_t.isBinaryExpression(leftmost) && leftmost.operator === "+") {
      leftmost = leftmost.left;
    }
    if (_t.isStringLiteral(leftmost)) {
      var lv = leftmost.value;
      if (/^https?:/i.test(lv)) return false;
      if (lv.startsWith("//")) return false;
      if (lv.startsWith("/") || lv.startsWith("./") || lv.startsWith("../")) return true;
      return false;
    }
    return false;
  }

  // .toString() — unwrap to receiver via loop continuation.
  if (_t.isCallExpression(node) && _t.isMemberExpression(node.callee) &&
      !node.callee.computed && _t.isIdentifier(node.callee.property, { name: "toString" })) {
    argPath = argPath.get("callee.object");
    continue;
  }

  return false;
  }
}

// Base argument to new URL(...) — is it a known same-origin value?
// Recognizes: location.origin, location.href, window.location.origin/href,
// self.location.origin/href, document.baseURI, document.URL.
function _isSameOriginBaseExpr(node, scope) {
  if (!node) return false;
  // `location.origin` / `location.href` / `location.toString()`.
  if (_t.isMemberExpression(node) && !node.computed &&
      _t.isIdentifier(node.object, { name: "location" }) &&
      scope && !scope.getBinding("location")) {
    return _isSameOriginLocationProp(node.property);
  }
  // `window.location.origin`, `self.location.origin`, `globalThis.location.origin`.
  if (_t.isMemberExpression(node) && !node.computed &&
      _t.isMemberExpression(node.object) && !node.object.computed &&
      _t.isIdentifier(node.object.object) &&
      (node.object.object.name === "window" || node.object.object.name === "self" || node.object.object.name === "globalThis") &&
      scope && !scope.getBinding(node.object.object.name) &&
      _t.isIdentifier(node.object.property, { name: "location" })) {
    return _isSameOriginLocationProp(node.property);
  }
  // `document.baseURI` / `document.URL` — reflect current page's URL.
  if (_t.isMemberExpression(node) && !node.computed &&
      _t.isIdentifier(node.object, { name: "document" }) &&
      scope && !scope.getBinding("document") &&
      _t.isIdentifier(node.property) &&
      (node.property.name === "baseURI" || node.property.name === "URL" || node.property.name === "documentURI")) {
    return true;
  }
  return false;
}

function _isSameOriginLocationProp(propNode) {
  if (!_t.isIdentifier(propNode)) return false;
  var name = propNode.name;
  // origin/host/hostname/protocol/port: all reflect the current page origin.
  // href/pathname: contain current origin too, plus path. Still same-origin base.
  return name === "origin" || name === "href" || name === "host" ||
         name === "hostname" || name === "protocol" || name === "port" ||
         name === "pathname" || name === "toString";
}

// Same-origin-by-spec channels — their "message" events never carry data
// from a different origin, so the usual `event.origin` check doesn't
// apply. Observed as FP class on github (BroadcastChannel listeners).
var _SAME_ORIGIN_CHANNEL_CONSTRUCTORS = {
  "BroadcastChannel": 1,
  "Worker": 1,
  "SharedWorker": 1,
  "MessageChannel": 1,
  // NOTE: MessagePort messages CAN come from another origin (the port was
  // transferred across postMessage). Intentionally NOT in this list.
  "EventSource": 1,
};

// Given a bare addEventListener callee — typically `<receiver>.addEventListener`
// or a bare `addEventListener` — decide whether the receiver is a possibly
// cross-origin window/frame. Cross-origin listeners SHOULD be checking
// event.origin; same-origin-only receivers (BroadcastChannel, Worker,
// SharedWorker, MessageChannel, EventSource) do not need it.
//
// Returns true when the receiver is (or could be) a cross-origin window,
// false when it's provably a same-origin-only channel.
function _isCrossOriginMsgReceiver(callee, callPath) {
  // Bare `addEventListener` call at global scope — that's window.addEventListener.
  if (_t.isIdentifier(callee)) return true;
  if (!_t.isMemberExpression(callee)) return false;
  var receiver = callee.object;
  // `window.addEventListener`, `self.addEventListener`, etc.
  if (_t.isIdentifier(receiver)) {
    var name = receiver.name;
    if (_SAME_ORIGIN_CHANNEL_CONSTRUCTORS[name]) return false;
    // Cross-origin-capable globals.
    if (name === "window" || name === "self" || name === "globalThis" ||
        name === "top" || name === "parent" || name === "opener") return true;
    // Resolve the binding — if it traces to a same-origin-only constructor,
    // skip. Otherwise conservatively assume cross-origin (e.g. a stored
    // reference to parent frame).
    var tracked = _getTrackedType(callPath, receiver);
    if (tracked && _SAME_ORIGIN_CHANNEL_CONSTRUCTORS[tracked]) return false;
    return true;
  }
  // `new BroadcastChannel("x").addEventListener(...)` and similar — the
  // receiver is a NewExpression whose constructor we can inspect.
  if (_t.isNewExpression(receiver) && _t.isIdentifier(receiver.callee) &&
      !callPath.scope.getBinding(receiver.callee.name) &&
      _SAME_ORIGIN_CHANNEL_CONSTRUCTORS[receiver.callee.name]) {
    return false;
  }
  // `worker.port.addEventListener(...)` where `worker = new SharedWorker(...)`.
  // The MessagePort exposed by a Worker/SharedWorker the current script
  // created is a same-origin channel — the port connects this page to the
  // worker script, no third party can send messages through it. Walk
  // MemberExpression one more level when the leaf property is `.port` and
  // the base is a same-origin-only worker constructor (either directly or
  // via scope-tracked type).
  if (_t.isMemberExpression(receiver) && !receiver.computed &&
      _t.isIdentifier(receiver.property, { name: "port" })) {
    var portBase = receiver.object;
    if (_t.isNewExpression(portBase) && _t.isIdentifier(portBase.callee) &&
        !callPath.scope.getBinding(portBase.callee.name) &&
        _SAME_ORIGIN_CHANNEL_CONSTRUCTORS[portBase.callee.name]) {
      return false;
    }
    if (_t.isIdentifier(portBase)) {
      var portBaseType = _getTrackedType(callPath, portBase);
      if (portBaseType && _SAME_ORIGIN_CHANNEL_CONSTRUCTORS[portBaseType]) return false;
    }
  }
  // `window.frames[N].addEventListener(...)`, `iframe.contentWindow.addEventListener(...)`,
  // `somePort.addEventListener(...)`, etc. — conservatively flag.
  return true;
}

// A MessageEvent's `.origin` is a browser-set property — the attacker
// can only set it to their own real origin. Using it as a postMessage
// reply target sends data back to the sender — a standard pattern, not
// a data leak. The taint source labels the param as "event.data" which
// correctly flags `event.data.foo` for XSS but OVER-flags `event.origin`
// for reply-target. Walk the trace's final hop: if it lands on a `.origin`
// read whose base resolves back to an addEventListener("message", …)
// param, the value is the reply target — safe.
function _targetTraceIsEventOrigin(src) {
  if (!src || !Array.isArray(src.taintPath) || src.taintPath.length < 2) return false;
  // Look for a `[member] .origin` hop immediately following an "event.data"
  // source. Later hops can be bindings / reassigns / param-caller wrapping
  // without changing the fact that the VALUE is the event's origin string.
  var first = src.taintPath[0];
  if (!first || first.kind !== "source" || first.desc !== "event.data") return false;
  for (var hi = 1; hi < src.taintPath.length; hi++) {
    var h = src.taintPath[hi];
    if (h && h.kind === "member" && h.desc === ".origin") return true;
    // Non-member, non-identity hops before `.origin` disqualify — they
    // indicate the trace has moved to a different attribute (e.g. event.data.foo)
    // that isn't the browser-set origin. Accept binding/reassign/param-caller
    // as pass-through (they don't change the value, just rename it).
    if (h && h.kind === "member" && h.desc !== ".origin") return false;
  }
  return false;
}


// Check if an AST node refers to the global location object:
// location, window.location, self.location, document.location
function _isLocationObject(objNode, path) {
  if (_t.isIdentifier(objNode, { name: "location" }) && !path.scope.getBinding("location")) return true;
  if (_t.isMemberExpression(objNode) && !objNode.computed &&
      _t.isIdentifier(objNode.property, { name: "location" })) {
    var base = objNode.object;
    if ((_t.isIdentifier(base, { name: "window" }) && !path.scope.getBinding("window")) ||
        (_t.isIdentifier(base, { name: "self" }) && !path.scope.getBinding("self")) ||
        (_t.isIdentifier(base, { name: "document" }) && !path.scope.getBinding("document"))) {
      return true;
    }
  }
  return false;
}

// ─── Security Analysis: DOM XSS Sink Detection ─────────────────────────────

// Assignment-based sinks: element.innerHTML = value, element.innerHTML += value
function _processSecurityAssignSink(path, result) {
  var node = path.node;
  if (node.operator !== "=" && node.operator !== "+=") return;
  var left = node.left;

  // Bare location = taint → open redirect (Identifier, not MemberExpression)
  if (_t.isIdentifier(left, { name: "location" }) && !path.scope.getBinding("location")) {
    var _bLocSrc = _traceValueSource(path.get("right"), 0);
    if (_bLocSrc.sourceType !== "user-controlled") return;
    // location = location is a reload, not an open redirect
    if (_t.isIdentifier(node.right, { name: "location" }) && !path.scope.getBinding("location")) return;
    _pushSink(result, node, "redirect", "location", _bLocSrc, path);
    return;
  }

  if (!_t.isMemberExpression(left)) return;

  // Resolve property name — supports both el.innerHTML and el["innerHTML"]
  var propName = null;
  if (!left.computed && _t.isIdentifier(left.property)) {
    propName = left.property.name;
  } else if (left.computed && _t.isStringLiteral(left.property)) {
    propName = left.property.value;
  }
  if (!propName) return;

  var _isLoc = _isLocationObject(left.object, path);
  var sinkType = null;

  // XSS sinks: HTML/content injection properties
  if (propName === "innerHTML" || propName === "outerHTML" || propName === "srcdoc" || propName === "formAction") {
    sinkType = "xss";
  }

  // URL property sinks: href, src, action — XSS unless on location
  // (which is redirect). Skip entirely when the target is a tracked
  // URL / URLSearchParams / Headers / Request / Response object —
  // `e.href = x` on a URL object just mutates the object's href
  // property (no navigation, no execution). Only navigable elements
  // (HTMLAnchorElement, Location, window) take .href-assignment as
  // an actionable navigate. Since we can't tell those apart from
  // HTMLElement at AST time, the safe discriminator is: skip known
  // non-navigable types.
  if (!sinkType && (propName === "href" || propName === "src" || propName === "action")) {
    var _lhsType = _getTrackedType(path, left.object);
    if (_lhsType === "URL" || _lhsType === "URLSearchParams" ||
        _lhsType === "Headers" || _lhsType === "Request" ||
        _lhsType === "Response" || _lhsType === "FormData") {
      return;
    }
    sinkType = _isLoc ? "redirect" : "xss";
  }

  // Open redirect sinks: location.pathname/search/hash
  if (!sinkType && _isLoc && (propName === "pathname" || propName === "search" || propName === "hash")) {
    sinkType = "redirect";
  }

  // document.location = taint / window.location = taint → open redirect
  if (!sinkType && propName === "location") {
    var _isDocWin = (_t.isIdentifier(left.object, { name: "document" }) && !path.scope.getBinding("document")) ||
                    (_t.isIdentifier(left.object, { name: "window" }) && !path.scope.getBinding("window")) ||
                    (_t.isIdentifier(left.object, { name: "self" }) && !path.scope.getBinding("self"));
    if (_isDocWin) sinkType = "redirect";
  }

  if (!sinkType) return;

  var valueSource = _traceValueSource(path.get("right"), 0);
  // Only flag when value traces to a user-controlled source — dynamic/literal
  // values produce massive noise in minified library code (frameworks do
  // innerHTML = expr constantly for legitimate DOM updates).
  if (valueSource.sourceType !== "user-controlled") return;

  // Self-assignment on location properties (e.g. location.href = location.href,
  // document.location = document.location) is a page reload, not an open redirect.
  if (sinkType === "redirect") {
    var right = node.right;
    // Helper: does this subexpression match the left-hand location property?
    // (e.g. `location.href` matches assignment target `location.href`)
    var _matchesSelfLocationProp = function (expr) {
      return _t.isMemberExpression(expr) && !expr.computed &&
        _t.isIdentifier(expr.property, { name: propName }) &&
        _isLocationObject(expr.object, path);
    };
    // location.href = location.href
    if (_isLoc && _matchesSelfLocationProp(right)) return;
    // location.href = X || location.href  /  ?? / &&  — fallback forms.
    // When the LEFT operand of a logical expression isn't traced to an
    // attacker source, the only way the right operand's value reaches
    // the sink is by the left being falsy/nullish. That reduces the
    // assignment to `location.href = location.href` — a reload, not
    // a redirect with attacker content. Only self-cancelling when one
    // operand matches the assignment target AND the other isn't
    // user-controlled.
    if (_isLoc && _t.isLogicalExpression(right)) {
      try {
        var _rLeft = _traceValueSource(path.get("right.left"), 0);
        var _rRight = _traceValueSource(path.get("right.right"), 0);
        var _rLeftMatches = _matchesSelfLocationProp(right.left);
        var _rRightMatches = _matchesSelfLocationProp(right.right);
        if (_rLeftMatches && _rRight.sourceType !== "user-controlled") return;
        if (_rRightMatches && _rLeft.sourceType !== "user-controlled") return;
      } catch (_) {}
    }
    // document.location = document.location / window.location = window.location
    if (propName === "location" && _t.isMemberExpression(right) && !right.computed &&
        _t.isIdentifier(right.property, { name: "location" }) &&
        _t.isIdentifier(left.object) && _t.isIdentifier(right.object, { name: left.object.name }) &&
        !path.scope.getBinding(right.object.name)) return;
  }

  // Reload-shaped redirect: RHS is a Binary+ concat whose leftmost leaf
  // is a browser-validated current-origin read (`location.origin`,
  // `window.location.origin`, `document.URL`, etc.). The URL parser
  // resolves the whole concat against that prefix — result is same-
  // origin, same-scheme. Right-side `location.pathname`/`search`/`hash`
  // reads only add current-page components, producing a reload rather
  // than a redirect. Real-world FP: Reddit _redirect pattern
  // `window.location.origin + window.location.pathname` assigned to
  // `window.location.href`.
  if (sinkType === "redirect" && _valueHasSameOriginPrefix(path.get("right"))) return;

  var pushOptions = null;
  if (sinkType === "redirect" && _taintFlowsOnlyIntoUrlQueryOrHash(path.get("right"))) {
    pushOptions = { severity: "medium", notes: "taint flows only into query/hash of an untainted URL prefix — scheme locked" };
  } else if (sinkType === "redirect" && _dimsRetainUrlPartNoOrigin(valueSource)) {
    // Dim-based downgrade: source is `location.hash`/`location.search`/
    // `location.pathname` used WITHOUT prefix-strip (still carries its
    // "#"/"?"/"/" structural marker). Concat with any untainted prefix
    // puts taint in the hash/query/path portion of an untainted URL —
    // scheme + host locked by URL parser structure. Real-world FP:
    // github permalink.ts `target.href + location.hash` → location.href.
    pushOptions = { severity: "medium", notes: "taint source retains URL-part structural prefix — taint can't reach scheme/host" };
  }
  _pushSink(result, node, sinkType, propName, valueSource, path, pushOptions);
}

// Table-driven global identifier sinks: name → { type, sink }
// Spec-defined global call sinks (each is a Web Platform / ECMA built-in
// whose semantics imply the listed taint-receiver category). Scope-checked
// in `_processSecurityCallSink` via `!path.scope.getBinding(callee.name)`
// so a local var named `eval` / `open` / `fetch` doesn't trip the table.
//
// Library-named sinks (`$`, `jQuery`) were removed per CLAUDE.md L29 ban
// on framework-specific recognition: the analyzer must reach jQuery-style
// DOM injection by tracing `$(value)` to jQuery's actual function body
// (which calls `.parseHTML` → `insertBefore` etc.), not by matching the
// global identifier's name.
var _GLOBAL_CALL_SINKS = {
  "eval": { type: "eval", sink: "eval" },
  "open": { type: "redirect", sink: "window.open" },
  "fetch": { type: "request-forgery", sink: "fetch" },
  "importScripts": { type: "eval", sink: "importScripts" },
};

// Call-based sinks: eval(), document.write(), setTimeout(string), insertAdjacentHTML, setAttribute("on*")
function _processSecurityCallSink(path, result) {
  var node = path.node;
  var callee = node.callee;

  // Indirect eval: (0, eval)(value) — SequenceExpression whose last element is eval
  if (_t.isSequenceExpression(callee) && callee.expressions.length > 0 && node.arguments.length > 0) {
    var _seqLast = callee.expressions[callee.expressions.length - 1];
    if (_t.isIdentifier(_seqLast, { name: "eval" }) && !path.scope.getBinding("eval")) {
      var _indEvalSrc = _traceValueSource(path.get("arguments.0"), 0);
      if (_indEvalSrc.sourceType === "user-controlled") _pushSink(result, node, "eval", "eval", _indEvalSrc, path);
      return;
    }
  }

  // Table-driven global identifier sinks: eval, open, fetch, importScripts
  if (_t.isIdentifier(callee) && node.arguments.length > 0) {
    var _gSink = _GLOBAL_CALL_SINKS[callee.name];
    if (_gSink && !path.scope.getBinding(callee.name)) {
      var _gSrc = _traceValueSource(path.get("arguments.0"), 0);
      if (_gSrc.sourceType === "user-controlled") {
        var _gOpts = (_gSink.type === "redirect" && _taintFlowsOnlyIntoUrlQueryOrHash(path.get("arguments.0")))
          ? { severity: "medium", notes: "taint flows only into query/hash of an untainted URL prefix — scheme locked" }
          : null;
        _pushSink(result, node, _gSink.type, _gSink.sink, _gSrc, path, _gOpts);
      }
      return;
    }
  }

  // setTimeout/setInterval with string first arg (not a function)
  if (_t.isIdentifier(callee) && (callee.name === "setTimeout" || callee.name === "setInterval") &&
      !path.scope.getBinding(callee.name) && node.arguments.length > 0) {
    var firstArg = node.arguments[0];
    // Skip only if the arg is definitely a function (expression, arrow, or identifier resolving to one)
    var isFunc = _t.isFunctionExpression(firstArg) || _t.isArrowFunctionExpression(firstArg);
    if (!isFunc && _t.isIdentifier(firstArg)) {
      var timerBinding = path.scope.getBinding(firstArg.name);
      if (timerBinding && timerBinding.path.node.init && (_t.isFunctionExpression(timerBinding.path.node.init) || _t.isArrowFunctionExpression(timerBinding.path.node.init)))
        isFunc = true;
    }
    if (!isFunc) {
      var timerSource = _traceValueSource(path.get("arguments.0"), 0);
      if (timerSource.sourceType !== "user-controlled") return;
      _pushSink(result, node, "eval", callee.name, timerSource, path);
      return;
    }
  }

  if (!_t.isMemberExpression(callee) || callee.computed) return;
  var methName = _t.isIdentifier(callee.property) ? callee.property.name : null;
  if (!methName) return;

  // window.open(url) / self.open(url) — open redirect (member expression form)
  if (methName === "open" && node.arguments.length > 0) {
    var _isWin = (_t.isIdentifier(callee.object, { name: "window" }) && !path.scope.getBinding("window")) ||
                 (_t.isIdentifier(callee.object, { name: "self" }) && !path.scope.getBinding("self"));
    if (_isWin) {
      var _woSrc2 = _traceValueSource(path.get("arguments.0"), 0);
      if (_woSrc2.sourceType === "user-controlled") {
        var _woOpts = _taintFlowsOnlyIntoUrlQueryOrHash(path.get("arguments.0"))
          ? { severity: "medium", notes: "taint flows only into query/hash of an untainted URL prefix — scheme locked" }
          : null;
        _pushSink(result, node, "redirect", "window.open", _woSrc2, path, _woOpts);
      }
      return;
    }
  }

  // Simple method sinks: method name → { type, sink, argIdx }
  // Handles setHTMLUnsafe, parseHTMLUnsafe, insertAdjacentHTML, createContextualFragment
  if (methName === "setHTMLUnsafe" || methName === "parseHTMLUnsafe" || methName === "createContextualFragment") {
    if (node.arguments.length > 0) {
      var _htmlSrc = _traceValueSource(path.get("arguments.0"), 0);
      if (_htmlSrc.sourceType === "user-controlled") _pushSink(result, node, "xss", methName, _htmlSrc, path);
    }
    return;
  }

  // document.write(value) / document.writeln(value)
  if ((methName === "write" || methName === "writeln") && _t.isIdentifier(callee.object, { name: "document" }) &&
      !path.scope.getBinding("document") && node.arguments.length > 0) {
    var dwSource = _traceValueSource(path.get("arguments.0"), 0);
    if (dwSource.sourceType === "user-controlled") _pushSink(result, node, "xss", "document." + methName, dwSource, path);
    return;
  }

  // element.insertAdjacentHTML(position, markup)
  if (methName === "insertAdjacentHTML" && node.arguments.length >= 2) {
    var iahSource = _traceValueSource(path.get("arguments.1"), 0);
    if (iahSource.sourceType === "user-controlled") _pushSink(result, node, "xss", "insertAdjacentHTML", iahSource, path);
    return;
  }

  // element.setAttribute("onclick"/href/src/action/style, value)
  if (methName === "setAttribute" && node.arguments.length >= 2 && _t.isStringLiteral(node.arguments[0])) {
    var attrName = node.arguments[0].value.toLowerCase();
    if (attrName.startsWith("on") || attrName === "href" || attrName === "src" ||
        attrName === "action" || attrName === "style") {
      var saSource = _traceValueSource(path.get("arguments.1"), 0);
      if (saSource.sourceType === "user-controlled") _pushSink(result, node, "xss", "setAttribute:" + attrName, saSource, path);
      return;
    }
  }

  // (jQuery DOM-manipulation method-name match — `.html`, `.append`,
  // `.prepend`, `.after`, `.before`, `.replaceWith` — was removed per
  // CLAUDE.md L29 ban on framework-specific recognition. The same
  // injection is reached spec-grounded: the analyzer follows `$(sel).html(x)`
  // through jQuery's bundle to the underlying `insertBefore` /
  // `outerHTML` / `innerHTML` mutation, then flags those native sinks.
  // No method-name table required.)

  // Implicit ReDoS: .match(), .search() with user-controlled first arg.
  // Note: .split() does NOT create an implicit RegExp — it does literal string matching.
  if ((methName === "match" || methName === "search") &&
      node.arguments.length > 0 && !_t.isRegExpLiteral(node.arguments[0])) {
    var _reImplSrc = _traceValueSource(path.get("arguments.0"), 0);
    if (_reImplSrc.sourceType === "user-controlled") {
      _pushDangerous(result, node, "regex-implicit",
        "String." + methName + " with user-controlled pattern (implicit RegExp, potential ReDoS)", "medium", _reImplSrc);
    }
    return;
  }

  // navigator.sendBeacon(url, data) — request forgery / data exfiltration.
  // Dim gate applied by _pushSink (suppresses when !dims.origin).
  if (methName === "sendBeacon" && node.arguments.length > 0 &&
      _t.isIdentifier(callee.object, { name: "navigator" }) && !path.scope.getBinding("navigator")) {
    var _sbSrc = _traceValueSource(path.get("arguments.0"), 0);
    if (_sbSrc.sourceType === "user-controlled") {
      _pushSink(result, node, "request-forgery", "navigator.sendBeacon", _sbSrc, path);
    }
    return;
  }

  // XMLHttpRequest.open(method, url) — request forgery via user-controlled URL.
  // Origin-dim check: same rule as fetch — only an attacker-controlled
  // ORIGIN redirects the request to a different server.
  if (methName === "open" && node.arguments.length >= 2) {
    // Exclude window.open / self.open (handled above as redirect)
    var _isXhrOpen = !(_t.isIdentifier(callee.object, { name: "window" }) && !path.scope.getBinding("window")) &&
                     !(_t.isIdentifier(callee.object, { name: "self" }) && !path.scope.getBinding("self"));
    if (_isXhrOpen) {
      var _xhrUrlSrc = _traceValueSource(path.get("arguments.1"), 0);
      if (_xhrUrlSrc.sourceType === "user-controlled") {
        _pushSink(result, node, "request-forgery", "XMLHttpRequest.open", _xhrUrlSrc, path);
      }
    }
    return;
  }

  // navigator.serviceWorker.register(url) — service worker hijacking
  if (methName === "register" && node.arguments.length > 0 &&
      _t.isMemberExpression(callee.object) && !callee.object.computed &&
      _t.isIdentifier(callee.object.property, { name: "serviceWorker" })) {
    var _swBase = callee.object.object;
    if (_t.isIdentifier(_swBase, { name: "navigator" }) && !path.scope.getBinding("navigator")) {
      var _swSrc = _traceValueSource(path.get("arguments.0"), 0);
      if (_swSrc.sourceType === "user-controlled") _pushSink(result, node, "eval", "serviceWorker.register", _swSrc, path);
      return;
    }
  }

  // location.assign(value) / location.replace(value) — open redirect
  if ((methName === "assign" || methName === "replace") && node.arguments.length > 0) {
    if (!_isLocationObject(callee.object, path)) return;
    var _locArgPath = path.get("arguments.0");
    var locSource = _traceValueSource(_locArgPath, 0);
    if (locSource.sourceType !== "user-controlled") return;
    // URL-object argument whose construction reads a browser-validated
    // whole-URL source (`new URL(location.href); t.searchParams.set(k,v);
    // location.assign(t)`) has a scheme and host the browser already
    // constrained to the current origin. `.searchParams`/`.search`/`.hash`/
    // `.pathname` mutations can't drive cross-origin navigation, and
    // origin-modifying assignments (`.host`/`.protocol`/`.href`) would be
    // their own AssignmentExpression taint finding. Reddit devvit playtest
    // "Reloading" case produced an HIGH FP here before this suppression.
    var _argType = _getTrackedType(_locArgPath, _locArgPath.node);
    if (_argType === "URL" && (
          locSource.source === "location" ||
          locSource.source === "location.href" ||
          locSource.source === "document.URL" ||
          locSource.source === "document.documentURI")) {
      return;
    }
    // Reload-shaped redirect: see _valueHasSameOriginPrefix. Same pattern
    // for call-form (`location.assign(location.origin + location.pathname)`).
    if (_valueHasSameOriginPrefix(_locArgPath)) return;
    var locOpts = null;
    if (_taintFlowsOnlyIntoUrlQueryOrHash(_locArgPath)) {
      locOpts = { severity: "medium", notes: "taint flows only into query/hash of an untainted URL prefix — scheme locked" };
    } else if (_dimsRetainUrlPartNoOrigin(locSource)) {
      locOpts = { severity: "medium", notes: "taint source retains URL-part structural prefix — taint can't reach scheme/host" };
    }
    _pushSink(result, node, "redirect", "location." + methName, locSource, path, locOpts);
  }
}

// NewExpression sinks: new Function(value), new RegExp(value), new Worker(url), etc.
function _processSecurityNewSink(path, result) {
  var node = path.node;
  var ctorName = _t.isIdentifier(node.callee) ? node.callee.name : null;
  if (!ctorName || path.scope.getBinding(ctorName) || node.arguments.length === 0) return;

  // new Function(code) — only flag user-controlled
  if (ctorName === "Function") {
    var lastArg = node.arguments[node.arguments.length - 1];
    if (!_t.isStringLiteral(lastArg)) {
      var fnSource = _traceValueSource(path.get("arguments." + (node.arguments.length - 1)), 0);
      if (fnSource.sourceType === "user-controlled") _pushSink(result, node, "eval", "new Function", fnSource, path);
    }
    return;
  }

  // new RegExp(dynamicPattern) — ReDoS risk
  if (ctorName === "RegExp") {
    if (!_t.isStringLiteral(node.arguments[0])) {
      var reSource = _traceValueSource(path.get("arguments.0"), 0);
      if (reSource.sourceType === "user-controlled") {
        _pushDangerous(result, node, "regex-dynamic",
          "RegExp constructor with user-controlled pattern (potential ReDoS)", "high", reSource);
      }
    }
    return;
  }

  // new Worker/SharedWorker/WebSocket/EventSource(url) — skip string literals
  if (ctorName === "Worker" || ctorName === "SharedWorker" ||
      ctorName === "WebSocket" || ctorName === "EventSource") {
    if (_t.isStringLiteral(node.arguments[0])) return;
    var _newSrc = _traceValueSource(path.get("arguments.0"), 0);
    if (_newSrc.sourceType !== "user-controlled") return;
    var _isNetwork = ctorName === "WebSocket" || ctorName === "EventSource";
    _pushSink(result, node,
      _isNetwork ? "request-forgery" : "eval",
      "new " + ctorName, _newSrc, path);
  }
}

// (Removed React-specific dangerouslySetInnerHTML detection per
// CLAUDE.md L29 ban on framework-specific recognition. The actual
// XSS happens when React internally writes
// `el.innerHTML = props.dangerouslySetInnerHTML.__html` — that
// assignment is detected spec-compliantly by
// _processSecurityAssignSink when React's source is in the analyzed
// bundle. For React apps that import React from a separate bundle,
// closing this gap requires multi-bundle correlation
// infrastructure that the analyzer doesn't yet have — that's its
// own pillar of work, not something to paper over with a
// framework-name shortcut.)

// Is `path` an expression whose value is an object with attacker-chosen
// KEYS (as opposed to merely attacker-chosen values)? Prototype pollution
// via `Object.assign(target, source)` requires keys the attacker can set
// to e.g. `"__proto__"` or `"constructor.prototype.*"`, so a string
// source, a matched-params object whose keys come from route declarations,
// or any function-call result whose shape is controlled by the callee is
// NOT a real vector.
//
// Flag only on structures that literally build objects from arbitrary
// user input:
//   - JSON.parse(taintedString)
//   - Object.fromEntries(taintedPairsIterable)
//   - structuredClone(<arbitrary-key-source>)
//   - Object.assign({}, …arbitrary-key-source…)
//   - Spread/rest of any of the above (`{ ...JSON.parse(x) }`)
//   - ObjectExpression with a computed key whose source is user-controlled
//   - MemberExpression whose root object is arbitrary-key (e.g. JSON.parse
//     result then `.nested`)
//
// Implementation is iterative with a visited set so deep spread/chain
// expressions can't stack-overflow (matches the CLAUDE.md AST policy),
// and follows variable chains across multiple scope hops rather than
// stopping at the first binding.
// Returns null when the path is NOT an arbitrary-key source, or a
// user-controlled source trace ({ sourceType, source, sourceLoc, taintPath })
// when it is. The trace points at the taint source that made the shape
// arbitrary — caller can then pass it directly to _pushDangerous so the
// finding carries the full taint chain, not just "some key was tainted".
function _isArbitraryKeyObjectSource(path) {
  if (!path || !path.node) return null;
  var stack = [path];
  var visited = new Set();
  while (stack.length) {
    var p = stack.pop();
    if (!p || !p.node) continue;
    if (visited.has(p.node)) continue;
    visited.add(p.node);
    var n = p.node;

    // Identifier: follow the scope binding chain. Initializer of the
    // VariableDeclarator inherits the classification; loop to absorb
    // `var a = x; var b = a; var c = b;` chains.
    if (_t.isIdentifier(n)) {
      var binding = p.scope.getBinding(n.name);
      if (binding && binding.path && binding.path.isVariableDeclarator() && binding.path.node.init) {
        stack.push(binding.path.get("init"));
      }
      continue;
    }

    // MemberExpression: `.x` / `[y]`. If the root object is arbitrary-key,
    // then any property access on it is also arbitrary-key — `data.user`
    // where `data = JSON.parse(t)` still carries the attacker's ability
    // to name keys in the nested structure.
    if (_t.isMemberExpression(n)) {
      stack.push(p.get("object"));
      continue;
    }

    // ObjectExpression: fixed keys unless a computed key is tainted or a
    // spread element carries an arbitrary-key source.
    if (_t.isObjectExpression(n)) {
      for (var i = 0; i < n.properties.length; i++) {
        var prop = n.properties[i];
        if (_t.isSpreadElement(prop)) {
          stack.push(p.get("properties." + i + ".argument"));
        } else if (_t.isObjectProperty(prop) && prop.computed) {
          var keySrc = _traceValueSource(p.get("properties." + i + ".key"), 0);
          if (keySrc.sourceType === "user-controlled") {
            return _tvsHop(keySrc, "computed-key", "{[key]: v} — key is user-controlled", n.loc ? { line: n.loc.start.line, column: n.loc.start.column } : null);
          }
        }
      }
      continue;
    }

    if (_t.isCallExpression(n)) {
      var callee = n.callee;
      var jsonParse = _t.isMemberExpression(callee) && !callee.computed &&
        _t.isIdentifier(callee.property, { name: "parse" }) &&
        _t.isIdentifier(callee.object, { name: "JSON" }) && !p.scope.getBinding("JSON");
      if (jsonParse && n.arguments.length >= 1) {
        var argSrc = _traceValueSource(p.get("arguments.0"), 0);
        if (argSrc.sourceType === "user-controlled") {
          return _tvsHop(argSrc, "json-parse", "JSON.parse(…)", n.loc ? { line: n.loc.start.line, column: n.loc.start.column } : null);
        }
        continue;
      }
      var objFromEntries = _t.isMemberExpression(callee) && !callee.computed &&
        _t.isIdentifier(callee.property, { name: "fromEntries" }) &&
        _t.isIdentifier(callee.object, { name: "Object" }) && !p.scope.getBinding("Object");
      if (objFromEntries && n.arguments.length >= 1) {
        var feSrc = _traceValueSource(p.get("arguments.0"), 0);
        if (feSrc.sourceType === "user-controlled") {
          return _tvsHop(feSrc, "object-fromEntries", "Object.fromEntries(…)", n.loc ? { line: n.loc.start.line, column: n.loc.start.column } : null);
        }
        continue;
      }
      var structuredCloneCall =
        _t.isIdentifier(callee, { name: "structuredClone" }) && !p.scope.getBinding("structuredClone");
      if (structuredCloneCall && n.arguments.length >= 1) {
        stack.push(p.get("arguments.0"));
        continue;
      }
      var objAssignCall = _t.isMemberExpression(callee) && !callee.computed &&
        _t.isIdentifier(callee.property, { name: "assign" }) &&
        _t.isIdentifier(callee.object, { name: "Object" }) && !p.scope.getBinding("Object");
      if (objAssignCall) {
        for (var oa = 1; oa < n.arguments.length; oa++) stack.push(p.get("arguments." + oa));
        continue;
      }
    }
  }
  return null;
}

// ─── Security Analysis: Dangerous Pattern Detection ─────────────────────────

function _processDangerousPattern(path, result) {
  var node = path.node;
  var callee = node.callee;

  // addEventListener("message", handler) — classify origin check.
  // Only flag when the receiver is a possibly cross-origin window — skip
  // BroadcastChannel, Worker, SharedWorker, MessageChannel, EventSource
  // which are same-origin-by-spec and don't carry cross-origin messages.
  var _isMsgAddListener =
    (_t.isMemberExpression(callee) && _t.isIdentifier(callee.property, { name: "addEventListener" })) ||
    (_t.isIdentifier(callee, { name: "addEventListener" }) && !path.scope.getBinding("addEventListener"));
  if (_isMsgAddListener &&
      node.arguments.length >= 2 && _t.isStringLiteral(node.arguments[0], { value: "message" })) {
    if (_isCrossOriginMsgReceiver(callee, path)) {
      var handlerFuncPath = _resolveHandlerFunc(path.get("arguments.1"));
      if (handlerFuncPath && handlerFuncPath.node.body) _classifyAndReportMessageHandler(handlerFuncPath, node, result);
    }
    return;
  }

  // postMessage(data, targetOrigin) — scan targetOrigin for danger patterns.
  if (_t.isMemberExpression(callee) && _t.isIdentifier(callee.property, { name: "postMessage" }) &&
      node.arguments.length >= 2) {
    var _isOpener = _t.isMemberExpression(callee.object) && _t.isIdentifier(callee.object.property, { name: "opener" });
    var _targetArg = node.arguments[1];
    // `"*"` — wildcard: any origin can receive the message. Resolve
    // through bindings too so `var t = "*"; postMessage(d, t)` is caught
    // at parity with the inline string.
    var _targetResolved = _t.isStringLiteral(_targetArg)
      ? [_targetArg.value]
      : _resolveAllValues(path.get("arguments.1"), 0);
    if (_targetResolved.indexOf("*") >= 0) {
      _pushDangerous(result, node, "postmessage-wildcard-target",
        _isOpener ? "postMessage to opener with wildcard '*' targetOrigin" : "postMessage with wildcard '*' targetOrigin", "high", null);
      return;
    }
    // Non-literal target traced back to a user-controlled source — the
    // attacker can redirect the message to any origin they pick. Even
    // if the PAYLOAD is safe, sending it to the wrong origin leaks
    // whatever the message contains. Literal-origin configs are fine;
    // only flag when the target resolves to a user-controlled value.
    if (!_t.isStringLiteral(_targetArg)) {
      var _tgtSrc = _traceValueSource(path.get("arguments.1"), 0);
      if (_tgtSrc.sourceType === "user-controlled" && !_targetTraceIsEventOrigin(_tgtSrc)) {
        _pushDangerous(result, node, "postmessage-dynamic-target",
          "postMessage targetOrigin is user-controlled — attacker can redirect message to any origin",
          "high", _tgtSrc);
        return;
      }
    }
  }

  // Object.defineProperty(obj, userControlledKey, desc) — prototype pollution
  if (_t.isMemberExpression(callee) && !callee.computed &&
      _t.isIdentifier(callee.property, { name: "defineProperty" }) &&
      _t.isIdentifier(callee.object, { name: "Object" }) && !path.scope.getBinding("Object") &&
      node.arguments.length >= 3) {
    var _dpKeySrc = _traceValueSource(path.get("arguments.1"), 0);
    if (_dpKeySrc.sourceType === "user-controlled") {
      _pushDangerous(result, node, "prototype-pollution", "Object.defineProperty with user-controlled key", "high", _dpKeySrc);
    }
    return;
  }

  // Reflect.set(obj, userControlledKey, val) — prototype pollution
  if (_t.isMemberExpression(callee) && !callee.computed &&
      _t.isIdentifier(callee.property, { name: "set" }) &&
      _t.isIdentifier(callee.object, { name: "Reflect" }) && !path.scope.getBinding("Reflect") &&
      node.arguments.length >= 3) {
    var _rsKeySrc = _traceValueSource(path.get("arguments.1"), 0);
    if (_rsKeySrc.sourceType === "user-controlled") {
      _pushDangerous(result, node, "prototype-pollution", "Reflect.set with user-controlled key", "high", _rsKeySrc);
    }
    return;
  }

  // Object.assign(target, userControlledSource) — prototype pollution via merge.
  // Only a real problem when the source's KEY SET can be attacker-chosen:
  // a string like `location.hash` or a router-matched `.params` whose key
  // names come from route declarations isn't a proto-pollution vector —
  // Object.assign over a string enumerates numeric indices ("0", "1", …)
  // which can't alias `__proto__`, and a matched-params object only
  // carries the declared param names as keys, never `__proto__` from a
  // URL value. Narrow to sources whose structure actually admits arbitrary
  // attacker-chosen keys: JSON.parse(tainted), Object.fromEntries(tainted),
  // or a spread/destructure from one of those.
  if (_t.isMemberExpression(callee) && !callee.computed &&
      _t.isIdentifier(callee.property, { name: "assign" }) &&
      _t.isIdentifier(callee.object, { name: "Object" }) && !path.scope.getBinding("Object") &&
      node.arguments.length >= 2) {
    for (var _oaI = 1; _oaI < node.arguments.length; _oaI++) {
      var _oaArgPath = path.get("arguments." + _oaI);
      var _oaSrc = _isArbitraryKeyObjectSource(_oaArgPath);
      if (_oaSrc) {
        _pushDangerous(result, node, "prototype-pollution-merge", "Object.assign with user-controlled source object", "medium", _oaSrc);
        break;
      }
    }
    return;
  }

  // trustedTypes.createPolicy with passthrough identity functions — defeats Trusted Types
  if (_t.isMemberExpression(callee) && !callee.computed &&
      _t.isIdentifier(callee.property, { name: "createPolicy" }) &&
      _t.isIdentifier(callee.object, { name: "trustedTypes" }) && !path.scope.getBinding("trustedTypes") &&
      node.arguments.length >= 2 && _t.isObjectExpression(node.arguments[1])) {
    var _ttProps = node.arguments[1].properties;
    for (var _tti = 0; _tti < _ttProps.length; _tti++) {
      var _ttProp = _ttProps[_tti];
      if (!_t.isObjectProperty(_ttProp) || _ttProp.computed) continue;
      var _ttKey = _getKeyName(_ttProp.key);
      if (_ttKey !== "createHTML" && _ttKey !== "createScript" && _ttKey !== "createScriptURL") continue;
      // Check if the function is an identity function: (s) => s or function(s) { return s; }
      var _ttFn = _ttProp.value;
      var _isIdentityFn = false;
      if ((_t.isArrowFunctionExpression(_ttFn) || _t.isFunctionExpression(_ttFn)) &&
          _ttFn.params.length === 1 && _t.isIdentifier(_ttFn.params[0])) {
        var _pName = _ttFn.params[0].name;
        // Arrow with expression body: (s) => s
        if (_t.isIdentifier(_ttFn.body, { name: _pName })) _isIdentityFn = true;
        // Block body: (s) => { return s; } or function(s) { return s; }
        if (_t.isBlockStatement(_ttFn.body) && _ttFn.body.body.length === 1 &&
            _t.isReturnStatement(_ttFn.body.body[0]) &&
            _t.isIdentifier(_ttFn.body.body[0].argument, { name: _pName })) _isIdentityFn = true;
      }
      if (_isIdentityFn) {
        _pushDangerous(result, node, "trusted-types-passthrough",
          "Trusted Types policy with passthrough " + _ttKey + " (defeats Trusted Types protection)", "high", null);
        break;
      }
    }
    return;
  }
}

// Resolve handler function from addEventListener argument or identifier
// Returns a Babel path to the resolved function, or null.
function _resolveHandlerFunc(handlerPath) {
  var handler = handlerPath.node;
  if (_t.isFunctionExpression(handler) || _t.isArrowFunctionExpression(handler)) return handlerPath;
  if (_t.isIdentifier(handler)) {
    var hBinding = handlerPath.scope.getBinding(handler.name);
    if (hBinding) {
      if (_t.isFunctionDeclaration(hBinding.path.node)) return hBinding.path;
      if (hBinding.path.node.init && (_t.isFunctionExpression(hBinding.path.node.init) || _t.isArrowFunctionExpression(hBinding.path.node.init)))
        return hBinding.path.get("init");
    }
  }
  // CallExpression: handler is makeHandler(...) — resolve function and find returned function.
  // Handles factory patterns: addEventListener("message", makeHandler(callback))
  if (_t.isCallExpression(handler)) {
    var _rhCallee = handler.callee;
    var _rhFuncPath = null;
    if (_t.isIdentifier(_rhCallee)) {
      var _rhBind = handlerPath.scope.getBinding(_rhCallee.name);
      if (_rhBind) {
        if (_t.isFunctionDeclaration(_rhBind.path.node)) _rhFuncPath = _rhBind.path;
        else if (_rhBind.path.isVariableDeclarator() && _rhBind.path.node.init && _t.isFunction(_rhBind.path.node.init))
          _rhFuncPath = _rhBind.path.get("init");
      }
    }
    if (_rhFuncPath && _rhFuncPath.node.body && _t.isBlockStatement(_rhFuncPath.node.body)) {
      // Find the returned function in the body
      var _rhBody = _rhFuncPath.node.body.body;
      for (var _rhi = 0; _rhi < _rhBody.length; _rhi++) {
        if (_t.isReturnStatement(_rhBody[_rhi]) && _rhBody[_rhi].argument) {
          var _rhRet = _rhBody[_rhi].argument;
          if (_t.isFunctionExpression(_rhRet) || _t.isArrowFunctionExpression(_rhRet)) return _rhFuncPath.get("body.body." + _rhi + ".argument");
        }
      }
    }
  }
  return null;
}

// Classify origin check in message handler and report findings
function _classifyAndReportMessageHandler(handlerFuncPath, eventNode, result) {
  var handlerFunc = handlerFuncPath.node;
  var bodyPath = handlerFuncPath.get("body");
  var classification = _classifyOriginCheck(bodyPath);
  if (classification.level === "strong") return;

  var loc = _nodeLoc(eventNode);
  // Beautified handler code as context — include enclosing function for scope context
  // (shows what free variables like Promise resolvers or callback params are)
  var enclosingFunc = handlerFuncPath.findParent(function(p) { return p.isFunction(); });
  var ctxNode = enclosingFunc ? enclosingFunc.node : handlerFunc;
  var ctx = _generateCode(ctxNode, 30) ||
    _extractCodeContext(eventNode, { sourceLoc: handlerFunc.loc ? { line: handlerFunc.body.loc ? handlerFunc.body.loc.end.line : handlerFunc.loc.end.line } : null });

  var description;
  if (classification.level === "weak") {
    description = _buildWeakOriginDescription(classification.weakMethod, classification.checkedValue);
  } else if (classification.level === "source-only") {
    description = "postMessage listener with source check but no origin validation";
  } else {
    description = "postMessage listener without origin check";
  }

  var _pmType = classification.level === "weak" ? "postmessage-weak-origin" : "postmessage-no-origin";
  // Store handler line range for post-traversal severity classification
  var handlerStart = handlerFunc.loc ? handlerFunc.loc.start.line : 0;
  var handlerEnd = handlerFunc.loc ? handlerFunc.loc.end.line : 0;
  result.dangerousPatterns.push({
    type: _pmType, description: description,
    location: loc, severity: "medium", codeContext: ctx,
    weakMethod: classification.weakMethod || null,
    checkedValue: classification.checkedValue || null,
    _handlerRange: handlerStart && handlerEnd ? [handlerStart, handlerEnd] : null,
  });
}

// Classify origin check strength in a message handler body
// Returns: { level: "strong"|"weak"|"source-only"|"none", weakMethod: string|null, checkedValue: string|null }
function _classifyOriginCheck(bodyPath) {
  var _noneResult = { level: "none", weakMethod: null, checkedValue: null };
  if (!bodyPath || !bodyPath.node) return _noneResult;
  var best = { level: "none", weakMethod: null, checkedValue: null };

  var _originVisitor = {
    BinaryExpression: function(innerPath) {
      if (best.level === "strong") { innerPath.stop(); return; }
      var op = innerPath.node.operator;
      if (op === "===" || op === "!==" || op === "==" || op === "!=") {
        if (_hasPropertyMember(innerPath.node.left, "origin") || _hasPropertyMember(innerPath.node.right, "origin")) {
          best = { level: "strong", weakMethod: null, checkedValue: null }; innerPath.stop(); return;
        }
        // dict[x.origin] === true — dictionary lookup is exact match (strong)
        if (_isDictOriginLookup(innerPath.node.left) || _isDictOriginLookup(innerPath.node.right)) {
          best = { level: "strong", weakMethod: null, checkedValue: null }; innerPath.stop(); return;
        }
        if (_hasPropertyMember(innerPath.node.left, "source") || _hasPropertyMember(innerPath.node.right, "source")) {
          if (best.level === "none") best = { level: "source-only", weakMethod: null, checkedValue: null };
        }
      }
    },
    CallExpression: function(innerPath) {
      if (best.level === "strong") { innerPath.stop(); return; }
      // Whitelist patterns: set.has(x.origin), array.includes(x.origin) — strong
      var _wlResult = _classifyWhitelistCheck(innerPath);
      if (_wlResult === "strong") {
        best = { level: "strong", weakMethod: null, checkedValue: null }; innerPath.stop(); return;
      }
      var callee = innerPath.node.callee;
      if (_t.isMemberExpression(callee) && _t.isIdentifier(callee.property)) {
        var mn = callee.property.name;
        if (mn === "indexOf" || mn === "includes" || mn === "startsWith" || mn === "endsWith") {
          if (_hasPropertyMember(callee.object, "origin")) {
            var _cv = (innerPath.node.arguments.length > 0 && _t.isStringLiteral(innerPath.node.arguments[0]))
              ? innerPath.node.arguments[0].value : null;
            if (best.level !== "weak" || !best.weakMethod) {
              best = { level: "weak", weakMethod: mn, checkedValue: _cv };
            }
          }
        }
      }
      // Trace into function calls that receive .origin as an argument:
      // e.g., c.i(m.origin), validate(event.origin)
      var args = innerPath.node.arguments;
      for (var _oci = 0; _oci < args.length; _oci++) {
        if (_hasPropertyMember(args[_oci], "origin")) {
          var _ocResult = _classifyOriginCheckInCallee(innerPath, _oci);
          if (_ocResult.level === "strong") { best = _ocResult; innerPath.stop(); return; }
          if (_ocResult.level === "weak" && best.level !== "weak") best = _ocResult;
        }
      }
    },
  };

  // Scope-aware traversal using path.traverse (walks all node types)
  bodyPath.traverse(_originVisitor);
  return best;
}

// Trace into a called function to classify how it checks a parameter.
// callPath: the CallExpression path, argIdx: which argument is .origin.
// Returns: { level: "strong"|"weak"|"none", weakMethod: string|null, checkedValue: string|null }
function _classifyOriginCheckInCallee(callPath, argIdx) {
  var _noneResult = { level: "none", weakMethod: null, checkedValue: null };
  var callee = callPath.node.callee;
  var funcPath = null;

  // Direct identifier: validate(event.origin) → resolve binding
  if (_t.isIdentifier(callee)) {
    var binding = callPath.scope.getBinding(callee.name);
    if (binding) {
      if (_t.isFunctionDeclaration(binding.path.node)) funcPath = binding.path;
      else if (binding.path.isVariableDeclarator() && binding.path.node.init &&
               _t.isFunction(binding.path.node.init)) funcPath = binding.path.get("init");
    }
  }

  // Member expression: c.i(event.origin) → resolve object binding, find method
  if (!funcPath && _t.isMemberExpression(callee) && !callee.computed && _t.isIdentifier(callee.property)) {
    var methodName = callee.property.name;
    var objNode = callee.object;
    var objBinding = _t.isIdentifier(objNode) ? callPath.scope.getBinding(objNode.name) : null;
    if (objBinding && objBinding.path.isVariableDeclarator() && objBinding.path.node.init) {
      var objInit = objBinding.path.node.init;
      if (_t.isObjectExpression(objInit)) {
        for (var pi = 0; pi < objInit.properties.length; pi++) {
          var prop = objInit.properties[pi];
          if (_t.isObjectProperty(prop) && !prop.computed &&
              ((_t.isIdentifier(prop.key) && prop.key.name === methodName) ||
               (_t.isStringLiteral(prop.key) && prop.key.value === methodName)) &&
              _t.isFunction(prop.value)) {
            funcPath = objBinding.path.get("init.properties." + pi + ".value");
            break;
          }
          if (_t.isObjectMethod(prop) && !prop.computed &&
              _t.isIdentifier(prop.key) && prop.key.name === methodName) {
            funcPath = objBinding.path.get("init.properties." + pi);
            break;
          }
        }
      }
      // NewExpression: c = new Zp(checkerFn) — class instance with this.i = param
      if (!funcPath && _t.isNewExpression(objInit)) {
        funcPath = _resolveClassInstanceMethod(objBinding.path, objInit, methodName, callPath);
      }
    }
    // Destructured parameter: ({kh: c}) => { c.i(origin) }
    // Trace through callers to find what value arrives for property 'kh'
    if (!funcPath && objBinding && objBinding.kind === "param" &&
        _t.isObjectPattern(objBinding.path.node)) {
      funcPath = _resolveDestructuredParamMethod(objBinding, methodName, callPath);
    }
  }

  if (!funcPath || !funcPath.node.params || argIdx >= funcPath.node.params.length) return _noneResult;
  var param = funcPath.node.params[argIdx];
  var paramName = _t.isIdentifier(param) ? param.name :
                  (_t.isAssignmentPattern(param) && _t.isIdentifier(param.left)) ? param.left.name : null;
  if (!paramName) return _noneResult;

  // Traverse the function body checking for comparisons/method calls on the parameter
  var innerResult = { level: "none", weakMethod: null, checkedValue: null };
  // Check the function body. For arrow functions with expression bodies (c => expr),
  // funcPath.get("body") is the expression itself — traverse only visits descendants,
  // so we also directly check the body node for expression bodies.
  var _checkBinaryNode = function(node, bp) {
    if (innerResult.level === "strong") return;
    var op = node.operator;
    if (op === "===" || op === "!==" || op === "==" || op === "!=") {
      if (_t.isIdentifier(node.left, { name: paramName }) || _t.isIdentifier(node.right, { name: paramName })) {
        var paramBinding = bp.scope.getBinding(paramName);
        if (paramBinding && paramBinding.kind === "param" && paramBinding.scope === funcPath.scope) {
          innerResult = { level: "strong", weakMethod: null, checkedValue: null };
        }
      }
      if (_isComputedParamLookup(node.left, paramName, bp, funcPath) ||
          _isComputedParamLookup(node.right, paramName, bp, funcPath)) {
        innerResult = { level: "strong", weakMethod: null, checkedValue: null };
      }
    }
  };
  var _checkCallNode = function(node, cp) {
    if (innerResult.level === "strong") return;
    var cc = node.callee;
    if (_t.isMemberExpression(cc) && _t.isIdentifier(cc.property)) {
      var cmn = cc.property.name;
      if ((cmn === "indexOf" || cmn === "includes" || cmn === "startsWith" || cmn === "endsWith") &&
          _t.isIdentifier(cc.object, { name: paramName })) {
        var paramBinding = cp.scope.getBinding(paramName);
        if (paramBinding && paramBinding.kind === "param" && paramBinding.scope === funcPath.scope) {
          var _cv = (node.arguments.length > 0 && _t.isStringLiteral(node.arguments[0]))
            ? node.arguments[0].value : null;
          if (innerResult.level !== "weak") innerResult = { level: "weak", weakMethod: cmn, checkedValue: _cv };
        }
      }
      if ((cmn === "has" || cmn === "includes") && node.arguments.length >= 1 &&
          _t.isIdentifier(node.arguments[0], { name: paramName })) {
        if (!_t.isIdentifier(cc.object, { name: paramName })) {
          var paramBinding = cp.scope.getBinding(paramName);
          if (paramBinding && paramBinding.kind === "param" && paramBinding.scope === funcPath.scope) {
            innerResult = { level: "strong", weakMethod: null, checkedValue: null };
          }
        }
      }
    }
  };
  try {
    var bodyPath = funcPath.get("body");
    // For expression bodies (arrow => expr), check the body node directly
    if (_t.isBinaryExpression(bodyPath.node)) _checkBinaryNode(bodyPath.node, bodyPath);
    if (_t.isCallExpression(bodyPath.node)) _checkCallNode(bodyPath.node, bodyPath);
    // Traverse descendants for both block and expression bodies
    bodyPath.traverse({
      BinaryExpression: function(bp) {
        _checkBinaryNode(bp.node, bp);
        if (innerResult.level === "strong") bp.stop();
      },
      CallExpression: function(cp) {
        _checkCallNode(cp.node, cp);
        if (innerResult.level === "strong") cp.stop();
      },
    });
  } catch (e) { _resolver.collectError(e, "paramValidationType"); }
  return innerResult;
}

function _hasPropertyMember(node, propName) {
  if (!node) return false;
  return _t.isMemberExpression(node) && _t.isIdentifier(node.property, { name: propName });
}

// Detect dict[x.origin] — computed member expression where the computed key is .origin
function _isDictOriginLookup(node) {
  return _t.isMemberExpression(node) && node.computed && _hasPropertyMember(node.property, "origin");
}

// Check if node is dict[paramName] where paramName refers to the function parameter
function _isComputedParamLookup(node, paramName, innerPath, funcPath) {
  if (!_t.isMemberExpression(node) || !node.computed) return false;
  if (!_t.isIdentifier(node.property, { name: paramName })) return false;
  var paramBinding = innerPath.scope.getBinding(paramName);
  return paramBinding && paramBinding.kind === "param" && paramBinding.scope === funcPath.scope;
}

// Resolve c.methodName(origin) where c is a destructured parameter ({propKey: c}).
// Traces through the enclosing function's callers to find what value arrives for the property,
// then resolves class instances through constructors.
// Returns: funcPath (Babel path) of the resolved function, or null
function _resolveDestructuredParamMethod(objBinding, methodName, callPath) {
  var pattern = objBinding.path.node; // ObjectPattern
  var identName = objBinding.identifier.name;

  // Find which property key maps to this identifier
  var propKey = null;
  for (var pi = 0; pi < pattern.properties.length; pi++) {
    var p = pattern.properties[pi];
    if (_t.isObjectProperty(p) && _t.isIdentifier(p.value, { name: identName })) {
      propKey = _t.isIdentifier(p.key) ? p.key.name : (_t.isStringLiteral(p.key) ? p.key.value : null);
      break;
    }
  }
  if (!propKey) return null;

  // Find the enclosing function that owns this destructured parameter
  var encFunc = objBinding.path.parentPath;
  if (!encFunc || !encFunc.isFunction()) return null;

  // Find the function's binding (the variable it's assigned to)
  var funcBinding = null;
  if (encFunc.parent && _t.isAssignmentExpression(encFunc.parent) &&
      _t.isIdentifier(encFunc.parent.left)) {
    funcBinding = encFunc.scope.getBinding(encFunc.parent.left.name);
  } else if (encFunc.parent && _t.isVariableDeclarator(encFunc.parent) &&
             _t.isIdentifier(encFunc.parent.id)) {
    funcBinding = encFunc.parentPath.scope.getBinding(encFunc.parent.id.name);
  }
  if (!funcBinding || !funcBinding.referencePaths) return null;

  // Walk callers to find what value is passed for the property
  for (var ri = 0; ri < funcBinding.referencePaths.length; ri++) {
    var ref = funcBinding.referencePaths[ri];
    if (!ref.parent || !_t.isCallExpression(ref.parent) || ref.parent.callee !== ref.node) continue;
    // The function takes a destructured object param, so the call arg should be an ObjectExpression
    if (ref.parent.arguments.length === 0) continue;
    var argNode = ref.parent.arguments[0];
    if (!_t.isObjectExpression(argNode)) continue;

    // Find the property matching propKey in the argument
    for (var ai = 0; ai < argNode.properties.length; ai++) {
      var argProp = argNode.properties[ai];
      if (!_t.isObjectProperty(argProp) || argProp.computed) continue;
      var argKeyName = _t.isIdentifier(argProp.key) ? argProp.key.name :
                       (_t.isStringLiteral(argProp.key) ? argProp.key.value : null);
      if (argKeyName !== propKey) continue;

      var valNode = argProp.value;
      // Direct function
      if (_t.isFunctionExpression(valNode) || _t.isArrowFunctionExpression(valNode)) {
        return ref.parentPath.get("arguments.0.properties." + ai + ".value");
      }
      // NewExpression: new Zp(checkerFn) — resolve through class constructor
      if (_t.isNewExpression(valNode)) {
        return _resolveClassInstanceMethod(ref.parentPath.get("arguments.0.properties." + ai), valNode, methodName, callPath);
      }
      // ConditionalExpression / LogicalExpression: may contain new Zp(...) in branches
      // Collect all NewExpressions from branches and try to resolve
      var newExprs = _collectNewExprsFromBranches(valNode);
      for (var ni = 0; ni < newExprs.length; ni++) {
        var resolved = _resolveClassInstanceMethod(ref.parentPath.get("arguments.0.properties." + ai), newExprs[ni], methodName, callPath);
        if (resolved) return resolved;
      }
      break;
    }
  }
  return null;
}

// Collect NewExpression nodes from conditional/logical branches (iterative)
function _collectNewExprsFromBranches(node) {
  var result = [];
  var stack = [node];
  while (stack.length > 0) {
    var cur = stack.pop();
    if (!cur) continue;
    if (_t.isNewExpression(cur)) {
      result.push(cur);
    } else if (_t.isConditionalExpression(cur)) {
      stack.push(cur.consequent);
      stack.push(cur.alternate);
    } else if (_t.isLogicalExpression(cur)) {
      stack.push(cur.left);
      stack.push(cur.right);
    }
  }
  return result;
}

// Detect whitelist/dictionary origin checks that are exact-match (strong):
// - set.has(x.origin) — Set.prototype.has is exact match
// - array.includes(x.origin) where the ARRAY is the receiver, not origin
// Returns "strong" or "none".
function _classifyWhitelistCheck(callPath) {
  var callee = callPath.node.callee;
  if (!_t.isMemberExpression(callee) || !_t.isIdentifier(callee.property)) return "none";
  var mn = callee.property.name;
  var args = callPath.node.arguments;

  if (mn === "has" && args.length >= 1 && _hasPropertyMember(args[0], "origin")) {
    if (!_hasPropertyMember(callee.object, "origin")) return "strong";
  }
  if (mn === "includes" && args.length >= 1 && _hasPropertyMember(args[0], "origin")) {
    if (!_hasPropertyMember(callee.object, "origin")) return "strong";
  }
  return "none";
}

// Resolve c.methodName(origin) where c = new ClassName(arg) and this.methodName = constructor_param
// newExprPath: Babel path whose node is the NewExpression (or a parent — argNode is resolved from newExpr.arguments)
// newExpr: the raw NewExpression node
// Returns: funcPath (Babel path) of the resolved function, or null
function _resolveClassInstanceMethod(newExprPath, newExpr, methodName, callPath) {
  var classCallee = newExpr.callee;
  var className = _t.isIdentifier(classCallee) ? classCallee.name : null;
  if (!className) return null;

  var classBinding = callPath.scope.getBinding(className);
  if (!classBinding) return null;
  var classDecl = classBinding.path;

  var classNode = null;
  var classDeclPath = null;
  if (_t.isClassDeclaration(classDecl.node)) {
    classNode = classDecl.node;
    classDeclPath = classDecl;
  } else if (_t.isVariableDeclarator(classDecl.node) && _t.isClassExpression(classDecl.node.init)) {
    classNode = classDecl.node.init;
    classDeclPath = classDecl.get("init");
  }
  // VariableDeclarator with no init: check for later assignment (var Zp; ... Zp = class { ... })
  if (!classNode && _t.isVariableDeclarator(classDecl.node) && !classDecl.node.init &&
      classBinding.constantViolations && classBinding.constantViolations.length > 0) {
    for (var cvi = 0; cvi < classBinding.constantViolations.length; cvi++) {
      var cv = classBinding.constantViolations[cvi];
      if (cv.isAssignmentExpression() && _t.isClassExpression(cv.node.right)) {
        classNode = cv.node.right;
        classDeclPath = cv.get("right");
        break;
      }
    }
  }
  if (!classNode || !classNode.body) return null;

  // Find the constructor
  var ctorMethodPath = null;
  for (var ci = 0; ci < classNode.body.body.length; ci++) {
    var member = classNode.body.body[ci];
    if (_t.isClassMethod(member) && member.kind === "constructor") {
      ctorMethodPath = classDeclPath.get("body.body." + ci);
      break;
    }
  }
  if (!ctorMethodPath) return null;

  // Find this.methodName = paramN in constructor
  var assignedParamName = _findThisAssignedParam(ctorMethodPath, methodName);
  if (!assignedParamName) return null;

  var paramIdx = _findParamIndex(ctorMethodPath.node.params, assignedParamName);
  if (paramIdx === -1) return null;

  // Get the corresponding argument from new ClassName(arg0, arg1, ...)
  if (paramIdx >= newExpr.arguments.length) return null;
  var argNode = newExpr.arguments[paramIdx];
  // Try to get a precise Babel path to the argument
  var argPath = null;
  try {
    if (newExprPath.node === newExpr) argPath = newExprPath.get("arguments." + paramIdx);
    else if (newExprPath.node && newExprPath.node.init === newExpr) argPath = newExprPath.get("init.arguments." + paramIdx);
    else if (newExprPath.node && newExprPath.node.value === newExpr) argPath = newExprPath.get("value.arguments." + paramIdx);
  } catch (e) { /* path navigation failed, use scope-based fallback */ }

  // Direct function: new Zp(function(o) { ... })
  if (_t.isFunctionExpression(argNode) || _t.isArrowFunctionExpression(argNode)) {
    if (argPath && argPath.node) return argPath;
  }

  // Identifier: new Zp(myValidator) where myValidator is a function
  if (_t.isIdentifier(argNode)) {
    var argBinding = callPath.scope.getBinding(argNode.name);
    if (argBinding) {
      if (_t.isFunctionDeclaration(argBinding.path.node)) return argBinding.path;
      if (argBinding.path.isVariableDeclarator() && argBinding.path.node.init &&
          _t.isFunction(argBinding.path.node.init)) return argBinding.path.get("init");
    }
  }

  // CallExpression: new Zp($p(origins)) where $p returns a function
  // Use path-based resolution when available, fall back to scope-based callee resolution
  if (_t.isCallExpression(argNode)) {
    if (argPath && argPath.node) {
      var retFunc = _resolveCallReturnToFunction(argPath, 0);
      if (retFunc && retFunc._path) return retFunc._path;
    }
    // Scope-based fallback: resolve the callee function directly and find its return
    var factoryCallee = argNode.callee;
    var factoryFuncPath = null;
    if (_t.isIdentifier(factoryCallee)) {
      var factoryBinding = callPath.scope.getBinding(factoryCallee.name);
      if (factoryBinding) {
        if (_t.isFunctionDeclaration(factoryBinding.path.node)) factoryFuncPath = factoryBinding.path;
        else if (factoryBinding.path.isVariableDeclarator() && factoryBinding.path.node.init &&
                 _t.isFunction(factoryBinding.path.node.init)) factoryFuncPath = factoryBinding.path.get("init");
        // Handle split declaration: var $p; ... $p = function(...) { ... }
        else if (factoryBinding.path.isVariableDeclarator() && !factoryBinding.path.node.init &&
                 factoryBinding.constantViolations && factoryBinding.constantViolations.length > 0) {
          for (var fvi = 0; fvi < factoryBinding.constantViolations.length; fvi++) {
            var fv = factoryBinding.constantViolations[fvi];
            if (fv.isAssignmentExpression() && _t.isFunction(fv.node.right)) {
              factoryFuncPath = fv.get("right");
              break;
            }
          }
        }
      }
    }
    if (factoryFuncPath) {
      // Find return statement that returns a function
      var retResult = null;
      try {
        factoryFuncPath.traverse(Object.assign({
          ReturnStatement: function(retPath) {
            if (retResult) return;
            var arg = retPath.node.argument;
            if (arg && (_t.isFunctionExpression(arg) || _t.isArrowFunctionExpression(arg))) {
              retResult = retPath.get("argument");
            }
          },
        }, _SKIP_NESTED_FUNCS));
      } catch (e) { /* ignore */ }
      if (retResult) return retResult;
    }
  }

  return null;
}

// Build a human-readable description explaining how a specific weak origin check is bypassable
function _buildWeakOriginDescription(method, checkedValue) {
  var valueStr = checkedValue ? ' ("' + checkedValue + '")' : "";
  switch (method) {
    case "startsWith":
      return "postMessage origin check uses startsWith" + valueStr +
        " — bypassable: attacker domain like " +
        (checkedValue ? checkedValue + ".evil.com" : "prefix.evil.com") + " passes the check";
    case "endsWith":
      return "postMessage origin check uses endsWith" + valueStr +
        " — accepts any subdomain/scheme, wider attack surface";
    case "includes":
      return "postMessage origin check uses includes" + valueStr +
        " — weak substring match, bypassable with attacker domain containing the substring";
    case "indexOf":
      return "postMessage origin check uses indexOf" + valueStr +
        " — weak substring match, bypassable with attacker domain containing the substring";
    default:
      return "postMessage listener with bypassable origin check (use strict === comparison)";
  }
}

// Assignment-based dangerous patterns: prototype pollution, onmessage handler
function _processDangerousAssignment(path, result) {
  var node = path.node;
  if (node.operator !== "=") return;
  var left = node.left;

  // window.onmessage = handler / self.onmessage = handler — postMessage handler
  if (_t.isMemberExpression(left) && !left.computed && _t.isIdentifier(left.property, { name: "onmessage" })) {
    var _omBase = left.object;
    var _isGlobal = (_t.isIdentifier(_omBase, { name: "window" }) && !path.scope.getBinding("window")) ||
                    (_t.isIdentifier(_omBase, { name: "self" }) && !path.scope.getBinding("self"));
    if (_isGlobal) {
      var _omResolved = _resolveHandlerFunc(path.get("right"));
      if (_omResolved && _omResolved.node.body) _classifyAndReportMessageHandler(_omResolved, node, result);
      return;
    }
  }

  // Prototype pollution: obj.__proto__ = tainted
  if (_t.isMemberExpression(left) && !left.computed &&
      _t.isIdentifier(left.property, { name: "__proto__" })) {
    var _protoSrc = _traceValueSource(path.get("right"), 0);
    if (_protoSrc.sourceType === "user-controlled") {
      _pushDangerous(result, node, "prototype-pollution", "Direct __proto__ assignment with user-controlled value", "high", _protoSrc);
    }
    return;
  }

  // Prototype pollution: obj[dynamicKey] = value
  if (_t.isMemberExpression(left) && left.computed) {
    var keyNode = left.property;
    if (!_t.isStringLiteral(keyNode) && !_t.isNumericLiteral(keyNode)) {
      var keySource = _traceValueSource(path.get("left.property"), 0);
      if (keySource.sourceType === "user-controlled") {
        // An ancestor allowlist guard with a non-tainted collection
        // dominates the sink → downgrade to "info". See _checkKeyValidation
        // for the safety requirements enforced (scope-aware identifier
        // matching + collection-purity check).
        var keyValidated = false;
        try { keyValidated = _checkKeyValidation(path, keyNode); }
        catch (e) { _resolver.collectError(e, "checkKeyValidation"); }

        // Severity depends on BOTH the key (already user-controlled) AND
        // the value shape. Per ES spec:
        //   obj["__proto__"] = <primitive>  → NO-OP (spec-defined)
        //   obj["__proto__"] = <object>     → prototype is replaced (real pollution)
        // Setting `hasOwnProperty` / `constructor` to a primitive only
        // shadows the own property locally — it does NOT mutate
        // Object.prototype and doesn't affect other objects. That's a
        // functional/availability concern (downstream `obj.hasOwnProperty(x)`
        // breaks) requiring an attack chain; severity MEDIUM.
        //
        // Nested assignments (`obj[k1][k2] = v`) bypass this — the outer
        // access returns a proto-link-bearing object regardless of the
        // value shape — so we promote those to HIGH.
        var valueShape = _classifyAssignmentValueShape(path.get("right"));
        var isNested = _t.isMemberExpression(path.parent) || _t.isMemberExpression(left.object) && left.object.computed;
        var severity, description;
        if (keyValidated) {
          severity = "info";
          description = "Dynamic property assignment, key validated against non-tainted allowlist";
        } else if (valueShape === "object" || isNested) {
          severity = "high";
          description = "Dynamic property assignment with user-controlled key (object value — prototype pollution)";
        } else {
          severity = "medium";
          description = "Dynamic property assignment with user-controlled key (primitive value — local shadowing only, needs attack chain)";
        }
        _pushDangerous(result, node, "prototype-pollution", description, severity, keySource);
      }
    }
  }
}

// Classify the shape of a value being assigned. "object" means an object,
// array, or something that traces to an object/array/JSON.parse result.
// "primitive" means string/number/boolean/null literal or a call like
// `x.split(..)[1]` which yields a primitive element. "unknown" when we
// can't tell — conservative: treat as "object" so severity stays HIGH.
// Iterative classification: outer loop allows the Identifier branch to
// alias-walk to a leaf init expression and re-classify in-place via
// `continue` instead of self-recursion.
function _classifyAssignmentValueShape(rightPath) {
  if (!rightPath || !rightPath.node) return "unknown";
  var aliasVisited = null;
  while (true) {
    if (!rightPath || !rightPath.node) return "unknown";
    var node = rightPath.node;
    if (_t.isStringLiteral(node) || _t.isNumericLiteral(node) ||
        _t.isBooleanLiteral(node) || _t.isNullLiteral(node) ||
        _t.isTemplateLiteral(node)) return "primitive";
    if (_t.isObjectExpression(node) || _t.isArrayExpression(node)) return "object";
    if (_t.isNewExpression(node) && _t.isIdentifier(node.callee) &&
        !rightPath.scope.getBinding(node.callee.name) &&
        (node.callee.name === "Object" || node.callee.name === "Array" ||
         node.callee.name === "Set" || node.callee.name === "Map")) return "object";
    if (_t.isCallExpression(node) && _t.isMemberExpression(node.callee) && !node.callee.computed &&
        _t.isIdentifier(node.callee.object) && _t.isIdentifier(node.callee.property)) {
      var obj = node.callee.object.name;
      var meth = node.callee.property.name;
      if (obj === "JSON" && meth === "parse" && !rightPath.scope.getBinding("JSON")) return "object";
      if (obj === "Object" && (meth === "assign" || meth === "create" || meth === "fromEntries") &&
          !rightPath.scope.getBinding("Object")) return "object";
      if (meth === "toString" || meth === "toLowerCase" || meth === "toUpperCase" || meth === "trim") {
        return "primitive";
      }
    }
    if (_t.isMemberExpression(node) && _t.isNumericLiteral(node.property) &&
        _t.isCallExpression(node.object) && _t.isMemberExpression(node.object.callee) &&
        _t.isIdentifier(node.object.callee.property)) {
      var splitMeth = node.object.callee.property.name;
      if (splitMeth === "split" || splitMeth === "match" || splitMeth === "slice") return "primitive";
    }
    // Identifier — alias-walk to leaf init, then re-classify via continue.
    if (_t.isIdentifier(node)) {
      if (!aliasVisited) aliasVisited = new Set();
      var curPath = rightPath;
      var curNode = node;
      while (_t.isIdentifier(curNode)) {
        if (aliasVisited.has(curNode)) return "unknown";
        aliasVisited.add(curNode);
        var b = curPath.scope.getBinding(curNode.name);
        if (!b || !b.path.isVariableDeclarator() || !b.path.node.init) return "unknown";
        curPath = b.path.get("init");
        curNode = b.path.node.init;
      }
      // Re-enter classification with the leaf init via outer continue.
      rightPath = curPath;
      continue;
    }
    return "unknown";
  }
}

// ─── Proto Field Detection ──────────────────────────────────────────────────

function _detectProtoFieldAssignment(path, result) {
  var node = path.node;
  if (!_t.isFunctionExpression(node.right) && !_t.isArrowFunctionExpression(node.right)) return;
  if (!_t.isMemberExpression(node.left)) return;

  var memberProp = node.left.property;
  var accessorName = _t.isIdentifier(memberProp) ? memberProp.name : null;
  if (!accessorName) return;

  var leftObj = node.left.object;
  if (!_t.isMemberExpression(leftObj)) return;
  var objProp = leftObj.property;
  if (!(_t.isIdentifier(objProp, { name: "prototype" }) ||
        (_t.isStringLiteral(objProp) && objProp.value === "prototype"))) return;

  _stats.protoMethods++;

  var protoOwner = _t.isIdentifier(leftObj.object) ? leftObj.object.name :
    (_t.isMemberExpression(leftObj.object) && _t.isIdentifier(leftObj.object.property) ? leftObj.object.property.name : "?");

  var fieldNumber = _findFieldNumberInFunction(path.get("right"));
  if (fieldNumber == null) {
    _stats.protoMethodsNoField++;
    return;
  }

  result.protoFieldMaps.push({
    fieldNumber: fieldNumber,
    fieldName: accessorName,
    accessorName: accessorName,
    minified: true,
  });
  console.debug("[AST:proto] Field #%d → %s (%s.prototype.%s)", fieldNumber, accessorName, protoOwner, accessorName);
}

function _findFieldNumberInFunction(funcPath) {
  var found = null;
  try {
    funcPath.traverse({
      CallExpression: function(innerPath) {
        if (found != null) { innerPath.stop(); return; }
        var callee = innerPath.node.callee;
        var args = innerPath.node.arguments;

        // obj.method(this, N)
        if (_t.isMemberExpression(callee) && args.length >= 2 &&
            _t.isThisExpression(args[0]) &&
            _t.isNumericLiteral(args[1]) && args[1].value >= 1) {
          found = args[1].value;
          innerPath.stop();
          return;
        }
        // f(this, N)
        if (_t.isIdentifier(callee) && args.length >= 2 &&
            _t.isThisExpression(args[0]) &&
            _t.isNumericLiteral(args[1]) && args[1].value >= 1) {
          found = args[1].value;
          innerPath.stop();
        }
      },
      MemberExpression: function(innerPath) {
        if (found != null) { innerPath.stop(); return; }
        var node = innerPath.node;
        // this.array[N]
        if (node.computed && _t.isMemberExpression(node.object) &&
            _t.isThisExpression(node.object.object) &&
            _t.isNumericLiteral(node.property) && node.property.value >= 1) {
          found = node.property.value;
          innerPath.stop();
        }
      },
    });
  } catch (e) { _resolver.collectError(e, "findFieldNumber"); }
  return found;
}

// ─── Enum Detection ─────────────────────────────────────────────────────────

function _detectEnumObject(node, result) {
  var props = node.properties;
  if (!props || props.length < 4) return; // minimum: 2 forward + 2 reverse

  // Collect forward (string/identifier key → integer value) and reverse (numeric key → string value)
  var forward = {}; // stringKey → numericValue
  var reverse = {}; // numericValue → stringKey (keyed by the numeric KEY from reverse entries)
  var forwardCount = 0, reverseCount = 0;

  for (var i = 0; i < props.length; i++) {
    var prop = props[i];
    if (!_t.isObjectProperty(prop) || prop.computed) return;

    var key = prop.key;
    var val = prop.value;

    // Forward entry: string/identifier key → numeric literal value
    if ((_t.isIdentifier(key) || _t.isStringLiteral(key)) &&
        (_t.isNumericLiteral(val) || (_t.isUnaryExpression(val, { operator: "-" }) && _t.isNumericLiteral(val.argument)))) {
      var kStr = _t.isIdentifier(key) ? key.name : key.value;
      var kVal = _t.isNumericLiteral(val) ? val.value : -val.argument.value;
      forward[kStr] = kVal;
      forwardCount++;
    }
    // Reverse entry: numeric literal key → string literal value
    else if (_t.isNumericLiteral(key) && _t.isStringLiteral(val)) {
      reverse[key.value] = val.value;
      reverseCount++;
    }
    // Any other property type — not a bidirectional enum
    else {
      return;
    }
  }

  // Require both directions present
  if (forwardCount === 0 || reverseCount === 0) return;
  if (forwardCount !== reverseCount) return;

  // Verify bidirectional consistency: forward[k]=v ↔ reverse[v]=k
  var forwardKeys = Object.keys(forward);
  for (var fi = 0; fi < forwardKeys.length; fi++) {
    var fk = forwardKeys[fi];
    var fv = forward[fk];
    if (reverse[fv] !== fk) return;
  }

  result.protoEnums.push({ values: forward });
}

// ─── Helpers ────────────────────────────────────────────────────────────────

function _getKeyName(node) {
  if (_t.isIdentifier(node)) return node.name;
  if (_t.isStringLiteral(node)) return node.value;
  if (_t.isNumericLiteral(node)) return String(node.value);
  return null;
}

function _getObjectKeys(objNode) {
  var keys = [];
  for (var i = 0; i < objNode.properties.length; i++) {
    var p = objNode.properties[i];
    if (_t.isSpreadElement(p) || (p.computed)) continue;
    var name = _getKeyName(p.key);
    if (name) keys.push(name);
  }
  return keys;
}

// Find the property key name for a binding inside an ObjectPattern.
// Given function f({url, method: m, endpoint: ep = "/default"}) { ... }
// and bindingName "url" → returns "url", "m" → returns "method", "ep" → returns "endpoint"
function _findDestructuredKey(objPattern, bindingName) {
  for (var i = 0; i < objPattern.properties.length; i++) {
    var dp = objPattern.properties[i];
    if (_t.isRestElement(dp)) continue;
    if (!_t.isObjectProperty(dp)) continue;
    var keyName = _t.isIdentifier(dp.key) ? dp.key.name :
      (_t.isStringLiteral(dp.key) ? dp.key.value : null);
    if (!keyName) continue;

    // Shorthand: {url} → key=url, value=url (Identifier)
    // Renamed: {method: m} → key=method, value=m (Identifier)
    // With default: {url = "/default"} → key=url, value=AssignmentPattern(left=url)
    var valName = null;
    if (_t.isIdentifier(dp.value)) {
      valName = dp.value.name;
    } else if (_t.isAssignmentPattern(dp.value) && _t.isIdentifier(dp.value.left)) {
      valName = dp.value.left.name;
    }
    if (valName === bindingName) return keyName;
  }
  return null;
}

function _isJsonStringify(node, path) {
  if (!node || !_t.isCallExpression(node)) return false;
  var c = node.callee;
  if (!_t.isMemberExpression(c) ||
      !_t.isIdentifier(c.object, { name: "JSON" }) ||
      !_t.isIdentifier(c.property, { name: "stringify" })) return false;
  // Verify JSON is the global, not a shadowed local
  if (path && path.scope.getBinding("JSON")) return false;
  return true;
}

function _extractLiteralArray(node) {
  if (!_t.isArrayExpression(node)) return [];
  var values = [];
  for (var i = 0; i < node.elements.length; i++) {
    var el = node.elements[i];
    if (!el) continue;
    if (_t.isStringLiteral(el) || _t.isNumericLiteral(el)) values.push(el.value);
  }
  return values;
}

function _collectIdentifiers(node, set) {
  // Iterative: walk all expression types via explicit stack
  var stack = [node];
  while (stack.length > 0) {
    var n = stack.pop();
    if (!n) continue;
    if (_t.isIdentifier(n)) { set.add(n.name); continue; }
    if (_t.isBinaryExpression(n) || _t.isLogicalExpression(n)) {
      stack.push(n.left, n.right);
    }
    if (_t.isConditionalExpression(n)) {
      stack.push(n.test, n.consequent, n.alternate);
    }
    if (_t.isCallExpression(n)) {
      for (var i = 0; i < n.arguments.length; i++) { stack.push(n.arguments[i]); }
    }
    if (_t.isNewExpression(n)) {
      for (var ni = 0; ni < n.arguments.length; ni++) { stack.push(n.arguments[ni]); }
    }
    if (_t.isTemplateLiteral(n)) {
      for (var j = 0; j < n.expressions.length; j++) { stack.push(n.expressions[j]); }
    }
    if (_t.isMemberExpression(n)) {
      stack.push(n.object);
    }
  }
}

// Iterative MemberExpression chain walk: collect prop names leaf→root,
// then build the dotted path string. Replaces tail-recursion on
// node.object — chain depth is unbounded for minified `a.b.c.d.e...`.
function _describeNode(node) {
  if (_t.isIdentifier(node)) return node.name;
  if (!_t.isMemberExpression(node)) return "(" + node.type + ")";
  var parts = [];
  var cur = node;
  while (_t.isMemberExpression(cur)) {
    var propName = _t.isIdentifier(cur.property) ? cur.property.name :
      (_t.isStringLiteral(cur.property) ? "[" + cur.property.value + "]" : "?");
    parts.push(propName);
    cur = cur.object;
  }
  parts.reverse();
  var prefix;
  if (_t.isIdentifier(cur)) prefix = cur.name;
  else if (_t.isThisExpression(cur)) prefix = "this";
  else prefix = "(" + cur.type + ")";
  return prefix + "." + parts.join(".");
}

// ─── CFG Builder + Sanitizer Path Analysis ──────────────────────────────────

// Known sanitizer globals — calling these on tainted data neutralizes it
// Use Object.create(null) so prototype methods like toString /
// hasOwnProperty / valueOf don't shadow-match the sanitizer set. An
// earlier implementation used plain object literals which made
// `_SANITIZER_METHODS["toString"]` truthy (inherited prototype
// function), silently downgrading real XSS findings where the taint
// chain passed through `.toString()` (observed on the GitHub
// remote-input-element bundle).
var _SANITIZER_GLOBALS = Object.assign(Object.create(null),
  { "encodeURIComponent":1, "encodeURI":1, "parseInt":1, "parseFloat":1, "escape":1, "btoa":1 });

// Known sanitizer methods — obj.sanitize(), obj.encode(), DOMPurify.sanitize()
var _SANITIZER_METHODS = Object.assign(Object.create(null), { "sanitize":1, "encode":1 });

// Known sanitizer objects — DOMPurify.sanitize()
var _SANITIZER_OBJECTS = Object.assign(Object.create(null), { "DOMPurify":1 });

// Check if a single call expression node is a known sanitizer.
// When a path is provided, verifies that sanitizer globals aren't shadowed by local bindings.
function _isSanitizerCall(node, path) {
  if (!_t.isCallExpression(node)) return false;
  var callee = node.callee;
  // Global sanitizer functions: encodeURIComponent, parseInt, etc.
  if (_t.isIdentifier(callee) && _SANITIZER_GLOBALS[callee.name]) {
    // If path available, verify the identifier isn't shadowed
    if (path && path.scope) {
      return !path.scope.getBinding(callee.name);
    }
    return true;
  }
  // Method sanitizers: DOMPurify.sanitize(), obj.encode(), etc.
  if (_t.isMemberExpression(callee) && !callee.computed && _t.isIdentifier(callee.property)) {
    if (_SANITIZER_METHODS[callee.property.name]) {
      // For known sanitizer objects (DOMPurify), verify the object isn't shadowed
      if (_t.isIdentifier(callee.object) && _SANITIZER_OBJECTS[callee.object.name]) {
        if (path && path.scope) return !path.scope.getBinding(callee.object.name);
        return true;
      }
      return true;
    }
    if (_t.isIdentifier(callee.object) && _SANITIZER_OBJECTS[callee.object.name]) {
      if (path && path.scope) return !path.scope.getBinding(callee.object.name);
      return true;
    }
  }
  return false;
}

// Check if a statement path contains a sanitizer call at this level only.
// Does NOT recurse into sub-statements (if/else/for/while) since those are
// represented as separate blocks in the CFG.
function _stmtContainsSanitizer(stmtPath) {
  if (!stmtPath || !stmtPath.node) return false;
  var node = stmtPath.node;
  // For IfStatement: only check the test expression, not consequent/alternate
  // (those are separate blocks in the CFG)
  if (_t.isIfStatement(node)) {
    return _exprContainsSanitizer(stmtPath.get("test"));
  }
  // For loop statements: only check the init/test/update, not body
  if (_t.isForStatement(node)) {
    return _exprContainsSanitizer(stmtPath.get("init")) ||
           _exprContainsSanitizer(stmtPath.get("test")) ||
           _exprContainsSanitizer(stmtPath.get("update"));
  }
  if (_t.isWhileStatement(node) || _t.isDoWhileStatement(node)) {
    return _exprContainsSanitizer(stmtPath.get("test"));
  }
  // For BlockStatement/ExpressionStatement/VariableDeclaration: traverse with scope
  var found = false;
  stmtPath.traverse({
    // Skip nested control flow — those are separate CFG blocks
    IfStatement: function(p) { p.skip(); },
    ForStatement: function(p) { p.skip(); },
    WhileStatement: function(p) { p.skip(); },
    DoWhileStatement: function(p) { p.skip(); },
    SwitchStatement: function(p) { p.skip(); },
    CallExpression: function(innerPath) {
      if (found) { innerPath.stop(); return; }
      if (_isSanitizerCall(innerPath.node, innerPath)) {
        found = true; innerPath.stop();
      }
    },
  });
  return found;
}

// Check if an expression path contains a sanitizer call
function _exprContainsSanitizer(exprPath) {
  if (!exprPath || !exprPath.node) return false;
  if (_isSanitizerCall(exprPath.node, exprPath)) return true;
  var found = false;
  exprPath.traverse({
    CallExpression: function(innerPath) {
      if (found) { innerPath.stop(); return; }
      if (_isSanitizerCall(innerPath.node, innerPath)) {
        found = true; innerPath.stop();
      }
    },
  });
  return found;
}

// Build a basic-block CFG from a function body path (BlockStatement)
// Stores statement paths (not raw nodes) so sanitizer detection can use scope.
// Build a basic-block CFG from a function body path (BlockStatement).
//
// Iterative (queue-based) to stay safe on deeply nested control flow —
// minified bundles can have 10k+ nested blocks, which would overflow the
// native call stack if we recursed (per CLAUDE.md: no recursive AST walks
// on chainable structures).
//
// Handles: linear sequences, IfStatement, ForStatement, ForInStatement,
// ForOfStatement, WhileStatement, DoWhileStatement, TryStatement (with
// optional handler/finalizer), SwitchStatement (each case treated as a
// linear body from the test block).  Other statement types become opaque
// single blocks — their control flow is a simple pass-through.
//
// Block semantics preserved for backward compatibility with existing
// sanitizer tests:
//   • Each statement still occupies its own block.
//   • If-consequent / if-alternate / loop-body / try-block / catch-body /
//     finalizer each form a nested linear sequence, connected to the
//     appropriate join block.
//   • Loop back-edges point body-exit → loop-header, so DFS from entry
//     still terminates via visited[].
function _buildCFG(bodyPath) {
  if (!bodyPath || !bodyPath.node) return null;
  if (!_t.isBlockStatement(bodyPath.node)) return null;

  var blockId = 0;
  var blocks = {};
  function makeBlock(stmtPaths) {
    var id = blockId++;
    blocks[id] = { id: id, stmts: stmtPaths || [], succs: [], preds: [] };
    return id;
  }
  function link(from, to) {
    if (from == null || to == null) return;
    blocks[from].succs.push(to);
    blocks[to].preds.push(from);
  }

  var entryId = makeBlock([]);
  var exitId = makeBlock([]);

  // Work queue of statement-sequences to expand. Each scope:
  //   { parentPath: Path (BlockStatement or wrapping one stmt),
  //     stmts:      [{path, node}] — ordered statements to process,
  //     entry:      block id to continue from at start,
  //     exit:       block id to connect to after the last statement }
  //
  // For a single-statement body (e.g. `if (x) doThing();` with no braces),
  // we synthesize a scope with a one-element `stmts`. This keeps the
  // algorithm uniform.
  function seqFromBody(blockStmtPath) {
    var body = blockStmtPath.node.body;
    var out = [];
    for (var i = 0; i < body.length; i++) {
      out.push({ path: blockStmtPath.get("body." + i), node: body[i] });
    }
    return out;
  }
  function seqFromSingle(stmtPath) {
    return [{ path: stmtPath, node: stmtPath.node }];
  }
  function seqFromMaybeBlock(stmtPath) {
    return _t.isBlockStatement(stmtPath.node) ? seqFromBody(stmtPath) : seqFromSingle(stmtPath);
  }

  var queue = [{
    stmts: seqFromBody(bodyPath),
    entry: entryId,
    exit: exitId,
  }];

  while (queue.length > 0) {
    var scope = queue.shift();
    var prev = scope.entry;

    for (var i = 0; i < scope.stmts.length; i++) {
      var item = scope.stmts[i];
      var stmtPath = item.path;
      var stmt = item.node;
      var bid = makeBlock([stmtPath]);
      link(prev, bid);

      if (_t.isIfStatement(stmt)) {
        var joinId = makeBlock([]);
        // Consequent.
        if (stmt.consequent) {
          var consPath = stmtPath.get("consequent");
          var consEntry = makeBlock([]);
          // Tag the consequent-entry block with the IfStatement that gates
          // it. _findKeyValidatorBlocks uses this to mark only the branch
          // reached BECAUSE of the test, not the if-statement block itself
          // (which a sibling statement after the if would also pass through).
          blocks[consEntry]._gatedByTestOf = stmtPath;
          link(bid, consEntry);
          queue.push({ stmts: seqFromMaybeBlock(consPath), entry: consEntry, exit: joinId });
        } else {
          link(bid, joinId);
        }
        // Alternate (else).
        if (stmt.alternate) {
          var altPath = stmtPath.get("alternate");
          var altEntry = makeBlock([]);
          // Tag with the NEGATED test — the alternate is entered iff the
          // test was false. For key-validation this never downgrades
          // (validator returning false doesn't sanitize), so we don't
          // flag alternate branches as validator blocks.
          link(bid, altEntry);
          queue.push({ stmts: seqFromMaybeBlock(altPath), entry: altEntry, exit: joinId });
        } else {
          link(bid, joinId);
        }
        prev = joinId;
        continue;
      }

      if (_t.isForStatement(stmt) || _t.isForInStatement(stmt) || _t.isForOfStatement(stmt) ||
          _t.isWhileStatement(stmt) || _t.isDoWhileStatement(stmt)) {
        // Header block = `bid`. It contains the loop statement itself; the
        // loop test expression is reachable as bid.stmts[0].get('test'),
        // which is exactly what _stmtContainsSanitizer inspects for loop
        // heads.
        var loopJoinId = makeBlock([]);
        // Header may directly reach join (empty iterable / false test).
        link(bid, loopJoinId);
        if (stmt.body) {
          var bodyPath2 = stmtPath.get("body");
          var bodyEntry = makeBlock([]);
          link(bid, bodyEntry);
          // body exit loops back to header — creates a cycle but DFS visit
          // tracking prevents infinite traversal.
          queue.push({ stmts: seqFromMaybeBlock(bodyPath2), entry: bodyEntry, exit: bid });
        }
        prev = loopJoinId;
        continue;
      }

      if (_t.isTryStatement(stmt)) {
        var tryJoinId = makeBlock([]);
        // Try body.
        var tryEntry = makeBlock([]);
        link(bid, tryEntry);
        queue.push({ stmts: seqFromBody(stmtPath.get("block")), entry: tryEntry, exit: tryJoinId });
        // Catch body — reachable directly from the try header (conservatively
        // assume any statement in the try can throw).
        if (stmt.handler) {
          var catchEntry = makeBlock([]);
          link(bid, catchEntry);
          queue.push({
            stmts: seqFromBody(stmtPath.get("handler.body")),
            entry: catchEntry,
            exit: tryJoinId,
          });
        }
        // Finalizer runs after try/catch; continuation flows through it.
        if (stmt.finalizer) {
          var finEntry = makeBlock([]);
          link(tryJoinId, finEntry);
          var postFinId = makeBlock([]);
          queue.push({
            stmts: seqFromBody(stmtPath.get("finalizer")),
            entry: finEntry,
            exit: postFinId,
          });
          prev = postFinId;
        } else {
          prev = tryJoinId;
        }
        continue;
      }

      if (_t.isSwitchStatement(stmt)) {
        // Header = `bid` (holds the discriminant). Each case body is a
        // linear sequence. Fall-through is modeled by linking case i's
        // body-exit to case i+1's body-entry. A `break` in source would
        // ideally redirect to switchJoinId, but since we don't track
        // break semantics we conservatively also link body-exit to
        // switchJoinId so reachability analyses don't miss the join.
        var switchJoinId = makeBlock([]);
        var prevCaseExit = null;
        var cases = stmt.cases || [];
        for (var ci = 0; ci < cases.length; ci++) {
          var casePath = stmtPath.get("cases." + ci);
          var caseEntry = makeBlock([]);
          link(bid, caseEntry);
          if (prevCaseExit != null) link(prevCaseExit, caseEntry);
          // Build statement list from the case's consequents.
          var caseStmts = [];
          for (var cj = 0; cj < (casePath.node.consequent || []).length; cj++) {
            caseStmts.push({ path: casePath.get("consequent." + cj), node: casePath.node.consequent[cj] });
          }
          var caseExit = makeBlock([]);
          queue.push({ stmts: caseStmts, entry: caseEntry, exit: caseExit });
          link(caseExit, switchJoinId);
          prevCaseExit = caseExit;
        }
        // If no case matches (no default), switch directly to join.
        link(bid, switchJoinId);
        prev = switchJoinId;
        continue;
      }

      // Default: opaque block, pass through.
      prev = bid;
    }

    // Connect the last statement block (or the scope entry if empty) to
    // the scope exit.
    link(prev, scope.exit);
  }

  return { blocks: blocks, entry: entryId, exit: exitId };
}

// Find which block contains a statement at a given line. When several blocks
// contain the line (e.g. `if (x) assign();` puts both the IfStatement and
// the inner ExpressionStatement on the same source line but in different
// CFG blocks), prefer the block with the TIGHTEST line range — that's the
// innermost statement and the one that actually contains the sink. Ties
// broken by preferring non-compound statements (leaf > container), since
// a leaf statement is what physically executes at the sink.
function _findBlockForLine(cfg, line) {
  if (!cfg) return -1;
  var best = -1;
  var bestSpan = Infinity;
  var bestIsLeaf = false;
  var keys = Object.keys(cfg.blocks);
  for (var i = 0; i < keys.length; i++) {
    var blk = cfg.blocks[keys[i]];
    for (var j = 0; j < blk.stmts.length; j++) {
      var s = blk.stmts[j];
      var sNode = s && s.node ? s.node : s;
      if (!sNode || !sNode.loc) continue;
      if (!(sNode.loc.start.line <= line && sNode.loc.end.line >= line)) continue;
      var span = sNode.loc.end.line - sNode.loc.start.line;
      var isLeaf = !_t.isIfStatement(sNode) && !_t.isForStatement(sNode) &&
                   !_t.isForInStatement(sNode) && !_t.isForOfStatement(sNode) &&
                   !_t.isWhileStatement(sNode) && !_t.isDoWhileStatement(sNode) &&
                   !_t.isTryStatement(sNode) && !_t.isSwitchStatement(sNode) &&
                   !_t.isBlockStatement(sNode);
      if (span < bestSpan || (span === bestSpan && isLeaf && !bestIsLeaf)) {
        best = blk.id;
        bestSpan = span;
        bestIsLeaf = isLeaf;
      }
    }
  }
  return best;
}

// Find all blocks that contain sanitizer calls
function _findSanitizerBlocks(cfg) {
  var result = {};
  if (!cfg) return result;
  var keys = Object.keys(cfg.blocks);
  for (var i = 0; i < keys.length; i++) {
    var blk = cfg.blocks[keys[i]];
    for (var j = 0; j < blk.stmts.length; j++) {
      if (_stmtContainsSanitizer(blk.stmts[j])) {
        result[blk.id] = true;
        break;
      }
    }
  }
  return result;
}

// Check if all paths from entry to sinkBlock pass through a sanitizer block
function _hasSanitizerOnAllPaths(cfg, sinkBlockId, sanitizerBlocks) {
  if (!cfg) return false;
  // BFS from entry to sinkBlock, checking if every path passes through a sanitizer
  // Use DFS with path tracking
  var found = false;
  var allSanitized = true;

  function dfs(blockId, visited, sawSanitizer) {
    if (!allSanitized) return;
    if (blockId === sinkBlockId) {
      found = true;
      if (!sawSanitizer) allSanitized = false;
      return;
    }
    var blk = cfg.blocks[blockId];
    if (!blk) return;
    var nextSanitizer = sawSanitizer || !!sanitizerBlocks[blockId];
    for (var i = 0; i < blk.succs.length; i++) {
      var next = blk.succs[i];
      if (visited[next]) continue;
      visited[next] = true;
      dfs(next, visited, nextSanitizer);
      visited[next] = false;
    }
  }

  var visited = {};
  visited[cfg.entry] = true;
  dfs(cfg.entry, visited, !!sanitizerBlocks[cfg.entry]);
  return found && allSanitized;
}

// ─── Allowlist-key validation (for prototype-pollution downgrade) ──────────
//
// Same CFG infrastructure as _checkSanitization, but with validator
// blocks identified by a predicate that matches the sink's dynamic key.
// Uses `_buildCFG` + `_hasSanitizerOnAllPaths` unchanged; only the block
// matcher differs.
//
// Requirements enforced:
//   1. Argument of `.includes(X)` / `.has(X)` / `X in coll` must resolve
//      to the same binding as the sink's dynamic key, via scope.getBinding
//      (not string-name comparison).
//   2. Receiver (collection) must NOT trace back to user-controlled data
//      per _traceValueSource — a tainted allowlist isn't sanitization.
//   3. Only recognized predicates count: `.includes`, `.has`,
//      `.hasOwnProperty`, and the `in` operator. Avoids false "sanitized"
//      on `.indexOf()` without `>= 0`, `Array.from()`, etc.

function _keyNodeToIdentifierName(keyNode) {
  if (_t.isIdentifier(keyNode)) return keyNode.name;
  // Sink key can be a call like `k.toLowerCase()` or `k.trim()` — underlying
  // binding is the callee's object.
  if (_t.isCallExpression(keyNode) && _t.isMemberExpression(keyNode.callee) &&
      !keyNode.callee.computed && _t.isIdentifier(keyNode.callee.object)) {
    return keyNode.callee.object.name;
  }
  return null;
}

function _keyArgMatches(argExpr, argPath, keyIdent, keyBinding) {
  if (!argExpr) return false;
  var name;
  if (_t.isIdentifier(argExpr)) {
    name = argExpr.name;
  } else if (_t.isCallExpression(argExpr) && _t.isMemberExpression(argExpr.callee) &&
             !argExpr.callee.computed && _t.isIdentifier(argExpr.callee.object)) {
    name = argExpr.callee.object.name;
  } else {
    return false;
  }
  if (name !== keyIdent) return false;
  // Scope-aware binding comparison: the validator's identifier must resolve
  // to the same binding as the sink's key. Prevents shadowing FPs.
  if (keyBinding && argPath && argPath.scope) {
    var argBinding = argPath.scope.getBinding(name);
    if (argBinding) return argBinding === keyBinding;
  }
  // If either binding is unavailable, fall back to name equality (rare —
  // but don't crash on unbound locals).
  return true;
}

// Does `testPath` validate the sink's key against a non-tainted collection?
function _testValidatesKey(testPath, keyIdent, keyBinding) {
  if (!testPath || !testPath.node) return false;
  var found = false;
  function matchesOne(subPath) {
    if (found || !subPath || !subPath.node) return;
    var n = subPath.node;
    // `key in collection`
    if (_t.isBinaryExpression(n) && n.operator === "in") {
      if (_keyArgMatches(n.left, subPath.get("left"), keyIdent, keyBinding)) {
        var srcIn = _traceValueSource(subPath.get("right"));
        if (srcIn.sourceType !== "user-controlled") { found = true; return; }
      }
    }
    // `collection.<validator>(key)`
    if (_t.isCallExpression(n) && _t.isMemberExpression(n.callee) && !n.callee.computed &&
        _t.isIdentifier(n.callee.property)) {
      var m = n.callee.property.name;
      if (m === "includes" || m === "has" || m === "hasOwnProperty") {
        var arg = n.arguments[0];
        if (arg) {
          var argPath = subPath.get("arguments.0");
          if (_keyArgMatches(arg, argPath, keyIdent, keyBinding)) {
            var srcColl = _traceValueSource(subPath.get("callee.object"));
            if (srcColl.sourceType !== "user-controlled") { found = true; return; }
          }
        }
      }
    }
  }
  matchesOne(testPath);
  if (found) return true;
  testPath.traverse({
    LogicalExpression: function(p) {
      if (found) { p.stop(); return; }
      matchesOne(p.get("left"));
      if (!found) matchesOne(p.get("right"));
      if (found) p.stop();
    },
    CallExpression: function(p) { if (found) { p.stop(); return; } matchesOne(p); if (found) p.stop(); },
    BinaryExpression: function(p) { if (found) { p.stop(); return; } matchesOne(p); if (found) p.stop(); },
  });
  return found;
}

// Mark a block as a validator if it's the consequent-entry of an
// IfStatement whose test validates the sink's key. Only consequent
// branches count — the if-block itself is shared by paths that bypass
// the consequent (e.g. statements after the if), so treating the if-block
// as a validator would incorrectly whitelist unvalidated sinks.
function _findKeyValidatorBlocks(cfg, keyIdent, keyBinding) {
  var out = {};
  if (!cfg) return out;
  var keys = Object.keys(cfg.blocks);
  for (var i = 0; i < keys.length; i++) {
    var blk = cfg.blocks[keys[i]];
    var gate = blk._gatedByTestOf;
    if (gate && gate.node && _t.isIfStatement(gate.node) &&
        _testValidatesKey(gate.get("test"), keyIdent, keyBinding)) {
      out[blk.id] = true;
    }
  }
  return out;
}

// Inline short-circuit check: is the sink's expression positioned as the
// RHS of a `&&` (or consequent arm of `?:`) whose test validates the key?
// JavaScript's short-circuit semantics guarantee the sink won't evaluate
// unless the test passed — so this is true sanitization even though the
// CFG puts both in one block.
function _isInlineShortCircuitGuarded(sinkPath, keyIdent, keyBinding) {
  var cur = sinkPath;
  while (cur && cur.parentPath) {
    var parent = cur.parentPath;
    var p = parent.node;
    // Stop walking once we leave the expression context.
    if (_t.isStatement(p) && !_t.isExpressionStatement(p)) return false;
    // `TEST && (sink)` — sink is on the right of &&
    if (_t.isLogicalExpression(p) && p.operator === "&&" && p.right === cur.node) {
      if (_testValidatesKey(parent.get("left"), keyIdent, keyBinding)) return true;
    }
    // `TEST ? sink : fallback` — sink is in consequent
    if (_t.isConditionalExpression(p) && p.consequent === cur.node) {
      if (_testValidatesKey(parent.get("test"), keyIdent, keyBinding)) return true;
    }
    cur = parent;
  }
  return false;
}

// Check if every control-flow path from the function entry to the sink's
// block passes through a block that validates the sink's key against a
// non-tainted allowlist. Uses the same _hasSanitizerOnAllPaths engine as
// the existing sanitizer analysis, with one fix-up: when the validator is
// inline with the sink (`validator && sink`), short-circuit semantics
// cover it even though both share a CFG block.
function _checkKeyValidation(sinkPath, keyNode) {
  var keyIdent = _keyNodeToIdentifierName(keyNode);
  if (!keyIdent) return false;
  var keyBinding = sinkPath.scope.getBinding(keyIdent);

  // Fast path: inline `validator && sink` / `validator ? sink : …` — the
  // validator is literally an operand of the same expression that
  // contains the sink, so short-circuit evaluation dominates the sink.
  if (_isInlineShortCircuitGuarded(sinkPath, keyIdent, keyBinding)) return true;

  // Slower path: validator is in an earlier statement. Use the shared
  // CFG + _hasSanitizerOnAllPaths engine.
  var funcPath = sinkPath.getFunctionParent();
  if (!funcPath) return false;
  var bodyPath = funcPath.get("body");
  if (!bodyPath.isBlockStatement()) return false;
  var cfg = _buildCFG(bodyPath);
  if (!cfg) return false;
  var validatorBlocks = _findKeyValidatorBlocks(cfg, keyIdent, keyBinding);
  if (Object.keys(validatorBlocks).length === 0) return false;
  var sinkLine = sinkPath.node.loc ? sinkPath.node.loc.start.line : -1;
  if (sinkLine === -1) return false;
  var sinkBlockId = _findBlockForLine(cfg, sinkLine);
  if (sinkBlockId === -1) return false;
  return _hasSanitizerOnAllPaths(cfg, sinkBlockId, validatorBlocks);
}

// Check if the sink at the given path is sanitized on all control flow paths
function _checkSanitization(path) {
  // Find the enclosing function
  var funcPath = path.getFunctionParent();
  if (!funcPath) return false;
  var funcBody = funcPath.node.body;
  if (!_t.isBlockStatement(funcBody)) return false;

  // Build CFG from body path (stores statement paths for scope-aware sanitizer detection)
  var bodyPath = funcPath.get("body");
  var cfg = _buildCFG(bodyPath);
  if (!cfg) return false;

  // Find sanitizer blocks
  var sanitizerBlocks = _findSanitizerBlocks(cfg);
  if (Object.keys(sanitizerBlocks).length === 0) return false;

  // Find sink block
  var sinkLine = path.node.loc ? path.node.loc.start.line : -1;
  if (sinkLine === -1) return false;
  var sinkBlockId = _findBlockForLine(cfg, sinkLine);
  if (sinkBlockId === -1) return false;

  return _hasSanitizerOnAllPaths(cfg, sinkBlockId, sanitizerBlocks);
}

// Return a per-call classification for the sanitizer decision the
// analyzer just made. This extends _isSanitizerCall's boolean with a
// short label + the reason the call was accepted or rejected, so a
// finding auditor can see WHICH calls the classifier considered and
// WHY each was or wasn't treated as a sanitizer.
//
// Returns null for a call that isn't a sanitizer candidate at all
// (i.e., the call's shape doesn't even resemble the sanitizer set —
// most calls in real code fall here, so we don't flood the output).
function _classifySanitizerCall(node, path) {
  if (!_t.isCallExpression(node)) return null;
  var callee = node.callee;
  if (_t.isIdentifier(callee)) {
    if (!_SANITIZER_GLOBALS[callee.name]) return null;
    var shadowed = !!(path && path.scope && path.scope.getBinding(callee.name));
    return {
      label: callee.name + "()",
      matched: !shadowed,
      matchReason: shadowed
        ? "'" + callee.name + "' is a known sanitizer global but shadowed in this scope — can't verify behavior"
        : "known sanitizer global '" + callee.name + "' (unbound — real global)",
    };
  }
  if (_t.isMemberExpression(callee) && !callee.computed && _t.isIdentifier(callee.property)) {
    var methodName = callee.property.name;
    var methodKnown = !!_SANITIZER_METHODS[methodName];
    var objIsIdent = _t.isIdentifier(callee.object);
    var objName = objIsIdent ? callee.object.name : null;
    var objKnown = objIsIdent && !!_SANITIZER_OBJECTS[objName];
    if (methodKnown && objKnown) {
      var shadowedObj = !!(path && path.scope && path.scope.getBinding(objName));
      return {
        label: objName + "." + methodName + "()",
        matched: !shadowedObj,
        matchReason: shadowedObj
          ? "object '" + objName + "' is a known sanitizer but shadowed — can't verify this call is the real one"
          : "known sanitizer method '" + methodName + "' on known object '" + objName + "'",
      };
    }
    if (methodKnown) {
      return {
        label: (objName || "<expr>") + "." + methodName + "()",
        matched: true,
        matchReason: "method name '" + methodName + "' is in the known-sanitizer-method set (object not verified)",
      };
    }
    if (objKnown) {
      var shadowedObj2 = !!(path && path.scope && path.scope.getBinding(objName));
      return {
        label: objName + "." + methodName + "()",
        matched: !shadowedObj2,
        matchReason: shadowedObj2
          ? "sanitizer object '" + objName + "' is shadowed — can't verify"
          : "any method call on known sanitizer object '" + objName + "'",
      };
    }
  }
  return null;
}

// Build a structured audit report for the sanitizer decision around a
// sink. Every recognised sanitizer-shaped call in the enclosing function
// is recorded with its match verdict, match reason, and whether the call
// dominates the sink (on every path from entry to sink block). Surfaced
// on the finding so an auditor can see the classifier's work without
// reading the minified source.
function _buildSanitizerReport(sinkPath) {
  var funcPath = sinkPath.getFunctionParent();
  if (!funcPath) return { decision: "no-function-scope", candidates: [] };
  if (!_t.isBlockStatement(funcPath.node.body)) return { decision: "no-block-body", candidates: [] };
  var bodyPath = funcPath.get("body");
  var candidates = [];
  try {
    bodyPath.traverse({
      CallExpression: function(p) {
        var c = _classifySanitizerCall(p.node, p);
        if (!c) return;
        candidates.push({
          label: c.label,
          loc: p.node.loc ? { line: p.node.loc.start.line, column: p.node.loc.start.column } : null,
          matched: c.matched,
          matchReason: c.matchReason,
          onPath: null,
        });
      },
    });
  } catch (e) { _resolver.collectError(e, "sanitizerReport-traverse"); }
  if (candidates.length === 0) return { decision: "no-candidates", candidates: [] };

  // Compute on-path: is this candidate's block visited by every path
  // from entry to the sink's block? A sanitizer that only fires in one
  // branch (if/else) doesn't sanitize the sink — it must dominate.
  var cfg = _buildCFG(bodyPath);
  var anyMatchedOnPath = false;
  if (cfg) {
    var sinkLine = sinkPath.node.loc ? sinkPath.node.loc.start.line : -1;
    var sinkBlockId = sinkLine !== -1 ? _findBlockForLine(cfg, sinkLine) : -1;
    if (sinkBlockId !== -1) {
      for (var i = 0; i < candidates.length; i++) {
        var c = candidates[i];
        if (!c.matched || !c.loc) { c.onPath = false; continue; }
        var candBlockId = _findBlockForLine(cfg, c.loc.line);
        if (candBlockId === -1) { c.onPath = false; continue; }
        var singleton = {}; singleton[candBlockId] = true;
        c.onPath = _hasSanitizerOnAllPaths(cfg, sinkBlockId, singleton);
        if (c.onPath) anyMatchedOnPath = true;
      }
    }
  }
  return {
    decision: anyMatchedOnPath ? "fires-on-all-paths" : "missing-on-some-paths",
    candidates: candidates,
  };
}

function extractSourceMapUrl(code) {
  var tail = code.length > 500 ? code.slice(-500) : code;
  var marker = "sourceMappingURL=";
  var idx = tail.indexOf(marker);
  if (idx === -1) return null;
  var start = idx + marker.length;
  while (start < tail.length && (tail.charCodeAt(start) === 32 || tail.charCodeAt(start) === 9)) start++;
  var end = start;
  while (end < tail.length && tail.charCodeAt(end) > 32) end++;
  return end > start ? tail.substring(start, end) : null;
}

// ─── Definition Map (for viewer click-to-definition) ────────────────────────
// Builds scope-resolved reference → definition mappings using the same Babel
// scope system and type tracking as analyzeJSBundle.
// Returns { defMap, refMap, funcMap, allFuncRanges, ast }.

// buildDefinitionMap takes an optional third `opts.mode` arg:
//   "full"    — build defMap + refMap + funcMap + allFuncRanges + propDefs.
//               Needs the Identifier visitor which calls
//               path.scope.getBinding() on every identifier — millions
//               of scope walks on a minified 5 MB bundle. Only the
//               source-viewer (click-to-definition) consumes refMap, so
//               pay this cost on demand.
//   "defOnly" — build defMap + propDefs only (the eager-path consumers).
//               Skips the Identifier visitor entirely, saving ~24 s on
//               real github's combined react-core bundle. Used by
//               AST_ANALYZE's eager buildDefinitionMap companion call.
function buildDefinitionMap(code, preParsedAst, opts) {
  // Reset per-analysis type state (shared with analyzeJSBundle)
  _typeEnv = {};

  var mode = (opts && opts.mode) || "full";
  var collectRefs = mode === "full";

  var defMap = {};         // { name: line } — named function/variable definitions
  var refMap = {};         // { refLine: { name: defLine } } — scope-resolved per-reference
  var funcMap = {};        // { name: { line, endLine, calls } } — named functions for call graph
  var allFuncRanges = [];  // [{ line, endLine, calls }] — all functions (named + anon)
  var propDefs = {};       // { "defLine:name" → { propName: propDefLine } } — property definitions
  var pendingProps = [];   // [{ refLine, propName, ownerKey }] — deferred property accesses
  var ast = preParsedAst || null;

  var _bdmParseT0 = Date.now();
  if (!ast) {
    try {
      ast = _babelParse(code, { sourceType: "unambiguous", plugins: ["jsx"], errorRecovery: true });
    } catch (e) {
      return { defMap: defMap, refMap: refMap, funcMap: funcMap, allFuncRanges: allFuncRanges, propDefs: propDefs, ast: null };
    }
  }
  var _bdmParseMs = Date.now() - _bdmParseT0;
  var _bdmTraverseT0 = Date.now();

  // Function stack for call-graph collection. Previously, each function's
  // calls were gathered via `funcPath.traverse(...)` — a sub-tree walk per
  // function. On a 6.5MB minified bundle with thousands of nested
  // functions, this is quadratic: the same inner node gets visited once
  // per ancestor function. Cost on real github main page: ~36s of the
  // ~45s full AST pass. The single-traversal version below walks each
  // node ONCE and attributes CallExpressions to the top of an explicit
  // function stack — linear in AST size.
  var _funcStack = [];
  function registerFunc(path, name, funcNode) {
    var loc = funcNode ? funcNode.loc : path.node.loc;
    if (!loc) return null;
    var entry = { line: loc.start.line, endLine: loc.end.line, calls: new Set() };
    allFuncRanges.push(entry);
    if (name) {
      funcMap[name] = entry;
      defMap[name] = loc.start.line;
    }
    return entry;
  }

  try {
  _babelTraverse(ast, {
    FunctionDeclaration: {
      enter: function(path) {
        _funcStack.push(registerFunc(path, path.node.id ? path.node.id.name : null, path.node));
      },
      exit: function() { _funcStack.pop(); },
    },
    FunctionExpression: {
      enter: function(path) {
        var name = null;
        if (path.parent.type === "VariableDeclarator" && path.parent.id && path.parent.id.name)
          name = path.parent.id.name;
        else if (path.parent.type === "AssignmentExpression" && path.parent.left.type === "Identifier")
          name = path.parent.left.name;
        _funcStack.push(registerFunc(path, name, path.node));
      },
      exit: function() { _funcStack.pop(); },
    },
    ArrowFunctionExpression: {
      enter: function(path) {
        var name = null;
        if (path.parent.type === "VariableDeclarator" && path.parent.id && path.parent.id.name)
          name = path.parent.id.name;
        else if (path.parent.type === "AssignmentExpression" && path.parent.left.type === "Identifier")
          name = path.parent.left.name;
        _funcStack.push(registerFunc(path, name, path.node));
      },
      exit: function() { _funcStack.pop(); },
    },
    ClassDeclaration: function(path) {
      registerFunc(path, path.node.id ? path.node.id.name : null, path.node);
    },
    ObjectMethod: {
      enter: function(path) {
        _funcStack.push(registerFunc(path, path.node.key && path.node.key.name ? path.node.key.name : null, path.node));
      },
      exit: function() { _funcStack.pop(); },
    },
    // CallExpression: attribute each call to the innermost enclosing
    // function. Scope-aware binding check preserves the previous filter
    // (parameters don't count as callable targets).
    CallExpression: function(inner) {
      if (!_funcStack.length) return;
      var top = _funcStack[_funcStack.length - 1];
      if (!top) return;
      var callee = inner.node.callee;
      if (_t.isIdentifier(callee)) {
        var binding = inner.scope.getBinding(callee.name);
        if (!binding || _t.isFunctionDeclaration(binding.path.node) ||
            (binding.path.isVariableDeclarator() && binding.path.node.init &&
             _t.isFunction(binding.path.node.init)) ||
            (binding.path.isAssignmentExpression && binding.constantViolations &&
             binding.constantViolations.some(function(cv) { return cv.isAssignmentExpression() && _t.isFunction(cv.node.right); })))
          top.calls.add(callee.name);
      } else if (_t.isMemberExpression(callee) && _t.isIdentifier(callee.property)) {
        top.calls.add(callee.property.name);
      }
    },
    ClassMethod: {
      enter: function(path) {
      var name = path.node.key && path.node.key.name ? path.node.key.name : null;
      _funcStack.push(registerFunc(path, name, path.node));
      // Track class methods as properties of the class binding
      if (name) {
        var classBody = path.parentPath;
        var classNode = classBody ? classBody.parentPath : null;
        if (classNode && classNode.node.id && _t.isIdentifier(classNode.node.id)) {
          var cbinding = classNode.scope.getBinding(classNode.node.id.name);
          if (cbinding && cbinding.identifier && cbinding.identifier.loc) {
            var ckey = cbinding.identifier.loc.start.line + ":" + classNode.node.id.name;
            if (!propDefs[ckey]) propDefs[ckey] = {};
            propDefs[ckey][name] = path.node.loc ? path.node.loc.start.line : 0;
          }
        }
      }
      },
      exit: function() { _funcStack.pop(); },
    },
    // Populate type tracker: var x = new XMLHttpRequest() → "XMLHttpRequest"
    VariableDeclarator: function(path) {
      _trackTypeFromDeclarator(path);
      // Track object literal properties: var obj = { foo: fn, bar: 42 }
      if (_t.isIdentifier(path.node.id) && _t.isObjectExpression(path.node.init)) {
        var props = path.node.init.properties;
        if (props.length > 0) {
          var ownerBinding = path.scope.getBinding(path.node.id.name);
          if (ownerBinding && ownerBinding.identifier && ownerBinding.identifier.loc) {
            var okey = ownerBinding.identifier.loc.start.line + ":" + path.node.id.name;
            if (!propDefs[okey]) propDefs[okey] = {};
            for (var pi = 0; pi < props.length; pi++) {
              var p = props[pi];
              if (p.computed || !p.key) continue;
              var pname = _t.isIdentifier(p.key) ? p.key.name : (_t.isStringLiteral(p.key) ? p.key.value : null);
              if (pname && p.key.loc) propDefs[okey][pname] = p.key.loc.start.line;
            }
          }
        }
      }
    },
    // Track property definitions via assignment: obj.prop = value, obj = { ... }, this.prop = value
    AssignmentExpression: function(path) {
      var left = path.node.left;
      if (!left) return;
      // Handle this.prop = value inside constructors/functions
      // Tracks as propDefs for the enclosing class or named function
      if (_t.isMemberExpression(left) && !left.computed &&
          _t.isThisExpression(left.object) && _t.isIdentifier(left.property)) {
        var thisPropName = left.property.name;
        var thisPropLine = left.property.loc ? left.property.loc.start.line : 0;
        if (thisPropLine) {
          // Walk up to find the enclosing class or named function
          var enclosing = path.getFunctionParent();
          if (enclosing) {
            var classOrFunc = null;
            // Class constructor: method named "constructor" inside ClassBody
            if (enclosing.node.type === "ClassMethod" && enclosing.node.kind === "constructor") {
              var cbody = enclosing.parentPath;
              classOrFunc = cbody ? cbody.parentPath : null;
            }
            // Function used as constructor: function Foo() { this.x = ... }
            // or var Foo = function() { this.x = ... }
            if (!classOrFunc) {
              if (enclosing.node.type === "FunctionDeclaration" && enclosing.node.id) {
                classOrFunc = enclosing;
              } else if (enclosing.node.type === "FunctionExpression") {
                if (enclosing.parent.type === "VariableDeclarator" && _t.isIdentifier(enclosing.parent.id)) {
                  classOrFunc = enclosing.parentPath.parentPath ? enclosing.parentPath : enclosing;
                } else if (enclosing.parent.type === "AssignmentExpression" && _t.isIdentifier(enclosing.parent.left)) {
                  classOrFunc = enclosing.parentPath;
                }
              }
            }
            if (classOrFunc) {
              var ctorName = null;
              var ctorNode = classOrFunc.node;
              if (ctorNode.type === "ClassDeclaration" || ctorNode.type === "ClassExpression") {
                if (ctorNode.id && _t.isIdentifier(ctorNode.id)) ctorName = ctorNode.id.name;
                else if (classOrFunc.parent.type === "VariableDeclarator" && _t.isIdentifier(classOrFunc.parent.id)) ctorName = classOrFunc.parent.id.name;
                else if (classOrFunc.parent.type === "AssignmentExpression" && _t.isIdentifier(classOrFunc.parent.left)) ctorName = classOrFunc.parent.left.name;
              } else if (ctorNode.type === "FunctionDeclaration" && ctorNode.id) {
                ctorName = ctorNode.id.name;
              } else if (ctorNode.type === "VariableDeclaration") {
                var decl = ctorNode.declarations && ctorNode.declarations[0];
                if (decl && _t.isIdentifier(decl.id)) ctorName = decl.id.name;
              } else if (ctorNode.type === "AssignmentExpression" && _t.isIdentifier(ctorNode.left)) {
                ctorName = ctorNode.left.name;
              }
              if (ctorName) {
                var ctorBinding = path.scope.getBinding(ctorName);
                if (ctorBinding && ctorBinding.identifier && ctorBinding.identifier.loc) {
                  var ctorKey = ctorBinding.identifier.loc.start.line + ":" + ctorName;
                  if (!propDefs[ctorKey]) propDefs[ctorKey] = {};
                  if (!propDefs[ctorKey][thisPropName]) propDefs[ctorKey][thisPropName] = thisPropLine;
                }
              }
            }
          }
        }
      }
      // Handle obj = { prop: value } — object literal assigned to identifier
      if (_t.isIdentifier(left) && _t.isObjectExpression(path.node.right)) {
        var oprops = path.node.right.properties;
        if (oprops.length > 0) {
          var olbinding = path.scope.getBinding(left.name);
          if (olbinding && olbinding.identifier && olbinding.identifier.loc) {
            var olkey = olbinding.identifier.loc.start.line + ":" + left.name;
            if (!propDefs[olkey]) propDefs[olkey] = {};
            for (var oi = 0; oi < oprops.length; oi++) {
              var op = oprops[oi];
              if (op.computed || !op.key) continue;
              var opname = _t.isIdentifier(op.key) ? op.key.name : (_t.isStringLiteral(op.key) ? op.key.value : null);
              if (opname && op.key.loc) propDefs[olkey][opname] = op.key.loc.start.line;
            }
          }
        }
      }
      if (!_t.isMemberExpression(left) || left.computed) return;
      if (!_t.isIdentifier(left.property)) return;
      var propName = left.property.name;
      var propLine = left.property.loc ? left.property.loc.start.line : 0;
      if (!propLine) return;
      if (_t.isIdentifier(left.object)) {
        var abinding = path.scope.getBinding(left.object.name);
        if (abinding && abinding.identifier && abinding.identifier.loc) {
          var akey = abinding.identifier.loc.start.line + ":" + left.object.name;
          if (!propDefs[akey]) propDefs[akey] = {};
          propDefs[akey][propName] = propLine;
        }
      // Prototype: Cls.prototype.method = value
      } else if (_t.isMemberExpression(left.object) && !left.object.computed &&
                 _t.isIdentifier(left.object.property, { name: "prototype" }) &&
                 _t.isIdentifier(left.object.object)) {
        var pbinding = path.scope.getBinding(left.object.object.name);
        if (pbinding && pbinding.identifier && pbinding.identifier.loc) {
          var pkey = pbinding.identifier.loc.start.line + ":" + left.object.object.name;
          if (!propDefs[pkey]) propDefs[pkey] = {};
          propDefs[pkey][propName] = propLine;
        }
      }
    },
    // Collect property accesses for deferred resolution
    MemberExpression: function(path) {
      if (path.node.computed) return;
      var prop = path.node.property;
      if (!prop || !_t.isIdentifier(prop) || !prop.loc) return;
      var obj = path.node.object;
      if (!_t.isIdentifier(obj)) return;
      var mbinding = path.scope.getBinding(obj.name);
      if (!mbinding || !mbinding.identifier || !mbinding.identifier.loc) return;
      var ownerKey = mbinding.identifier.loc.start.line + ":" + obj.name;
      pendingProps.push({ refLine: prop.loc.start.line, propName: prop.name, ownerKey: ownerKey, binding: mbinding });
    },
    // Scope-resolved identifier references. This visitor is the main
    // cost of a "full" build (~24 s on a 5 MB minified bundle) because
    // path.scope.getBinding() walks the scope chain on every identifier.
    // Skip it entirely in "defOnly" mode — only the viewer consumes
    // refMap, so the caller passes mode:"full" when the viewer actually
    // asks for it.
    Identifier: function(path) {
      if (!collectRefs) return;
      if (!path.node.loc) return;
      var name = path.node.name;
      // Skip property keys and object literal keys
      if (_t.isMemberExpression(path.parent) && path.parent.property === path.node && !path.parent.computed) return;
      if ((_t.isObjectProperty(path.parent) || _t.isObjectMethod(path.parent)) && path.parent.key === path.node && !path.parent.computed) return;
      var binding = path.scope.getBinding(name);
      if (!binding || !binding.identifier || !binding.identifier.loc) return;
      var defLine = binding.identifier.loc.start.line;
      var refLine = path.node.loc.start.line;
      if (refLine === defLine) return;
      if (!refMap[refLine]) refMap[refLine] = {};
      refMap[refLine][name] = defLine;
    },
  });
  } catch (e) {
    console.debug("[AST:defMap] Traversal error:", e.message);
  }

  // Extract constructor name from a NewExpression node
  function _extractNewCtor(node) {
    if (!node || node.type !== "NewExpression") return null;
    if (_t.isIdentifier(node.callee)) return node.callee.name;
    if (_t.isMemberExpression(node.callee) && _t.isIdentifier(node.callee.property))
      return node.callee.property.name;
    return null;
  }

  // Walk an expression tree to find all NewExpression constructor names.
  // Handles ternary chains (a ? new X() : new Y()), logical (a || new X()),
  // and instanceof checks (a instanceof X ? a : new X(...)).
  function _collectNewCtors(node) {
    if (!node) return [];
    if (node.type === "NewExpression") {
      var n = _extractNewCtor(node);
      return n ? [n] : [];
    }
    if (node.type === "ConditionalExpression") {
      return _collectNewCtors(node.consequent).concat(_collectNewCtors(node.alternate));
    }
    if (node.type === "LogicalExpression") {
      return _collectNewCtors(node.left).concat(_collectNewCtors(node.right));
    }
    // instanceof check: `x instanceof Ctor` means x is Ctor too
    if (node.type === "Identifier") return []; // can't determine
    return [];
  }

  // Resolve pending property accesses against collected propDefs.
  // Also check type-based resolution: if obj's type is tracked, look up
  // propDefs for the constructor's binding.
  for (var ri = 0; ri < pendingProps.length; ri++) {
    var pp = pendingProps[ri];
    var resolved = false;
    // Direct: propDefs has the owner key
    if (propDefs[pp.ownerKey] && propDefs[pp.ownerKey][pp.propName]) {
      var directLine = propDefs[pp.ownerKey][pp.propName];
      if (pp.refLine !== directLine) {
        if (!refMap[pp.refLine]) refMap[pp.refLine] = {};
        if (!refMap[pp.refLine][pp.propName]) {
          refMap[pp.refLine][pp.propName] = directLine;
          resolved = true;
        }
      }
    }
    // Type-based: obj was created via `new Ctor()`, check Ctor's propDefs
    if (!resolved) {
      var parts = pp.ownerKey.split(":");
      var ownerName = parts.slice(1).join(":");
      // Walk scopes to find the binding and its type
      for (var tk in _typeEnv) {
        if (tk.endsWith(":" + ownerName) && _typeEnv[tk]) {
          var trackedType = _typeEnv[tk];
          // Find propDefs for the constructor
          for (var dk in propDefs) {
            if (dk.endsWith(":" + trackedType) && propDefs[dk][pp.propName]) {
              var typeLine = propDefs[dk][pp.propName];
              if (pp.refLine !== typeLine) {
                if (!refMap[pp.refLine]) refMap[pp.refLine] = {};
                if (!refMap[pp.refLine][pp.propName]) {
                  refMap[pp.refLine][pp.propName] = typeLine;
                }
              }
              break;
            }
          }
          break;
        }
      }
    }
    // Inter-procedural: obj is a parameter — trace call sites of the
    // enclosing function to find what constructor type is passed.
    if (!resolved && pp.binding && pp.binding.kind === "param") {
      var funcPath = pp.binding.path;
      // Walk up to the function that owns this parameter
      while (funcPath && funcPath.type !== "FunctionDeclaration" &&
             funcPath.type !== "FunctionExpression" &&
             funcPath.type !== "ArrowFunctionExpression") {
        funcPath = funcPath.parentPath;
      }
      if (funcPath) {
        // Find which parameter index this binding corresponds to
        var paramIdx = -1;
        var paramKey = null;  // for destructured object patterns, the property key
        var funcParams = funcPath.node.params;
        for (var fpi = 0; fpi < funcParams.length; fpi++) {
          var fp = funcParams[fpi];
          if (_t.isIdentifier(fp) && fp === pp.binding.identifier) {
            paramIdx = fpi;
            break;
          }
          // Destructured: { kh: c } — find the key name
          if (_t.isObjectPattern(fp)) {
            for (var opi = 0; opi < fp.properties.length; opi++) {
              var oprop = fp.properties[opi];
              if (oprop.value === pp.binding.identifier ||
                  (_t.isIdentifier(oprop.value) && oprop.value.name === pp.binding.identifier.name &&
                   oprop.value.start === pp.binding.identifier.start)) {
                paramIdx = fpi;
                paramKey = _t.isIdentifier(oprop.key) ? oprop.key.name :
                           (_t.isStringLiteral(oprop.key) ? oprop.key.value : null);
                break;
              }
            }
            if (paramIdx >= 0) break;
          }
        }
        if (paramIdx >= 0) {
          // Find the function's name binding to check call sites
          var funcName = null;
          if (funcPath.node.id && _t.isIdentifier(funcPath.node.id)) {
            funcName = funcPath.node.id.name;
          } else if (funcPath.parent.type === "VariableDeclarator" && _t.isIdentifier(funcPath.parent.id)) {
            funcName = funcPath.parent.id.name;
          } else if (funcPath.parent.type === "AssignmentExpression" && _t.isIdentifier(funcPath.parent.left)) {
            funcName = funcPath.parent.left.name;
          }
          if (funcName) {
            var funcBinding = funcPath.scope.getBinding(funcName);
            if (!funcBinding) {
              // Function might be declared in outer scope
              var outerScope = funcPath.scope.parent;
              if (outerScope) funcBinding = outerScope.getBinding(funcName);
            }
            if (funcBinding && funcBinding.referencePaths) {
              // Check each call site
              for (var csi = 0; csi < funcBinding.referencePaths.length && !resolved; csi++) {
                var csRef = funcBinding.referencePaths[csi];
                if (!csRef.parent || csRef.parent.type !== "CallExpression" || csRef.parent.callee !== csRef.node) continue;
                var csArgs = csRef.parent.arguments;
                if (paramIdx >= csArgs.length) continue;
                var argNode = csArgs[paramIdx];
                // For destructured params, the argument is an ObjectExpression —
                // find the matching property key
                if (paramKey && _t.isObjectExpression(argNode)) {
                  var foundProp = null;
                  for (var cpi = 0; cpi < argNode.properties.length; cpi++) {
                    var cp = argNode.properties[cpi];
                    var cpKey = _t.isIdentifier(cp.key) ? cp.key.name :
                                (_t.isStringLiteral(cp.key) ? cp.key.value : null);
                    if (cpKey === paramKey) { foundProp = cp; break; }
                  }
                  if (foundProp) argNode = foundProp.value;
                  else continue;
                }
                // Extract constructor names from the argument expression
                var ctors = _collectNewCtors(argNode);
                for (var cti = 0; cti < ctors.length && !resolved; cti++) {
                  var ctorName = ctors[cti];
                  for (var dk2 in propDefs) {
                    if (dk2.endsWith(":" + ctorName) && propDefs[dk2][pp.propName]) {
                      var interLine = propDefs[dk2][pp.propName];
                      if (pp.refLine !== interLine) {
                        if (!refMap[pp.refLine]) refMap[pp.refLine] = {};
                        if (!refMap[pp.refLine][pp.propName]) {
                          refMap[pp.refLine][pp.propName] = interLine;
                          resolved = true;
                        }
                      }
                      break;
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  }

  var _bdmTraverseMs = Date.now() - _bdmTraverseT0;
  return {
    defMap: defMap,
    refMap: refMap,
    funcMap: funcMap,
    allFuncRanges: allFuncRanges,
    propDefs: propDefs,
    ast: ast,
    _timings: { parseMs: _bdmParseMs, traverseMs: _bdmTraverseMs },
  };
}
