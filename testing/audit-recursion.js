// Lint rule: recursion is banned in our own code — direct AND indirect.
//
// Walks every top-level `function _name(` definition in our analyzer
// source, builds a call graph (name → set of names it calls), and
// flags any cycle:
//   - Self-loop:  fn → fn  (the canonical "self-recursion")
//   - Mutual:     a → b → a (indirect recursion through one or more
//                            helpers — same JS-stack risk, just via a
//                            cycle of frames)
//
// The earlier version only caught self-loops, which let mutual
// recursion sneak in (e.g. _resolveAllValues → _resolveBodyParam →
// _resolveAllValues). Tarjan SCC flags every non-trivial strongly
// connected component, so any cycle of any length lights up.
//
// Member calls (`obj.fn()`) and method properties (`{ fn() {} }`)
// are excluded — only top-level `\bname(` calls inside a function
// body count. This avoids false positives where a top-level helper
// shares its name with a Babel-traverse visitor key (`Identifier`,
// `Program`, …) which is dispatched by an external traverser, not
// from our own call sites.
//
// Exit codes: 0 = clean (no cycles), 1 = at least one cycle.
// Wired into the test runner so a regression causes the suite to fail.

const fs = require("fs");
const path = require("path");

// Auto-discover every JS file under extension/ so new files added in
// the future are checked automatically. Excludes third-party bundles
// that aren't authored here (babel-bundle.js, prism.js) — the
// recursion ban applies to OUR code; vendored libraries aren't
// modified to comply with our lint.
const SKIP_FILES = new Set([
  "extension/lib/babel-bundle.js",
  "extension/lib/prism.js",
]);
function _discoverExtensionJsFiles() {
  // Iterative BFS over directories — the lint enforces no recursion
  // in scoped code, and applying the same rule here keeps the lint
  // self-consistent.
  const out = [];
  const stack = ["extension"];
  while (stack.length > 0) {
    const dir = stack.pop();
    const entries = fs.readdirSync(dir, { withFileTypes: true });
    for (const e of entries) {
      const full = path.join(dir, e.name).replace(/\\/g, "/");
      if (e.isDirectory()) stack.push(full);
      else if (e.isFile() && full.endsWith(".js") && !SKIP_FILES.has(full)) {
        out.push(full);
      }
    }
  }
  return out.sort();
}
const FILES = _discoverExtensionJsFiles();

