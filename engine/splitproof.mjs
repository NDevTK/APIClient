/* Is a split move-only? Reassemble the file and compare bytes.
 *
 *   node engine/splitproof.mjs <path> [--base <rev>] [--head <rev>]
 *
 * A split moves a run of whole lines out of <path> into a new file beside it and leaves one line,
 * `#include "<file>"`, where the text stood. This replaces every such line with the bytes of the file it names
 * and compares the result with <path> at the base. An include line is one this range added when the file it
 * names is absent at the base and present at the head; every other include is left as written.
 * Defaults: with --head the base is that commit's first parent; without it the head is the working tree and
 * the base is HEAD.
 * Control: for each part, one byte of the part is changed and the reassembly must change with it, read by the
 * comparison that gives the answer. A part whose changed byte does not register makes the answer VOID.
 * Exit 0 byte-equal, 1 different, 2 usage or git failure, 3 the range added no include (nothing was asked),
 * 5 VOID. */
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { readFileSync, existsSync } from "node:fs";
import { dirname, join, posix } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..");
const say = (l) => console.log(`[splitproof] ${l}`);
function die(code, ...lines) { for (const l of lines) console.error(`[splitproof] ${l}`); process.exit(code); }

const USAGE = "usage: node engine/splitproof.mjs <path> [--base <rev>] [--head <rev>]";
const argv = process.argv.slice(2);
let path = null, base = null, head = null;
for (let i = 0; i < argv.length; i++) {
  if (argv[i] === "--base" || argv[i] === "--head") {
    if (i + 1 >= argv.length) die(2, `${argv[i]} needs a revision`, USAGE);
    if (argv[i] === "--base") base = argv[++i]; else head = argv[++i];
  } else if (argv[i].startsWith("--")) die(2, `unknown argument ${argv[i]}`, USAGE);
  else if (path === null) path = argv[i];
  else die(2, `one path per run; got ${path} and ${argv[i]}`, USAGE);
}
if (path === null) die(2, USAGE);
path = posix.normalize(path.split("\\").join("/"));
if (base === null) base = head === null ? "HEAD" : `${head}^`;

/* Git answers in bytes: a text decode would repair an invalid sequence on both sides and compare the repair. */
function git(args, allowFail = false) {
  try { return execFileSync("git", args, { cwd: ROOT, maxBuffer: 1 << 30, stdio: ["ignore", "pipe", "pipe"] }); }
  catch (e) {
    if (allowFail) return null;
    die(2, `git ${args.join(" ")} failed: ${String(e.stderr || e.message).split("\n")[0]}`);
  }
}
const resolveRev = (rev) => git(["rev-parse", "--verify", `${rev}^{commit}`]).toString().trim();
const baseSha = resolveRev(base);
const headSha = head === null ? null : resolveRev(head);
const headName = headSha === null ? "the working tree" : headSha.slice(0, 8);

const atBase = (p) => git(["cat-file", "-e", `${baseSha}:${p}`], true) !== null;
const readBase = (p) => git(["show", `${baseSha}:${p}`]);
function readHead(p) {
  if (headSha !== null) return git(["show", `${headSha}:${p}`], true);
  const abs = join(ROOT, p);
  return existsSync(abs) ? readFileSync(abs) : null;
}

if (!atBase(path)) die(2, `${path} does not exist at ${baseSha.slice(0, 8)}, so there is nothing to reassemble`);
const want = readBase(path);
const have = readHead(path);
if (have === null) die(2, `${path} does not exist at ${headName}`);

/* The head's lines as byte ranges, each with its terminator, so the pieces concatenate back to the file. */
const lines = [];
for (let at = 0; at < have.length;) {
  const nl = have.indexOf(0x0a, at);
  const end = nl < 0 ? have.length : nl + 1;
  lines.push(have.subarray(at, end));
  at = end;
}
const INCLUDE = /^#include "([^"\n]+)"\n?$/;
const parts = [];
lines.forEach((bytes, k) => {
  const m = INCLUDE.exec(bytes.toString("latin1"));
  if (!m) return;
  const file = posix.normalize(posix.join(posix.dirname(path), m[1]));
  if (atBase(file)) return;
  const body = readHead(file);
  if (body === null) return;      /* resolved through an include root, not beside <path>: not a part */
  parts.push({ index: k, line: k + 1, file, body });
});

const countLines = (b) => { let n = 0; for (const c of b) if (c === 0x0a) n++; return n; };
const sha256 = (b) => createHash("sha256").update(b).digest("hex");
say(`${path}: base ${baseSha.slice(0, 8)} ${countLines(want)} lines ${want.length} bytes; ` +
    `head ${headName} ${countLines(have)} lines ${have.length} bytes`);
if (!parts.length) {
  say(`NOTHING ASKED — ${headName} adds no \`#include\` of a file that is new since ${baseSha.slice(0, 8)}, ` +
      `so no split was examined (the two sides are ${want.equals(have) ? "identical" : "different"})`);
  process.exit(3);
}
for (const p of parts)
  say(`part ${p.file}: ${countLines(p.body)} lines ${p.body.length} bytes, included at line ${p.line}`);

function reassemble(changed) {
  const byIndex = new Map(parts.map((p) => [p.index, p]));
  return Buffer.concat(lines.map((bytes, k) => {
    const p = byIndex.get(k);
    if (!p) return bytes;
    if (p !== changed || !p.body.length) return p.body;
    const body = Buffer.from(p.body);
    body[body.length >> 1] ^= 1;
    return body;
  }));
}
function firstDifference(a, b) {
  const n = Math.min(a.length, b.length);
  let at = 0;
  while (at < n && a[at] === b[at]) at++;
  return at === n && a.length === b.length ? -1 : at;
}

/* The control runs before the answer is read, through the same reassembly and the same comparison. It is
   measured against the unchanged reassembly, so a file that already differs from the base cannot arm it. */
const got = reassemble(null);
const silent = parts.filter((p) => firstDifference(got, reassemble(p)) < 0);
if (silent.length) {
  for (const p of silent)
    say(`VOID — one byte of ${p.file} was changed and the reassembly did not change, so this part's bytes do ` +
        `not reach the comparison`);
  process.exit(5);
}
say(`control: one byte changed in each of ${parts.length} part(s), and each change registered as a difference`);

const at = firstDifference(want, got);
if (at >= 0) {
  const line = countLines(want.subarray(0, at)) + 1;
  const show = (b) => JSON.stringify(b.subarray(Math.max(0, at - 40), at + 40).toString("latin1"));
  say(`DIFFERENT — the reassembly is ${got.length} bytes against the base's ${want.length}, and they first ` +
      `differ at byte ${at} (base line ${line})`);
  say(`  base       ${show(want)}`);
  say(`  reassembly ${show(got)}`);
  process.exit(1);
}

/* Reported, not judged: the proof is about <path>, and a range may carry other commits. */
const range = headSha === null ? [baseSha] : [baseSha, headSha];
const mine = new Set([path, ...parts.map((p) => p.file)]);
const others = git(["diff", "--name-only", ...range]).toString().split("\n").filter((f) => f && !mine.has(f));
if (others.length) say(`outside this proof, the range also changes ${others.length} path(s): ${others.join(", ")}`);
say(`sha256 ${sha256(got)} for the reassembly and for ${baseSha.slice(0, 8)}:${path}`);
say(`PROVEN — replacing ${parts.length} added include line(s) with the named files' bytes reproduces ` +
    `${baseSha.slice(0, 8)}:${path} byte for byte`);
