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
var _propAssignMemo = new WeakMap();        // binding node → { propName: [resolved values] from `obj.propName = X` assignments via that binding's referencePaths }
var _resolveToObjectMemo = new WeakMap();   // node → resolved ObjectExpression (or null) — caches _resolveToObject result so repeated lookups on the same node are O(1)

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
function _isGlobalFetchCall(callee, scope) {
  if (_t.isIdentifier(callee, { name: "fetch" }) && !scope.getBinding("fetch")) return true;
  if (_t.isMemberExpression(callee) && _t.isIdentifier(callee.property, { name: "fetch" }) &&
      _t.isIdentifier(callee.object) && _isGlobalObject(callee.object.name, scope)) return true;
  if (_t.isLogicalExpression(callee)) {
    return _isGlobalFetchCall(callee.left, scope) || _isGlobalFetchCall(callee.right, scope);
  }
  return false;
}

// Check if a MemberExpression's object node traces to an XMLHttpRequest instance.
// Uses the type tracker first, then falls back to binding init resolution.
function _isXhrObject(path, objectNode) {
  if (!_t.isIdentifier(objectNode)) return false;
  var objType = _getTrackedType(path, objectNode);
  if (objType === "XMLHttpRequest") return true;
  if (objType) return false;
  var binding = path.scope.getBinding(objectNode.name);
  if (binding && _t.isVariableDeclarator(binding.path.node) && binding.path.node.init) {
    var init = binding.path.node.init;
    if (_t.isNewExpression(init) && _t.isIdentifier(init.callee, { name: "XMLHttpRequest" }) &&
        !path.scope.getBinding("XMLHttpRequest")) return true;
    if (_t.isCallExpression(init) && _t.isMemberExpression(init.callee) &&
        _t.isIdentifier(init.callee.property, { name: "xhr" })) return true;
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
      _processReactDangerousHTML(path, result);
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

    // Extract body params from second argument options object.
    // Two recognised shapes — both safe here because the receiver is a
    // globally-tracked object AND the method name is an HTTP verb:
    //   1. ky-style:    k.post(url, {json: {name: "x"}})
    //   2. jQuery/axios: $.post(url, {fkey: "x", payload: y})
    if (node.arguments.length >= 2 && _t.isObjectExpression(node.arguments[1])) {
      var optsNode = node.arguments[1];
      var jsonProp = null;
      for (var pi = 0; pi < optsNode.properties.length; pi++) {
        var prop = optsNode.properties[pi];
        if (!_t.isObjectProperty(prop) || prop.computed) continue;
        var keyName = _t.isIdentifier(prop.key) ? prop.key.name :
          (_t.isStringLiteral(prop.key) ? prop.key.value : null);
        if (keyName === "json" && _t.isObjectExpression(prop.value)) { jsonProp = prop; break; }
      }
      if (jsonProp) {
        site.params = _extractObjectProperties(jsonProp.value);
        for (var bpi = 0; bpi < site.params.length; bpi++) site.params[bpi].location = "body";
      } else {
        site.params = _extractObjectProperties(optsNode);
        for (var bpj = 0; bpj < site.params.length; bpj++) site.params[bpj].location = "body";
      }
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
      var xhrCheckBinding = path.scope.getBinding(callee.object.name);
      if (xhrCheckBinding && _t.isVariableDeclarator(xhrCheckBinding.path.node) && xhrCheckBinding.path.node.init) {
        var _initN = xhrCheckBinding.path.node.init;
        if (_t.isNewExpression(_initN) && _t.isIdentifier(_initN.callee, { name: "XMLHttpRequest" }) &&
            !path.scope.getBinding("XMLHttpRequest")) isXhr = true;
        if (_t.isCallExpression(_initN) && _t.isMemberExpression(_initN.callee) &&
            _t.isIdentifier(_initN.callee.property, { name: "xhr" })) isXhr = true;
      }
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
                // Extract body params from the "data" property of caller args
                var corrBodyParams = xhrBodyParams.length > 0 ? xhrBodyParams : [];
                if (corrBodyParams.length === 0) {
                  // Resolve the caller arg to an object, then extract the "data" property
                  var callerArgObj = null;
                  try { callerArgObj = _resolveToObject(callerArgs[cai], 1); } catch(e) { _resolver.collectError(e, "xhrCallerArgResolve"); }
                  if (callerArgObj) {
                    for (var cpi = 0; cpi < callerArgObj.properties.length; cpi++) {
                      var cprop = callerArgObj.properties[cpi];
                      if (!_t.isObjectProperty(cprop) || cprop.computed) continue;
                      var cpKey = _getKeyName(cprop.key);
                      if (cpKey === "data") {
                        var dataValNode = cprop.value;
                        dataValNode = _unwrapJsonStringify(dataValNode, path);
                        if (_t.isObjectExpression(dataValNode)) {
                          corrBodyParams = _extractObjectProperties(dataValNode);
                          for (var dbp = 0; dbp < corrBodyParams.length; dbp++) corrBodyParams[dbp].location = "body";
                          break;
                        }
                      }
                    }
                  }
                }
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
  var funcPath = _resolveCalleeToFunction(path);
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

// Resolve a call expression's callee to its function node
// Returns a Babel path to the resolved function node, or null.
function _resolveCalleeToFunction(callPath) {
  var callee = callPath.node.callee;

  // Common case: identifier → scope binding, member expr → object property
  var commonPath = _resolveCalleeFuncPath(callPath, 0);
  if (commonPath) return commonPath;

  // Extended identifier resolution: global assignments, factory returns
  if (_t.isIdentifier(callee)) {
    var binding = callPath.scope.getBinding(callee.name);
    if (!binding) {
      var globalDef = _globalAssignments[callee.name];
      if (globalDef && globalDef.valueNode && globalDef.valuePath) {
        if (_t.isFunctionExpression(globalDef.valueNode) || _t.isArrowFunctionExpression(globalDef.valueNode))
          return globalDef.valuePath;
        if (_t.isCallExpression(globalDef.valueNode)) {
          var retFunc = _resolveCallReturnToFunction(globalDef.valuePath, 0);
          if (retFunc && retFunc._path) return retFunc._path;
        }
      }
      return null;
    }
    if (!binding.path) return null;
    // Higher-order: var fn = factory() where factory returns a function
    if (_t.isVariableDeclarator(binding.path.node) && binding.path.node.init &&
        _t.isCallExpression(binding.path.node.init)) {
      var retFunc = _resolveCallReturnToFunction(binding.path.get("init"), 0);
      if (retFunc && retFunc._path) return retFunc._path;
    }
    return null;
  }

  // Member expression: obj.method(url) → resolve obj, find method property
  if (_t.isMemberExpression(callee) && !callee.computed) {
    var propName = _t.isIdentifier(callee.property) ? callee.property.name : null;
    if (!propName) return null;

    // Try: obj = IIFE() returning a function/object with properties assigned inside
    // Handles: var e = function(){...n.get=fn...return n}(); e.get(url)
    if (_t.isIdentifier(callee.object)) {
      var iifeObjBinding = callPath.scope.getBinding(callee.object.name);
      if (iifeObjBinding && _t.isVariableDeclarator(iifeObjBinding.path.node) &&
          iifeObjBinding.path.node.init && _t.isCallExpression(iifeObjBinding.path.node.init)) {
        var iifePropFn = _resolveIIFEReturnedProperty(iifeObjBinding.path.get("init"), propName);
        if (iifePropFn) return iifePropFn;
      }
      // Also check global assignments: window.X = IIFE()
      if (!iifeObjBinding) {
        var gDef = _globalAssignments[callee.object.name];
        if (gDef && gDef.valuePath && _t.isCallExpression(gDef.valueNode)) {
          var gPropFn = _resolveIIFEReturnedProperty(gDef.valuePath, propName);
          if (gPropFn) return gPropFn;
        }
      }
    }

    // Second try: method assigned separately (Closure pattern: a.b = function(url) { ... })
    // Use referencePaths since Babel doesn't track property mutations as constantViolations
    if (_t.isIdentifier(callee.object)) {
      var objBinding = callPath.scope.getBinding(callee.object.name);
      // Fallback: if no local binding, check _globalAssignments.
      // Handles window.jQuery = jQuery inside IIFE → user code calls jQuery.ajax() outside.
      // Unwraps chained assignments: window.jQuery = window.$ = jQuery → jQuery
      if (!objBinding) {
        var globalDef = _globalAssignments[callee.object.name];
        if (globalDef && globalDef.valueNode) {
          var gVal = globalDef.valueNode;
          while (_t.isAssignmentExpression(gVal)) gVal = gVal.right;
          if (_t.isIdentifier(gVal)) {
            objBinding = globalDef.valuePath.scope.getBinding(gVal.name);
          }
        }
      }
      if (objBinding) {
        var refs = objBinding.referencePaths;
        for (var cv = 0; cv < refs.length; cv++) {
          var refParent = refs[cv].parent;
          if (_t.isMemberExpression(refParent) && refParent.object === refs[cv].node &&
              !refParent.computed && _t.isIdentifier(refParent.property, { name: propName })) {
            var assignParentPath = refs[cv].parentPath ? refs[cv].parentPath.parentPath : null;
            var assignExpr = assignParentPath ? assignParentPath.node : null;
            if (assignExpr && _t.isAssignmentExpression(assignExpr) && assignExpr.operator === "=" &&
                assignExpr.left === refParent) {
              if (_t.isFunctionExpression(assignExpr.right) || _t.isArrowFunctionExpression(assignExpr.right)) {
                return assignParentPath.get("right");
              }
            }
          }
          // Third try: property defined via obj.extend({method: function(){}})
          if (_t.isMemberExpression(refParent) && refParent.object === refs[cv].node && !refParent.computed) {
            var extName = _t.isIdentifier(refParent.property) ? refParent.property.name : null;
            if (extName === "extend" || extName === "mixin" || extName === "assign") {
              var extCallPath = refs[cv].parentPath ? refs[cv].parentPath.parentPath : null;
              var extCallNode = extCallPath ? extCallPath.node : null;
              if (extCallNode && _t.isCallExpression(extCallNode) && extCallNode.callee === refParent) {
                for (var ea = 0; ea < extCallNode.arguments.length; ea++) {
                  var extArgObj = extCallNode.arguments[ea];
                  if (!_t.isObjectExpression(extArgObj)) continue;
                  for (var ep = 0; ep < extArgObj.properties.length; ep++) {
                    var extProp = extArgObj.properties[ep];
                    if (!_t.isObjectProperty(extProp) || extProp.computed) continue;
                    var epKey = _t.isIdentifier(extProp.key) ? extProp.key.name :
                      (_t.isStringLiteral(extProp.key) ? extProp.key.value : null);
                    if (epKey === propName) {
                      if (_t.isFunctionExpression(extProp.value) || _t.isArrowFunctionExpression(extProp.value)) {
                        return extCallPath.get("arguments." + ea + ".properties." + ep + ".value");
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

  return null;
}

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
      // Skip known non-body config properties
      if (dpKey === "headers" || dpKey === "contentType" || dpKey === "dataType" ||
          dpKey === "success" || dpKey === "error" || dpKey === "complete" ||
          dpKey === "beforeSend" || dpKey === "async" || dpKey === "cache" ||
          dpKey === "timeout" || dpKey === "crossDomain" || dpKey === "processData") continue;
      // "data" property: extract its sub-properties as body params
      if (dpKey === "data") {
        if (_t.isObjectExpression(dp.value)) {
          var dataParams = _extractObjectProperties(dp.value);
          for (var ddp = 0; ddp < dataParams.length; ddp++) { dataParams[ddp].location = "body"; deepParams.push(dataParams[ddp]); }
        } else if (_t.isCallExpression(dp.value) && _isJsonStringify(dp.value, callPath) &&
                   dp.value.arguments[0] && _t.isObjectExpression(dp.value.arguments[0])) {
          var jsonParams = _extractObjectProperties(dp.value.arguments[0]);
          for (var djp = 0; djp < jsonParams.length; djp++) { jsonParams[djp].location = "body"; deepParams.push(jsonParams[djp]); }
        }
        continue;
      }
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
function _unwrapGapToRootCause(argPath, visited) {
  if (!argPath || !argPath.node) return argPath;
  // Cycle-safe: short-circuit when we re-visit a node we've already
  // unwrapped through. Replaces a magic-number depth cap. Real cycles
  // appear when a binding's init references the binding itself
  // (recursive var) or when an IIFE's return references its callee.
  if (!visited) visited = new Set();
  if (visited.has(argPath.node)) return argPath;
  visited.add(argPath.node);
  var n = argPath.node;
  // Identifier → var init (only if constant-init binding)
  if (_t.isIdentifier(n)) {
    var binding = argPath.scope.getBinding(n.name);
    if (binding && binding.constant && _t.isVariableDeclarator(binding.path.node) && binding.path.node.init) {
      return _unwrapGapToRootCause(binding.path.get("init"), visited);
    }
    return argPath;
  }
  // new Request(URL, init) — URL arg is what matters
  if (_t.isNewExpression(n) && _t.isIdentifier(n.callee, { name: "Request" }) &&
      !argPath.scope.getBinding("Request") && n.arguments.length >= 1) {
    return _unwrapGapToRootCause(argPath.get("arguments.0"), visited);
  }
  // x.toString() / x.valueOf() — receiver is what matters
  if (_t.isCallExpression(n) && _t.isMemberExpression(n.callee) && !n.callee.computed &&
      _t.isIdentifier(n.callee.property) &&
      (n.callee.property.name === "toString" || n.callee.property.name === "valueOf")) {
    return _unwrapGapToRootCause(argPath.get("callee.object"), visited);
  }
  // IIFE / arrow IIFE — the first return statement's argument is the
  // root cause. Avoids labelling the gap as opaque `<IIFE>()` when the
  // actual failure is the unresolved value inside the IIFE body.
  if (_t.isCallExpression(n) &&
      (_t.isFunctionExpression(n.callee) || _t.isArrowFunctionExpression(n.callee))) {
    var fn = n.callee;
    // Arrow with expression body: the body IS the return value.
    if (_t.isArrowFunctionExpression(fn) && !_t.isBlockStatement(fn.body)) {
      return _unwrapGapToRootCause(argPath.get("callee.body"), visited);
    }
    // Function body: find the FIRST return statement at top level.
    if (_t.isBlockStatement(fn.body)) {
      for (var bi = 0; bi < fn.body.body.length; bi++) {
        if (_t.isReturnStatement(fn.body.body[bi]) && fn.body.body[bi].argument) {
          return _unwrapGapToRootCause(argPath.get("callee.body.body." + bi + ".argument"), visited);
        }
      }
    }
  }
  // SequenceExpression `(a, b, c)` — only the LAST element is the value.
  // Common minifier shape: `return x.search = y.toString(), x.toString()`.
  if (_t.isSequenceExpression(n) && n.expressions.length > 0) {
    var lastIdx = n.expressions.length - 1;
    return _unwrapGapToRootCause(argPath.get("expressions." + lastIdx), visited);
  }
  // ConditionalExpression `cond ? a : b` — both branches contribute to
  // the value. For gap reporting, unwrap to the consequent (truthy
  // branch) so the descriptor shows a concrete leaf the reviewer can
  // trace, not the opaque `ConditionalExpression` type name.
  if (_t.isConditionalExpression(n)) {
    return _unwrapGapToRootCause(argPath.get("consequent"), visited);
  }
  // TemplateLiteral with at least one interpolation — unwrap to the
  // FIRST interpolation. The TemplateLiteral itself fails to resolve
  // when any interpolation is unresolvable; surfacing the actual
  // unresolved leaf lets the reviewer trace what's missing instead of
  // the opaque `<template>` label.
  if (_t.isTemplateLiteral(n) && n.expressions.length > 0) {
    return _unwrapGapToRootCause(argPath.get("expressions.0"), visited);
  }
  // BinaryExpression `+` (concat) — unwrap to the LEFT operand so the
  // descriptor shows the leftmost leaf of the concat chain.
  if (_t.isBinaryExpression(n) && n.operator === "+") {
    return _unwrapGapToRootCause(argPath.get("left"), visited);
  }
  // LogicalExpression `a || b` / `a && b` / `a ?? b` — the value at
  // runtime depends on truthiness. Unwrap to the LEFT (the operand
  // that's evaluated first, and frequently the dominant value-producer).
  if (_t.isLogicalExpression(n)) {
    return _unwrapGapToRootCause(argPath.get("left"), visited);
  }
  // UnaryExpression `+x` / `-x` / `!x` / `~x` / `void x` / `typeof x`
  // — the value depends on the operand. Unwrap to the operand to surface
  // the underlying gap leaf instead of the opaque `UnaryExpression`.
  if (_t.isUnaryExpression(n)) {
    return _unwrapGapToRootCause(argPath.get("argument"), visited);
  }
  // AwaitExpression / YieldExpression — value comes from the
  // wrapped/yielded expression. Unwrap.
  if (_t.isAwaitExpression(n) || _t.isYieldExpression(n)) {
    if (n.argument) return _unwrapGapToRootCause(argPath.get("argument"), visited);
  }
  return argPath;
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
  var n = urlArgPath.node;

  // `new URL(input, base).href` / `.toString()` — the WHATWG URL parser
  // produces the same path/query shape as the input argument itself.
  // Recurse into the input arg; the base only contributes scheme/host
  // (which the structural emit doesn't include anyway).
  if (_t.isMemberExpression(n) && !n.computed &&
      _t.isIdentifier(n.property) && (n.property.name === "href" || n.property.name === "toString") &&
      _t.isNewExpression(n.object) && _t.isIdentifier(n.object.callee, { name: "URL" }) &&
      !urlArgPath.scope.getBinding("URL") &&
      n.object.arguments.length >= 1) {
    return _resolveUrlStructuralShape(urlArgPath.get("object.arguments.0"));
  }
  // `(new URL(input, base)).toString()` is a CallExpression on the
  // MemberExpression — handle the call form too.
  if (_t.isCallExpression(n) && _t.isMemberExpression(n.callee) && !n.callee.computed &&
      _t.isIdentifier(n.callee.property, { name: "toString" }) &&
      _t.isNewExpression(n.callee.object) && _t.isIdentifier(n.callee.object.callee, { name: "URL" }) &&
      !urlArgPath.scope.getBinding("URL") &&
      n.callee.object.arguments.length >= 1) {
    return _resolveUrlStructuralShape(urlArgPath.get("callee.object.arguments.0"));
  }

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
        bodyParams = _extractBodyParams(optVal, path);
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

function _resolveAllValues(path, depth) {
  var node = path.node;
  if (!node) return [];

  // Literals — no recursion, return directly regardless of depth
  if (_t.isStringLiteral(node)) return [node.value];
  if (_t.isNumericLiteral(node)) return [String(node.value)];

  if (!_resolver.guard("V", node)) return [];
  try {

  // String-encoding transforms applied to a resolved string. WHATWG/ES
  // spec: encodeURIComponent / encodeURI / decodeURIComponent / decodeURI
  // are pure functions on a single string argument. When the argument
  // resolves to a string literal (possibly via inter-procedural caller
  // tracing), apply the transform and return the encoded value — that's
  // a real example value the URL would receive at runtime, not a
  // placeholder. btoa / atob get the same treatment for base64 round-
  // trips. All four globals are scope-checked before applying.
  if (_t.isCallExpression(node) && _t.isIdentifier(node.callee) &&
      node.arguments.length >= 1 && !path.scope.getBinding(node.callee.name)) {
    var ecName = node.callee.name;
    if (ecName === "encodeURIComponent" || ecName === "encodeURI" ||
        ecName === "decodeURIComponent" || ecName === "decodeURI" ||
        ecName === "btoa" || ecName === "atob") {
      var argVals = _resolveAllValues(path.get("arguments.0"), depth + 1);
      if (argVals.length === 0) return [];
      var fn = ecName === "encodeURIComponent" ? encodeURIComponent :
               ecName === "encodeURI" ? encodeURI :
               ecName === "decodeURIComponent" ? decodeURIComponent :
               ecName === "decodeURI" ? decodeURI :
               ecName === "btoa" ? (typeof btoa !== "undefined" ? btoa : function(s) { return Buffer.from(s, "binary").toString("base64"); }) :
                                    (typeof atob !== "undefined" ? atob : function(s) { return Buffer.from(s, "base64").toString("binary"); });
      var out = [];
      for (var avi = 0; avi < argVals.length; avi++) {
        if (typeof argVals[avi] === "string") {
          try { out.push(fn(argVals[avi])); } catch (_) { /* malformed input — skip */ }
        }
      }
      if (out.length > 0) return out;
    }
  }

  // Simple template literal without interpolations
  if (_t.isTemplateLiteral(node) && node.expressions.length === 0 && node.quasis.length === 1) {
    return [node.quasis[0].value.cooked || node.quasis[0].value.raw];
  }

  // Template literal with resolvable expressions
  if (_t.isTemplateLiteral(node) && node.expressions.length > 0) {
    var parts = [];
    for (var ti = 0; ti < node.quasis.length; ti++) {
      parts.push(node.quasis[ti].value.cooked || node.quasis[ti].value.raw || "");
      if (ti < node.expressions.length) {
        var exprPath = path.get("expressions." + ti);
        var exprVals = _resolveAllValues(exprPath, depth + 1);
        // Every interpolation must resolve to a literal — returning
        // partial parts would produce false-concrete URLs the caller
        // then misattributes. Empty result is how `_resolveAllValues`
        // reports "this subtree has no literal value to emit".
        if (exprVals.length === 0) return [];
        parts.push(exprVals[0]);
      }
    }
    return [parts.join("")];
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
  if (_t.isBinaryExpression(node, { operator: "+" })) {
    var concatParts = [];
    var cur = path;
    while (_t.isBinaryExpression(cur.node, { operator: "+" })) {
      concatParts.push(cur.get("right"));
      cur = cur.get("left");
    }
    concatParts.push(cur); // leftmost non-+ term
    concatParts.reverse();

    var termResults = [];
    for (var ci = 0; ci < concatParts.length; ci++) {
      var partVals = _resolveAllValues(concatParts[ci], depth + 1);
      if (partVals.length === 0) return [];
      termResults.push(partVals);
    }
    var concatResult = [""];
    for (var tri = 0; tri < termResults.length; tri++) {
      var next = [];
      var maxLen = Math.max(concatResult.length, termResults[tri].length);
      for (var zi = 0; zi < maxLen; zi++) {
        var l = concatResult[Math.min(zi, concatResult.length - 1)];
        var r = termResults[tri][Math.min(zi, termResults[tri].length - 1)];
        next.push(String(l) + String(r));
      }
      concatResult = next;
    }
    return concatResult;
  }

  // Conditional expression: a ? b : (c ? d : e) — flatten alternate-recursive chain iteratively.
  if (_t.isConditionalExpression(node)) {
    var ternaryVals = [];
    var cur = path;
    while (_t.isConditionalExpression(cur.node)) {
      ternaryVals = ternaryVals.concat(_resolveAllValues(cur.get("consequent"), depth + 1));
      cur = cur.get("alternate");
    }
    ternaryVals = ternaryVals.concat(_resolveAllValues(cur, depth + 1));
    if (ternaryVals.length > 0) return ternaryVals;
  }

  // Logical OR: (a || b) || c — flatten left-recursive chain iteratively.
  if (_t.isLogicalExpression(node, { operator: "||" })) {
    var orParts = [];
    var cur = path;
    while (_t.isLogicalExpression(cur.node, { operator: "||" })) {
      orParts.push(cur.get("right"));
      cur = cur.get("left");
    }
    orParts.push(cur);
    orParts.reverse();
    var orVals = [];
    for (var oi = 0; oi < orParts.length; oi++) {
      orVals = orVals.concat(_resolveAllValues(orParts[oi], depth + 1));
    }
    if (orVals.length > 0) return orVals;
  }

  // new Request(input, init?) — the Fetch API's Request constructor.
  // `fetch(c)` where `c = new Request(url, {method, body, headers})` is a
  // common indirection in handlers that want to share init between a
  // cancel-capable fetch and a Request validation pass. The first
  // argument can be a URL string, a URL object, or another Request; we
  // only need to resolve it as a URL value — the existing URL/toString
  // paths pick up the other two forms when they recurse.
  if (_t.isNewExpression(node) && _t.isIdentifier(node.callee, { name: "Request" }) &&
      !path.scope.getBinding("Request") && node.arguments.length >= 1) {
    return _resolveAllValues(path.get("arguments.0"), depth + 1);
  }

  // new URL(input, base?) — when both arguments resolve to string values
  // the whole construction resolves to the absolute URL. The minified
  // `fetch(new URL(rel, base).toString())` shape is common; the .toString()
  // passthrough handler brings us back here. `URL` must be the unshadowed global.
  if (_t.isNewExpression(node) && _t.isIdentifier(node.callee, { name: "URL" }) &&
      !path.scope.getBinding("URL") && node.arguments.length >= 1) {
    var urlInputVals = _resolveAllValues(path.get("arguments.0"), depth + 1);
    if (urlInputVals.length === 0) return [];
    var urlBaseVals = null;
    if (node.arguments.length >= 2) {
      urlBaseVals = _resolveAllValues(path.get("arguments.1"), depth + 1);
      if (urlBaseVals.length === 0) return [];
    }
    var urlOut = [];
    for (var uli = 0; uli < urlInputVals.length; uli++) {
      var urlInStr = String(urlInputVals[uli]);
      if (urlBaseVals) {
        for (var ulb = 0; ulb < urlBaseVals.length; ulb++) {
          try {
            var abs = new URL(urlInStr, String(urlBaseVals[ulb])).href;
            if (urlOut.indexOf(abs) < 0) urlOut.push(abs);
          } catch (_) { /* URL constructor would throw at runtime for this (input,base) pair — runtime never emits a URL here either */ }
        }
      } else {
        try {
          var absOnly = new URL(urlInStr).href;
          if (urlOut.indexOf(absOnly) < 0) urlOut.push(absOnly);
        } catch (_) { /* single-arg new URL() requires absolute input — relative string throws, matching runtime */ }
      }
    }
    return urlOut;
  }

  // new URLSearchParams({k: v, ...}) / new URLSearchParams("a=1&b=2")
  // coerces to a querystring via implicit toString() — resolve to the
  // encoded form so concats like `"/api?" + params` surface a concrete
  // URL. Every value term must resolve; if the analyzer can't trace
  // one of them that's a resolver gap, surfaced by the concat-level
  // throw below once control returns here.
  if (_t.isNewExpression(node) && _t.isIdentifier(node.callee, { name: "URLSearchParams" }) &&
      !path.scope.getBinding("URLSearchParams")) {
    var uspArg = node.arguments && node.arguments[0];
    if (uspArg && _t.isObjectExpression(uspArg)) {
      var uspParts = [];
      for (var upi = 0; upi < uspArg.properties.length; upi++) {
        var uspProp = uspArg.properties[upi];
        if (!_t.isObjectProperty(uspProp) || uspProp.computed) return [];
        var uspKeyName = _t.isIdentifier(uspProp.key) ? uspProp.key.name :
          (_t.isStringLiteral(uspProp.key) ? uspProp.key.value : null);
        if (!uspKeyName) return [];
        var uspValVals = _resolveAllValues(path.get("arguments.0.properties." + upi + ".value"), depth + 1);
        if (!uspValVals.length) return [];
        uspParts.push(encodeURIComponent(uspKeyName) + "=" + String(uspValVals[0]));
      }
      return [uspParts.join("&")];
    }
    if (uspArg && _t.isStringLiteral(uspArg)) {
      return [uspArg.value.replace(/^\?/, "")];
    }
  }

  // Call expression — resolve through function return values
  // Handles: fetch(getUrl()), fetch(buildUrl("/api", id)), var x = config.get("key")
  if (_t.isCallExpression(node)) {
    var retVals = _resolveCallReturnValues(path, depth);
    if (retVals.length > 0) return retVals;
    // String method passthrough: .replace(), .trim(), .toLowerCase(), .toUpperCase(), .slice(), .substring()
    // These return a modified version of the string — resolve the object for URL analysis
    if (_t.isMemberExpression(node.callee) && !node.callee.computed) {
      var smName = _t.isIdentifier(node.callee.property) ? node.callee.property.name : null;
      if (smName === "replace" || smName === "trim" || smName === "toLowerCase" ||
          smName === "toUpperCase" || smName === "slice" || smName === "substring" || smName === "substr") {
        var smVals = _resolveAllValues(path.get("callee.object"), depth + 1);
        if (smVals.length > 0) return smVals;
      }
      // .toString() / .valueOf() — identity passthrough. Minified bundles
      // emit `fetch(n.toString())` after building `n = new URL(rel, base)`;
      // the URL-construction branch below resolves the receiver and
      // .toString() is the no-op terminator that commits it to a string.
      // For URLSearchParams the receiver is already resolved to the
      // query-string form in the _resolveToObject path.
      if (smName === "toString" || smName === "valueOf") {
        var tsVals = _resolveAllValues(path.get("callee.object"), depth + 1);
        if (tsVals.length > 0) return tsVals;
      }
      // el.getAttribute(KEY) roundtrip: if el's scope has a matching
      // el.setAttribute(KEY, VALUE) call whose value resolves, the
      // getter returns that value. Real-world: custom elements that
      // stash config via data-* attrs in one method and read them in
      // another. When setAttribute is NOT found, leave the getter
      // unresolved (returns []) — runtime-DOM, analyzer has no static
      // ground truth.
      if (smName === "getAttribute" && node.arguments.length >= 1 &&
          _t.isStringLiteral(node.arguments[0])) {
        var gaKey = node.arguments[0].value;
        var gaReceiverPath = path.get("callee.object");
        var gaReceiver = gaReceiverPath.node;
        if (_t.isIdentifier(gaReceiver)) {
          var gaBinding = path.scope.getBinding(gaReceiver.name);
          if (gaBinding && gaBinding.referencePaths) {
            var setVals = [];
            for (var gari = 0; gari < gaBinding.referencePaths.length; gari++) {
              var gaRef = gaBinding.referencePaths[gari];
              // Pattern: el.setAttribute(KEY, VALUE)
              var gaMem = gaRef.parentPath;
              if (!gaMem || !gaMem.isMemberExpression() || gaMem.node.object !== gaRef.node || gaMem.node.computed) continue;
              if (!_t.isIdentifier(gaMem.node.property, { name: "setAttribute" })) continue;
              var gaCall = gaMem.parentPath;
              if (!gaCall || !gaCall.isCallExpression() || gaCall.node.callee !== gaMem.node) continue;
              if (gaCall.node.arguments.length < 2) continue;
              var gaSetKey = gaCall.node.arguments[0];
              if (!_t.isStringLiteral(gaSetKey) || gaSetKey.value !== gaKey) continue;
              var gaValVals = _resolveAllValues(gaCall.get("arguments.1"), depth + 1);
              setVals = setVals.concat(gaValVals);
            }
            if (setVals.length > 0) return setVals;
          }
        }
      }
      // Array.join(separator): resolve array elements (recursively, through
      // scope bindings / return values / concats) and join with separator.
      // Minified bundles frequently emit `[base, "/api/", id].join("")`
      // where each element is an identifier bound elsewhere — literal-only
      // matching would drop these on the floor and misattribute to page
      // origin. If any element can't be resolved, return [] so the
      // fetch-level gap recorder names the unresolved subtree.
      if (smName === "join" && node.arguments.length <= 1) {
        var joinArrNode = _resolveToArray(path.get("callee.object"), 0);
        if (joinArrNode && joinArrNode.elements && joinArrNode.elements.length > 0 && joinArrNode._path) {
          var sep = ",";
          if (node.arguments.length === 1 && _t.isStringLiteral(node.arguments[0])) sep = node.arguments[0].value;
          var joinParts = [];
          var joinOk = true;
          for (var ji = 0; ji < joinArrNode.elements.length; ji++) {
            var joinElem = joinArrNode.elements[ji];
            if (joinElem === null) { joinParts.push(""); continue; } // hole → empty string per ES spec
            var joinElemVals = _resolveAllValues(joinArrNode._path.get("elements." + ji), depth + 1);
            if (joinElemVals.length === 0) { joinOk = false; break; }
            joinParts.push(String(joinElemVals[0]));
          }
          if (joinOk) return [joinParts.join(sep)];
          return [];
        }
      }
    }
  }

  // Variable reference — use Babel scope analysis
  if (_t.isIdentifier(node)) {
    var binding = path.scope.getBinding(node.name);
    if (!binding) {
      // Fallback: try global assignments (window.X = value from another script)
      var globalDef = _globalAssignments[node.name];
      if (globalDef && globalDef.valuePath) {
        return _resolveAllValues(globalDef.valuePath, depth + 1);
      }
      return [];
    }

    // Constant with initializer
    if (binding.constant && _t.isVariableDeclarator(binding.path.node) && binding.path.node.init) {
      var initVals = _resolveAllValues(binding.path.get("init"), depth + 1);
      if (initVals.length > 0) {
        _stats.resolvedUrls++;
        return initVals;
      }
    }

    // Function parameter — inter-procedural tracing
    if (binding.kind === "param") {
      var callerValues = _resolveParamFromCallers(binding, depth);
      if (callerValues.length > 0) {
        _stats.interProcTraces++;
        return callerValues;
      }
    }

    // Non-constant variable — collect all reassignments. No cap on
    // count: every reassigned value belongs in the result set per the
    // resolver-completeness rule.
    if (!binding.constant && binding.constantViolations.length > 0) {
      var vals = [];
      if (_t.isVariableDeclarator(binding.path.node) && binding.path.node.init) {
        var initVal = _resolveAllValues(binding.path.get("init"), depth + 1);
        vals = vals.concat(initVal);
      }
      for (var cv = 0; cv < binding.constantViolations.length; cv++) {
        var violation = binding.constantViolations[cv];
        if (_t.isAssignmentExpression(violation.node) && violation.node.operator === "=") {
          var rhs = _resolveAllValues(violation.get("right"), depth + 1);
          vals = vals.concat(rhs);
        }
      }
      if (vals.length > 0) return vals;
    }
    // for-in / for-of loop variable: `for (var k in obj)` → k iterates
    // over obj's keys; `for (var v of arr)` → v iterates over arr's
    // elements. When the iterable resolves to a literal-keyed object or
    // array, the loop variable resolves to the union of those keys /
    // elements. Pure ECMAScript iteration semantics.
    if (_t.isVariableDeclarator(binding.path.node) && !binding.path.node.init) {
      var loopOwner = binding.path.parentPath && binding.path.parentPath.parentPath;
      if (loopOwner && (loopOwner.isForInStatement() || loopOwner.isForOfStatement()) &&
          loopOwner.node.left === binding.path.parent) {
        var iterableP = loopOwner.get("right");
        if (loopOwner.isForInStatement()) {
          // Keys of the iterable's resolved object.
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
            if (keys.length > 0) return keys;
          }
        } else {
          // for-of: elements of the iterable's resolved array.
          var iterArr = _resolveToArray(iterableP, depth);
          if (iterArr && iterArr._path && iterArr.elements) {
            var elemVals = [];
            for (var eli = 0; eli < iterArr.elements.length; eli++) {
              if (iterArr.elements[eli]) {
                elemVals = elemVals.concat(_resolveAllValues(iterArr._path.get("elements." + eli), depth + 1));
              }
            }
            if (elemVals.length > 0) return elemVals;
          }
        }
      }
    }
    // Destructured binding via VariableDeclarator with ArrayPattern or
    // ObjectPattern id. Babel groups destructured names under a single
    // VariableDeclarator (binding.path), so we walk the pattern shape on
    // binding.path.node.id to find this name's position.
    //   var [first, url] = arr;   // url → arr[1]
    //   var {url} = cfg;          // url → cfg.url
    //   var {key: u} = cfg;       // u → cfg.key (renamed)
    if (_t.isVariableDeclarator(binding.path.node) && binding.path.node.init &&
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
            return _resolveAllValues(binding.path.get("init.elements." + elemIdx), depth + 1);
          }
          var arrInit = _resolveToArray(binding.path.get("init"), depth);
          if (arrInit && arrInit._path && elemIdx < arrInit.elements.length && arrInit.elements[elemIdx]) {
            return _resolveAllValues(arrInit._path.get("elements." + elemIdx), depth + 1);
          }
        }
      } else {
        var keyForName = _findDestructuredKey(pat, node.name);
        if (keyForName) {
          if (_t.isObjectExpression(initExpr)) {
            for (var oei = 0; oei < initExpr.properties.length; oei++) {
              var oep = initExpr.properties[oei];
              if (_t.isObjectProperty(oep) && !oep.computed && _getKeyName(oep.key) === keyForName) {
                return _resolveAllValues(binding.path.get("init.properties." + oei + ".value"), depth + 1);
              }
            }
          }
          var initObj = _resolveToObject(binding.path.get("init"), depth);
          if (initObj && initObj._path) {
            for (var oej = 0; oej < initObj.properties.length; oej++) {
              var oeq = initObj.properties[oej];
              if (_t.isObjectProperty(oeq) && !oeq.computed && _getKeyName(oeq.key) === keyForName) {
                return _resolveAllValues(initObj._path.get("properties." + oej + ".value"), depth + 1);
              }
            }
          }
        }
      }
    }
  }

  // Inline _collectDefinePropertyEffects + helpers — defined later in
  // this file via top-level function declarations (hoisted).

  // Member expression — resolve obj.prop through scope
  if (_t.isMemberExpression(node) && !node.computed) {
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
        if (dpVals.length > 0) return dpVals;
      }
      if (_t.isCallExpression(node.object)) {
        var rcVals = _collectDefinePropertyEffectsOnCallReturn(path.get("object"), propName, depth, new Set());
        if (rcVals.length > 0) return rcVals;
      }
      // `<urlExpr>.<URL-prop>` — extract WHATWG URL instance properties
      // from a fully-resolved URL string. Triggers when the receiver is
      // either a literal `new URL(input, base?)` OR an Identifier whose
      // tracked type is "URL" (set by the lightweight type tracker when
      // the identifier was assigned from `new URL(...)`). Without the
      // type-tracker check we'd extract properties from arbitrary string
      // values that happen to parse as URLs — but plain strings don't
      // have `.pathname` at runtime; only URL instances do.
      if (_URL_INSTANCE_PROPS[propName]) {
        var canExtract = false;
        if (_t.isNewExpression(node.object) && _t.isIdentifier(node.object.callee, { name: "URL" }) &&
            !path.scope.getBinding("URL")) {
          canExtract = true;
        } else if (_t.isIdentifier(node.object) && _getTrackedType(path, node.object) === "URL") {
          canExtract = true;
        }
        if (canExtract) {
          var urlAbs = _resolveAllValues(path.get("object"), depth + 1);
          if (urlAbs.length > 0) {
            var urlPropOut = [];
            for (var upi = 0; upi < urlAbs.length; upi++) {
              try {
                var u = new URL(String(urlAbs[upi]));
                var v = u[propName];
                if (v != null && urlPropOut.indexOf(v) < 0) urlPropOut.push(typeof v === "string" ? v : String(v));
              } catch (_) { /* not a parseable URL; runtime would also fail */ }
            }
            if (urlPropOut.length > 0) return urlPropOut;
          }
        }
      }
      // `el.dataset.X` — HTMLElement.dataset proxy. Reading `el.dataset.foo`
      // equals reading `el.getAttribute("data-foo")`; writing assigns the
      // corresponding attribute. Scan the element binding for any of:
      //   el.dataset.foo = VALUE
      //   el.dataset["foo"] = VALUE
      //   el.setAttribute("data-foo", VALUE)
      // — same binding, same underlying attribute, so any set resolves
      // the read.
      if (_t.isMemberExpression(node.object) && !node.object.computed &&
          _t.isIdentifier(node.object.property, { name: "dataset" })) {
        var dsReceiver = node.object.object;
        var dsKey = propName;
        var attrKey = "data-" + dsKey.replace(/[A-Z]/g, function(c) { return "-" + c.toLowerCase(); });
        if (_t.isIdentifier(dsReceiver)) {
          var dsBinding = path.scope.getBinding(dsReceiver.name);
          if (dsBinding && dsBinding.referencePaths) {
            var dsVals = [];
            for (var dsi = 0; dsi < dsBinding.referencePaths.length; dsi++) {
              var dsRef = dsBinding.referencePaths[dsi];
              var dsMem = dsRef.parentPath;
              if (!dsMem || !dsMem.isMemberExpression() || dsMem.node.object !== dsRef.node) continue;
              // Pattern 1: el.dataset.foo = VALUE or el.dataset["foo"] = VALUE
              if (!dsMem.node.computed && _t.isIdentifier(dsMem.node.property, { name: "dataset" })) {
                var dsInner = dsMem.parentPath;
                if (!dsInner || !dsInner.isMemberExpression() || dsInner.node.object !== dsMem.node) continue;
                var dsInnerKey = null;
                if (!dsInner.node.computed && _t.isIdentifier(dsInner.node.property)) dsInnerKey = dsInner.node.property.name;
                else if (dsInner.node.computed && _t.isStringLiteral(dsInner.node.property)) dsInnerKey = dsInner.node.property.value;
                if (dsInnerKey !== dsKey) continue;
                var dsAssign = dsInner.parentPath;
                if (!dsAssign || !dsAssign.isAssignmentExpression() || dsAssign.node.operator !== "=" || dsAssign.node.left !== dsInner.node) continue;
                var dsVv = _resolveAllValues(dsAssign.get("right"), depth + 1);
                dsVals = dsVals.concat(dsVv);
              }
              // Pattern 2: el.setAttribute("data-foo", VALUE)
              if (!dsMem.node.computed && _t.isIdentifier(dsMem.node.property, { name: "setAttribute" })) {
                var saCall = dsMem.parentPath;
                if (!saCall || !saCall.isCallExpression() || saCall.node.callee !== dsMem.node) continue;
                if (saCall.node.arguments.length < 2) continue;
                var saKeyArg = saCall.node.arguments[0];
                if (!_t.isStringLiteral(saKeyArg) || saKeyArg.value !== attrKey) continue;
                var saVv = _resolveAllValues(saCall.get("arguments.1"), depth + 1);
                dsVals = dsVals.concat(saVv);
              }
            }
            if (dsVals.length > 0) return dsVals;
          }
        }
      }
      // `location.origin` / `window.location.origin` / `self.location.origin`
      // — resolvable from the analysis tab URL. `location` must be the
      // unshadowed global. Only substitute for fields the user CAN'T
      // control at runtime: origin/protocol/hostname/host. `.href`,
      // `.pathname`, `.search`, `.hash` are user-influenceable (via
      // navigation) — emitting a fixed value for them would hide a
      // real taint signal, so leave those unresolved here (they flow
      // through the taint model separately).
      if (_sourceUrl && (propName === "origin" || propName === "protocol" || propName === "hostname" || propName === "host")) {
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
            if (propName === "origin") return [pageUrl.origin];
            if (propName === "protocol") return [pageUrl.protocol];
            if (propName === "hostname") return [pageUrl.hostname];
            if (propName === "host") return [pageUrl.host];
          } catch (_) { /* sourceUrl wasn't a parseable URL — fall through */ }
        }
      }
      // this.prop — resolve by walking up to the enclosing ObjectExpression
      if (_t.isThisExpression(node.object)) {
        var funcPath = path.getFunctionParent();
        if (funcPath) {
          // Walk up: function → ObjectProperty.value → ObjectExpression
          var funcParentPath = funcPath.parentPath;
          if (funcParentPath && _t.isObjectProperty(funcParentPath.node) && funcParentPath.node.value === funcPath.node) {
            var objExprPath = funcParentPath.parentPath;
            if (objExprPath && _t.isObjectExpression(objExprPath.node)) {
              var objProps = objExprPath.node.properties;
              for (var ti = 0; ti < objProps.length; ti++) {
                var tp = objProps[ti];
                if (_t.isObjectProperty(tp) && !tp.computed) {
                  var tpKey = _t.isIdentifier(tp.key) ? tp.key.name :
                    (_t.isStringLiteral(tp.key) ? tp.key.value : null);
                  if (tpKey === propName) {
                    var thisVals = _resolveAllValues(objExprPath.get("properties." + ti + ".value"), depth + 1);
                    if (thisVals.length > 0) return thisVals;
                  }
                }
              }
            }
          }
          // this.prop in a prototype method: SomeClass.prototype.method = function() { this.prop }
          // Trace through constructor's this.prop = param assignment to find values from new SomeClass() calls
          if (funcParentPath && _t.isAssignmentExpression(funcParentPath.node) && funcParentPath.node.right === funcPath.node) {
            var assignLeft = funcParentPath.node.left;
            if (_t.isMemberExpression(assignLeft) && _t.isMemberExpression(assignLeft.object) &&
                (_t.isIdentifier(assignLeft.object.property, { name: "prototype" }) ||
                 (_t.isStringLiteral(assignLeft.object.property) && assignLeft.object.property.value === "prototype"))) {
              var ctorIdent = assignLeft.object.object;
              var ctorName = _t.isIdentifier(ctorIdent) ? ctorIdent.name : null;
              if (ctorName) {
                var ctorVals = _resolveConstructorProperty(path, ctorName, propName, depth);
                if (ctorVals.length > 0) return ctorVals;
              }
            }
          }
          // this.prop in an ES6 class method: class Foo { method() { this.prop } }
          // Trace through the class constructor's this.prop = param assignment
          if (_t.isClassMethod(funcPath.node) && _t.isClassBody(funcPath.parent)) {
            var classDecl = funcPath.parentPath.parentPath;
            if (classDecl && (_t.isClassDeclaration(classDecl.node) || _t.isClassExpression(classDecl.node)) && classDecl.node.id) {
              var className = classDecl.node.id.name;
              // Getter: `get collectorUrl() { return ...; }` — inline its
              // return expression. Real-world pattern: github telemetry
              // classes expose config via `get xUrl()` returning
              // `this.options.xUrl`. Without this, fetch(this.xUrl, ...)
              // trails off at an unresolved `this.xUrl` gap.
              var classBody = classDecl.node.body.body;
              for (var gmi = 0; gmi < classBody.length; gmi++) {
                var gm = classBody[gmi];
                if (_t.isClassMethod(gm) && gm.kind === "get" && !gm.computed) {
                  var getterName = _t.isIdentifier(gm.key) ? gm.key.name :
                    (_t.isStringLiteral(gm.key) ? gm.key.value : null);
                  if (getterName === propName) {
                    var getterValues = [];
                    var getterPath = classDecl.get("body.body." + gmi);
                    try {
                      getterPath.traverse(Object.assign({
                        ReturnStatement: function(retPath) {
                          if (retPath.node.argument) {
                            var rv = _resolveAllValues(retPath.get("argument"), depth + 1);
                            getterValues = getterValues.concat(rv);
                          }
                        },
                      }, _SKIP_NESTED_FUNCS));
                    } catch (e) { _resolver.collectError(e, "resolveGetter"); }
                    if (getterValues.length > 0) return getterValues;
                  }
                }
              }
              var classCtorVals = _resolveClassConstructorProperty(path, classDecl, className, propName, depth);
              if (classCtorVals.length > 0) return classCtorVals;
            }
          }
        }
      }

      // Try inline object properties first
      var objVals = _resolveToObject(path.get("object"), depth);
      if (objVals) {
        for (var oi = 0; oi < objVals.properties.length; oi++) {
          var op = objVals.properties[oi];
          if (_t.isObjectProperty(op) && !op.computed) {
            var opKey = _t.isIdentifier(op.key) ? op.key.name :
              (_t.isStringLiteral(op.key) ? op.key.value : null);
            if (opKey === propName) {
              if (_t.isStringLiteral(op.value)) return [op.value.value];
              if (_t.isNumericLiteral(op.value)) return [String(op.value.value)];
              // Recurse for nested resolution
              var nestedVals = _resolveAllValues(objVals._path.get("properties." + oi + ".value"), depth + 1);
              if (nestedVals.length > 0) return nestedVals;
            }
          }
        }
      }
      // Try property assignments: obj.prop = value
      // Babel doesn't count property mutations as constantViolations, so scan referencePaths.
      // Memoize per (binding identity, propName): when the same `obj` is
      // a module-exports param with many property assignments (e.g. React's
      // scheduler module sets ~15 `t.unstable_X = fn`), every property
      // read elsewhere re-iterates ALL referencePaths. Cache the resolved
      // value list per (binding.path.node, propName) so repeated lookups
      // return immediately. Scope is fixed per binding, so the cache is
      // safe — different identifiers in different scopes have distinct
      // bindings.
      if (_t.isIdentifier(node.object)) {
        var objBinding = path.scope.getBinding(node.object.name);
        if (objBinding) {
          var bindingNode = objBinding.path.node;
          var bindingMemo = _propAssignMemo.get(bindingNode);
          var refs = objBinding.referencePaths;
          if (bindingMemo && propName in bindingMemo) {
            if (bindingMemo[propName].length > 0) return bindingMemo[propName];
          } else {
            var collectedRhs = [];
            for (var ri = 0; ri < refs.length; ri++) {
              var refParent = refs[ri].parent;
              // Looking for: mod.propName = value
              if (_t.isMemberExpression(refParent) && refParent.object === refs[ri].node &&
                  !refParent.computed && _t.isIdentifier(refParent.property, { name: propName })) {
                var assignNode = refs[ri].parentPath ? refs[ri].parentPath.parent : null;
                if (assignNode && _t.isAssignmentExpression(assignNode) && assignNode.operator === "=" &&
                    assignNode.left === refParent) {
                  var rhsVals = _resolveAllValues(refs[ri].parentPath.parentPath.get("right"), depth + 1);
                  collectedRhs = collectedRhs.concat(rhsVals);
                }
              }
            }
            if (!bindingMemo) { bindingMemo = {}; _propAssignMemo.set(bindingNode, bindingMemo); }
            bindingMemo[propName] = collectedRhs;
            if (collectedRhs.length > 0) return collectedRhs;
          }

          // TypeScript-compiled enum / namespace pattern:
          //   !function(e){ e.Login = "..."; e.Logout = "..."; }(X || (X = {}));
          // Assignments are on the IIFE PARAMETER, not on X directly, so
          // the loop above won't find them. Walk X's referencePaths looking
          // for a reference inside a LogicalExpression that is a CallExpression
          // argument, then scan that function body for `e.propName = literal`.
          for (var rj = 0; rj < refs.length; rj++) {
            var ref = refs[rj];
            // Walk up to find an enclosing LogicalExpression that's a
            // CallExpression argument. Stop at function boundaries
            // (we're looking for the IIFE-wrapping pattern, which keeps
            // the LogicalExpression in the same function scope as the
            // ref). Cycle-safe via parentPath chain — Babel paths form a
            // tree, no cycles possible.
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
                if (_t.isStringLiteral(expr2.right)) return [expr2.right.value];
                if (_t.isNumericLiteral(expr2.right)) return [String(expr2.right.value)];
                if (_t.isTemplateLiteral(expr2.right) && expr2.right.expressions.length === 0 && expr2.right.quasis.length === 1) {
                  return [expr2.right.quasis[0].value.cooked || expr2.right.quasis[0].value.raw];
                }
              }
            }
          }
        }
      }

      // Inter-procedural: obj is a function parameter → trace to callers, extract property
      // from their object literal arguments. Handles patterns like:
      //   function request(opts) { fetch(opts.url, {method: opts.method}); }
      //   request({url: "/api/users", method: "GET"});
      if (_t.isIdentifier(node.object)) {
        var objParamBinding = path.scope.getBinding(node.object.name);
        if (objParamBinding && objParamBinding.kind === "param") {
          console.debug("[AST:trace]   param.prop: %s.%s (depth=%d)", node.object.name, propName, depth);
          var paramPropValues = _resolveParamFromCallers(objParamBinding, depth, propName);
          console.debug("[AST:trace]   param.prop result: [%s] (%d values)", paramPropValues.join(", "), paramPropValues.length);
          if (paramPropValues.length > 0) {
            _stats.interProcTraces++;
            return paramPropValues;
          }
        }
      }

      // arr[i].prop — extract prop from all array elements
      if (_t.isMemberExpression(node.object) && node.object.computed) {
        var arrNode = _resolveToArray(path.get("object.object"), depth);
        if (arrNode) {
          var arrPropVals = [];
          for (var ai = 0; ai < arrNode.elements.length; ai++) {
            var aElem = arrNode.elements[ai];
            if (aElem && _t.isObjectExpression(aElem)) {
              for (var api = 0; api < aElem.properties.length; api++) {
                var aep = aElem.properties[api];
                if (_t.isObjectProperty(aep) && !aep.computed && _getKeyName(aep.key) === propName) {
                  arrPropVals = arrPropVals.concat(_resolveAllValues(arrNode._path.get("elements." + ai + ".properties." + api + ".value"), depth + 1));
                }
              }
            }
          }
          if (arrPropVals.length > 0) return arrPropVals;
        }
      }
    }
  }

  // Computed member access: obj[key] or arr[idx]
  if (_t.isMemberExpression(node) && node.computed) {
    // Mutation-aware: for `obj[K]` reads, scan ALL `<receiver>[K2] = V`
    // assignments visible via the root identifier's binding where
    // <receiver> is the SAME expression as `obj` and K2 resolves to the
    // same value as K. Pure ECMAScript heap mutation tracking: object
    // property assignment mutates the heap; later reads see it.
    //
    // Receiver can be a plain Identifier (`d[K]`) or a MemberExpression
    // (`f.m[K]`). For both, we walk the root identifier's binding refs
    // and verify the assignment expression's receiver matches the read
    // expression structurally.
    var muRootName = null;
    if (_t.isIdentifier(node.object)) muRootName = node.object.name;
    else if (_t.isMemberExpression(node.object) && !node.object.computed && _t.isIdentifier(node.object.object)) {
      muRootName = node.object.object.name;
    }
    if (muRootName) {
      var muBinding = path.scope.getBinding(muRootName);
      if (muBinding && muBinding.referencePaths) {
        var muReadKeyVals = _resolveAllValues(path.get("property"), depth + 1);
        if (muReadKeyVals.length > 0) {
          var muVals = [];
          for (var muri = 0; muri < muBinding.referencePaths.length; muri++) {
            var muRef = muBinding.referencePaths[muri];
            // Walk up to the computed MemberExpression that represents
            // the receiver. For `d[K] = V`, ref is `d` and parent is the
            // computed MemberExpression. For `f.m[K] = V`, ref is `f`,
            // parent is `f.m` (non-computed MemberExpression), and
            // grandparent is `f.m[K]` (computed MemberExpression).
            var muRecvPath = muRef.parentPath;
            if (!muRecvPath) continue;
            // If the read receiver is `obj.X` (MemberExpression), match
            // the corresponding shape on the assignment side.
            if (_t.isMemberExpression(node.object) && !node.object.computed) {
              if (!muRecvPath.isMemberExpression() || muRecvPath.node.object !== muRef.node || muRecvPath.node.computed) continue;
              if (!_t.isIdentifier(muRecvPath.node.property, { name: node.object.property.name })) continue;
              muRecvPath = muRecvPath.parentPath;
              if (!muRecvPath) continue;
            }
            if (!muRecvPath.isMemberExpression() || !muRecvPath.node.computed) continue;
            var muAssign = muRecvPath.parent;
            if (!muAssign || !_t.isAssignmentExpression(muAssign) || muAssign.operator !== "=" || muAssign.left !== muRecvPath.node) continue;
            // Resolve assignment key.
            var muAssignKeyPath = muRecvPath.get("property");
            var muAssignKeyVals = _resolveAllValues(muAssignKeyPath, depth + 1);
            if (muAssignKeyVals.length === 0) continue;
            var muMatched = false;
            for (var mki = 0; mki < muReadKeyVals.length && !muMatched; mki++) {
              for (var mai = 0; mai < muAssignKeyVals.length && !muMatched; mai++) {
                if (String(muReadKeyVals[mki]) === String(muAssignKeyVals[mai])) muMatched = true;
              }
            }
            if (!muMatched) continue;
            var muRhs = muRecvPath.parentPath.get("right");
            muVals = muVals.concat(_resolveAllValues(muRhs, depth + 1));
          }
          if (muVals.length > 0) return muVals;
        }
      }
    }
    // Object with resolvable or unresolvable key — try specific keys first, fallback to all values.
    // No result-count caps: every reachable property value belongs in the
    // returned set per the resolver-completeness rule.
    var compObj = _resolveToObject(path.get("object"), depth);
    if (compObj) {
      var keyVals = _resolveAllValues(path.get("property"), depth + 1);
      if (keyVals.length > 0) {
        var resolvedVals = [];
        for (var ki = 0; ki < keyVals.length; ki++) {
          for (var vi = 0; vi < compObj.properties.length; vi++) {
            var vp = compObj.properties[vi];
            if (_t.isObjectProperty(vp) && !vp.computed && _getKeyName(vp.key) === String(keyVals[ki])) {
              resolvedVals = resolvedVals.concat(_resolveAllValues(compObj._path.get("properties." + vi + ".value"), depth + 1));
            }
          }
        }
        if (resolvedVals.length > 0) {
          // For variable keys (not literal), also include remaining property values for discovery
          if (!_t.isStringLiteral(node.property) && !_t.isNumericLiteral(node.property)) {
            for (var dpi = 0; dpi < compObj.properties.length; dpi++) {
              var dp = compObj.properties[dpi];
              if (_t.isObjectProperty(dp) && !dp.computed) {
                var dpVals = _resolveAllValues(compObj._path.get("properties." + dpi + ".value"), depth + 1);
                for (var dvi = 0; dvi < dpVals.length; dvi++) {
                  if (resolvedVals.indexOf(dpVals[dvi]) < 0) resolvedVals.push(dpVals[dvi]);
                }
              }
            }
          }
          return resolvedVals;
        }
      }
      // Can't resolve key — return all property values
      var allPropVals = [];
      for (var fpi = 0; fpi < compObj.properties.length; fpi++) {
        var fp = compObj.properties[fpi];
        if (_t.isObjectProperty(fp) && !fp.computed) {
          allPropVals = allPropVals.concat(_resolveAllValues(compObj._path.get("properties." + fpi + ".value"), depth + 1));
        }
      }
      if (allPropVals.length > 0) return allPropVals;
    }
    // Array with computed index — return all element values
    var compArr = _resolveToArray(path.get("object"), depth);
    if (compArr) {
      var elemVals = [];
      for (var ei = 0; ei < compArr.elements.length; ei++) {
        if (compArr.elements[ei]) {
          elemVals = elemVals.concat(_resolveAllValues(compArr._path.get("elements." + ei), depth + 1));
        }
      }
      if (elemVals.length > 0) return elemVals;
    }
  }

  return [];
  } catch (_rave) {
    if (_rave instanceof RangeError) { _resolver.collectError(_rave, "resolveAllValues"); return []; }
    throw _rave;
  } finally { _resolver.unguard("V", node); }
}

// Resolve a call expression's callee to its function path (with scope info).
// Covers the common cases: identifier → scope binding, member expr → object property.
// Returns the Babel path to the function node, or null.
function _resolveCalleeFuncPath(callPath, depth) {
  var callee = callPath.node.callee;
  if (_t.isIdentifier(callee)) {
    var binding = callPath.scope.getBinding(callee.name);
    if (binding) {
      if (_t.isFunctionDeclaration(binding.path.node)) return binding.path;
      if (_t.isVariableDeclarator(binding.path.node) && binding.path.node.init) {
        var init = binding.path.node.init;
        if (_t.isFunctionExpression(init) || _t.isArrowFunctionExpression(init))
          return binding.path.get("init");
      }
    }
  }
  if (_t.isMemberExpression(callee) && !callee.computed) {
    var propName = _t.isIdentifier(callee.property) ? callee.property.name : null;
    if (propName) {
      // Function.prototype.call / .apply — when the receiver is itself a
      // function, `f.call(thisArg, ...args)` invokes f with args. Native
      // ECMAScript semantics: the callee.object IS the function being
      // called. Resolve the receiver to a function path and return it.
      // .apply has the same callee resolution; arg unpacking differs but
      // for the purpose of finding the function body the result is the
      // same.
      if (propName === "call" || propName === "apply") {
        var recvFunc = _resolveExprToFunctionPath(callPath.get("callee.object"), depth || 0);
        if (recvFunc) return recvFunc;
      }
      var objNode = _resolveToObject(callPath.get("callee.object"), depth || 0);
      if (objNode) {
        for (var i = 0; i < objNode.properties.length; i++) {
          var prop = objNode.properties[i];
          if (!_t.isObjectProperty(prop) || prop.computed) continue;
          var key = _t.isIdentifier(prop.key) ? prop.key.name :
            (_t.isStringLiteral(prop.key) ? prop.key.value : null);
          if (key === propName && (_t.isFunctionExpression(prop.value) || _t.isArrowFunctionExpression(prop.value)))
            return objNode._path ? objNode._path.get("properties." + i + ".value") : null;
        }
      }
      // `obj.key = function/arrow` mutation pattern: walk the receiver
      // identifier's referencePaths for assignment expressions whose
      // left side is `obj.key` and right side is a function. Native
      // ECMAScript scope/value flow — same mechanism that backs
      // module-exports modules (`t.fn = function() {...}`) and
      // prototype patterns (`Ctor.prototype.method = function() {...}`).
      if (_t.isIdentifier(callee.object)) {
        var rcvBind = callPath.scope.getBinding(callee.object.name);
        if (rcvBind && rcvBind.referencePaths) {
          for (var ri = 0; ri < rcvBind.referencePaths.length; ri++) {
            var rp = rcvBind.referencePaths[ri];
            var rpParent = rp.parent;
            if (!rpParent || !_t.isMemberExpression(rpParent) || rpParent.object !== rp.node) continue;
            if (rpParent.computed || !_t.isIdentifier(rpParent.property, { name: propName })) continue;
            var assn = rp.parentPath ? rp.parentPath.parent : null;
            if (!assn || !_t.isAssignmentExpression(assn) || assn.operator !== "=" || assn.left !== rpParent) continue;
            var rhs = assn.right;
            if (_t.isFunctionExpression(rhs) || _t.isArrowFunctionExpression(rhs)) {
              return rp.parentPath.parentPath.get("right");
            }
          }
        }
      }
    }
  }
  // Identifier callee that aliases a callable expression: `var f = X.bind(Y); f()`
  // or `var f = obj.method; f()`. Pure ECMAScript value flow — follow the
  // initializer through the general expression-to-function resolver.
  if (_t.isIdentifier(callee)) {
    var b = callPath.scope.getBinding(callee.name);
    if (b && _t.isVariableDeclarator(b.path.node) && b.path.node.init) {
      var initNode = b.path.node.init;
      // Already covered above for direct function-init; only follow
      // through CallExpression (.bind etc.) here.
      if (_t.isCallExpression(initNode)) {
        return _resolveExprToFunctionPath(b.path.get("init"), depth || 0);
      }
    }
  }
  return null;
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
function _resolveExprToFunctionPath(exprPath, depth) {
  var node = exprPath.node;
  if (_t.isFunctionExpression(node) || _t.isArrowFunctionExpression(node)) return exprPath;
  // Function.prototype.bind — `f.bind(thisArg, ...preArgs)` returns a
  // function whose body is f's body (with this and prepended args
  // bound). For body resolution we just need to follow back to f.
  if (_t.isCallExpression(node) && _t.isMemberExpression(node.callee) && !node.callee.computed &&
      _t.isIdentifier(node.callee.property, { name: "bind" })) {
    return _resolveExprToFunctionPath(exprPath.get("callee.object"), depth);
  }
  if (_t.isIdentifier(node)) {
    var b = exprPath.scope.getBinding(node.name);
    if (!b) return null;
    if (_t.isFunctionDeclaration(b.path.node)) return b.path;
    if (_t.isVariableDeclarator(b.path.node) && b.path.node.init) {
      var init = b.path.node.init;
      if (_t.isFunctionExpression(init) || _t.isArrowFunctionExpression(init)) {
        return b.path.get("init");
      }
      // `.bind(…)` chain: follow back to the bound function.
      if (_t.isCallExpression(init) && _t.isMemberExpression(init.callee) && !init.callee.computed &&
          _t.isIdentifier(init.callee.property, { name: "bind" })) {
        return _resolveExprToFunctionPath(b.path.get("init"), depth);
      }
    }
    return null;
  }
  if (_t.isMemberExpression(node)) {
    var keyName = null;
    if (!node.computed && _t.isIdentifier(node.property)) keyName = node.property.name;
    else if (node.computed) {
      var keyVals = _resolveAllValues(exprPath.get("property"), (depth || 0) + 1);
      if (keyVals.length > 0) keyName = String(keyVals[0]);
    }
    if (!keyName) return null;
    var objNode = _resolveToObject(exprPath.get("object"), depth || 0);
    if (objNode && objNode._path) {
      for (var i = 0; i < objNode.properties.length; i++) {
        var p = objNode.properties[i];
        if (_t.isObjectProperty(p) && !p.computed) {
          var k = _t.isIdentifier(p.key) ? p.key.name :
            (_t.isStringLiteral(p.key) ? p.key.value : (_t.isNumericLiteral(p.key) ? String(p.key.value) : null));
          if (k === keyName && (_t.isFunctionExpression(p.value) || _t.isArrowFunctionExpression(p.value))) {
            return objNode._path.get("properties." + i + ".value");
          }
        }
        if (_t.isObjectMethod(p) && !p.computed) {
          var mk = _t.isIdentifier(p.key) ? p.key.name :
            (_t.isStringLiteral(p.key) ? p.key.value : (_t.isNumericLiteral(p.key) ? String(p.key.value) : null));
          if (mk === keyName) return objNode._path.get("properties." + i);
        }
      }
    }
    // Also handle `obj.key = function/arrow` and `obj[K] = function/arrow`
    // assignments via the receiver's referencePaths (mutation after
    // initial declaration). Used by webpack-style `f.d = (e, a) => {...}`
    // and module-table installs `d[97088] = function(...) {...}`.
    if (_t.isIdentifier(node.object)) {
      var objBind = exprPath.scope.getBinding(node.object.name);
      if (objBind && objBind.referencePaths) {
        for (var ri = 0; ri < objBind.referencePaths.length; ri++) {
          var rp = objBind.referencePaths[ri];
          var rpParent = rp.parent;
          if (!rpParent || !_t.isMemberExpression(rpParent) || rpParent.object !== rp.node) continue;
          // Match property: support both `.key` and `[K]` where K resolves
          // to keyName.
          var propMatches = false;
          if (!rpParent.computed && _t.isIdentifier(rpParent.property, { name: keyName })) {
            propMatches = true;
          } else if (rpParent.computed) {
            if (_t.isStringLiteral(rpParent.property) && rpParent.property.value === keyName) propMatches = true;
            else if (_t.isNumericLiteral(rpParent.property) && String(rpParent.property.value) === keyName) propMatches = true;
            else {
              var rpKeyVals = _resolveAllValues(rp.parentPath.get("property"), (depth || 0) + 1);
              if (rpKeyVals.length > 0 && String(rpKeyVals[0]) === keyName) propMatches = true;
            }
          }
          if (!propMatches) continue;
          var assignNode = rp.parentPath ? rp.parentPath.parent : null;
          if (!assignNode || !_t.isAssignmentExpression(assignNode) || assignNode.operator !== "=" ||
              assignNode.left !== rpParent) continue;
          var rhsNode = assignNode.right;
          var rhsPath = rp.parentPath.parentPath.get("right");
          if (_t.isFunctionExpression(rhsNode) || _t.isArrowFunctionExpression(rhsNode)) {
            return rhsPath;
          }
          // RHS is a CallExpression / MemberExpression / Identifier that
          // itself resolves to a function — e.g. `c.push = a.bind(null, …)`.
          // Recurse through the general expression-to-function resolver,
          // which already handles .bind, var aliases, etc.
          if (_t.isCallExpression(rhsNode) || _t.isMemberExpression(rhsNode) || _t.isIdentifier(rhsNode)) {
            var rhsFn = _resolveExprToFunctionPath(rhsPath, depth);
            if (rhsFn) return rhsFn;
          }
        }
      }
      // Receiver is a parameter — resolve via callers. Caller passes some
      // expression as the corresponding arg; that expression is what
      // `obj.key` resolves against. For each caller's arg, retry
      // resolution with the arg substituted for obj.
      if (objBind && objBind.kind === "param") {
        var paramName = node.object.name;
        var hostFunc = objBind.scope.path;
        if (hostFunc && hostFunc.node.params) {
          var paramIdx = -1;
          for (var pi = 0; pi < hostFunc.node.params.length; pi++) {
            if (_t.isIdentifier(hostFunc.node.params[pi], { name: paramName })) { paramIdx = pi; break; }
          }
          if (paramIdx >= 0) {
            var callerArgs = _findFunctionCallerArgs(hostFunc);
            for (var ci = 0; ci < callerArgs.length; ci++) {
              if (paramIdx >= callerArgs[ci].length) continue;
              var argP = callerArgs[ci][paramIdx];
              if (!argP) continue;
              // Build a synthetic MemberExpression path: argP.<keyName>.
              // We can't actually construct a path without mutating the
              // AST, so probe argP directly with the same key resolution
              // logic: if argP is an Identifier and assignments to
              // `<argP>.keyName` exist, return them. We do this by
              // recursing: temporarily wrap argP into a MemberExpression
              // by recursing on a fresh path using argP as the object.
              var argSubFn = _probePropertyOnExpr(argP, keyName, depth);
              if (argSubFn) return argSubFn;
            }
          }
        }
      }
    }
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
function _resolveCallReturnValues(callPath, depth) {
  if (!_resolver.guard("R", callPath.node)) return [];
  try {
  var funcPath = _resolveCalleeFuncPath(callPath, depth);
  // IIFE / sequence-wrapped-IIFE: the callee IS the function. Not handled
  // in the shared _resolveCalleeFuncPath because wrapper-sink tracing
  // would then double-count the sinks inside the IIFE body (the direct
  // ast walker already processes them). For value-return resolution,
  // traverse the IIFE's body here to pick up return values.
  if (!funcPath) {
    var callee = callPath.node.callee;
    if (_t.isFunctionExpression(callee) || _t.isArrowFunctionExpression(callee)) {
      funcPath = callPath.get("callee");
    } else if (_t.isSequenceExpression(callee) && callee.expressions.length > 0) {
      var lastIdx = callee.expressions.length - 1;
      var last = callee.expressions[lastIdx];
      if (_t.isFunctionExpression(last) || _t.isArrowFunctionExpression(last)) {
        funcPath = callPath.get("callee.expressions." + lastIdx);
      } else if (_t.isIdentifier(last)) {
        var seqBinding = callPath.scope.getBinding(last.name);
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
  if (!funcPath) return [];

  // Arrow function with expression body: () => "/api/data" or (x) => base + x
  if (_t.isArrowFunctionExpression(funcPath.node) && !_t.isBlockStatement(funcPath.node.body)) {
    return _resolveAllValues(funcPath.get("body"), depth + 1);
  }

  // Collect return values from the function body
  var values = [];
  try {
    funcPath.traverse(Object.assign({
      ReturnStatement: function(retPath) {
        if (retPath.node.argument) {
          var retVals = _resolveAllValues(retPath.get("argument"), depth + 1);
          values = values.concat(retVals);
        }
      },
    }, _SKIP_NESTED_FUNCS));
  } catch (e) { _resolver.collectError(e, "resolveCallReturn"); }
  return values;
  } catch (_rcre) {
    if (_rcre instanceof RangeError) { _resolver.collectError(_rcre, "resolveCallReturnValues"); return []; }
    throw _rcre;
  } finally { _resolver.unguard("R", callPath.node); }
}

// Resolve a call expression to its returned ObjectExpression (if any)
function _resolveCallReturnToObject(callPath, depth) {
  if (!_resolver.guard("O", callPath.node)) return null;
  try {
  var funcPath = _resolveCalleeFuncPath(callPath, depth);
  if (!funcPath) return null;

  // Arrow function with expression body: () => ({url: "/api"})
  if (_t.isArrowFunctionExpression(funcPath.node) && !_t.isBlockStatement(funcPath.node.body)) {
    var bodyPath = funcPath.get("body");
    if (_t.isObjectExpression(bodyPath.node)) {
      bodyPath.node._path = bodyPath;
      return bodyPath.node;
    }
    return _resolveToObject(bodyPath, depth + 1);
  }

  // Traverse function body for return statements that return objects
  var result = null;
  try {
    funcPath.traverse(Object.assign({
      ReturnStatement: function(retPath) {
        if (result) return;
        if (retPath.node.argument) {
          var argPath = retPath.get("argument");
          if (_t.isObjectExpression(argPath.node)) {
            argPath.node._path = argPath;
            result = argPath.node;
          } else {
            result = _resolveToObject(argPath, depth + 1);
          }
        }
      },
    }, _SKIP_NESTED_FUNCS));
  } catch (e) { _resolver.collectError(e, "resolveCallReturnToObject"); }
  return result;
  } catch (_roe) {
    if (_roe instanceof RangeError) { _resolver.collectError(_roe, "resolveCallReturnToObject"); return null; }
    throw _roe;
  } finally { _resolver.unguard("O", callPath.node); }
}

// Resolve an expression to its ObjectExpression node (if it's a variable pointing to one).
// Memoization wrapper: profiling on real github bundles (react-lib's 380KB
// module-exports object) showed _resolveToObjectImpl called 11M+ times
// during one analysis (each `t.X` read triggers the chain). Cache by
// node identity — results are deterministic per-analysis, cycles caught
// inside _resolveToObjectImpl via _resolver.guard.
function _resolveToObject(path, depth) {
  var node = path.node;
  if (_resolveToObjectMemo.has(node)) return _resolveToObjectMemo.get(node);
  var result = _resolveToObjectImpl(path, depth);
  _resolveToObjectMemo.set(node, result);
  return result;
}
function _resolveToObjectImpl(path, depth) {
  var node = path.node;
  // Literal ObjectExpression — no recursion, return directly regardless of depth
  if (_t.isObjectExpression(node)) {
    node._path = path;
    return node;
  }
  if (!_resolver.guard("T", node)) return null;
  try {
  if (_t.isIdentifier(node)) {
    var binding = path.scope.getBinding(node.name);
    if (!binding) {
      // Fallback: try global assignments (window.X = {...})
      var globalDef = _globalAssignments[node.name];
      if (globalDef && _t.isObjectExpression(globalDef.valueNode)) {
        globalDef.valueNode._path = globalDef.valuePath;
        return globalDef.valueNode;
      }
      return null;
    }
    if (_t.isVariableDeclarator(binding.path.node) && binding.path.node.init) {
      if (_t.isObjectExpression(binding.path.node.init)) {
        binding.path.node.init._path = binding.path.get("init");
        return binding.path.node.init;
      }
      // Call expression: var cfg = getConfig() → resolve through return values
      if (_t.isCallExpression(binding.path.node.init)) {
        return _resolveCallReturnToObject(binding.path.get("init"), depth);
      }
      // Destructured: `var [a, modules] = data` or `var {key} = obj`.
      // Resolve the corresponding element/property of the init.
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
            return _resolveToObject(binding.path.get("init.elements." + elemIdx2), depth);
          }
          var arrI = _resolveToArray(binding.path.get("init"), depth);
          if (arrI && arrI._path && elemIdx2 < arrI.elements.length && arrI.elements[elemIdx2]) {
            return _resolveToObject(arrI._path.get("elements." + elemIdx2), depth);
          }
        }
      }
      if (_t.isObjectPattern(binding.path.node.id)) {
        var keyForName2 = _findDestructuredKey(binding.path.node.id, node.name);
        if (keyForName2) {
          var initO = binding.path.node.init;
          if (_t.isObjectExpression(initO)) {
            for (var oki2 = 0; oki2 < initO.properties.length; oki2++) {
              var okp2 = initO.properties[oki2];
              if (_t.isObjectProperty(okp2) && !okp2.computed && _getKeyName(okp2.key) === keyForName2) {
                return _resolveToObject(binding.path.get("init.properties." + oki2 + ".value"), depth);
              }
            }
          }
          var initOO = _resolveToObject(binding.path.get("init"), depth);
          if (initOO && initOO._path) {
            for (var oki3 = 0; oki3 < initOO.properties.length; oki3++) {
              var okp3 = initOO.properties[oki3];
              if (_t.isObjectProperty(okp3) && !okp3.computed && _getKeyName(okp3.key) === keyForName2) {
                return _resolveToObject(initOO._path.get("properties." + oki3 + ".value"), depth);
              }
            }
          }
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
            for (var rfi = 0; rfi < fbObj.referencePaths.length; rfi++) {
              var refO = fbObj.referencePaths[rfi];
              if (_t.isCallExpression(refO.parent) && refO.parent.callee === refO.node &&
                  pIdxObj < refO.parent.arguments.length) {
                var argP = refO.parentPath.get("arguments." + pIdxObj);
                var pObj = _resolveToObject(argP, depth);
                if (pObj) return pObj;
              }
            }
          }
        }
      }
    }
  }
  // Object.assign({}, src1, src2, ...) → merge all object arguments.
  // Each source's properties may live in different files / different
  // ASTs; the synthesized merged object can't have a single Babel _path
  // that supports child navigation (path.get("properties.N.value")
  // requires the underlying node to have a .properties array, which the
  // CallExpression to Object.assign does not). Track per-property paths
  // so downstream consumers reading prop.value._path see the actual
  // source location, and don't set a node-level _path that would crash
  // navigation.
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
            // Attach the source path to the property's value node only
            // when not already attached — _path is a shared mutable AST
            // field and overwriting on every Object.assign visit causes
            // pathological re-traversal in bundles where the same value
            // node is reachable from many sources.
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
      // Synthetic ObjectExpression — no node-level _path because there
      // is no single Babel path that owns these merged properties.
      // Per-property _path was attached above; consumers must navigate
      // via prop.value._path directly, not synObj._path.get(...).
      var synObj = { type: "ObjectExpression", properties: mergedProps };
      return synObj;
    }
  }
  // Generic CallExpression — resolve through the callee's return value(s).
  // Symmetric with the `var x = call()` Identifier branch above (line ~5498)
  // but for cases where the CallExpression itself is the node we need to
  // resolve as an object — e.g. `this.X = readMeta()` viewed from
  // `_resolveToObject(rhsPath)`. Placed AFTER the Object.assign branch so
  // its dedicated merge handling runs first.
  if (_t.isCallExpression(node)) {
    return _resolveCallReturnToObject(path, depth);
  }
  // Member expression: obj.prop where prop's value is an ObjectExpression
  if (_t.isMemberExpression(node) && !node.computed) {
    var propName = _t.isIdentifier(node.property) ? node.property.name : null;
    if (propName) {
      // `this.prop` inside a class method: trace through the constructor's
      // `this.prop = param` assignment and pick up the object-literal
      // arg at a `new C({...})` call site. Complements the primitive
      // value resolution in _resolveAllValues so nested chains like
      // `this.options.xUrl` (getter returns `this.options.xUrl` which
      // needs the options object) can resolve.
      if (_t.isThisExpression(node.object)) {
        var thisFuncPath = path.getFunctionParent && path.getFunctionParent();
        if (thisFuncPath && _t.isClassMethod(thisFuncPath.node) && _t.isClassBody(thisFuncPath.parent)) {
          var classDecl = thisFuncPath.parentPath.parentPath;
          if (classDecl && (_t.isClassDeclaration(classDecl.node) || _t.isClassExpression(classDecl.node)) && classDecl.node.id) {
            var className = classDecl.node.id.name;
            // Reuse the existing resolver — it returns VALUE arrays, not
            // object nodes, so we need the AST path to the object
            // expression. Scan class bindings: find `new ClassName(arg)`
            // callers, pull the matching param's object-literal arg.
            var ctorMethod = null, ctorMethodNode = null;
            for (var cmi = 0; cmi < classDecl.node.body.body.length; cmi++) {
              if (_t.isClassMethod(classDecl.node.body.body[cmi]) && classDecl.node.body.body[cmi].kind === "constructor") {
                ctorMethod = classDecl.get("body.body." + cmi);
                ctorMethodNode = classDecl.node.body.body[cmi];
                break;
              }
            }
            if (ctorMethod && ctorMethodNode) {
              // Fast path: `this.X = expr` where expr resolves to an object
              // in the constructor's scope (no caller substitution needed).
              // Covers `this.options = readMeta()` style assignments where
              // the RHS is a function call returning an object literal.
              var _ctorRhs = _findThisAssignmentRhsPath(ctorMethod, propName);
              if (_ctorRhs) {
                var _rhsObj = _resolveToObject(_ctorRhs, depth + 1);
                if (_rhsObj) return _rhsObj;
              }
              var _assignedRich = _findThisAssignedParamRich(ctorMethod, propName);
              if (_assignedRich) {
                var _ctrMatch = _findCtorParamOrDestr(ctorMethodNode.params, _assignedRich.paramName);
                if (_ctrMatch) {
                  var paramIdx = _ctrMatch.idx;
                  var _destrKey = _assignedRich.propFromParam || _ctrMatch.key || null;
                  // Pull the object node out of a caller's arg, honoring
                  // destructured-param shape. Returns the inner object when
                  // the param is `{destrKey}` and the caller's arg is
                  // `{destrKey: {...}}`; otherwise the arg itself.
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
                    // Indirect callers via Object.defineProperty getter exposure.
                    var _indirect = _findClassIndirectCtorArgs(classBinding, classDecl, paramIdx, depth);
                    for (var _ii = 0; _ii < _indirect.length; _ii++) {
                      var _pulledI = _pullObj(_indirect[_ii]);
                      if (_pulledI) return _pulledI;
                    }
                  }
                }
              }
            }
          }
        }
      }
      var parentObj = _resolveToObject(path.get("object"), depth + 1);
      if (parentObj) {
        for (var i = 0; i < parentObj.properties.length; i++) {
          var prop = parentObj.properties[i];
          if (!_t.isObjectProperty(prop) || prop.computed) continue;
          var key = _t.isIdentifier(prop.key) ? prop.key.name :
            (_t.isStringLiteral(prop.key) ? prop.key.value : null);
          if (key === propName && _t.isObjectExpression(prop.value)) {
            // For synthetic Object.assign-merged parentObj there is no
            // node-level _path (no Babel path owns the merged set).
            // Each merged property already has its own correct _path
            // attached at synthesis time, so skip overwriting in that
            // case. For real ObjectExpression parentObj, derive the
            // child path from the parent.
            if (parentObj._path) {
              prop.value._path = parentObj._path.get("properties." + i + ".value");
            }
            return prop.value;
          }
        }
      }
    }
  }
  return null;
  } catch (_rtoe) {
    if (_rtoe instanceof RangeError) { _resolver.collectError(_rtoe, "resolveToObject"); return null; }
    throw _rtoe;
  } finally { _resolver.unguard("T", node); }
}

// Resolve an expression to its ArrayExpression node (if it's a variable pointing to one)
function _resolveToArray(path, depth) {
  var node = path.node;
  if (_t.isArrayExpression(node)) {
    node._path = path;
    return node;
  }
  if (!_resolver.guard("A", node)) return null;
  try {
  if (_t.isIdentifier(node)) {
    var binding = path.scope.getBinding(node.name);
    if (!binding) return null;
    if (_t.isVariableDeclarator(binding.path.node) && binding.path.node.init &&
        _t.isArrayExpression(binding.path.node.init)) {
      binding.path.node.init._path = binding.path.get("init");
      return binding.path.node.init;
    }
    // Parameter: resolve from callers to find array literal argument
    if (binding.kind === "param") {
      var paramFuncPath = binding.scope.path;
      if (_t.isFunction(paramFuncPath.node)) {
        var pIdx = _findParamIndex(paramFuncPath.node.params, node.name);
        if (pIdx >= 0) {
          var fb = null;
          if (paramFuncPath.node.id) fb = (paramFuncPath.scope.parent || paramFuncPath.scope).getBinding(paramFuncPath.node.id.name);
          if (!fb && _t.isVariableDeclarator(paramFuncPath.parent)) fb = (paramFuncPath.scope.parent || paramFuncPath.scope).getBinding(paramFuncPath.parent.id.name);
          if (fb && fb.referencePaths) {
            for (var ri = 0; ri < fb.referencePaths.length; ri++) {
              var ref = fb.referencePaths[ri];
              if (_t.isCallExpression(ref.parent) && ref.parent.callee === ref.node && pIdx < ref.parent.arguments.length) {
                var argPath = ref.parentPath.get("arguments." + pIdx);
                if (_t.isArrayExpression(argPath.node)) { argPath.node._path = argPath; return argPath.node; }
                var resolved = _resolveToArray(argPath, depth + 1);
                if (resolved) return resolved;
              }
            }
          }
        }
      }
    }
  }
  return null;
  } catch (_rtae) {
    if (_rtae instanceof RangeError) { _resolver.collectError(_rtae, "resolveToArray"); return null; }
    throw _rtae;
  } finally { _resolver.unguard("A", node); }
}

function _resolveParamFromCallers(binding, depth, propName) {
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
            // Pattern 2: obj.extend({prop: value}) — property defined in an extend/mixin call
            if (_t.isMemberExpression(refP) && refP.object === refs[ri].node && !refP.computed) {
              var extProp = _t.isIdentifier(refP.property) ? refP.property.name : null;
              if (extProp === "extend" || extProp === "mixin" || extProp === "assign") {
                var extCall = refs[ri].parentPath ? refs[ri].parentPath.parent : null;
                if (extCall && _t.isCallExpression(extCall) && extCall.callee === refP) {
                  // Scan arguments for object literals containing our property
                  var extCallPath = refs[ri].parentPath.parentPath;
                  for (var ai = 0; ai < extCall.arguments.length && !targetFuncNode; ai++) {
                    var extArg = extCall.arguments[ai];
                    if (!_t.isObjectExpression(extArg)) continue;
                    for (var epi = 0; epi < extArg.properties.length; epi++) {
                      var ep = extArg.properties[epi];
                      if (!_t.isObjectProperty(ep) || ep.computed) continue;
                      var epKey = _t.isIdentifier(ep.key) ? ep.key.name : (_t.isStringLiteral(ep.key) ? ep.key.value : null);
                      if (epKey !== mProp) continue;
                      // Found the property — check if value is a function or call return
                      if (_t.isFunctionExpression(ep.value) || _t.isArrowFunctionExpression(ep.value)) {
                        targetFuncNode = ep.value;
                        targetFuncPath = extCallPath.get("arguments." + ai + ".properties." + epi + ".value");
                      } else if (_t.isCallExpression(ep.value)) {
                        var extRetFunc = _resolveCallReturnToFunction(extCallPath.get("arguments." + ai + ".properties." + epi + ".value"), depth + 1);
                        if (extRetFunc) {
                          targetFuncNode = extRetFunc.node || extRetFunc;
                          targetFuncPath = extRetFunc._path || null;
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

  if (!targetFuncNode) {
    // Fallback: .forEach(fn) or X.each(arr, fn) — resolve array element values
    // forEach: arr.forEach(fn) — fn(element, index, array)
    // jQuery.each: X.each(arr, fn) — fn(index, element)
    if (_t.isMemberExpression(calleeNode) && !calleeNode.computed) {
      var iterMethod = _t.isIdentifier(calleeNode.property) ? calleeNode.property.name : null;
      if (_ITERATION_METHODS[iterMethod] || iterMethod === "each") {
        // V4: skip if callee object is a known non-iterable type
        var _cbObjType = (iterMethod !== "each") ? _getTrackedType(callExprPath.get("callee.object"), calleeNode.object) : null;
        if (_cbObjType && _NON_ITERABLE_TYPES[_cbObjType]) {
          // Known non-iterable — skip
        } else {
        var arrPath = null;
        var elemParamIdx = -1; // which param of callback receives elements
        if (_ITERATION_METHODS[iterMethod]) {
          // arr.forEach/map/filter(fn) — arr is callee.object, fn gets (element, index, array)
          arrPath = callExprPath.get("callee.object");
          elemParamIdx = 0;
        } else if (iterMethod === "each" && callExprPath.node.arguments.length >= 2) {
          // X.each(arr, fn) — arr is first arg, fn gets (index, element)
          arrPath = callExprPath.get("arguments.0");
          elemParamIdx = 1;
        }
        if (arrPath && paramIdx === elemParamIdx) {
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

  // Fallback: if traversal of callee body yielded nothing, try iteration patterns.
  // .forEach(fn) — fn(element, index, array); jQuery.each(arr, fn) — fn(index, element)
  if (values.length === 0 && _t.isMemberExpression(calleeNode) && !calleeNode.computed) {
    var iterMethod = _t.isIdentifier(calleeNode.property) ? calleeNode.property.name : null;
    if (iterMethod === "forEach" || iterMethod === "each") {
      // V4: skip if callee object is a known non-iterable type
      var _fb2ObjType = (iterMethod !== "each") ? _getTrackedType(callExprPath.get("callee.object"), calleeNode.object) : null;
      if (!(_fb2ObjType && _NON_ITERABLE_TYPES[_fb2ObjType])) {
        var arrPath = null;
        var elemParamIdx = -1;
        if (iterMethod === "forEach") {
          arrPath = callExprPath.get("callee.object");
          elemParamIdx = 0;
        } else if (iterMethod === "each" && callExprPath.node.arguments.length >= 2) {
          arrPath = callExprPath.get("arguments.0");
          elemParamIdx = 1;
        }
        if (arrPath && paramIdx === elemParamIdx) {
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
function _resolveStoredCallbackArgs(containerBinding, paramIdx, depth, propName) {
  if (!_resolver.guard("S", containerBinding.identifier)) return [];
  try {
  var values = [];

  // If the container is a parameter of its function, resolve from callers
  // to find the actual variable. E.g., addToStore(structure) { structure.push(cb) }
  // → callers pass the actual array variable.
  if (containerBinding.kind === "param") {
    var enclosingFunc = containerBinding.scope.path;
    var containerParamIdx = -1;
    var params = enclosingFunc.node.params;
    for (var pi = 0; pi < params.length; pi++) {
      if (_t.isIdentifier(params[pi]) && params[pi].name === containerBinding.identifier.name) {
        containerParamIdx = pi; break;
      }
    }
    if (containerParamIdx < 0) return [];

    var funcBinding = _getFunctionBinding(enclosingFunc);
    if (!funcBinding || !funcBinding.referencePaths) return [];

    var callerRefs = funcBinding.referencePaths;
    for (var ri = 0; ri < callerRefs.length; ri++) {
      if (!callerRefs[ri].parent || !_t.isCallExpression(callerRefs[ri].parent) ||
          callerRefs[ri].parent.callee !== callerRefs[ri].node) continue;
      if (containerParamIdx >= callerRefs[ri].parent.arguments.length) continue;

      var actualArg = callerRefs[ri].parent.arguments[containerParamIdx];
      if (_t.isIdentifier(actualArg)) {
        var actualBinding = callerRefs[ri].parentPath.scope.getBinding(actualArg.name);
        if (actualBinding) {
          console.debug("[AST:trace]     stored-callback: container param '%s' bound to '%s'", containerBinding.identifier.name, actualArg.name);
          var subValues = _resolveStoredCallbackArgs(actualBinding, paramIdx, depth + 1, propName);
          values = values.concat(subValues);
        }
      }
    }
    return values;
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
    // E.g., someFunc(container, ...) or someFunc(container[key] || [], ...)
    // Inside that function, a parameter derived from the container is iterated.
    if (_t.isCallExpression(refPath.parent) && refPath.parent.callee !== refPath.node) {
      var iterCallPath = refPath.parentPath;
      var iterArgIdx = -1;
      for (var iai = 0; iai < iterCallPath.node.arguments.length; iai++) {
        if (iterCallPath.node.arguments[iai] === refPath.node) { iterArgIdx = iai; break; }
        // Also check container[key] || [] patterns — container is nested in MemberExpression/LogicalExpression
        if (_containsNode(iterCallPath.node.arguments[iai], refPath.node)) { iterArgIdx = iai; break; }
      }
      if (iterArgIdx >= 0) {
        // Resolve the called function and find where it calls items from the parameter
        var iterVals = _resolveItemCallsInFunction(iterCallPath, iterArgIdx, paramIdx, depth + 1, propName);
        values = values.concat(iterVals);
      }
    }
  }

  return values;
  } catch (_rse) {
    if (_rse instanceof RangeError) { _resolver.collectError(_rse, "resolveStoredCallbackArgs"); return []; }
    throw _rse;
  } finally { _resolver.unguard("S", containerBinding.identifier); }
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
  var targetFuncPath = _resolveCalleeToFunction(callPath);
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

        // Pattern: someLib.each(containerParam[key], function(_, item) { item(args); })
        // or containerParam[key].forEach(function(item) { item(args); })
        // or containerParam.forEach(function(item) { item(args); })
        var iterContainer = null;
        var cbArgStartIdx = -1;

        // .forEach() / .each() on the container or container[key]
        if (_t.isMemberExpression(ic) && !ic.computed) {
          var methodName = _t.isIdentifier(ic.property) ? ic.property.name : null;
          if (methodName === "forEach" || methodName === "each") {
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

        // jQuery.each(containerParam[key], fn) or anyLib.each(containerParam[key], fn)
        if (!iterContainer && _t.isMemberExpression(ic) && !ic.computed &&
            _t.isIdentifier(ic.property, { name: "each" })) {
          var eachArgs = innerPath.node.arguments;
          for (var eai = 0; eai < eachArgs.length; eai++) {
            var eaArg = eachArgs[eai];
            if (_t.isIdentifier(eaArg, { name: containerParamName })) {
              iterContainer = eaArg; cbArgStartIdx = eai + 1; break;
            }
            if (_t.isMemberExpression(eaArg) && _t.isIdentifier(eaArg.object, { name: containerParamName })) {
              iterContainer = eaArg; cbArgStartIdx = eai + 1; break;
            }
            // (containerParam[key] || [])
            if (_t.isLogicalExpression(eaArg)) {
              var eaLeft = eaArg.left;
              if (_t.isIdentifier(eaLeft, { name: containerParamName }) ||
                  (_t.isMemberExpression(eaLeft) && _t.isIdentifier(eaLeft.object, { name: containerParamName }))) {
                iterContainer = eaLeft; cbArgStartIdx = eai + 1; break;
              }
            }
          }
        }

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

function _traceParamToArgs(binding) {
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
      return _traceObjectMethodToArgs(funcPath, paramIdx, methodName);
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
        return _collectMethodCallerArgs(assignObjBinding.referencePaths, assignMethodName, paramIdx);
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
  // Resolve method to string values, search for obj.get/post/... call sites
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
          var cmArgs = _collectMethodCallerArgs(compObjBinding.referencePaths, compPropVals[cvi], paramIdx);
          compArgs = compArgs.concat(cmArgs);
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
              var cgArgs = _collectGlobalMethodCallerArgs(funcPath, gn, compPropVals[cvi2], paramIdx);
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
      return _traceCallbackArgToArgs(cbCallExpr, cbArgIdx, paramIdx);
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
                ptArgs = ptArgs.concat(_collectMethodCallerArgs(instBinding.referencePaths, ptMethodName, paramIdx));
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
          if (iifeVarBinding) return _collectCallerArgs(iifeVarBinding.referencePaths, paramIdx);
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
                  clArgs = clArgs.concat(_collectMethodCallerArgs(instBinding.referencePaths, clMethodName, paramIdx));
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
  return _collectCallerArgs(funcBinding.referencePaths, paramIdx);
}

// Collect concrete caller argument paths from reference paths to a function
function _collectCallerArgs(refs, paramIdx) {
  var args = [];
  if (!refs) return args;
  for (var r = 0; r < refs.length; r++) {
    var refPath = refs[r];
    if (refPath.parent && _t.isCallExpression(refPath.parent) && refPath.parent.callee === refPath.node) {
      var effectiveIdx = paramIdx < refPath.parent.arguments.length ? paramIdx :
        (refPath.parent.arguments.length > 0 ? refPath.parent.arguments.length - 1 : -1);
      if (effectiveIdx >= 0) {
        _collectOrTraceArg(refPath.parentPath.get("arguments." + effectiveIdx), args);
      }
    }
  }
  return args;
}

// If arg is a param identifier, trace further to its callers; otherwise collect it directly
function _collectOrTraceArg(argPath, out) {
  if (_t.isIdentifier(argPath.node)) {
    var binding = argPath.scope.getBinding(argPath.node.name);
    if (binding && binding.kind === "param") {
      var subArgs = _traceParamToArgs(binding);
      if (subArgs.length > 0) {
        for (var i = 0; i < subArgs.length; i++) out.push(subArgs[i]);
        return;
      }
    }
    // Local variable initialized from function call: var s = merge({}, options)
    // Trace through the call's arguments that are params to their callers
    if (binding && binding.path.isVariableDeclarator && binding.path.isVariableDeclarator()) {
      var initNode = binding.path.node.init;
      if (initNode && _t.isCallExpression(initNode)) {
        var initArgs = initNode.arguments;
        for (var ai = 0; ai < initArgs.length; ai++) {
          if (_t.isIdentifier(initArgs[ai])) {
            var argBinding = binding.path.get("init").scope.getBinding(initArgs[ai].name);
            if (argBinding && argBinding.kind === "param") {
              var callSubArgs = _traceParamToArgs(argBinding);
              if (callSubArgs.length > 0) {
                for (var ci = 0; ci < callSubArgs.length; ci++) out.push(callSubArgs[ci]);
                return;
              }
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
function _traceObjectMethodToArgs(funcPath, paramIdx, methodName) {
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
        args = _collectMethodCallerArgs(extObjBinding.referencePaths, methodName, paramIdx);
        // Also check global aliases
        for (var gn in _globalAssignments) {
          var ga = _globalAssignments[gn];
          if (!ga.valueNode) continue;
          var gaVal = ga.valueNode;
          while (_t.isAssignmentExpression(gaVal)) gaVal = gaVal.right;
          if (_t.isIdentifier(gaVal) && gaVal.name === extObjBinding.identifier.name) {
            var globalArgs = _collectGlobalMethodCallerArgs(funcPath, gn, methodName, paramIdx);
            args = args.concat(globalArgs);
          }
        }
      }
      return args;
    }
  }
  if (!objBinding) return [];
  return _collectMethodCallerArgs(objBinding.referencePaths, methodName, paramIdx);
}

// Collect caller args from obj.method(...) call sites
function _collectMethodCallerArgs(refs, methodName, paramIdx) {
  var args = [];
  if (!refs) return args;
  for (var r = 0; r < refs.length; r++) {
    var refPath = refs[r];
    if (refPath.parent && _t.isMemberExpression(refPath.parent) &&
        refPath.parent.object === refPath.node && !refPath.parent.computed &&
        _t.isIdentifier(refPath.parent.property, { name: methodName })) {
      var callNode = refPath.parentPath ? refPath.parentPath.parent : null;
      if (callNode && _t.isCallExpression(callNode) && callNode.callee === refPath.parent) {
        var effectiveIdx = paramIdx < callNode.arguments.length ? paramIdx :
          (callNode.arguments.length > 0 ? callNode.arguments.length - 1 : -1);
        if (effectiveIdx >= 0) {
          _collectOrTraceArg(refPath.parentPath.parentPath.get("arguments." + effectiveIdx), args);
        }
      }
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

function _collectGlobalMethodCallerArgs(funcPath, globalName, methodName, paramIdx) {
  var args = [];
  _traverseGlobalCallers(funcPath, globalName, methodName, function(innerPath) {
    var effectiveIdx = paramIdx < innerPath.node.arguments.length ? paramIdx :
      (innerPath.node.arguments.length > 0 ? innerPath.node.arguments.length - 1 : -1);
    if (effectiveIdx >= 0) _collectOrTraceArg(innerPath.get("arguments." + effectiveIdx), args);
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
function _traceCallbackArgToArgs(callExprPath, cbArgIdx, paramIdx) {
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
          var refs = objBinding.referencePaths;
          for (var ri = 0; ri < refs.length && !targetFuncPath; ri++) {
            var refP = refs[ri].parent;
            if (_t.isMemberExpression(refP) && refP.object === refs[ri].node && !refP.computed) {
              var extProp = _t.isIdentifier(refP.property) ? refP.property.name : null;
              if (extProp === "extend" || extProp === "mixin" || extProp === "assign") {
                var extCall = refs[ri].parentPath ? refs[ri].parentPath.parent : null;
                if (extCall && _t.isCallExpression(extCall) && extCall.callee === refP) {
                  var extCallPath = refs[ri].parentPath.parentPath;
                  for (var ai = 0; ai < extCall.arguments.length && !targetFuncPath; ai++) {
                    var extArg = extCall.arguments[ai];
                    if (!_t.isObjectExpression(extArg)) continue;
                    for (var epi = 0; epi < extArg.properties.length; epi++) {
                      var ep = extArg.properties[epi];
                      if (!_t.isObjectProperty(ep) || ep.computed) continue;
                      var epKey = _t.isIdentifier(ep.key) ? ep.key.name : (_t.isStringLiteral(ep.key) ? ep.key.value : null);
                      if (epKey !== mProp) continue;
                      if (_t.isFunctionExpression(ep.value) || _t.isArrowFunctionExpression(ep.value))
                        targetFuncPath = extCallPath.get("arguments." + ai + ".properties." + epi + ".value");
                      else if (_t.isCallExpression(ep.value)) {
                        var retFunc = _resolveCallReturnToFunction(extCallPath.get("arguments." + ai + ".properties." + epi + ".value"), 0);
                        if (retFunc) targetFuncPath = retFunc._path || null;
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
                  var storedArgs = _traceStoredCallbackToArgs(containerBinding, paramIdx);
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
function _traceStoredCallbackToArgs(containerBinding, paramIdx) {
  if (!_resolver.guard("ZS", containerBinding.identifier)) return [];
  try {
    // If container is a param, resolve from callers
    if (containerBinding.kind === "param") {
      var enclosingFunc = containerBinding.scope.path;
      var containerParamIdx = -1;
      var params = enclosingFunc.node.params;
      for (var pi = 0; pi < params.length; pi++) {
        if (_t.isIdentifier(params[pi]) && params[pi].name === containerBinding.identifier.name) {
          containerParamIdx = pi; break;
        }
      }
      if (containerParamIdx < 0) return [];
      var funcBinding = _getFunctionBinding(enclosingFunc);
      if (!funcBinding || !funcBinding.referencePaths) return [];
      var args = [];
      var callerRefs = funcBinding.referencePaths;
      for (var ri = 0; ri < callerRefs.length; ri++) {
        if (!callerRefs[ri].parent || !_t.isCallExpression(callerRefs[ri].parent) ||
            callerRefs[ri].parent.callee !== callerRefs[ri].node) continue;
        if (containerParamIdx >= callerRefs[ri].parent.arguments.length) continue;
        var actualArg = callerRefs[ri].parent.arguments[containerParamIdx];
        if (_t.isIdentifier(actualArg)) {
          var actualBinding = callerRefs[ri].parentPath.scope.getBinding(actualArg.name);
          if (actualBinding) {
            var subArgs = _traceStoredCallbackToArgs(actualBinding, paramIdx);
            args = args.concat(subArgs);
          }
        }
      }
      return args;
    }

    // Container is local — find where items are called
    var args = [];
    var refs = containerBinding.referencePaths;
    for (var ri = 0; ri < refs.length; ri++) {
      var refPath = refs[ri];
      // Pattern 1: container[i](args) — direct indexed call
      if (_t.isMemberExpression(refPath.parent) && refPath.parent.object === refPath.node && refPath.parent.computed) {
        var memberCallParent = refPath.parentPath ? refPath.parentPath.parent : null;
        if (memberCallParent && _t.isCallExpression(memberCallParent) && memberCallParent.callee === refPath.parent) {
          if (paramIdx < memberCallParent.arguments.length) {
            var directArgPath = refPath.parentPath.parentPath.get("arguments." + paramIdx);
            // If the argument is a local variable or param, trace further to callers
            if (_t.isIdentifier(directArgPath.node)) {
              var directArgBinding = directArgPath.scope.getBinding(directArgPath.node.name);
              if (directArgBinding && directArgBinding.kind === "param") {
                var subArgs = _traceParamToArgs(directArgBinding);
                args = args.concat(subArgs);
              } else {
                args.push(directArgPath);
              }
            } else {
              args.push(directArgPath);
            }
          }
        }
      }
      // Pattern 2: container passed to a function that iterates and calls items
      if (_t.isCallExpression(refPath.parent) && refPath.parent.callee !== refPath.node) {
        var iterCallPath = refPath.parentPath;
        var iterArgIdx = -1;
        for (var iai = 0; iai < iterCallPath.node.arguments.length; iai++) {
          if (iterCallPath.node.arguments[iai] === refPath.node) { iterArgIdx = iai; break; }
          if (_containsNode(iterCallPath.node.arguments[iai], refPath.node)) { iterArgIdx = iai; break; }
        }
        if (iterArgIdx >= 0) {
          var iterArgs = _traceItemCallsToArgs(iterCallPath, iterArgIdx, paramIdx);
          args = args.concat(iterArgs);
        }
      }
    }
    return args;
  } catch (_rzse) {
    if (_rzse instanceof RangeError) { _resolver.collectError(_rzse, "traceStoredCallbackToArgs"); return []; }
    throw _rzse;
  } finally { _resolver.unguard("ZS", containerBinding.identifier); }
}

// Trace iteration pattern to find where item callbacks are called with args
function _traceItemCallsToArgs(callPath, iterArgIdx, paramIdx) {
  if (!_resolver.guard("ZI", callPath.node)) return [];
  try {
    // Resolve the called function
    var funcPath = _resolveCalleeToFunction(callPath);
    if (!funcPath) return [];
    if (iterArgIdx >= funcPath.node.params.length) return [];
    var containerParamName = _t.isIdentifier(funcPath.node.params[iterArgIdx]) ? funcPath.node.params[iterArgIdx].name : null;
    if (!containerParamName) return [];

    // Find iteration patterns inside the function that call items from container
    var args = [];
    try {
      funcPath.traverse({
        CallExpression: function(innerPath) {
          var ic = innerPath.node.callee;
          // jQuery.each(container[key] || [], function(_, item) { item(args); })
          if (_t.isMemberExpression(ic) && !ic.computed && _t.isIdentifier(ic.property, { name: "each" })) {
            var eachArgs = innerPath.node.arguments;
            var containerFound = false;
            for (var eai = 0; eai < eachArgs.length && !containerFound; eai++) {
              var eaArg = eachArgs[eai];
              var isContainer = false;
              if (_t.isIdentifier(eaArg, { name: containerParamName })) isContainer = true;
              else if (_t.isMemberExpression(eaArg) && _t.isIdentifier(eaArg.object, { name: containerParamName })) isContainer = true;
              else if (_t.isLogicalExpression(eaArg)) {
                var left = eaArg.left;
                if (_t.isIdentifier(left, { name: containerParamName }) ||
                    (_t.isMemberExpression(left) && _t.isIdentifier(left.object, { name: containerParamName }))) isContainer = true;
              }
              if (!isContainer) continue;
              containerFound = true;
              // Find callback after container arg
              for (var cai = eai + 1; cai < eachArgs.length; cai++) {
                if (!_t.isFunctionExpression(eachArgs[cai]) && !_t.isArrowFunctionExpression(eachArgs[cai])) continue;
                var cbPath = innerPath.get("arguments." + cai);
                try {
                  cbPath.traverse(Object.assign({
                    CallExpression: function(cbInner) {
                      var cbc = cbInner.node.callee;
                      if (!_t.isIdentifier(cbc)) return;
                      var cbBinding = cbInner.scope.getBinding(cbc.name);
                      if (!cbBinding || cbBinding.kind !== "param" || cbBinding.scope.path !== cbPath) return;
                      if (paramIdx < cbInner.node.arguments.length) {
                        _collectOrTraceArg(cbInner.get("arguments." + paramIdx), args);
                      }
                    },
                  }, _SKIP_NESTED_FUNCS));
                } catch (e) { _resolver.collectError(e, "traceItemCallsInner"); }
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
function _resolvePropsFromArg(argPath, propNames) {
  var result = {};
  for (var i = 0; i < propNames.length; i++) {
    result[propNames[i]] = _resolvePropertyFromArg(argPath, propNames[i], 0);
  }
  // CallExpression arg: X.extend({url: url, type: method}, ...) — check ObjectExpression args
  // Handles: jQuery.ajax(jQuery.extend({url: url, type: method, ...}, ...))
  if (allEmpty(result, propNames) && _t.isCallExpression(argPath.node)) {
    var callArgs = argPath.node.arguments;
    for (var cai = 0; cai < callArgs.length; cai++) {
      var subResult = _resolvePropsFromArg(argPath.get("arguments." + cai), propNames);
      for (var pi = 0; pi < propNames.length; pi++) {
        if (result[propNames[pi]].length === 0) result[propNames[pi]] = subResult[propNames[pi]];
      }
    }
  }
  // If arg is an identifier that's a local variable initialized from a function call (merge/extend),
  // also check the call's arguments for the properties.
  // Handles: var s = jQuery.ajaxSetup({}, options); → check options for each prop
  if (allEmpty(result, propNames) && _t.isIdentifier(argPath.node)) {
    var binding = argPath.scope.getBinding(argPath.node.name);
    if (binding && binding.path.isVariableDeclarator && binding.path.isVariableDeclarator()) {
      var initNode = binding.path.node.init;
      if (initNode && _t.isCallExpression(initNode)) {
        var initPath = binding.path.get("init");
        var initArgs = initNode.arguments;
        for (var iai = 0; iai < initArgs.length; iai++) {
          var subResult = _resolvePropsFromArg(initPath.get("arguments." + iai), propNames);
          for (var pi = 0; pi < propNames.length; pi++) {
            if (result[propNames[pi]].length === 0) result[propNames[pi]] = subResult[propNames[pi]];
          }
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
function _resolveParamFromMethodCalls(objBinding, methodName, paramIdx, depth, propName) {
  // Iterative cycle protection: factory chains (a.create()→b, b.create()→c) can cycle
  var isRoot = !_methodCallVisited;
  if (isRoot) _methodCallVisited = new Set();
  var bindKey = objBinding.identifier.start + ":" + objBinding.identifier.end + ":" + methodName;
  if (_methodCallVisited.has(bindKey)) return [];
  _methodCallVisited.add(bindKey);
  try {
  var values = [];
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
      var cloneVals = _resolveParamFromMethodCalls(fcAssignee, methodName, paramIdx, depth, propName);
      values = values.concat(cloneVals);
    }
  }
  // If no callers found locally, check if the binding is returned from an IIFE
  // that's assigned to an outer variable/global. Pattern:
  // var e = function(){ function n(){} n.get=fn; return n; }(); e.get(url)
  // n has no method callers, but e (= n returned from IIFE) does.
  if (values.length === 0) {
    var encFuncScope = objBinding.scope.path;
    if (encFuncScope && _t.isFunction(encFuncScope.node)) {
      var encParent = encFuncScope.parentPath;
      // Direct IIFE: enclosing function is the callee of a CallExpression
      if (encParent && _t.isCallExpression(encParent.node) && encParent.node.callee === encFuncScope.node) {
        var iifeCallParent = encParent.parentPath;
        // var x = IIFE() — check x.method() callers
        if (iifeCallParent && _t.isVariableDeclarator(iifeCallParent.node) && _t.isIdentifier(iifeCallParent.node.id)) {
          var outerBinding = iifeCallParent.scope.getBinding(iifeCallParent.node.id.name);
          if (outerBinding) {
            console.debug("[AST:trace]     → IIFE-return alias: %s → %s, checking %s.%s() callers",
              objBinding.identifier.name, iifeCallParent.node.id.name, iifeCallParent.node.id.name, methodName);
            values = _resolveParamFromMethodCalls(outerBinding, methodName, paramIdx, depth, propName);
          }
        }
        // (win).X = IIFE() — check global X.method() callers
        if (values.length === 0 && iifeCallParent && _t.isAssignmentExpression(iifeCallParent.node) &&
            _t.isMemberExpression(iifeCallParent.node.left)) {
          var gProp = iifeCallParent.node.left.property;
          var gName = _t.isIdentifier(gProp) ? gProp.name : null;
          if (gName && _globalAssignments[gName]) {
            console.debug("[AST:trace]     → IIFE-return global alias: %s → global %s, checking %s.%s() callers",
              objBinding.identifier.name, gName, gName, methodName);
            values = _resolveParamFromGlobalCallers(objBinding.scope.path, gName, paramIdx, depth, propName, methodName);
          }
        }
      }
      // Factory-argument pattern: !function(t){ win.X = t() }(factoryFunc)
      // factoryFunc returns the object (Se), but it's not the callee — it's an argument
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
                console.debug("[AST:trace]     → factory-arg global: %s → %s(), checking %s.%s() callers",
                  objBinding.identifier.name, _gn, _gn, methodName);
                values = _resolveParamFromGlobalCallers(objBinding.scope.path, _gn, paramIdx, depth, propName, methodName);
                break;
              }
            }
          }
        }
      }
    }
  }
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


// ─── Object Property Resolution from Caller Arguments ───────────────────────
// When a function parameter is used as obj.prop (MemberExpression), resolve the
// property value by finding callers, getting their object literal arguments,
// and extracting the named property.

function _resolvePropertyFromArg(argPath, propName, depth) {
  var objNode = _resolveToObject(argPath, depth + 1);
  if (!objNode || !objNode._path) {
    if (_t.isIdentifier(argPath.node)) {
      var paramBinding = argPath.scope.getBinding(argPath.node.name);
      // Fallback 1: if arg is a param, resolve through caller arguments
      if (paramBinding && paramBinding.kind === "param") {
        return _resolveParamFromCallers(paramBinding, depth + 1, propName);
      }
      // Fallback 2: local variable — look for obj.prop = value assignments
      // Handles: var s = jQuery.ajaxSetup({}, opts); s.type = opts.method || "GET";
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
              var rhsVals = _resolveAllValues(refs[ri].parentPath.parentPath.get("right"), depth + 1);
              assignVals = assignVals.concat(rhsVals);
            }
          }
        }
        if (assignVals.length > 0) return assignVals;
      }
      // Fallback 3: variable initialized from function call — check if property flows through args
      // Handles: var s = merge({}, options); s.url → try options.url → callers' options.url
      if (paramBinding.path.isVariableDeclarator && paramBinding.path.isVariableDeclarator()) {
        var initNode = paramBinding.path.node.init;
        if (initNode && _t.isCallExpression(initNode)) {
          var initPath = paramBinding.path.get("init");
          var initArgs = initNode.arguments;
          for (var iai = 0; iai < initArgs.length; iai++) {
            var initArgVals = _resolvePropertyFromArg(initPath.get("arguments." + iai), propName, depth + 1);
            if (initArgVals.length > 0) return initArgVals;
          }
        }
      }
    }
    return [];
  }
  for (var i = 0; i < objNode.properties.length; i++) {
    var prop = objNode.properties[i];
    if (!_t.isObjectProperty(prop) || prop.computed) continue;
    if (_getKeyName(prop.key) === propName) {
      return _resolveAllValues(objNode._path.get("properties." + i + ".value"), depth + 1);
    }
  }
  return [];
}

// ─── Data Extraction Helpers ────────────────────────────────────────────────

// Resolve a header value node to a string using parameter bindings (for closure variables)
function _resolveHeaderValue(node, bindings) {
  if (_t.isStringLiteral(node)) return node.value;
  if (_t.isIdentifier(node) && bindings[node.name] && bindings[node.name].length > 0 &&
      typeof bindings[node.name][0] === "string") return bindings[node.name][0];
  if (_t.isBinaryExpression(node) && node.operator === "+") {
    var left = _resolveHeaderValue(node.left, bindings);
    var right = _resolveHeaderValue(node.right, bindings);
    if (left !== null && right !== null) return left + right;
  }
  if (_t.isTemplateLiteral(node)) {
    var parts = [];
    for (var qi = 0; qi < node.quasis.length; qi++) {
      parts.push(node.quasis[qi].value.cooked || node.quasis[qi].value.raw);
      if (qi < node.expressions.length) {
        var exprVal = _resolveHeaderValue(node.expressions[qi], bindings);
        if (exprVal === null) return null;
        parts.push(exprVal);
      }
    }
    return parts.join("");
  }
  return null;
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

var _bodyParamVisited = null;
function _extractBodyParams(valNode, scopePath) {
  var isRoot = !_bodyParamVisited;
  if (isRoot) _bodyParamVisited = new Set();
  var nodeKey = (valNode.start != null && valNode.end != null) ? "B" + valNode.start + ":" + valNode.end : null;
  if (nodeKey) {
    if (_bodyParamVisited.has(nodeKey)) return [];
    _bodyParamVisited.add(nodeKey);
  }
  try { return _extractBodyParamsInner(valNode, scopePath); }
  finally { if (isRoot) _bodyParamVisited = null; }
}
function _extractBodyParamsInner(valNode, scopePath) {
  var params = [];
  // Resolve identifiers through scope (variable reference or function parameter)
  if (_t.isIdentifier(valNode) && scopePath && scopePath.scope) {
    var bpBinding = scopePath.scope.getBinding(valNode.name);
    if (bpBinding) {
      // FormData / URLSearchParams field accumulation: when the body
      // identifier traces to a `new FormData()` / `new URLSearchParams()`
      // and code does `e.append(name, value)` / `e.set(name, value)`,
      // each append/set call's first arg is a body field name.
      // Walks the binding's referencePaths for member-call patterns —
      // pure data-flow through W3C native APIs.
      var bpInit = _t.isVariableDeclarator(bpBinding.path.node) ? bpBinding.path.node.init : null;
      var bpType = bpInit && _t.isNewExpression(bpInit) && _t.isIdentifier(bpInit.callee) ? bpInit.callee.name : null;
      if ((bpType === "FormData" || bpType === "URLSearchParams") &&
          !bpBinding.path.scope.getBinding(bpType) && bpBinding.referencePaths) {
        for (var fdri = 0; fdri < bpBinding.referencePaths.length; fdri++) {
          var fdRef = bpBinding.referencePaths[fdri];
          var fdMem = fdRef.parentPath;
          if (!fdMem || !fdMem.isMemberExpression() || fdMem.node.object !== fdRef.node || fdMem.node.computed) continue;
          var fdMethod = _t.isIdentifier(fdMem.node.property) ? fdMem.node.property.name : null;
          if (fdMethod !== "append" && fdMethod !== "set") continue;
          var fdCall = fdMem.parentPath;
          if (!fdCall || !fdCall.isCallExpression() || fdCall.node.callee !== fdMem.node ||
              fdCall.node.arguments.length < 1) continue;
          // Resolve the first arg to a string — the field NAME.
          var fdNameVals = _resolveAllValues(fdCall.get("arguments.0"), 0);
          for (var fdni = 0; fdni < fdNameVals.length; fdni++) {
            if (typeof fdNameVals[fdni] !== "string" || !fdNameVals[fdni]) continue;
            // Avoid duplicate names
            var fdAlreadyHave = false;
            for (var fdpi = 0; fdpi < params.length; fdpi++) {
              if (params[fdpi].name === fdNameVals[fdni]) { fdAlreadyHave = true; break; }
            }
            if (fdAlreadyHave) continue;
            // Try to also resolve the value (arg[1]) for an example.
            var fdField = { name: fdNameVals[fdni], required: true, location: "body" };
            if (fdCall.node.arguments.length >= 2) {
              var fdValVals = _resolveAllValues(fdCall.get("arguments.1"), 0);
              if (fdValVals.length > 0 && typeof fdValVals[0] === "string") {
                fdField.defaultValue = fdValVals[0];
                fdField.type = "string";
              }
            }
            params.push(fdField);
          }
        }
        if (params.length > 0) return params;
      }
      if (_t.isVariableDeclarator(bpBinding.path.node) && bpBinding.path.node.init) {
        return _extractBodyParams(bpBinding.path.node.init, bpBinding.path);
      }
      // Function parameter — resolve from callers
      if (bpBinding.kind === "param") {
        var bpFuncPath = bpBinding.scope.path;
        var bpFuncB = null;
        if (bpFuncPath.node.id) bpFuncB = bpFuncPath.scope.parent ? bpFuncPath.scope.parent.getBinding(bpFuncPath.node.id.name) : null;
        if (!bpFuncB && _t.isVariableDeclarator(bpFuncPath.parent)) bpFuncB = bpFuncPath.scope.parent ? bpFuncPath.scope.parent.getBinding(bpFuncPath.parent.id.name) : null;
        if (bpFuncB && bpFuncB.referencePaths) {
          var bpIdx = _findParamIndex(bpFuncPath.node.params, valNode.name);
          if (bpIdx >= 0) {
            for (var bri = 0; bri < bpFuncB.referencePaths.length; bri++) {
              var bRef = bpFuncB.referencePaths[bri];
              if (_t.isCallExpression(bRef.parent) && bRef.parent.callee === bRef.node &&
                  bpIdx < bRef.parent.arguments.length) {
                var bCallerArg = _extractBodyParams(bRef.parent.arguments[bpIdx], bRef.parentPath);
                if (bCallerArg.length > 0) {
                  for (var bci = 0; bci < bCallerArg.length; bci++) params.push(bCallerArg[bci]);
                }
              }
            }
            if (params.length > 0) return params;
          }
        }
      }
    }
  }
  if (_isJsonStringify(valNode, scopePath)) {
    if (valNode.arguments[0] && _t.isObjectExpression(valNode.arguments[0])) {
      params = _extractObjectProperties(valNode.arguments[0]);
      for (var i = 0; i < params.length; i++) params[i].location = "body";
    } else if (valNode.arguments[0] && _t.isIdentifier(valNode.arguments[0]) && scopePath) {
      // JSON.stringify(identifier) — resolve the identifier
      params = _extractBodyParams(valNode.arguments[0], scopePath);
    }
  } else if (_t.isNewExpression(valNode) && _t.isIdentifier(valNode.callee, { name: "URLSearchParams" }) &&
             (!scopePath || !scopePath.scope.getBinding("URLSearchParams")) &&
             valNode.arguments[0] && _t.isObjectExpression(valNode.arguments[0])) {
    params = _extractObjectProperties(valNode.arguments[0]);
    for (var j = 0; j < params.length; j++) params[j].location = "body";
  } else if (_t.isCallExpression(valNode) && _t.isMemberExpression(valNode.callee) &&
             !valNode.callee.computed && _t.isIdentifier(valNode.callee.property, { name: "toString" }) &&
             scopePath) {
    // `body: new URLSearchParams({...}).toString()` / `body: fd.toString()`
    // — strip the .toString() wrap and recurse on the receiver. Native
    // ECMAScript: .toString() is identity for body-shape extraction.
    params = _extractBodyParams(valNode.callee.object, scopePath);
  } else if (_t.isObjectExpression(valNode)) {
    params = _extractObjectProperties(valNode);
    for (var k = 0; k < params.length; k++) params[k].location = "body";
  } else if (_t.isMemberExpression(valNode) && !valNode.computed &&
             _t.isIdentifier(valNode.object) && _t.isIdentifier(valNode.property) &&
             scopePath && scopePath.scope) {
    // `xhr.send(opts.data)` style: the body is a property on a binding.
    // Real-world wrapper pattern: `function ajax(opts){ xhr.send(opts.data) }`
    // called from `$.post(url, data){ return ajax({url, data}) }` — the
    // outer caller's body literal must flow through opts.data to xhr.send.
    var objName = valNode.object.name;
    var propName = valNode.property.name;
    var objBinding = scopePath.scope.getBinding(objName);
    if (objBinding) {
      var objCandidates = []; // {obj, path}
      if (_t.isVariableDeclarator(objBinding.path.node) && objBinding.path.node.init) {
        var localObj = _resolveToObject(objBinding.path.get("init"), 0);
        if (localObj) objCandidates.push({ obj: localObj, path: objBinding.path.get("init") });
      } else if (objBinding.kind === "param") {
        // Walk callers and resolve each arg at the param position.
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
      // For each candidate object, find the named property and recurse
      // body extraction on its value. First non-empty result wins.
      for (var ci = 0; ci < objCandidates.length; ci++) {
        var co = objCandidates[ci].obj;
        var coPath = objCandidates[ci].path;
        if (!co.properties) continue;
        for (var pi = 0; pi < co.properties.length; pi++) {
          var pp = co.properties[pi];
          if (!_t.isObjectProperty(pp) || pp.computed) continue;
          var pk = _t.isIdentifier(pp.key) ? pp.key.name :
            (_t.isStringLiteral(pp.key) ? pp.key.value : null);
          if (pk !== propName) continue;
          var subScope = co._path ? co._path.get("properties." + pi + ".value") : coPath;
          var sub = _extractBodyParams(pp.value, subScope);
          if (sub.length > 0) { params = sub; break; }
        }
        if (params.length > 0) break;
      }
    }
  }
  return params;
}

function _extractObjectProperties(node) {
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
function _memberChainKey(node) {
  if (_t.isIdentifier(node)) return node.name;
  if (_t.isMemberExpression(node) && !node.computed) {
    var obj = _memberChainKey(node.object);
    if (!obj) return null;
    var prop = _t.isIdentifier(node.property) ? node.property.name :
      (_t.isStringLiteral(node.property) ? node.property.value : null);
    return prop ? obj + "." + prop : null;
  }
  return null;
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

// Detect iteration constraints: arr.forEach(fn), X.each(arr, fn), arr.map(fn)
// The callback parameter is constrained to the array's element values.
function _collectIterationConstraints(path) {
  var node = path.node;
  if (!_t.isMemberExpression(node.callee)) return;
  var methodName = _t.isIdentifier(node.callee.property) ? node.callee.property.name : null;
  if (!methodName) return;

  var arrNode = null, callbackNode = null;

  // Pattern: arr.forEach(fn) / arr.map(fn)
  if ((methodName === "forEach" || methodName === "map") && node.arguments.length >= 1) {
    // V4: skip if callee object is a known non-iterable type
    var _icObjType = _getTrackedType(path.get("callee.object"), node.callee.object);
    if (_icObjType && _NON_ITERABLE_TYPES[_icObjType]) return;
    var arrPath = path.get("callee.object");
    arrNode = _resolveToArray(arrPath, 0);
    callbackNode = node.arguments[0];
  }
  // Pattern: X.each(arr, fn) — jQuery.each / $.each
  else if (methodName === "each" && node.arguments.length >= 2) {
    var eachArrArg = node.arguments[0];
    if (_t.isArrayExpression(eachArrArg)) {
      arrNode = eachArrArg;
      arrNode._path = path.get("arguments.0");
    } else if (_t.isIdentifier(eachArrArg)) {
      arrNode = _resolveToArray(path.get("arguments.0"), 0);
    }
    callbackNode = node.arguments[1];
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

  // Determine which callback parameter receives the element value
  // forEach/map: fn(element, index) — param 0 is element
  // X.each: fn(index, element) — param 1 is element
  var elemParamIdx = (methodName === "each") ? 1 : 0;
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
function _traceValueSource(path, _unused) {
  if (!path || !path.node) return { sourceType: "dynamic", source: null };
  var node = path.node;

  // Cycle detection via visited set keyed on AST node position
  var isRoot = !_taintVisited;
  if (isRoot) _taintVisited = new Set();
  var nodeKey = (node.start != null && node.end != null) ? node.start + ":" + node.end : null;
  if (nodeKey) {
    if (_taintVisited.has(nodeKey)) return { sourceType: "dynamic", source: null };
    _taintVisited.add(nodeKey);
  }
  try { return _traceValueSourceInner(path, node); }
  catch (_tvse) {
    if (_tvse instanceof RangeError) { _resolver.collectError(_tvse, "traceValueSource"); return { sourceType: "dynamic", source: null }; }
    throw _tvse;
  }
  finally { if (isRoot) _taintVisited = null; }
}

// Recursively walk a block statement to find all ReturnStatement nodes,
// including those inside if/else, switch/case, try/catch, for/while, etc.
// Returns the first user-controlled return source found, or null.
function _traceReturnsInBlock(blockPath) {
  if (!blockPath || !blockPath.node) return null;
  var stmts = blockPath.node.body;
  if (!stmts) return null;
  for (var _rbi = 0; _rbi < stmts.length; _rbi++) {
    var stmt = stmts[_rbi];
    var stmtPath = blockPath.get("body." + _rbi);
    if (_t.isReturnStatement(stmt) && stmt.argument) {
      var _rs = _traceValueSource(stmtPath.get("argument"));
      if (_rs.sourceType === "user-controlled") return _rs;
    } else if (_t.isIfStatement(stmt)) {
      if (stmt.consequent && _t.isBlockStatement(stmt.consequent)) {
        var _ifR = _traceReturnsInBlock(stmtPath.get("consequent"));
        if (_ifR && _ifR.sourceType === "user-controlled") return _ifR;
      }
      if (stmt.alternate) {
        if (_t.isBlockStatement(stmt.alternate)) {
          var _elR = _traceReturnsInBlock(stmtPath.get("alternate"));
          if (_elR && _elR.sourceType === "user-controlled") return _elR;
        } else if (_t.isIfStatement(stmt.alternate)) {
          // else if — recurse on the IfStatement's branches via a wrapper
          var _eiR = _traceReturnsInIfChain(stmtPath.get("alternate"));
          if (_eiR && _eiR.sourceType === "user-controlled") return _eiR;
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
            var _csBR = _traceReturnsInBlock(stmtPath.get("cases." + _sci + ".consequent." + _csj));
            if (_csBR && _csBR.sourceType === "user-controlled") return _csBR;
          }
        }
      }
    } else if (_t.isTryStatement(stmt)) {
      if (stmt.block) {
        var _tryR = _traceReturnsInBlock(stmtPath.get("block"));
        if (_tryR && _tryR.sourceType === "user-controlled") return _tryR;
      }
      if (stmt.handler && stmt.handler.body) {
        var _catchR = _traceReturnsInBlock(stmtPath.get("handler.body"));
        if (_catchR && _catchR.sourceType === "user-controlled") return _catchR;
      }
    } else if (_t.isBlockStatement(stmt)) {
      var _blkR = _traceReturnsInBlock(stmtPath);
      if (_blkR && _blkR.sourceType === "user-controlled") return _blkR;
    } else if (stmt.body && _t.isBlockStatement(stmt.body)) {
      // for, while, do-while, etc.
      var _loopR = _traceReturnsInBlock(stmtPath.get("body"));
      if (_loopR && _loopR.sourceType === "user-controlled") return _loopR;
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

function _traceValueSourceInner(path, node) {
  var nodeLoc = node.loc ? { line: node.loc.start.line, column: node.loc.start.column } : null;

  // Literals are safe
  if (_t.isStringLiteral(node) || _t.isNumericLiteral(node) || _t.isBooleanLiteral(node) ||
      _t.isNullLiteral(node) || _t.isTemplateLiteral(node) && node.expressions.length === 0) {
    return { sourceType: "literal", source: null };
  }

  // MemberExpression: check against known user-controlled sources using structural AST matching.
  // Also matches OptionalMemberExpression (`a?.b`) — same taint semantics.
  if ((_t.isMemberExpression(node) || _t.isOptionalMemberExpression(node)) && !node.computed) {
    var taintMatch = _matchTaintSource(path, node);
    if (taintMatch) {
      // Source-specific dimensions. Fact-based, not heuristic: each
      // source intrinsically carries specific parts of a URL.
      //   location.href        → whole URL (user navigates, picks all parts)
      //   location.origin/host → just origin
      //   location.pathname    → just path
      //   location.search      → just query
      //   location.hash        → just hash
      //   document.referrer    → origin+path+query (browsers strip hash from referer)
      //   document.URL         → whole URL (same as location.href semantics)
      //   document.cookie      → content-only (not URL-shaped)
      //   document.domain      → origin only
      //   window.name          → content-only (string bag)
      //   event.data           → content-only
      //   storage.getItem      → content-only
      return _tvsSource(taintMatch, nodeLoc, _dimsForSource(taintMatch));
    }
    // Static access to a known-safe location prop (origin/hostname/protocol)
    // is server-controlled — treat as literal-ish so callers don't promote
    // it to HIGH. Falls through to dynamic for anything else.
    if (_t.isIdentifier(node.object, { name: "location" }) && !path.scope.getBinding("location") &&
        _t.isIdentifier(node.property) && _LOCATION_SAFE_PROPS[node.property.name]) {
      return { sourceType: "literal", source: null };
    }
    // `window.location.<safe-prop>` / `self.location.<safe-prop>`: same as
    // above but through the global prefix. Without this branch, patterns
    // like `new URL(x, window.location.origin)` (standard same-origin
    // idiom) get flagged HIGH. Observed on github's showMore paginator.
    if (_t.isMemberExpression(node.object) && !node.object.computed &&
        _t.isIdentifier(node.object.object) &&
        (node.object.object.name === "window" || node.object.object.name === "self") &&
        !path.scope.getBinding(node.object.object.name) &&
        _t.isIdentifier(node.object.property, { name: "location" }) &&
        _t.isIdentifier(node.property) && _LOCATION_SAFE_PROPS[node.property.name]) {
      return { sourceType: "literal", source: null };
    }
  }
  // Computed location access: `location[x]` where x may select a tainted
  // prop. Safest: classify dynamic; the sink severity will be MEDIUM, not
  // HIGH, unless the computed key itself traces to user-controlled data.
  if (_t.isMemberExpression(node) && node.computed &&
      _t.isIdentifier(node.object, { name: "location" }) && !path.scope.getBinding("location")) {
    return { sourceType: "dynamic", source: null };
  }

  // Object property access: cfg.redirectUrl → resolve to the property value in the initializer
  if (_t.isMemberExpression(node) && !node.computed && _t.isIdentifier(node.object)) {
    var objPropName = _t.isIdentifier(node.property) ? node.property.name : null;
    if (objPropName) {
      var objBinding = path.scope.getBinding(node.object.name);
      if (objBinding && objBinding.path.isVariableDeclarator() && objBinding.path.node.init) {
        var _objInit = objBinding.path.node.init;
        // Direct ObjectExpression initializer: cfg.redirectUrl → resolve property value
        if (_t.isObjectExpression(_objInit)) {
          var objProps = _objInit.properties;
          for (var oi = 0; oi < objProps.length; oi++) {
            if (_t.isObjectProperty(objProps[oi]) &&
                ((_t.isIdentifier(objProps[oi].key) && objProps[oi].key.name === objPropName) ||
                 (_t.isStringLiteral(objProps[oi].key) && objProps[oi].key.value === objPropName))) {
              var propValSource = _traceValueSource(objBinding.path.get("init.properties." + oi + ".value"));
              if (propValSource.sourceType === "user-controlled") return _tvsHop(propValSource, "obj-prop", "." + objPropName, nodeLoc);
            }
          }
        }
        // Object.assign({}, source1, source2) — resolve property from source objects.
        // var merged = Object.assign({}, config); merged.html → config.html
        if (_t.isCallExpression(_objInit) && _t.isMemberExpression(_objInit.callee) &&
            !_objInit.callee.computed && _t.isIdentifier(_objInit.callee.property, { name: "assign" }) &&
            _t.isIdentifier(_objInit.callee.object, { name: "Object" }) &&
            !path.scope.getBinding("Object") && _objInit.arguments.length >= 2) {
          // Search source args (skip target = arg 0) for the property
          for (var _oaPI = 1; _oaPI < _objInit.arguments.length; _oaPI++) {
            var _oaSrcArg = _objInit.arguments[_oaPI];
            // If source is an identifier, resolve to its init
            if (_t.isIdentifier(_oaSrcArg)) {
              var _oaSrcBind = path.scope.getBinding(_oaSrcArg.name);
              if (_oaSrcBind && _oaSrcBind.path.isVariableDeclarator() && _oaSrcBind.path.node.init &&
                  _t.isObjectExpression(_oaSrcBind.path.node.init)) {
                var _oaSrcProps = _oaSrcBind.path.node.init.properties;
                for (var _oaSPI = 0; _oaSPI < _oaSrcProps.length; _oaSPI++) {
                  if (_t.isObjectProperty(_oaSrcProps[_oaSPI]) &&
                      ((_t.isIdentifier(_oaSrcProps[_oaSPI].key) && _oaSrcProps[_oaSPI].key.name === objPropName) ||
                       (_t.isStringLiteral(_oaSrcProps[_oaSPI].key) && _oaSrcProps[_oaSPI].key.value === objPropName))) {
                    var _oaPropSrc = _traceValueSource(_oaSrcBind.path.get("init.properties." + _oaSPI + ".value"));
                    if (_oaPropSrc.sourceType === "user-controlled") return _tvsHop(_oaPropSrc, "obj-assign-prop", "." + objPropName + " (via Object.assign)", nodeLoc);
                  }
                }
              }
            }
            // If source arg is an inline ObjectExpression
            if (_t.isObjectExpression(_oaSrcArg)) {
              for (var _oaInlI = 0; _oaInlI < _oaSrcArg.properties.length; _oaInlI++) {
                if (_t.isObjectProperty(_oaSrcArg.properties[_oaInlI]) &&
                    ((_t.isIdentifier(_oaSrcArg.properties[_oaInlI].key) && _oaSrcArg.properties[_oaInlI].key.name === objPropName) ||
                     (_t.isStringLiteral(_oaSrcArg.properties[_oaInlI].key) && _oaSrcArg.properties[_oaInlI].key.value === objPropName))) {
                  var _oaInlSrc = _traceValueSource(objBinding.path.get("init.arguments." + _oaPI + ".properties." + _oaInlI + ".value"));
                  if (_oaInlSrc.sourceType === "user-controlled") return _tvsHop(_oaInlSrc, "obj-assign-prop", "." + objPropName + " (via Object.assign inline)", nodeLoc);
                }
              }
            }
          }
        }
      }
    }
  }

  // Non-computed MemberExpression: trace through deep property chains.
  // Handles patterns like doc.body.innerHTML where doc comes from a tainted source
  // (e.g., DOMParser().parseFromString(tainted)). Taint propagates through property access.
  // Optional-chained (`a?.b`) is a distinct AST node (OptionalMemberExpression)
  // in Babel but semantically equivalent for taint — handle both.
  if ((_t.isMemberExpression(node) || _t.isOptionalMemberExpression(node)) && !node.computed) {
    var _deepObjSource = _traceValueSource(path.get("object"));
    if (_deepObjSource.sourceType === "user-controlled") {
      var _deepPropName = _t.isIdentifier(node.property) ? node.property.name : "?";
      // URL-object property projections narrow dimensions. The object
      // here is whatever _deepObjSource describes; the property access
      // pulls a specific part of it.
      //   url.origin → origin dim only (as content, the stringified origin)
      //   url.host/hostname/port/protocol → origin dim
      //   url.pathname → path dim
      //   url.search → query dim (the "?..." string)
      //   url.hash → hash dim ("#..." string)
      //   url.href / url.toString() → union of all dims (full string)
      //   url.searchParams → query dim (USP object carries query content)
      // Non-URL property accesses default to pass-through (same dims
      // as the source). We don't try to detect "is this actually a URL
      // object?" — if the source's dimensions include URL parts, these
      // projections still apply; if the source is a non-URL object, the
      // projections happen to pass through whatever content dim it has,
      // which is correct by default.
      var _srcDims = _deepObjSource.dimensions || _tvsDimsContent();
      var _projDims = null;
      switch (_deepPropName) {
        case "origin": case "host": case "hostname": case "protocol": case "port":
          _projDims = { origin: _srcDims.origin, content: _srcDims.origin };
          break;
        case "pathname":
          _projDims = { path: _srcDims.path, content: _srcDims.path };
          break;
        case "search": case "searchParams":
          _projDims = { query: _srcDims.query, content: _srcDims.query };
          break;
        case "hash":
          _projDims = { hash: _srcDims.hash, content: _srcDims.hash };
          break;
        case "href":
          // href is the full serialization — includes all parts.
          _projDims = _srcDims;
          break;
      }
      if (_projDims) {
        return _tvsHopDims(_deepObjSource, "member", "." + _deepPropName, nodeLoc, _projDims);
      }
      return _tvsHop(_deepObjSource, "member", "." + _deepPropName, nodeLoc);
    }
  }

  // Computed MemberExpression: taintedArray[i], tainted.split("=")[1], etc.
  // Taint propagates through indexed access on tainted objects.
  if ((_t.isMemberExpression(node) || _t.isOptionalMemberExpression(node)) && node.computed) {
    var compObjSource = _traceValueSource(path.get("object"));
    if (compObjSource.sourceType === "user-controlled") return _tvsHop(compObjSource, "member-computed", "[…]", nodeLoc);
  }

  // Identifier: resolve via scope binding
  if (_t.isIdentifier(node)) {
    var binding = path.scope.getBinding(node.name);
    if (binding) {
      // Variable initializer. When the binding is NOT constant, the
      // initializer alone doesn't capture the final value at the sink —
      // follow Babel's `constantViolations` list (each entry is an
      // AssignmentExpression path where the var was reassigned) and
      // short-circuit on any user-controlled reassignment. Handles:
      //   let x = "safe"; x = location.hash; sink(x);
      if (binding.path.isVariableDeclarator() && binding.path.node.init) {
        var initSrc = _traceValueSource(binding.path.get("init"));
        if (initSrc.sourceType === "user-controlled") return _tvsHop(initSrc, "binding", node.name, nodeLoc);
        if (binding.constantViolations && binding.constantViolations.length > 0) {
          for (var _cvi = 0; _cvi < binding.constantViolations.length; _cvi++) {
            var _cvPath = binding.constantViolations[_cvi];
            // AssignmentExpression: `x = tainted` — trace the right-hand side.
            if (_cvPath && _cvPath.isAssignmentExpression()) {
              var _cvSrc = _traceValueSource(_cvPath.get("right"));
              if (_cvSrc.sourceType === "user-controlled") return _tvsHop(_cvSrc, "reassign", node.name, nodeLoc);
            }
          }
        }
        return initSrc;
      }
      // Uninitialised declaration: `let x; x = tainted; sink(x)`. The
      // VariableDeclarator has no init but reassignments still matter.
      if (binding.path.isVariableDeclarator() && !binding.path.node.init &&
          binding.constantViolations && binding.constantViolations.length > 0) {
        for (var _cvni = 0; _cvni < binding.constantViolations.length; _cvni++) {
          var _cvnPath = binding.constantViolations[_cvni];
          if (_cvnPath && _cvnPath.isAssignmentExpression()) {
            var _cvnSrc = _traceValueSource(_cvnPath.get("right"));
            if (_cvnSrc.sourceType === "user-controlled") return _tvsHop(_cvnSrc, "reassign-uninit", node.name, nodeLoc);
          }
        }
      }
      // For-in/for-of loop variable: for (var key in obj) → key is user-controlled if obj is.
      // Critical for detecting prototype pollution in recursive merge functions.
      if (binding.path.isVariableDeclarator() && !binding.path.node.init) {
        var _forParent = binding.path.parentPath && binding.path.parentPath.parentPath;
        if (_forParent && (_t.isForInStatement(_forParent.node) || _t.isForOfStatement(_forParent.node)) &&
            _forParent.node.left === binding.path.parent) {
          return _tvsHop(_traceValueSource(_forParent.get("right")), "for-iter", "iterates " + node.name, nodeLoc);
        }
      }
      // Destructured property: const { data } = event → trace back to the parent object.
      // binding.path is the ObjectProperty or RestElement inside the pattern.
      if (_t.isObjectProperty(binding.path.node) || _t.isRestElement(binding.path.node)) {
        // Walk up to find the VariableDeclarator that holds the pattern
        var _destrParent = binding.path.parentPath;
        while (_destrParent && !_destrParent.isVariableDeclarator()) {
          _destrParent = _destrParent.parentPath;
        }
        if (_destrParent && _destrParent.node.init) {
          return _tvsHop(_traceValueSource(_destrParent.get("init")), "destructure-obj", "{" + node.name + "}", nodeLoc);
        }
        // Destructuring in for-of/for-in head: `for (const {data} of events)`
        // — VariableDeclarator has no .init; the iterated expression lives
        // on the enclosing ForOfStatement's .right.
        if (_destrParent && !_destrParent.node.init) {
          var _forOwner = _destrParent.parentPath && _destrParent.parentPath.parentPath;
          if (_forOwner && (_t.isForOfStatement(_forOwner.node) || _t.isForInStatement(_forOwner.node))) {
            return _tvsHop(_traceValueSource(_forOwner.get("right")), "destructure-obj-for", "{" + node.name + "} of …", nodeLoc);
          }
        }
      }
      // Destructured array element: const [a, b] = arr → trace back to the array
      if (_t.isArrayPattern(binding.path.parent)) {
        var _arrDestrParent = binding.path.parentPath;
        while (_arrDestrParent && !_arrDestrParent.isVariableDeclarator()) {
          _arrDestrParent = _arrDestrParent.parentPath;
        }
        if (_arrDestrParent && _arrDestrParent.node.init) {
          return _tvsHop(_traceValueSource(_arrDestrParent.get("init")), "destructure-arr", "[…" + node.name + "…]", nodeLoc);
        }
        // `for (const [k, v] of URLSearchParams)` — the key AND value
        // inherit taint from the iterated source. The URLSearchParams
        // itself was constructed from location.search (user-controlled),
        // so both k and v must trace back to that source.
        if (_arrDestrParent && !_arrDestrParent.node.init) {
          var _forOwner2 = _arrDestrParent.parentPath && _arrDestrParent.parentPath.parentPath;
          if (_forOwner2 && (_t.isForOfStatement(_forOwner2.node) || _t.isForInStatement(_forOwner2.node))) {
            return _tvsHop(_traceValueSource(_forOwner2.get("right")), "destructure-arr-for", "[…" + node.name + "…] of …", nodeLoc);
          }
        }
      }
      // Function parameter — check callers for user-controlled values
      if (binding.kind === "param") {
        var paramIdx = -1;
        var funcPath = binding.scope.path;
        if (funcPath && funcPath.node.params) {
          for (var pi = 0; pi < funcPath.node.params.length; pi++) {
            if (_t.isIdentifier(funcPath.node.params[pi], { name: node.name })) { paramIdx = pi; break; }
          }
          // Destructured parameter: function f({data}) {} or function f({data} = {}) {}
          // Whole-object taint propagates: if the caller's argument is user-controlled,
          // the destructured binding is user-controlled.
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
          // Direct IIFE: (function(a) { ... })(tainted) — parent is CallExpression where callee is this function
          var _iifeParent = funcPath.parentPath;
          if (_iifeParent && _t.isCallExpression(_iifeParent.node) && _iifeParent.node.callee === funcPath.node &&
              paramIdx < _iifeParent.node.arguments.length) {
            var _iifeArgSrc = _traceValueSource(_iifeParent.get("arguments." + paramIdx));
            if (_iifeArgSrc.sourceType === "user-controlled") return _tvsHop(_iifeArgSrc, "param-iife", "param " + node.name + " @ arg " + paramIdx, nodeLoc);
          }
          var callerArgs = _findFunctionCallerArgs(funcPath);
          for (var ci = 0; ci < callerArgs.length; ci++) {
            if (paramIdx < callerArgs[ci].length) {
              var argSource = _traceValueSource(callerArgs[ci][paramIdx]);
              if (argSource.sourceType === "user-controlled") return _tvsHop(argSource, "param-caller", "param " + node.name + " @ arg " + paramIdx, nodeLoc);
            }
          }
          // Array iteration callback: arr.forEach(fn), arr.map(fn), etc.
          // First param receives array elements; if the array is tainted, the param is tainted.
          // Also handles .then(fn) — first param receives the resolved Promise value.
          // V4 fix: use type tracker to skip taint propagation when callee object is a known non-iterable type.
          if (paramIdx === 0) {
            var _iterParent = funcPath.parentPath;
            if (_iterParent && _iterParent.isCallExpression() &&
                _t.isMemberExpression(_iterParent.node.callee) && !_iterParent.node.callee.computed &&
                _iterParent.node.arguments.length >= 1 && _iterParent.node.arguments[0] === funcPath.node) {
              var _iterMethod = _t.isIdentifier(_iterParent.node.callee.property)
                ? _iterParent.node.callee.property.name : null;
              if (_ITERATION_METHODS[_iterMethod] || _iterMethod === "then" || _iterMethod === "catch") {
                // V4: Check if the callee object has a known non-iterable type
                var _iterObjType = _getTrackedType(_iterParent.get("callee.object"), _iterParent.node.callee.object);
                // For .then(fn): break taint propagation when the Promise
                // was produced by a network call (fetch/Request/XHR). The
                // resolved value is the SERVER'S response, not the URL
                // that was requested — even if the URL came from a tainted
                // source, the server's response bytes are server-controlled.
                // Reflected XSS via server echo is a server-side bug, not
                // a client-side DOM-XSS sink the AST should flag HIGH.
                if ((_iterMethod === "then" || _iterMethod === "catch") &&
                    _isNetworkProducingCall(_iterParent.get("callee.object"))) {
                  // skip — resolved value is server-controlled
                } else if (!(_ITERATION_METHODS[_iterMethod] && _iterObjType && _NON_ITERABLE_TYPES[_iterObjType])) {
                  var _iterObjSrc = _traceValueSource(_iterParent.get("callee.object"));
                  if (_iterObjSrc.sourceType === "user-controlled") return _tvsHop(_iterObjSrc, "iter-callback", "." + _iterMethod + " element", nodeLoc);
                }
              }
            }
            // Also check when function is passed by reference: var fn = function(x) {...}; arr.forEach(fn)
            var _iterBinding = _getFunctionBinding(funcPath);
            if (_iterBinding && _iterBinding.referencePaths) {
              for (var _iri = 0; _iri < _iterBinding.referencePaths.length; _iri++) {
                var _irRef = _iterBinding.referencePaths[_iri];
                var _irParent = _irRef.parentPath;
                if (_irParent && _irParent.isCallExpression() &&
                    _t.isMemberExpression(_irParent.node.callee) && !_irParent.node.callee.computed &&
                    _irParent.node.arguments.length >= 1 && _irParent.node.arguments[0] === _irRef.node) {
                  var _irMethod = _t.isIdentifier(_irParent.node.callee.property)
                    ? _irParent.node.callee.property.name : null;
                  if (_ITERATION_METHODS[_irMethod] || _irMethod === "then" || _irMethod === "catch") {
                    // V4: Check if the callee object has a known non-iterable type
                    var _irObjType = _getTrackedType(_irParent.get("callee.object"), _irParent.node.callee.object);
                    // Same network-response taint-break as the direct branch above.
                    if ((_irMethod === "then" || _irMethod === "catch") &&
                        _isNetworkProducingCall(_irParent.get("callee.object"))) {
                      // skip — resolved value is server-controlled
                    } else if (!(_ITERATION_METHODS[_irMethod] && _irObjType && _NON_ITERABLE_TYPES[_irObjType])) {
                      var _irObjSrc = _traceValueSource(_irParent.get("callee.object"));
                      if (_irObjSrc.sourceType === "user-controlled") return _tvsHop(_irObjSrc, "iter-callback-ref", "." + _irMethod + " element (via ref)", nodeLoc);
                    }
                  }
                }
              }
            }
          }
          // reduce callback: arr.reduce(fn, init) — second param (index 1) is the accumulator on first call,
          // but first param (index 0) is the accumulator on subsequent calls, receiving previous return.
          // For taint: if array is tainted, param index 1 (currentValue) receives elements.
          if (paramIdx === 1) {
            var _redParent = funcPath.parentPath;
            if (_redParent && _redParent.isCallExpression() &&
                _t.isMemberExpression(_redParent.node.callee) && !_redParent.node.callee.computed &&
                _redParent.node.arguments.length >= 1 && _redParent.node.arguments[0] === funcPath.node) {
              var _redMethod = _t.isIdentifier(_redParent.node.callee.property)
                ? _redParent.node.callee.property.name : null;
              if (_redMethod === "reduce" || _redMethod === "reduceRight") {
                // V4: Check if the callee object has a known non-iterable type
                var _redObjType = _getTrackedType(_redParent.get("callee.object"), _redParent.node.callee.object);
                if (!(_redObjType && _NON_ITERABLE_TYPES[_redObjType])) {
                  var _redObjSrc = _traceValueSource(_redParent.get("callee.object"));
                  if (_redObjSrc.sourceType === "user-controlled") return _tvsHop(_redObjSrc, "reduce-element", "." + _redMethod + " element", nodeLoc);
                }
              }
            }
          }
        }
        // Message event handler: first param is the MessageEvent object (user-controlled).
        // addEventListener("message", function(event) { ... }) or onmessage = function(e) { ... }
        if (paramIdx === 0 && funcPath) {
          var _isMsgHandler = false;
          var _mhParent = funcPath.parentPath;
          if (_mhParent) {
            // addEventListener("message", handler) or obj.addEventListener("message", handler)
            if (_mhParent.isCallExpression()) {
              var _aeNode = _mhParent.node;
              if (_aeNode.arguments.length >= 2 && _aeNode.arguments[1] === funcPath.node &&
                  _t.isStringLiteral(_aeNode.arguments[0], { value: "message" })) {
                var _aeCal = _aeNode.callee;
                if ((_t.isMemberExpression(_aeCal) && !_aeCal.computed &&
                     _t.isIdentifier(_aeCal.property, { name: "addEventListener" })) ||
                    (_t.isIdentifier(_aeCal, { name: "addEventListener" }) &&
                     !funcPath.scope.getBinding("addEventListener"))) {
                  // ONLY treat as a cross-origin postMessage listener when
                  // the receiver could be cross-origin. BroadcastChannel,
                  // Worker, SharedWorker, MessageChannel, EventSource are
                  // all same-origin-by-spec — their "message" events never
                  // carry cross-origin data, so origin checks don't apply.
                  if (_isCrossOriginMsgReceiver(_aeCal, _mhParent)) {
                    _isMsgHandler = true;
                  }
                }
              }
            }
            // onmessage = function(e) { ... }
            if (_mhParent.isAssignmentExpression() && _mhParent.node.right === funcPath.node) {
              var _omLeft = _mhParent.node.left;
              if (_t.isMemberExpression(_omLeft) && !_omLeft.computed &&
                  _t.isIdentifier(_omLeft.property, { name: "onmessage" })) {
                // Same same-origin discrimination as the addEventListener
                // path: Worker/SharedWorker/BroadcastChannel/MessageChannel/
                // EventSource receivers are not cross-origin. Reuse the
                // existing receiver classifier — treat the LHS MemberExpression
                // as if it were an addEventListener callee.
                if (_isCrossOriginMsgReceiver(_omLeft, _mhParent)) _isMsgHandler = true;
              }
            }
            // Factory pattern: function returned from enclosing function whose call site
            // is in a message handler position, e.g. addEventListener("message", makeHandler(...))
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
                      // addEventListener("message", enclosingFunc(...))
                      if (_mhCallParent && _mhCallParent.isCallExpression()) {
                        var _mhOuterCall = _mhCallParent.node;
                        if (_mhOuterCall.arguments.length >= 2 && _mhOuterCall.arguments[1] === _mhRefParent.node &&
                            _t.isStringLiteral(_mhOuterCall.arguments[0], { value: "message" })) {
                          var _mhOuterCallee = _mhOuterCall.callee;
                          if ((_t.isMemberExpression(_mhOuterCallee) && !_mhOuterCallee.computed &&
                               _t.isIdentifier(_mhOuterCallee.property, { name: "addEventListener" })) ||
                              (_t.isIdentifier(_mhOuterCallee, { name: "addEventListener" }) &&
                               !_mhCallParent.scope.getBinding("addEventListener"))) {
                            if (_isCrossOriginMsgReceiver(_mhOuterCallee, _mhCallParent)) {
                              _isMsgHandler = true;
                            }
                          }
                        }
                      }
                      // onmessage = enclosingFunc(...)
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
          // Named-handler pattern: `var h = function(msg) {...};
          // addEventListener("message", h)`. The function expression's
          // parent is a VariableDeclarator or AssignmentExpression — walk
          // its references to find an addEventListener("message", h) or
          // onmessage = h call.
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
                // addEventListener("message", h)
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
                // target.onmessage = h
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
            return _tvsSource("event.data", nodeLoc, _dimsForSource("event.data"));
          }
        }
        return { sourceType: "dynamic", source: null };
      }
    }
    // Bare `location` identifier (unbound) — the location object itself is user-controlled
    if (node.name === "location") return _tvsSource("location", nodeLoc, _dimsForSource("location"));
    // Unresolvable identifier
    return { sourceType: "dynamic", source: null };
  }

  // Template literal with expressions — check if any expression is
  // user-controlled. If the first quasi is a same-origin-relative
  // literal (e.g. `/api/${t}`), strip origin dim from the propagated
  // hop — the URL's origin is locked by the leading literal.
  if (_t.isTemplateLiteral(node)) {
    var _tmplStripsOrigin = false;
    var _q0 = node.quasis && node.quasis[0];
    var _q0Raw = _q0 && _q0.value ? _q0.value.raw : "";
    if (_q0Raw && !/^https?:/i.test(_q0Raw) && !_q0Raw.startsWith("//") &&
        (_q0Raw.startsWith("/") || _q0Raw.startsWith("./") || _q0Raw.startsWith("../"))) {
      _tmplStripsOrigin = true;
    }
    for (var ti = 0; ti < node.expressions.length; ti++) {
      var exprSource = _traceValueSource(path.get("expressions." + ti));
      if (exprSource.sourceType === "user-controlled") {
        var _hopT = _tvsHop(exprSource, "template-interp", "`…${" + ti + "}…`", nodeLoc);
        if (_tmplStripsOrigin && _hopT && _hopT.dimensions) {
          _hopT.dimensions = _tvsDims({ path: _hopT.dimensions.path, query: _hopT.dimensions.query, hash: _hopT.dimensions.hash, content: true });
        }
        return _hopT;
      }
    }
    return node.expressions.length > 0 ? { sourceType: "dynamic", source: null } : { sourceType: "literal", source: null };
  }

  // TaggedTemplateExpression: `tag\`…${expr}…\`` — the tag function may
  // transform its input, but for taint purposes we assume pass-through.
  // Any tainted interpolation propagates. The classic example is
  // `String.raw\`…${tainted}…\`` which literally concatenates the raw
  // expression value. User tags (html\`…\`, gql\`…\`, etc.) could in
  // theory sanitise, but we can't prove that without reading the tag —
  // conservative propagation is the safer default.
  if (_t.isTaggedTemplateExpression(node)) {
    var _quasiPath = path.get("quasi");
    for (var _tti = 0; _tti < node.quasi.expressions.length; _tti++) {
      var _ttExprSrc = _traceValueSource(_quasiPath.get("expressions." + _tti));
      if (_ttExprSrc.sourceType === "user-controlled") return _tvsHop(_ttExprSrc, "tagged-template", "tag`…${" + _tti + "}…`", nodeLoc);
    }
  }

  // Binary expression (string concat): check both sides. The origin
  // dim of a concatenation reflects the LEFTMOST POSITION's control —
  // the browser URL parser resolves based on the prefix. If the
  // leftmost leaf of a `+` chain is a same-origin-relative literal
  // ("/path", "./x", "../x"), the fetch origin is locked to same-origin
  // regardless of what the attacker appends on the right; strip origin
  // dim on the propagated hop.
  if (_t.isBinaryExpression(node) && node.operator === "+") {
    var leftSource = _traceValueSource(path.get("left"));
    var _concatStripsOrigin = _leftmostLiteralIsSameOriginPrefix(path);
    if (leftSource.sourceType === "user-controlled") {
      var _hopL = _tvsHop(leftSource, "concat-left", "… + …", nodeLoc);
      if (_concatStripsOrigin && _hopL && _hopL.dimensions) {
        _hopL.dimensions = _tvsDims({ path: _hopL.dimensions.path, query: _hopL.dimensions.query, hash: _hopL.dimensions.hash, content: true });
      }
      return _hopL;
    }
    var rightSource = _traceValueSource(path.get("right"));
    if (rightSource.sourceType === "user-controlled") {
      var _hopR = _tvsHop(rightSource, "concat-right", "… + …", nodeLoc);
      if (_concatStripsOrigin && _hopR && _hopR.dimensions) {
        _hopR.dimensions = _tvsDims({ path: _hopR.dimensions.path, query: _hopR.dimensions.query, hash: _hopR.dimensions.hash, content: true });
      }
      return _hopR;
    }
    return { sourceType: "dynamic", source: null };
  }

  // Conditional: check both branches
  if (_t.isConditionalExpression(node)) {
    var consSource = _traceValueSource(path.get("consequent"));
    if (consSource.sourceType === "user-controlled") return _tvsHop(consSource, "ternary-then", "? … : _", nodeLoc);
    var altSource = _traceValueSource(path.get("alternate"));
    if (altSource.sourceType === "user-controlled") return _tvsHop(altSource, "ternary-else", "? _ : …", nodeLoc);
  }

  // Logical expression (||, &&, ??): taint propagates through either side.
  // Common pattern: var url = params.get("url") || "/default"
  if (_t.isLogicalExpression(node)) {
    var _logLeft = _traceValueSource(path.get("left"));
    if (_logLeft.sourceType === "user-controlled") return _tvsHop(_logLeft, "logical-left", "… " + node.operator + " …", nodeLoc);
    var _logRight = _traceValueSource(path.get("right"));
    if (_logRight.sourceType === "user-controlled") return _tvsHop(_logRight, "logical-right", "… " + node.operator + " …", nodeLoc);
  }

  // Sequence expression (comma operator): value is the last expression.
  // Common pattern: (0, eval)(tainted) — indirect eval
  if (_t.isSequenceExpression(node) && node.expressions.length > 0) {
    var _lastIdx = node.expressions.length - 1;
    return _tvsHop(_traceValueSource(path.get("expressions." + _lastIdx)), "sequence", "(_, …)", nodeLoc);
  }

  // AwaitExpression: taint propagates through await.
  // const data = await response.json() where response is tainted
  if (_t.isAwaitExpression(node)) {
    return _tvsHop(_traceValueSource(path.get("argument")), "await", "await …", nodeLoc);
  }

  // SpreadElement: taint propagates through spread.
  // [...tainted] or fn(...tainted)
  if (_t.isSpreadElement(node)) {
    return _tvsHop(_traceValueSource(path.get("argument")), "spread", "…value", nodeLoc);
  }

  // Call expression — check if it's a wrapper around a taint source.
  // Optional-chained calls (`f?.(...)`) are OptionalCallExpression in
  // Babel — they behave identically for taint purposes, handle both.
  if (_t.isCallExpression(node) || _t.isOptionalCallExpression(node)) {
    // localStorage.getItem() / sessionStorage.getItem() — persistent DOM XSS sources.
    // Same-origin write requirement, but a recognized vulnerability class (stored DOM XSS).
    if (_t.isMemberExpression(node.callee) && !node.callee.computed &&
        _t.isIdentifier(node.callee.property, { name: "getItem" })) {
      var _storObj = node.callee.object;
      if ((_t.isIdentifier(_storObj, { name: "localStorage" }) && !path.scope.getBinding("localStorage")) ||
          (_t.isIdentifier(_storObj, { name: "sessionStorage" }) && !path.scope.getBinding("sessionStorage"))) {
        return _tvsSource(_storObj.name + ".getItem", nodeLoc, _dimsForSource(_storObj.name + ".getItem"));
      }
    }
    // Method calls on tainted objects: tainted.slice(), tainted.substring(), tainted.trim(), etc.
    // Also covers optional-chained method calls: `tainted?.slice(1)` — the
    // callee is an OptionalMemberExpression but propagation is identical.
    if ((_t.isMemberExpression(node.callee) || _t.isOptionalMemberExpression(node.callee)) && !node.callee.computed) {
      var callObjSource = _traceValueSource(path.get("callee.object"));
      if (callObjSource.sourceType === "user-controlled") {
        var _callMethodName = _t.isIdentifier(node.callee.property) ? node.callee.property.name : "?";
        // Record the first literal argument alongside the method name
        // so downstream tools can see exactly which key was looked up
        // (e.g. `.get("q")`). This is the AST's observation of the
        // real param name — downstream probes use it directly, no
        // guessing about param names.
        var _argLit = null;
        if (node.arguments.length > 0) {
          var _a0 = node.arguments[0];
          if (_t.isStringLiteral(_a0)) _argLit = _a0.value;
          else if (_t.isTemplateLiteral(_a0) && _a0.expressions.length === 0 && _a0.quasis.length === 1) _argLit = _a0.quasis[0].value.cooked;
        }
        var _hopDesc = "." + _callMethodName + "(" + (_argLit !== null ? JSON.stringify(_argLit) : "") + ")";
        // Method-call dim projection, driven by ECMAScript semantics of
        // standard string / URL methods — NOT by a blanket "all methods
        // strip the prefix" assumption. URL-part sources (location.hash/
        // search/pathname) carry a browser-enforced structural prefix
        // ("#"/"?"/"/") that keeps fetch same-origin. Whether a method
        // preserves that prefix is a FACT about the method's spec:
        //
        //   PREFIX-PRESERVING (dims pass through, no origin upgrade):
        //     - toString, valueOf             — identity on strings
        //     - toLowerCase, toUpperCase      — case only; "#"→"#"
        //     - normalize                     — Unicode; "#" unchanged
        //     - trim, trimStart, trimEnd      — strips whitespace only
        //                                       (location.hash can't start
        //                                       with whitespace — browser
        //                                       guarantees leading "#")
        //     - slice(0, …) / substring(0, …) / substr(0, …)
        //                                     — when literal arg 0, the
        //                                       start is preserved
        //
        //   EXTRACTION (URL-part → free-form content, upgrades origin if
        //   source had a URL-part dim):
        //     - get/getAll/has    — URLSearchParams / Headers / Map pull
        //     - slice/substring/substr with literal N>0  — strips N chars
        //     - at(N)                   — with literal N, returns char N
        //
        //   UNKNOWN (conservative: treat as prefix-stripping):
        //     - everything else (.replace, .split, .match, custom methods,
        //       slice with dynamic offset, …). Errs toward flagging real
        //       issues; non-URL sources don't have URL-part dims so this
        //       over-generalisation is a no-op for them anyway.
        var _sdM = callObjSource.dimensions || _tvsDimsContent();
        var _hasUrlPartDim = !!(_sdM.hash || _sdM.query || _sdM.path);
        var _mcProjDims;
        var _isPrefixPreservingName =
          _callMethodName === "toString" || _callMethodName === "valueOf" ||
          _callMethodName === "toLowerCase" || _callMethodName === "toUpperCase" ||
          _callMethodName === "normalize" ||
          _callMethodName === "trim" || _callMethodName === "trimStart" || _callMethodName === "trimEnd";
        // For slice-family methods, check if the FIRST arg is literal 0.
        var _isSliceFamily =
          _callMethodName === "slice" || _callMethodName === "substring" || _callMethodName === "substr";
        var _sliceStartZero = _isSliceFamily && node.arguments.length >= 1 &&
          _t.isNumericLiteral(node.arguments[0]) && node.arguments[0].value === 0;
        // Map/Set/WeakMap/WeakSet are handled upstream in the call-arg
        // propagation branch — `bfCache.get(tainted)` doesn't taint the
        // result when the receiver is a tracked collection. This block
        // only runs with a user-controlled receiver, so there's no need
        // to re-check type here.
        // If source has no surviving attacker dim, no method call can
        // synthesise one back. Earlier transforms (e.g. `new URL(rel, base)`
        // where base contributes only origin dim and origin gets stripped
        // for current-origin sources) can drive every dim to false; the
        // chain stays in the taint graph by binding identity, but there's
        // nothing for the method's projection to upgrade. Without this
        // guard, `.text()` (or any unknown method) on an already-cleansed
        // value re-introduces a `content` dim from thin air, producing
        // false-positive XSS findings.
        var _hasAnyAttackerDim = !!(_sdM.origin || _sdM.path || _sdM.query || _sdM.hash || _sdM.content);
        if (!_hasAnyAttackerDim) {
          _mcProjDims = { origin: false, content: false };
        } else if (_isPrefixPreservingName || _sliceStartZero) {
          _mcProjDims = {
            origin: _sdM.origin, path: _sdM.path, query: _sdM.query, hash: _sdM.hash,
            content: _sdM.origin || _sdM.path || _sdM.query || _sdM.hash || _sdM.content,
          };
        } else if (_callMethodName === "get" || _callMethodName === "getAll" || _callMethodName === "has") {
          // URLSearchParams / Headers pull — the URL-part structure
          // doesn't survive the extraction; a single value is attacker-
          // controlled free-form content.
          _mcProjDims = { origin: _sdM.origin || _hasUrlPartDim, content: true };
        } else {
          // Prefix-stripping (slice(N>0), substring, substr, replace,
          // split, at, match, …) or unknown method. URL-part dims lost;
          // origin upgrades when source had a URL-part dim.
          _mcProjDims = { origin: _sdM.origin || _hasUrlPartDim, content: true };
        }
        var _hop = _mcProjDims
          ? _tvsHopDims(callObjSource, "method-call", _hopDesc, nodeLoc, _mcProjDims)
          : _tvsHop(callObjSource, "method-call", _hopDesc, nodeLoc);
        if (_argLit !== null && _hop && _hop.taintPath && _hop.taintPath.length) {
          // Also attach the raw literal as a structured field so tools
          // don't have to parse the desc string.
          _hop.taintPath[_hop.taintPath.length - 1].arg = _argLit;
          _hop.taintPath[_hop.taintPath.length - 1].method = _callMethodName;
        }
        return _hop;
      }
    }
    // Object.assign(target, ...sources) / Object.create — taint propagates from any source arg.
    // This handles var merged = Object.assign({}, taintedConfig) where the tainted data is in arg 1+.
    if (_t.isMemberExpression(node.callee) && !node.callee.computed &&
        _t.isIdentifier(node.callee.property, { name: "assign" }) &&
        _t.isIdentifier(node.callee.object, { name: "Object" }) && !path.scope.getBinding("Object") &&
        node.arguments.length >= 2) {
      for (var _oaIdx = 0; _oaIdx < node.arguments.length; _oaIdx++) {
        var _oaArgSrc = _traceValueSource(path.get("arguments." + _oaIdx));
        if (_oaArgSrc.sourceType === "user-controlled") return _tvsHop(_oaArgSrc, "object-assign-arg", "Object.assign arg " + _oaIdx, nodeLoc);
      }
    }
    // decodeURIComponent(location.hash), atob(location.search), etc.
    if (node.arguments.length > 0) {
      // `.get(taintedKey)` / `.has(taintedKey)` — the attacker-control
      // of the KEY doesn't imply attacker-control of the looked-up
      // VALUE. URLSearchParams/Headers/Request/Response are the
      // exception: their lookups surface the URL query / header map /
      // body — content the attacker CAN inject. Everything else
      // (Map/Set/WeakMap/WeakSet, or custom collection classes like
      // github's LRU `bfCache`) stores values independent of the key,
      // so propagating key-taint to the value is a false positive by
      // default. Flip to propagate-ONLY-on-URL-like-receivers, matching
      // the semantics of what the key-value relationship actually is.
      if (_t.isMemberExpression(node.callee) && !node.callee.computed &&
          _t.isIdentifier(node.callee.property) &&
          (node.callee.property.name === "get" || node.callee.property.name === "has")) {
        var _ccmRecvType = _getTrackedType(path, node.callee.object);
        var _isUrlLikeLookup = _ccmRecvType === "URLSearchParams" ||
          _ccmRecvType === "Headers" || _ccmRecvType === "Request" || _ccmRecvType === "Response";
        if (_isUrlLikeLookup) {
          var argSource = _traceValueSource(path.get("arguments.0"));
          if (argSource.sourceType === "user-controlled") {
            var _callCalleeName = "." + node.callee.property.name + "()";
            return _tvsHop(argSource, "call-arg", _callCalleeName + " arg 0", nodeLoc);
          }
        }
        // else: collection-lookup or unknown type — don't propagate key
        // taint; fall through so function-return / inter-procedural
        // branches below still run (in case the stored value is itself
        // traceable to a user-controlled source through another flow).
      } else {
        var argSource = _traceValueSource(path.get("arguments.0"));
        if (argSource.sourceType === "user-controlled") {
          var _callCalleeName = _t.isIdentifier(node.callee) ? node.callee.name + "()" :
            (_t.isMemberExpression(node.callee) && _t.isIdentifier(node.callee.property) ? "." + node.callee.property.name + "()" : "call()");
          return _tvsHop(argSource, "call-arg", _callCalleeName + " arg 0", nodeLoc);
        }
      }
    }
    // Resolve function return value: buildApiUrl("x") → trace return statement in body
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
      var _retResult = _traceReturnsInBlock(_calleeFuncPath.get("body"));
      if (_retResult && _retResult.sourceType === "user-controlled") {
        var _retCalleeName = _t.isIdentifier(_calleeNode) ? _calleeNode.name + "()" : "call()";
        return _tvsHop(_retResult, "function-return", "return of " + _retCalleeName, nodeLoc);
      }
    }
  }

  // `new URL(input, base?)` — dimension-aware handling.
  //
  // The output URL is an object whose ORIGIN comes from:
  //   • `input` if it's absolute (http://… or //…)
  //   • `base` otherwise (relative-input resolves against base.origin)
  // And whose PATH/QUERY/HASH always come from `input` (the base's
  // path is replaced unless input is purely a fragment/query suffix —
  // we treat input as controlling path/query/hash in all cases for
  // the safer-than-sound direction).
  //
  // This matters because `new URL(this.container.src, location.href)`
  // in real code (remote-input-element, auto-complete-element) pulls
  // ORIGIN from location.href (same-origin / user-navigated) and
  // PATH/QUERY/HASH from the DOM attribute (author-controlled). A
  // downstream fetch sees a URL whose origin is NOT attacker-picked
  // (because location.href only reflects wherever the user navigated,
  // which is same-origin from this page's perspective) unless the
  // input is ALSO attacker-controlled AND could be absolute.
  if (_t.isNewExpression(node) && _t.isIdentifier(node.callee, { name: "URL" }) &&
      !path.scope.getBinding("URL") && node.arguments.length >= 1) {
    var _urlInputPath = path.get("arguments.0");
    var _urlInputNode = node.arguments[0];
    var _urlBasePath = node.arguments.length >= 2 ? path.get("arguments.1") : null;
    var _urlInputSrc = _traceValueSource(_urlInputPath);
    var _urlBaseSrc = _urlBasePath ? _traceValueSource(_urlBasePath) : null;

    // Detect if input is known-relative (literal starting with /, ./, ../,
    // or relative-prefixed template). Relative input → origin from base;
    // absolute / unknown input → origin could come from either.
    var _urlInputIsRelative =
      (_t.isStringLiteral(_urlInputNode) &&
        !/^https?:/i.test(_urlInputNode.value) &&
        !_urlInputNode.value.startsWith("//")) ||
      (_t.isTemplateLiteral(_urlInputNode) && (function () {
        var q = _urlInputNode.quasis;
        var h = (q[0] && q[0].value && q[0].value.raw) || "";
        return h !== "" && !/^https?:/i.test(h) && !h.startsWith("//");
      })());

    // Build the output URL object's dimensions from the pieces.
    var _inputDims = _urlInputSrc && _urlInputSrc.dimensions ? _urlInputSrc.dimensions : null;
    var _baseDims = _urlBaseSrc && _urlBaseSrc.dimensions ? _urlBaseSrc.dimensions : null;
    // Path/query/hash come from INPUT (whatever it controls).
    // The `content` stringification of a URL combines all dims — so
    // content-attacker means: either input content OR (when base
    // origin is attacker-controllable AND input is relative) base origin.
    var _outDims = {
      origin: _urlInputIsRelative
        ? !!(_baseDims && _baseDims.origin)
        : !!((_inputDims && _inputDims.origin) || (_baseDims && _baseDims.origin && !_urlInputIsRelative && !_urlInputSrc)),
      path: !!(_inputDims && _inputDims.path),
      query: !!(_inputDims && _inputDims.query),
      hash: !!(_inputDims && _inputDims.hash),
      content: !!((_inputDims && _inputDims.content) || (_baseDims && _baseDims.content && _urlInputIsRelative)),
    };
    // If EITHER input or base is user-controlled, the URL is tainted.
    // Pick the inner hop based on which arg carries the taint.
    if (_urlInputSrc && _urlInputSrc.sourceType === "user-controlled") {
      var h1 = _tvsHop(_urlInputSrc, "new-url-input", "new URL(input, …)", nodeLoc);
      if (h1) h1.dimensions = _tvsDims(_outDims);
      return h1;
    }
    if (_urlBaseSrc && _urlBaseSrc.sourceType === "user-controlled") {
      var h2 = _tvsHop(_urlBaseSrc, "new-url-base", "new URL(_, base)", nodeLoc);
      if (h2) h2.dimensions = _tvsDims(_outDims);
      return h2;
    }
    // Neither arg tainted → not user-controlled; fall through.
  }

  // NewExpression: taint propagates through constructor arguments.
  // new URLSearchParams(tainted), new URL(tainted), new Blob([tainted]) etc.
  if (_t.isNewExpression(node) && node.arguments.length > 0) {
    var _newCalleeName = _t.isIdentifier(node.callee) ? node.callee.name : "ctor";
    for (var _ni = 0; _ni < node.arguments.length; _ni++) {
      var _nArgSrc = _traceValueSource(path.get("arguments." + _ni));
      if (_nArgSrc.sourceType === "user-controlled") return _tvsHop(_nArgSrc, "new-arg", "new " + _newCalleeName + " arg " + _ni, nodeLoc);
    }
  }

  // ArrayExpression: taint propagates if any element is tainted.
  // [...tainted], [tainted, "safe"], etc.
  if (_t.isArrayExpression(node) && node.elements.length > 0) {
    for (var _ai = 0; _ai < node.elements.length; _ai++) {
      if (node.elements[_ai]) {
        var _aElSrc = _traceValueSource(path.get("elements." + _ai));
        if (_aElSrc.sourceType === "user-controlled") return _tvsHop(_aElSrc, "array-elem", "[…" + _ai + "…]", nodeLoc);
      }
    }
  }

  // ObjectExpression: taint propagates if any property value is tainted.
  // {html: location.hash, safe: "ok"} → tainted because of location.hash property
  if (_t.isObjectExpression(node) && node.properties.length > 0) {
    for (var _opi = 0; _opi < node.properties.length; _opi++) {
      var _opProp = node.properties[_opi];
      if (_t.isObjectProperty(_opProp)) {
        var _opValSrc = _traceValueSource(path.get("properties." + _opi + ".value"));
        if (_opValSrc.sourceType === "user-controlled") {
          var _opPropKey = _t.isIdentifier(_opProp.key) ? _opProp.key.name :
            _t.isStringLiteral(_opProp.key) ? _opProp.key.value : "?";
          return _tvsHop(_opValSrc, "obj-prop-value", "{" + _opPropKey + ": …}", nodeLoc);
        }
      }
      if (_t.isSpreadElement(_opProp)) {
        var _opSpreadSrc = _traceValueSource(path.get("properties." + _opi + ".argument"));
        if (_opSpreadSrc.sourceType === "user-controlled") return _tvsHop(_opSpreadSrc, "obj-spread", "{...…}", nodeLoc);
      }
    }
  }

  // Assignment expression: check right side
  if (_t.isAssignmentExpression(node)) {
    return _tvsHop(_traceValueSource(path.get("right")), "assignment-rhs", "lhs = …", nodeLoc);
  }

  return { sourceType: "dynamic", source: null };
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
function _valueHasSameOriginPrefix(valuePath, visited) {
  if (!valuePath || !valuePath.node) return false;
  visited = visited || new Set();
  var node = valuePath.node;
  if (visited.has(node)) return false;
  visited.add(node);
  var scope = valuePath.scope;
  if (_t.isBinaryExpression(node) && node.operator === "+") {
    return _valueHasSameOriginPrefix(valuePath.get("left"), visited);
  }
  if (_t.isIdentifier(node)) {
    var binding = scope.getBinding(node.name);
    if (binding && binding.path && binding.path.isVariableDeclarator() && binding.path.node.init) {
      return _valueHasSameOriginPrefix(binding.path.get("init"), visited);
    }
    return false;
  }
  return _isSameOriginBaseExpr(node, scope);
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
function _walkGuardsFromPath(start, sinkSrc, out, gateCache, visitedFns) {
  var ancestor = start;
  while (ancestor) {
    var parent = ancestor.parentPath;
    if (!parent) return;
    // Shape 1: IfStatement whose consequent contains the sink.
    if (parent.isIfStatement() && parent.node.consequent &&
        (parent.node.consequent === ancestor.node ||
         _nodeContains(parent.node.consequent, ancestor.node))) {
      _collectEqualities(parent.get("test"), sinkSrc, out, gateCache);
    } else if (parent.isConditionalExpression() && parent.node.consequent === ancestor.node) {
      _collectEqualities(parent.get("test"), sinkSrc, out, gateCache);
    }
    // Shape 2: SwitchCase whose body contains the sink.
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
    // Shape 3: early-return gate inside the enclosing block.
    if (parent.isBlockStatement() && Array.isArray(parent.node.body)) {
      for (var ri = 0; ri < parent.node.body.length; ri++) {
        var stmt = parent.node.body[ri];
        if (stmt === ancestor.node) break;
        _extractEarlyReturnGate(parent.get("body." + ri), sinkSrc, out, gateCache);
      }
    }
    // Inter-procedural: when we cross a function boundary, recurse from
    // each caller. This captures `if (cond) makeRequest(...)` style
    // gates even when the sink lives deep inside makeRequest.
    if (parent.isFunction()) {
      var fnKey = parent.node.start + ":" + parent.node.end;
      if (visitedFns.has(fnKey)) return;        // cycle — stop
      visitedFns.add(fnKey);
      var refs = _callerReferencePaths(parent);
      for (var ci = 0; ci < refs.length; ci++) {
        // The caller is an Identifier reference; walk from the
        // CallExpression that consumed it (or from the reference itself
        // when no call wraps it — shouldn't happen for resolved calls).
        var callPath = refs[ci].findParent(function(p) { return p.isCallExpression(); });
        _walkGuardsFromPath(callPath || refs[ci], sinkSrc, out, gateCache, visitedFns);
      }
      return;                                    // caller sites handle everything above the function
    }
    ancestor = parent;
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
function _collectEqualities(testPath, sinkSrc, out, gateCache) {
  if (!testPath || !testPath.node) return;
  var n = testPath.node;
  if (_t.isLogicalExpression(n) && n.operator === "&&") {
    _collectEqualities(testPath.get("left"), sinkSrc, out, gateCache);
    _collectEqualities(testPath.get("right"), sinkSrc, out, gateCache);
    return;
  }
  if (_t.isBinaryExpression(n) && (n.operator === "===" || n.operator === "==" ||
      n.operator === "!==" || n.operator === "!=")) {
    // Only record equality preconditions (`===`/`==`). Inequalities
    // don't pin the probe to a single value so they aren't usable.
    if (n.operator !== "===" && n.operator !== "==") return;
    var leftLit = _asLiteralValue(n.left);
    var rightLit = _asLiteralValue(n.right);
    var memberPath = null; var literalValue;
    if (leftLit === undefined && rightLit !== undefined) { memberPath = testPath.get("left"); literalValue = rightLit; }
    else if (rightLit === undefined && leftLit !== undefined) { memberPath = testPath.get("right"); literalValue = leftLit; }
    else return;
    var memChain = _memberChainFromSameSourceCached(memberPath, sinkSrc, gateCache);
    if (memChain) out.push({ path: memChain, op: "===", value: literalValue });
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
function _isNetworkProducingCall(callPath) {
  if (!callPath || !callPath.node) return false;
  var node = callPath.node;
  if (!_t.isCallExpression(node)) return false;
  var callee = node.callee;

  // Direct `fetch(urlArg)` / `window.fetch(urlArg)` etc.: only break taint
  // when the URL argument is provably same-origin.
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

  // Chained `fetch(...).then(...)` / `.then().then(...)` / `fetch(...).catch(...)`:
  // walk the prefix chain down to a root fetch and check its URL. Each
  // intermediate `.then(r => r.text())` preserves the server-origin
  // constraint — if the root fetch was same-origin, the response body at
  // any .then depth is still same-origin-served.
  if (_t.isMemberExpression(callee) && !callee.computed && _t.isIdentifier(callee.property)) {
    var m = callee.property.name;
    if (m === "then" || m === "catch" || m === "finally") {
      return _isNetworkProducingCall(callPath.get("callee.object"));
    }
    // Response-reader methods (resp.json(), resp.text(), etc.). These are
    // called on a Response object passed into a .then(r => ...). Whether
    // this "produces" same-origin content depends on the original fetch,
    // not visible from here. Conservative: return false (keep taint).
    if (m === "json" || m === "text" || m === "blob" || m === "arrayBuffer" || m === "formData") {
      return false;
    }
  }
  return false;
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
function _isSameOriginFetchTarget(argPath) {
  if (!argPath || !argPath.node) return false;
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

  // Identifier — resolve to init.
  if (_t.isIdentifier(node)) {
    var b = argPath.scope.getBinding(node.name);
    if (b && b.path.isVariableDeclarator() && b.path.node.init) {
      return _isSameOriginFetchTarget(b.path.get("init"));
    }
    return false;
  }

  // Binary `+` concatenation: `"/api?" + tainted` — walk left-most descendant
  // and check its string prefix. If the leftmost leaf is a same-origin
  // relative-path literal, the whole expression targets same-origin.
  // Use an iterative walker (no recursion on chainable + nodes) per
  // CLAUDE.md rules.
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
      // Empty prefix or leading-identifier prefix — can't prove same-origin.
      return false;
    }
    return false;
  }

  // Call expression like `someUrl.toString()` where someUrl is a URL we
  // know is same-origin — walk into the callee's object.
  if (_t.isCallExpression(node) && _t.isMemberExpression(node.callee) &&
      !node.callee.computed && _t.isIdentifier(node.callee.property, { name: "toString" })) {
    return _isSameOriginFetchTarget(argPath.get("callee.object"));
  }

  // Member expression like `URL_CONST.toString()` on a URL we can resolve — skip.
  return false;
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
var _GLOBAL_CALL_SINKS = {
  "eval": { type: "eval", sink: "eval" },
  "open": { type: "redirect", sink: "window.open" },
  "fetch": { type: "request-forgery", sink: "fetch" },
  "importScripts": { type: "eval", sink: "importScripts" },
  "$": { type: "xss", sink: "jQuery" },
  "jQuery": { type: "xss", sink: "jQuery" },
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

  // jQuery DOM manipulation: .html(), .append(), .prepend(), .after(), .before(), .replaceWith()
  // Skip when the receiver is a tracked non-DOM type — `.append()` is also
  // a method on FormData, URLSearchParams, and Headers (key/value push,
  // not HTML injection). Receiver type check matches the `href`/`src`
  // assign sink skip (8296-8301). Real-world FP: react-router builds a
  // FormData via `t.append(r, a)` inside a submission helper.
  if ((methName === "html" || methName === "append" || methName === "prepend" ||
       methName === "after" || methName === "before" || methName === "replaceWith") &&
      node.arguments.length > 0) {
    var _jqReceiverType = _getTrackedType(path, callee.object);
    if (_jqReceiverType === "FormData" || _jqReceiverType === "URLSearchParams" ||
        _jqReceiverType === "Headers" || _jqReceiverType === "Request" ||
        _jqReceiverType === "Response" || _jqReceiverType === "URL") return;
    var _jqSrc = _traceValueSource(path.get("arguments.0"), 0);
    if (_jqSrc.sourceType === "user-controlled") _pushSink(result, node, "xss", "." + methName, _jqSrc, path);
    return;
  }

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

// ─── Security Analysis: React dangerouslySetInnerHTML Detection ─────────────

// Detect { dangerouslySetInnerHTML: { __html: taintedValue } } in object literals.
// Property names survive minification — "dangerouslySetInnerHTML" and "__html" are string keys.
// Used in React.createElement and JSX-compiled output.
function _processReactDangerousHTML(path, result) {
  var node = path.node;
  if (!node.properties || node.properties.length === 0) return;
  for (var i = 0; i < node.properties.length; i++) {
    var prop = node.properties[i];
    if (!_t.isObjectProperty(prop) || prop.computed) continue;
    if (_getKeyName(prop.key) !== "dangerouslySetInnerHTML") continue;
    if (!_t.isObjectExpression(prop.value)) continue;
    for (var j = 0; j < prop.value.properties.length; j++) {
      var inner = prop.value.properties[j];
      if (!_t.isObjectProperty(inner) || inner.computed) continue;
      if (_getKeyName(inner.key) !== "__html") continue;
      var htmlSrc = _traceValueSource(path.get("properties." + i + ".value.properties." + j + ".value"), 0);
      if (htmlSrc.sourceType === "user-controlled") _pushSink(result, node, "xss", "dangerouslySetInnerHTML", htmlSrc, path);
    }
  }
}

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
function _classifyAssignmentValueShape(rightPath) {
  if (!rightPath || !rightPath.node) return "unknown";
  var node = rightPath.node;
  if (_t.isStringLiteral(node) || _t.isNumericLiteral(node) ||
      _t.isBooleanLiteral(node) || _t.isNullLiteral(node) ||
      _t.isTemplateLiteral(node)) return "primitive";
  if (_t.isObjectExpression(node) || _t.isArrayExpression(node)) return "object";
  // new Object(), new Array(), JSON.parse(…) — object-valued.
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
    // `x.split(...)[n]` — primitive (string slice).
    if (meth === "toString" || meth === "toLowerCase" || meth === "toUpperCase" || meth === "trim") {
      return "primitive";
    }
  }
  // Member access with numeric or literal index into a split/slice result
  // typically yields a primitive string element. Conservative fallback:
  // treat as string primitive when the chain is `<call>[<numeric>]`.
  if (_t.isMemberExpression(node) && _t.isNumericLiteral(node.property) &&
      _t.isCallExpression(node.object) && _t.isMemberExpression(node.object.callee) &&
      _t.isIdentifier(node.object.callee.property)) {
    var splitMeth = node.object.callee.property.name;
    if (splitMeth === "split" || splitMeth === "match" || splitMeth === "slice") return "primitive";
  }
  // Identifier — resolve binding and recurse on its init expression.
  if (_t.isIdentifier(node)) {
    var b = rightPath.scope.getBinding(node.name);
    if (b && b.path.isVariableDeclarator() && b.path.node.init) {
      return _classifyAssignmentValueShape(b.path.get("init"));
    }
    return "unknown";
  }
  return "unknown";
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

function _describeNode(node) {
  if (_t.isIdentifier(node)) return node.name;
  if (_t.isMemberExpression(node)) {
    var propName = _t.isIdentifier(node.property) ? node.property.name :
      (_t.isStringLiteral(node.property) ? "[" + node.property.value + "]" : "?");
    if (_t.isIdentifier(node.object)) return node.object.name + "." + propName;
    if (_t.isThisExpression(node.object)) return "this." + propName;
    if (_t.isMemberExpression(node.object)) return _describeNode(node.object) + "." + propName;
    return "(" + node.object.type + ")." + propName;
  }
  return "(" + node.type + ")";
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
