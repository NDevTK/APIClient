/* Comment-size gate (CLAUDE.md "Comments").
 *
 * Refuses a diff that adds a comment block longer than LIMIT lines, or grows one that already is, and any added
 * comment line that cites a CLAUDE.md section by its section sign. It judges only what the diff ADDS, so standing blocks are untouched
 * until someone edits them; editing one that is over the limit is allowed as long as the edit does not make it
 * longer.
 *
 *   node engine/commentgate.mjs [--base <rev>] [--head <rev>]
 *
 * Defaults: base origin/main, head the working tree. Exit 0 pass, 1 refused, 2 usage or git failure.
 */
import { execFileSync } from "node:child_process";
import { readFileSync, existsSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { parse as babelParse } from "@babel/parser";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..");
const LIMIT = 15;
const SOURCE = /\.(c|h|js|mjs)$/;
const EXCLUDED = [/^engine\/qjs\/test262\//, /(^|\/)node_modules\//, /^engine\/\.work\//];

function die(code, ...lines) { for (const l of lines) console.error(`[commentgate] ${l}`); process.exit(code); }
function git(args) {
  try { return execFileSync("git", args, { cwd: ROOT, encoding: "utf8", maxBuffer: 1 << 30 }); }
  catch (e) { die(2, `git ${args.join(" ")} failed: ${e.message.split("\n")[0]}`); }
}

const argv = process.argv.slice(2);
const opt = (name) => { const i = argv.indexOf(name); return i >= 0 ? argv[i + 1] : null; };
for (let i = 0; i < argv.length; i++) {
  if (argv[i] === "--base" || argv[i] === "--head") { i++; continue; }
  die(2, `unknown argument ${argv[i]}`, "usage: node engine/commentgate.mjs [--base <rev>] [--head <rev>]");
}
const base = opt("--base") || "origin/main";
const head = opt("--head");
const range = head ? [base, head] : [base];

/* C/C++ comments, skipping string and character literals. Consecutive `//` lines form one block. */
function cComments(src) {
  const out = [];
  let i = 0, line = 1, prevLineEnd = -2;
  const n = src.length;
  while (i < n) {
    const c = src[i];
    if (c === "\n") { line++; i++; continue; }
    if (c === '"' || c === "'") {
      const q = c; i++;
      while (i < n && src[i] !== q && src[i] !== "\n") { if (src[i] === "\\") i++; i++; }
      i++; continue;
    }
    if (c === "/" && src[i + 1] === "*") {
      const start = line, b = i; i += 2;
      while (i < n && !(src[i] === "*" && src[i + 1] === "/")) { if (src[i] === "\n") line++; i++; }
      i += 2;
      out.push({ start, end: line, text: src.slice(b, i) });
      continue;
    }
    if (c === "/" && src[i + 1] === "/") {
      const b = i;
      while (i < n && src[i] !== "\n") i++;
      const last = out[out.length - 1];
      if (last && last.line && prevLineEnd === line - 1) { last.end = line; last.text += "\n" + src.slice(b, i); }
      else out.push({ start: line, end: line, text: src.slice(b, i), line: true });
      prevLineEnd = line;
      continue;
    }
    i++;
  }
  return out;
}

/* JS comments from the real parser; adjacent `//` lines are merged the same way. */
function jsComments(src) {
  let ast;
  try {
    ast = babelParse(src, { sourceType: "unambiguous", errorRecovery: true, allowReturnOutsideFunction: true,
                            allowAwaitOutsideFunction: true, plugins: ["importAttributes"] });
  } catch (e) { return null; }
  const out = [];
  for (const c of ast.comments) {
    const start = c.loc.start.line, end = c.loc.end.line, isLine = c.type === "CommentLine";
    const last = out[out.length - 1];
    if (isLine && last && last.line && last.end === start - 1) { last.end = end; last.text += "\n//" + c.value; }
    else out.push({ start, end, text: (isLine ? "//" : "/*") + c.value, line: isLine });
  }
  return out;
}

/* Added new-side lines, and each hunk's deletion count keyed by its new-side position. */
function hunks(path) {
  const diff = git(["diff", "-U0", "--no-color", ...range, "--", path]);
  const added = new Set(), dels = [];
  let newLine = 0;
  for (const l of diff.split("\n")) {
    const m = /^@@ -\d+(?:,(\d+))? \+(\d+)(?:,(\d+))? @@/.exec(l);
    if (m) {
      const oldCount = m[1] === undefined ? 1 : Number(m[1]);
      newLine = Number(m[2]);
      if (oldCount) dels.push({ at: newLine, count: oldCount });
      continue;
    }
    if (l.startsWith("+") && !l.startsWith("+++")) added.add(newLine++);
  }
  return { added, dels };
}

const keep = (f) => f && SOURCE.test(f) && !EXCLUDED.some((r) => r.test(f));
const files = git(["diff", "--name-only", "--diff-filter=AMR", ...range]).split("\n").filter(keep);
/* In working-tree mode an untracked file is wholly added, and `git diff` does not list it. */
const untracked = head ? new Set() : new Set(git(["ls-files", "--others", "--exclude-standard"]).split("\n").filter(keep));
for (const f of untracked) files.push(f);

const refusals = [];
let judged = 0, unparsed = [];
for (const f of files) {
  const src = head ? git(["show", `${head}:${f}`]) : (existsSync(join(ROOT, f)) ? readFileSync(join(ROOT, f), "utf8") : null);
  if (src === null) continue;
  const blocks = /\.(c|h)$/.test(f) ? cComments(src) : jsComments(src);
  if (blocks === null) { unparsed.push(f); continue; }
  const { added, dels } = untracked.has(f)
    ? { added: new Set(src.split("\n").map((_, k) => k + 1)), dels: [] }
    : hunks(f);
  judged++;
  for (const b of blocks) {
    let plus = 0;
    for (let k = b.start; k <= b.end; k++) if (added.has(k)) plus++;
    if (!plus) continue;
    const minus = dels.filter((d) => d.at >= b.start - 1 && d.at <= b.end).reduce((s, d) => s + d.count, 0);
    const len = b.end - b.start + 1;
    if (len > LIMIT && plus > minus)
      refusals.push(`${f}:${b.start}  a ${len}-line comment block was added or grown (+${plus} -${minus}); the limit is ${LIMIT}`);
    const lines = b.text.split("\n");
    lines.forEach((t, k) => {
      if (added.has(b.start + k) && t.includes("CLAUDE.md §"))
        refusals.push(`${f}:${b.start + k}  an added comment line cites \`CLAUDE.md §\`; state the rule's content instead`);
    });
  }
}

const what = head ? `${base}..${head}` : `${base}..working tree`;
console.log(`[commentgate] ${what}: ${files.length} changed source file(s), ${judged} judged`);
if (unparsed.length) {
  console.log(`[commentgate] REFUSED — could not parse ${unparsed.length} JS file(s), so their comments were not judged: ${unparsed.join(", ")}`);
  process.exit(1);
}
if (refusals.length) {
  for (const r of refusals) console.log(`[commentgate] REFUSED ${r}`);
  console.log(`[commentgate] ${refusals.length} refusal(s). Shorten the comment; put history in the commit message.`);
  process.exit(1);
}
console.log("[commentgate] PASS");