// Match `function NAME(` and `async function NAME(` at line start. ALL
// top-level function definitions (any name) are subject to the recursion
// ban — public helpers, parsers, encoders, exporters alike.
const funcRe = /^(?:async\s+)?function\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(/;

// Find the matching closing brace for the function body that opens at
// `openIdx` (the index of `{`). Skips strings (single, double, template),
// regex literals, and comments. Returns the index of the matching `}` or
// -1 if unmatched. Brace-aware bounds eliminate false positives where a
// top-level invocation after the function body looks like a self-call to
// a naive line-range scan.
//
// Regex-vs-division: a `/` starts a regex literal when the previous
// non-whitespace, non-comment character permits a primary expression
// (operators, opening brackets, statement-start punctuation, or one of
// a small keyword set). Otherwise it's the division operator.
const REGEX_PRECEDERS = new Set([
  "(", ",", "=", ":", ";", "[", "{", "&", "|", "?", "+", "-", "*", "/", "%",
  "!", "<", ">", "~", "^", "}", "\n",
]);
const REGEX_PRECEDER_KEYWORDS = new Set([
  "return", "typeof", "instanceof", "in", "of", "void", "delete", "new",
  "throw", "yield", "await", "do", "else",
]);

function findFunctionEnd(src, openIdx) {
  let depth = 1;
  let i = openIdx + 1;
  let lastTok = "{";
  function setTok(c) { lastTok = c; }
  while (i < src.length) {
    const ch = src[i];
    if (ch === "/" && src[i + 1] === "/") {
      i = src.indexOf("\n", i);
      if (i < 0) return -1;
      i++;
      continue;
    }
    if (ch === "/" && src[i + 1] === "*") {
      const end = src.indexOf("*/", i + 2);
      if (end < 0) return -1;
      i = end + 2;
      continue;
    }
    if (ch === " " || ch === "\t" || ch === "\r") { i++; continue; }
    if (ch === "\n") { setTok("\n"); i++; continue; }
    if (ch === "/") {
      let isRegex = false;
      if (REGEX_PRECEDERS.has(lastTok)) isRegex = true;
      if (!isRegex && /[a-zA-Z_]/.test(lastTok)) {
        let wend = i - 1;
        while (wend >= 0 && /\s/.test(src[wend])) wend--;
        let wstart = wend;
        while (wstart >= 0 && /[a-zA-Z_]/.test(src[wstart])) wstart--;
        const word = src.substring(wstart + 1, wend + 1);
        if (REGEX_PRECEDER_KEYWORDS.has(word)) isRegex = true;
      }
      if (isRegex) {
        i++;
        let inClass = false;
        while (i < src.length) {
          const rc = src[i];
          if (rc === "\\") { i += 2; continue; }
          if (rc === "[") { inClass = true; i++; continue; }
          if (rc === "]") { inClass = false; i++; continue; }
          if (rc === "/" && !inClass) { i++; break; }
          if (rc === "\n") break;
          i++;
        }
        while (i < src.length && /[a-zA-Z]/.test(src[i])) i++;
        setTok("/");
        continue;
      }
      setTok("/");
      i++;
      continue;
    }
    if (ch === "'") {
      i++;
      while (i < src.length && src[i] !== "'") {
        if (src[i] === "\\") i += 2; else i++;
      }
      i++;
      setTok("'");
      continue;
    }
    if (ch === '"') {
      i++;
      while (i < src.length && src[i] !== '"') {
        if (src[i] === "\\") i += 2; else i++;
      }
      i++;
      setTok('"');
      continue;
    }
    if (ch === "`") {
      i++;
      while (i < src.length && src[i] !== "`") {
        if (src[i] === "\\") { i += 2; continue; }
        if (src[i] === "$" && src[i + 1] === "{") {
          let subDepth = 1;
          i += 2;
          while (i < src.length && subDepth > 0) {
            if (src[i] === "{") subDepth++;
            else if (src[i] === "}") subDepth--;
            else if (src[i] === "'" || src[i] === '"' || src[i] === "`") {
              const q = src[i];
              i++;
              while (i < src.length && src[i] !== q) {
                if (src[i] === "\\") i += 2; else i++;
              }
            }
            i++;
          }
          continue;
        }
        i++;
      }
      i++;
      setTok("`");
      continue;
    }
    if (ch === "{") { depth++; setTok("{"); i++; continue; }
    if (ch === "}") {
      depth--;
      if (depth === 0) return i;
      setTok("}");
      i++;
      continue;
    }
    setTok(ch);
    i++;
  }
  return -1;
}

// Scan a function body for top-level identifier-call sites of the form
// `\bname\s*\(`, EXCLUDING:
//   - Member calls: `.name(`, `?.name(`
//   - Property keys: `name:` (object property name) — wouldn't end in `(`
//     but kept here for clarity
//   - String/regex/comment contents (already stripped by the caller)
// Strings/comments/regexes are pre-stripped via `findFunctionEnd`'s
// scanner — for the call-site scan, we re-walk the body with the same
// string/comment/regex skipping logic so we don't match inside literals.
//
// Calls inside nested function literals (event handlers, setTimeout
// callbacks, promise .then closures) ARE counted: deferred execution
// still represents a logical call edge that a future caller might
// resolve synchronously, and conflating them would let event-driven
// "render → handler → render" cycles slip past the ban. The fix for
// such cycles is to restructure the rendering pipeline (single state
// → render dispatcher with no back-edge), not to teach the lint to
// ignore them.
function collectCalls(body, candidateNames) {
  const out = new Set();
  const len = body.length;
  let i = 0;
  let lastTok = "(";
  while (i < len) {
    const ch = body[i];
    // Comments
    if (ch === "/" && body[i + 1] === "/") {
      i = body.indexOf("\n", i);
      if (i < 0) return out;
      i++; continue;
    }
    if (ch === "/" && body[i + 1] === "*") {
      const e = body.indexOf("*/", i + 2);
      if (e < 0) return out;
      i = e + 2; continue;
    }
    // Strings
    if (ch === "'" || ch === '"') {
      const q = ch;
      i++;
      while (i < len && body[i] !== q) {
        if (body[i] === "\\") i += 2; else i++;
      }
      i++; lastTok = q; continue;
    }
    if (ch === "`") {
      i++;
      while (i < len && body[i] !== "`") {
        if (body[i] === "\\") { i += 2; continue; }
        if (body[i] === "$" && body[i + 1] === "{") {
          let sd = 1; i += 2;
          while (i < len && sd > 0) {
            if (body[i] === "{") sd++;
            else if (body[i] === "}") sd--;
            i++;
          }
          continue;
        }
        i++;
      }
      i++; lastTok = "`"; continue;
    }
    // Regex literal — only when context permits a primary expression.
    if (ch === "/") {
      let isRegex = false;
      if (REGEX_PRECEDERS.has(lastTok)) isRegex = true;
      if (!isRegex && /[a-zA-Z_]/.test(lastTok)) {
        let wend = i - 1;
        while (wend >= 0 && /\s/.test(body[wend])) wend--;
        let wstart = wend;
        while (wstart >= 0 && /[a-zA-Z_]/.test(body[wstart])) wstart--;
        const word = body.substring(wstart + 1, wend + 1);
        if (REGEX_PRECEDER_KEYWORDS.has(word)) isRegex = true;
      }
      if (isRegex) {
        i++;
        let inClass = false;
        while (i < len) {
          const rc = body[i];
          if (rc === "\\") { i += 2; continue; }
          if (rc === "[") { inClass = true; i++; continue; }
          if (rc === "]") { inClass = false; i++; continue; }
          if (rc === "/" && !inClass) { i++; break; }
          if (rc === "\n") break;
          i++;
        }
        while (i < len && /[a-zA-Z]/.test(body[i])) i++;
        lastTok = "/"; continue;
      }
      lastTok = "/"; i++; continue;
    }
    // Identifier? Capture full identifier, decide if it's a call site.
    if (/[A-Za-z_$]/.test(ch)) {
      let s = i;
      while (i < len && /[A-Za-z0-9_$]/.test(body[i])) i++;
      const ident = body.substring(s, i);
      // Skip whitespace to look for `(`.
      let j = i;
      while (j < len && /\s/.test(body[j])) j++;
      if (body[j] === "(") {
        // Member call check: walk back from `s` past whitespace to see
        // if preceding non-whitespace char is `.` or `?.` (optional
        // chain).
        let p = s - 1;
        while (p >= 0 && /\s/.test(body[p])) p--;
        const isMember = (p >= 0 && body[p] === ".");
        if (!isMember && candidateNames.has(ident)) {
          out.add(ident);
        }
      }
      lastTok = "x";
      continue;
    }
    if (ch === "\n") lastTok = "\n";
    else if (!/\s/.test(ch)) lastTok = ch;
    i++;
  }
  return out;
}

// ─── Pass 1: collect all top-level function definitions across files.
const allFns = new Map();  // name → { file, line, body }

for (const rel of FILES) {
  const src = fs.readFileSync(rel, "utf8");
  const lines = src.split(/\r?\n/);
  const lineStarts = [0];
  for (let i = 0; i < src.length; i++) {
    if (src[i] === "\n") lineStarts.push(i + 1);
  }
  for (let i = 0; i < lines.length; i++) {
    const m = lines[i].match(funcRe);
    if (!m) continue;
    const name = m[1];
    const lineStart = lineStarts[i];
    let openIdx = -1;
    for (let j = lineStart; j < src.length; j++) {
      if (src[j] === "{") { openIdx = j; break; }
    }
    if (openIdx < 0) continue;
    const closeIdx = findFunctionEnd(src, openIdx);
    if (closeIdx < 0) continue;
    const body = src.substring(openIdx + 1, closeIdx);
    // If the same name is defined in multiple files (rare), keep them
    // separate by qualifying with the file path.
    const key = allFns.has(name) ? rel + ":" + name : name;
    allFns.set(key, { name, file: rel, line: i + 1, body });
  }
}

// Build a name-set for quick membership tests during call extraction.
// Use bare names (Map keys may include the file qualifier suffix); the
// call-graph nodes still address the qualifier-bearing keys, so we
// resolve a callee match against THE FIRST function with that bare
// name. This matches the current codebase: function names are unique
// across files except for very small helpers.
const nameSet = new Set();
const nameToKey = new Map();
for (const [key, info] of allFns) {
  if (!nameToKey.has(info.name)) {
    nameToKey.set(info.name, key);
    nameSet.add(info.name);
  }
}

// ─── Pass 2: build the call graph.
const graph = new Map();  // key → Set<key>
for (const [key, info] of allFns) {
  const callees = collectCalls(info.body, nameSet);
  const edges = new Set();
  for (const cn of callees) {
    const ckey = nameToKey.get(cn);
    if (ckey && ckey !== key) edges.add(ckey);   // do not record self via this set; self-loops handled below
    if (ckey === key) edges.add(key);             // explicit self-loop edge
  }
  graph.set(key, edges);
}

// ─── Pass 3: Tarjan strongly-connected components.
// SCC of size > 1 = mutual-recursion cycle. SCC of size 1 with a
// self-edge = direct recursion. Both are violations.
let index = 0;
const indices = new Map();
const lowlink = new Map();
const onStack = new Set();
const stack = [];
const sccs = [];

function strongConnect(v) {
  // Iterative Tarjan to avoid blowing the JS stack on deep call graphs.
  // Each frame holds the node, an iterator over its successors, and the
  // post-processing flag indicating whether we still need to update
  // lowlink from a child whose SCC walk just returned.
  const work = [{ v, iter: null, lastChild: null }];
  while (work.length > 0) {
    const top = work[work.length - 1];
    if (top.iter === null) {
      indices.set(top.v, index);
      lowlink.set(top.v, index);
      index++;
      stack.push(top.v);
      onStack.add(top.v);
      const succs = graph.get(top.v) || new Set();
      top.iter = succs[Symbol.iterator]();
    }
    // If we returned from a child, fold its lowlink.
    if (top.lastChild !== null) {
      lowlink.set(top.v, Math.min(lowlink.get(top.v), lowlink.get(top.lastChild)));
      top.lastChild = null;
    }
    let advanced = false;
    while (true) {
      const next = top.iter.next();
      if (next.done) break;
      const w = next.value;
      if (!indices.has(w)) {
        top.lastChild = w;
        work.push({ v: w, iter: null, lastChild: null });
        advanced = true;
        break;
      } else if (onStack.has(w)) {
        lowlink.set(top.v, Math.min(lowlink.get(top.v), indices.get(w)));
      }
    }
    if (advanced) continue;
    if (lowlink.get(top.v) === indices.get(top.v)) {
      const comp = [];
      while (true) {
        const w = stack.pop();
        onStack.delete(w);
        comp.push(w);
        if (w === top.v) break;
      }
      sccs.push(comp);
    }
    work.pop();
  }
}

for (const v of graph.keys()) {
  if (!indices.has(v)) strongConnect(v);
}

// ─── Pass 4: report every violating SCC.
let totalViolations = 0;
for (const comp of sccs) {
  if (comp.length === 1) {
    // Self-loop only — direct recursion.
    const v = comp[0];
    if ((graph.get(v) || new Set()).has(v)) {
      const info = allFns.get(v);
      console.error("[recursion-lint] " + info.file + ":" + info.line +
        " — direct self-recursion: " + info.name + " calls itself");
      totalViolations++;
    }
  } else {
    // Mutual cycle. Render as a → b → c → a.
    const ring = comp.map(k => allFns.get(k).name).join(" → ") + " → " + allFns.get(comp[0]).name;
    const heads = comp.map(k => {
      const i = allFns.get(k);
      return i.file + ":" + i.line;
    }).join(", ");
    console.error("[recursion-lint] indirect-recursion cycle: " + ring + "    (" + heads + ")");
    totalViolations++;
  }
}

if (totalViolations > 0) {
  console.error("[recursion-lint] FAIL: " + totalViolations +
    " recursive cycle(s) detected. Recursion (direct or indirect) is " +
    "banned — convert each cycle to an iterative state machine on the " +
    "shared driver.");
  process.exit(1);
}
process.exit(0);
