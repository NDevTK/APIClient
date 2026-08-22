// Survey self-recursive function defs across our extension JS source.
// Used as the input list for the recursion lint when adding new files.
const fs = require("fs");
const FILES = [
  "extension/ast-thread.js",
  "extension/ast-worker.js",
  "extension/background.js",
  "extension/content.js",
  "extension/intercept.js",
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
// Match `function name(` and `async function name(` at line start.
const funcRe = /^(?:async\s+)?function\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(/;
let total = 0;
for (const rel of FILES) {
  if (!fs.existsSync(rel)) continue;
  const src = fs.readFileSync(rel, "utf8");
  const lines = src.split(/\r?\n/);
  const fns = [];
  for (let i = 0; i < lines.length; i++) {
    const m = lines[i].match(funcRe);
    if (m) fns.push({ name: m[1], startLine: i });
  }
  for (let i = 0; i < fns.length; i++) {
    const f = fns[i];
    const endLine = i + 1 < fns.length ? fns[i + 1].startLine : lines.length;
    const body = lines.slice(f.startLine + 1, endLine).join("\n");
    const escaped = f.name.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
    const re = new RegExp("\\b" + escaped + "\\s*\\(", "g");
    const matches = body.match(re) || [];
    if (matches.length > 0) {
      console.log(rel + ":" + (f.startLine + 1) + " " + f.name + " (" + matches.length + ")");
      total += matches.length;
    }
  }
}
console.log("---");
console.log("total self-recursive call sites:", total);
