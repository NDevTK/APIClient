// Lint rule: recursion is banned in our own code.
//
// Walks every top-level `function _name(` definition in our analyzer
// source and flags any function whose body invokes its own name. Self-
// recursion is the canonical "recursion" we want to ban — mutual
// recursion through helpers is bounded and shows up as JS-stack growth
// only when the cycle hits unbounded depth, while self-recursion grows
// the stack unconditionally with input size.
//
// Exit codes: 0 = clean (no self-recursion), 1 = at least one violation.
// Wired into the test runner so a regression causes the suite to fail.
const fs = require("fs");
const path = require("path");

// Files in scope for the recursion ban. New files are added once they
// have been audited recursion-free; converting an existing file to
// iterative is a prerequisite for adding it here.
const FILES = [
  "extension/background.js",
  "extension/lib/ast.js",
  "extension/lib/chains.js",
  "extension/lib/discovery.js",
  "extension/lib/protobuf.js",
  "extension/lib/req2proto.js",
  "extension/lib/sourcemap.js",
  "extension/lib/stats.js",
  "extension/popup.js",
  "extension/viewer.js",
];

let totalViolations = 0;
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
  // Track last meaningful character (non-whitespace, non-comment) so we
  // can decide whether `/` starts a regex literal.
  let lastTok = "{";
  function setTok(c) { lastTok = c; }
  while (i < src.length) {
    const ch = src[i];
    // Line comment
    if (ch === "/" && src[i + 1] === "/") {
      i = src.indexOf("\n", i);
      if (i < 0) return -1;
      i++;
      continue;
    }
    // Block comment
    if (ch === "/" && src[i + 1] === "*") {
      const end = src.indexOf("*/", i + 2);
      if (end < 0) return -1;
      i = end + 2;
      continue;
    }
    // Whitespace
    if (ch === " " || ch === "\t" || ch === "\r") { i++; continue; }
    if (ch === "\n") { setTok("\n"); i++; continue; }
    // Regex literal — only when context permits a primary expression
    if (ch === "/") {
      let isRegex = false;
      if (REGEX_PRECEDERS.has(lastTok)) isRegex = true;
      // Trailing keywords like `return /re/`, `typeof /re/` etc.
      if (!isRegex && /[a-zA-Z_]/.test(lastTok)) {
        // Re-extract last word to compare against keyword set.
        let wend = i - 1;
        while (wend >= 0 && /\s/.test(src[wend])) wend--;
        let wstart = wend;
        while (wstart >= 0 && /[a-zA-Z_]/.test(src[wstart])) wstart--;
        const word = src.substring(wstart + 1, wend + 1);
        if (REGEX_PRECEDER_KEYWORDS.has(word)) isRegex = true;
      }
      if (isRegex) {
        i++;
        // Scan to end of regex, handling escapes and char classes [...]
        let inClass = false;
        while (i < src.length) {
          const rc = src[i];
          if (rc === "\\") { i += 2; continue; }
          if (rc === "[") { inClass = true; i++; continue; }
          if (rc === "]") { inClass = false; i++; continue; }
          if (rc === "/" && !inClass) { i++; break; }
          if (rc === "\n") break;  // unterminated regex
          i++;
        }
        // Skip flags
        while (i < src.length && /[a-zA-Z]/.test(src[i])) i++;
        setTok("/");
        continue;
      }
      // Division operator
      setTok("/");
      i++;
      continue;
    }
    // Single-quote string
    if (ch === "'") {
      i++;
      while (i < src.length && src[i] !== "'") {
        if (src[i] === "\\") i += 2; else i++;
      }
      i++;
      setTok("'");
      continue;
    }
    // Double-quote string
    if (ch === '"') {
      i++;
      while (i < src.length && src[i] !== '"') {
        if (src[i] === "\\") i += 2; else i++;
      }
      i++;
      setTok('"');
      continue;
    }
    // Template literal — handles ${...} interpolations by tracking
    // brace depth within the interpolation.
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

for (const rel of FILES) {
  const src = fs.readFileSync(rel, "utf8");
  const lines = src.split(/\r?\n/);
  // Compute character offsets per line for converting line index → src index.
  const lineStarts = [0];
  for (let i = 0; i < src.length; i++) {
    if (src[i] === "\n") lineStarts.push(i + 1);
  }
  for (let i = 0; i < lines.length; i++) {
    const m = lines[i].match(funcRe);
    if (!m) continue;
    const name = m[1];
    // Find the opening brace of the function body — first `{` on/after
    // this line that isn't inside a string/comment. Naive scan: search
    // from the line start for `{`.
    const lineStart = lineStarts[i];
    let openIdx = -1;
    for (let j = lineStart; j < src.length; j++) {
      if (src[j] === "\n" && j > lineStart && src[j - 1] !== "\\") {
        // Function definition can wrap onto next line; keep scanning.
      }
      if (src[j] === "{") { openIdx = j; break; }
    }
    if (openIdx < 0) continue;
    const closeIdx = findFunctionEnd(src, openIdx);
    if (closeIdx < 0) continue;
    const body = src.substring(openIdx + 1, closeIdx);
    const escaped = name.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
    const re = new RegExp("\\b" + escaped + "\\s*\\(", "g");
    const matches = body.match(re) || [];
    if (matches.length > 0) {
      console.error("[recursion-lint] " + rel + ":" + (i + 1) +
        " — " + name + " calls itself " + matches.length + " time(s)");
      totalViolations += matches.length;
    }
  }
}

if (totalViolations > 0) {
  console.error("[recursion-lint] FAIL: " + totalViolations + " self-recursive call(s) detected. Recursion is banned — convert to a state machine on the shared _runResolverStack driver.");
  process.exit(1);
}
process.exit(0);
