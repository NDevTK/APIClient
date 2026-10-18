/* The extension's shared classic-script global scopes, audited for a declaration that silently replaces an
   earlier script's function: `node engine/scopegate.mjs [--substitute <extension path>=<file>]...`.
   ECMA-262 §16.1.7 GlobalDeclarationInstantiation binds every script's top-level function and var names in the
   one global environment, so a later script's `function f` or `var f = …` replaces an earlier `f` and every
   caller of the first reaches the second; within one script the last declaration of a name is used; and a
   lexical name clashing with an earlier declaration throws a SyntaxError, so that whole script does not run.
   A scope is an extension page's classic scripts in document order, the service worker followed by its
   importScripts, or one world's content scripts in injection order (the MAIN world's also share the page's
   realm, which no audit can see). `--substitute` reads a file in place of one shipped script, for a control.
   Exit 0 pass, 1 findings, 2 a self-test control failed or a script could not be read. */

import { readFileSync, readdirSync } from 'node:fs';
import { dirname, join, posix } from 'node:path';
import { fileURLToPath } from 'node:url';
import { parse } from '@babel/parser';

const EXT = join(dirname(fileURLToPath(import.meta.url)), '..', 'extension');

function die(msg) { console.log('[scopegate] UNREADABLE  ' + msg); process.exit(2); }

const substitutes = new Map();
for (let i = 2; i < process.argv.length; i++) {
  const a = process.argv[i];
  if (a !== '--substitute' || i + 1 >= process.argv.length || !process.argv[i + 1].includes('='))
    die('usage: node engine/scopegate.mjs [--substitute <extension path>=<file>]...');
  const [p, f] = process.argv[++i].split(/=(.*)/s);
  substitutes.set(p, f);
}
function scriptText(path) {
  return readFileSync(substitutes.has(path) ? substitutes.get(path) : join(EXT, path), 'utf8');
}

/* The classic scripts of one page, in document order: HTML's script data is raw text up to `</script`, and a
   comment hides a tag. A script whose type is not JavaScript is a data block and does not run; a module has
   its own scope. Script data holding `<!--` changes where the element ends (the escaped states), which this
   reader does not model, so it refuses that page. */
function pageScripts(page) {
  const html = readFileSync(join(EXT, page), 'utf8');
  const out = [];
  const re = /<!--[\s\S]*?-->|<script\b([^>]*)>([\s\S]*?)<\/script\s*>/gi;
  let m, inline = 0;
  while ((m = re.exec(html)) !== null) {
    if (m[0].startsWith('<!--')) continue;
    const attrs = m[1], body = m[2];
    const type = /\btype\s*=\s*["']?([^"'\s>]+)/i.exec(attrs);
    if (type && type[1].toLowerCase() === 'module') continue;
    if (type && !/^(text|application)\/(java|ecma)script$/i.test(type[1])) continue;
    const src = /\bsrc\s*=\s*["']([^"']+)["']/i.exec(attrs);
    if (src) out.push({ name: posix.join(posix.dirname(page), src[1]), text: null });
    else {
      if (body.includes('<!--')) die(page + ' has a classic script whose data holds `<!--`');
      out.push({ name: page + '#inline' + (++inline), text: body });
    }
  }
  return out;
}

/* The service worker, then the scripts its top level imports: the worker's own declarations are instantiated
   before its first statement runs, so an imported function replaces a worker function of the same name. */
function workerScripts(worker) {
  const ast = parseScript(worker, scriptText(worker));
  const out = [{ name: worker, text: null }];
  for (const st of ast.program.body) {
    const c = st.type === 'ExpressionStatement' ? st.expression : null;
    if (!c || c.type !== 'CallExpression' || c.callee.type !== 'Identifier' || c.callee.name !== 'importScripts')
      continue;
    for (const a of c.arguments) {
      if (a.type !== 'StringLiteral') die(worker + ' line ' + a.loc.start.line + ' imports a script it does not name');
      out.push({ name: a.value, text: null });
    }
  }
  return out;
}

function parseScript(name, text) {
  try { return parse(text, { sourceType: 'script', errorRecovery: false }); }
  catch (e) { die(name + ': ' + e.message); }
}

const aliasOf = (n) => (n && n.type === 'Identifier' ? n.name : null);
const isFunctionValue = (n) => !!n && (n.type === 'FunctionExpression' || n.type === 'ArrowFunctionExpression' ||
                                       n.type === 'ClassExpression');
function boundNames(pat, out) {
  if (pat.type === 'Identifier') out.push(pat.name);
  else if (pat.type === 'ObjectPattern')
    for (const p of pat.properties) boundNames(p.type === 'RestElement' ? p.argument : p.value, out);
  else if (pat.type === 'ArrayPattern')
    for (const p of pat.elements) { if (p) boundNames(p.type === 'RestElement' ? p.argument : p, out); }
  else if (pat.type === 'AssignmentPattern') boundNames(pat.left, out);
  else if (pat.type === 'RestElement') boundNames(pat.argument, out);
  else die('a binding pattern of kind ' + pat.type + ' this audit does not read');
  return out;
}

/* One script's global declarations: top-level functions and lexical names, every `var` its top-level
   statements hoist (ECMA-262 §8.2.13 Static Semantics: TopLevelVarScopedDeclarations, which descends into
   blocks and loops but not into functions), and each global property its load assigns: `self.x = …` in a
   statement that runs at load, directly or in a function expression it calls at once with the global object
   as an argument. A function declared in a block is bound only when the block runs (ECMA-262 §B.3.2
   Block-Level Function Declarations Web Legacy Compatibility Semantics); it is counted as unread. */
const GLOBAL_OBJECT = ['self', 'globalThis', 'window'];
function declarations(name, text) {
  const ast = parseScript(name, text);
  const decls = [], unread = { blockFunctions: 0 };
  /* `g.x = v` where `g` names the global object, as a declaration of `x`. */
  const assigned = (e, aliases) => {
    if (e.type !== 'AssignmentExpression' || e.operator !== '=' || e.left.type !== 'MemberExpression') return false;
    const o = e.left.object, k = e.left.property;
    if (o.type !== 'Identifier' || !aliases.includes(o.name)) return false;
    const key = !e.left.computed && k.type === 'Identifier' ? k.name : k.type === 'StringLiteral' ? k.value : null;
    if (key === null) return false;
    decls.push({ name: key, kind: 'assigned', fn: isFunctionValue(e.right), alias: aliasOf(e.right), init: true,
                 line: e.loc.start.line });
    return true;
  };
  /* A function expression called at once and handed the global object: its own statements run at load, with
     the parameter bound to that object. */
  const called = (e, aliases) => {
    const c = e.type === 'UnaryExpression' ? e.argument : e;
    if (c.type !== 'CallExpression') return false;
    const f = c.callee;
    if (f.type !== 'FunctionExpression' && f.type !== 'ArrowFunctionExpression') return false;
    const inner = GLOBAL_OBJECT.slice();
    c.arguments.forEach((a, i) => {
      if (a.type === 'Identifier' && aliases.includes(a.name) && f.params[i] && f.params[i].type === 'Identifier')
        inner.push(f.params[i].name);
    });
    if (f.body.type === 'BlockStatement') for (const s of f.body.body) loads(s, inner);
    return true;
  };
  /* A statement that runs at load: only an assignment to the global object, or a further such call, binds a
     global; a called body's own declarations are local. */
  const loads = (st, aliases) => {
    if (!st) return;
    if (st.type === 'ExpressionStatement') {
      const e = st.expression;
      for (const x of e.type === 'SequenceExpression' ? e.expressions : [e]) assigned(x, aliases) || called(x, aliases);
    } else if (st.type === 'BlockStatement') for (const s of st.body) loads(s, aliases);
    else if (st.type === 'IfStatement') { loads(st.consequent, aliases); loads(st.alternate, aliases); }
    else if (st.type === 'TryStatement') {
      loads(st.block, aliases); if (st.handler) loads(st.handler.body, aliases); loads(st.finalizer, aliases);
    }
  };
  const vars = (d) => {
    for (const v of d.declarations)
      for (const n of boundNames(v.id, []))
        decls.push({ name: n, kind: d.kind === 'var' ? 'var' : 'lexical', fn: isFunctionValue(v.init),
                     alias: aliasOf(v.init), init: v.init !== null, line: v.loc.start.line });
  };
  const nested = (st) => {
    if (!st) return;
    switch (st.type) {
      case 'VariableDeclaration': if (st.kind === 'var') vars(st); return;
      case 'FunctionDeclaration': unread.blockFunctions++; return;
      case 'BlockStatement': for (const s of st.body) nested(s); return;
      case 'IfStatement': nested(st.consequent); nested(st.alternate); return;
      case 'ForStatement':
        nested(st.init && st.init.type === 'VariableDeclaration' ? st.init : null); nested(st.body); return;
      case 'ForInStatement': case 'ForOfStatement':
        if (st.left.type === 'VariableDeclaration') nested(st.left);
        nested(st.body); return;
      case 'WhileStatement': case 'DoWhileStatement': case 'LabeledStatement': case 'WithStatement':
        nested(st.body); return;
      case 'TryStatement': nested(st.block); if (st.handler) nested(st.handler.body); nested(st.finalizer); return;
      case 'SwitchStatement': for (const c of st.cases) for (const s of c.consequent) nested(s); return;
      case 'ExpressionStatement': loads(st, GLOBAL_OBJECT); return;
      default: return;
    }
  };
  for (const st of ast.program.body) {
    if (st.type === 'FunctionDeclaration')
      decls.push({ name: st.id.name, kind: 'function', fn: true, alias: null, init: true, line: st.loc.start.line });
    else if (st.type === 'ClassDeclaration')
      decls.push({ name: st.id.name, kind: 'lexical', fn: true, alias: null, init: true, line: st.loc.start.line });
    else if (st.type === 'VariableDeclaration' && st.kind !== 'var') vars(st);
    else nested(st);
  }
  /* A value that is a name of this script's own function (`self.f = f`) is that function. */
  const fns = new Set(decls.filter((d) => d.kind === 'function').map((d) => d.name));
  for (const d of decls) if (d.alias !== null && fns.has(d.alias)) d.fn = true;
  return { decls, unread };
}

/* The findings over one scope, whose scripts are `{name, text}` in evaluation order. */
function audit(scripts) {
  const findings = [];
  const bound = new Map();   // name -> the declaration that last bound it, with its script
  const seen = new Set();
  let unreadBlockFunctions = 0;
  for (const s of scripts) {
    if (seen.has(s.name)) { findings.push('LOADED TWICE  `' + s.name + '` runs twice in this scope'); continue; }
    seen.add(s.name);
    const { decls, unread } = declarations(s.name, s.text === null ? scriptText(s.name) : s.text);
    unreadBlockFunctions += unread.blockFunctions;
    const own = new Map();
    for (const d of decls) {
      const was = own.get(d.name);
      if (was && was.fn && d.init && (d.kind === 'function' || d.kind === 'var'))
        findings.push('DUPLICATE  `' + s.name + '` line ' + d.line + ' replaces its own function `' + d.name +
                      '` declared on line ' + was.line);
      /* `self.f = f` re-states the declaration it names, which stays the one reported. */
      if (!was || (d.init && !(d.alias === d.name && was.kind === 'function'))) own.set(d.name, d);
    }
    for (const d of decls) {
      const e = bound.get(d.name);
      if (!e || e.script === s.name) continue;
      const lexical = d.kind === 'lexical' || e.kind === 'lexical';
      if (lexical && (d.kind === 'assigned' || e.kind === 'assigned'))
        findings.push('SHADOWS  `' + s.name + '` line ' + d.line + ' and `' + e.script + '` line ' + e.line +
                      ' bind `' + d.name + '` as a lexical name and as a global property, so a bare `' + d.name +
                      '` and `self.' + d.name + '` name two values');
      else if (lexical)
        findings.push('CLASH  `' + s.name + '` line ' + d.line + ' declares `' + d.name + '`, which `' + e.script +
                      '` line ' + e.line + ' already binds — a SyntaxError, and the later script does not run');
      else if (e.fn && d.init)
        findings.push('REPLACES  `' + s.name + '` line ' + d.line + ' replaces the function `' + d.name + '` that `' +
                      e.script + '` line ' + e.line + ' declares; every caller of the first reaches the second');
    }
    for (const [n, d] of own) if (d.init || !bound.has(n)) bound.set(n, Object.assign({ script: s.name }, d));
  }
  return { findings, names: bound.size, unreadBlockFunctions };
}

/* The audit's own grip, checked every run beside the scopes it judges. */
{
  const control = (label, scripts, expect) => {
    const got = audit(scripts.map(([name, text]) => ({ name, text }))).findings;
    const ok = expect === null ? got.length === 0 : got.length === 1 && got[0].startsWith(expect);
    if (!ok) { console.log('[scopegate] CONTROL FAILED  ' + label + ': ' + JSON.stringify(got)); process.exit(2); }
  };
  control('a later function over an earlier one', [['a.js', 'function f() {}'], ['b.js', 'function f(v) {}']],
          'REPLACES');
  control('a later initialized var over an earlier function',
          [['a.js', 'var f = () => 1;'], ['b.js', 'if (1) { var f = 2; }']], 'REPLACES');
  control('a later lexical over an earlier var', [['a.js', 'var f;'], ['b.js', 'const f = 1;']], 'CLASH');
  control('two declarations of one function in a script', [['a.js', 'function f() {}\nfunction f() {}']], 'DUPLICATE');
  control('a later global property over an earlier function',
          [['a.js', 'function f() {}'], ['b.js', '(function (g) { g.f = 1; })(self);']], 'REPLACES');
  control('a later function over an earlier global property',
          [['a.js', 'self.f = () => 0;'], ['b.js', 'function f() {}']], 'REPLACES');
  control('a later function over an earlier one its script also exports',
          [['a.js', 'function f() {}\nself.f = f;'], ['b.js', 'function f() {}']], 'REPLACES');
  control('distinct names, a bare var over a function, a local in a called body',
          [['a.js', 'function f() {}'],
           ['b.js', 'function g() {}\nvar f;\n(function (g) { function f() {} g.h = 1; })(self);']], null);
}

const manifest = JSON.parse(readFileSync(join(EXT, 'manifest.json'), 'utf8'));
const scopes = [];
for (const page of readdirSync(EXT).filter((f) => f.endsWith('.html')).sort())
  scopes.push({ label: page, scripts: pageScripts(page) });
if (manifest.background && manifest.background.service_worker)
  scopes.push({ label: 'service worker ' + manifest.background.service_worker,
                scripts: workerScripts(manifest.background.service_worker) });
const RUN_AT = ['document_start', 'document_end', 'document_idle'];
const worlds = new Map();
for (const cs of manifest.content_scripts || []) {
  const world = cs.world || 'ISOLATED';
  const at = RUN_AT.indexOf(cs.run_at || 'document_idle');
  if (at < 0) die('manifest.json names a content-script run_at `' + cs.run_at + '`');
  if (!worlds.has(world)) worlds.set(world, []);
  for (const js of cs.js || []) worlds.get(world).push({ at, name: js });
}
for (const [world, list] of worlds)
  scopes.push({ label: 'content scripts, ' + world + ' world',
                scripts: list.sort((a, b) => a.at - b.at).map((x) => ({ name: x.name, text: null })) });
for (const p of substitutes.keys())
  if (!scopes.some((s) => s.scripts.some((x) => x.name === p))) die('`' + p + '` is in no scope');

let total = 0;
for (const sc of scopes) {
  const r = audit(sc.scripts);
  console.log('[scopegate] ' + sc.label + ': ' + sc.scripts.length + ' classic script(s), ' + r.names +
              ' global name(s), ' + r.findings.length + ' finding(s)' +
              (r.unreadBlockFunctions ? ', ' + r.unreadBlockFunctions + ' block-level function(s) unread' : ''));
  for (const f of r.findings) console.log('[scopegate]   ' + f);
  total += r.findings.length;
}
console.log('[scopegate] ' + total + ' finding(s) over ' + scopes.length + ' scope(s); not read: a global property ' +
            'assigned by code that runs after load, and a block-level function counted above');
process.exit(total ? 1 : 0);
