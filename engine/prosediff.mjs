/* Is a diff prose-only? Ask the preprocessor (C) or the parser (JS), not the author.
 *
 *   node engine/prosediff.mjs <path.c|path.js|path.mjs>… [--base <rev>] [--release]
 *
 * C: both sides are preprocessed with clang -E against their own tree's headers (the base tree is materialised
 * from `git archive`), linemarkers and blank lines dropped, paths normalised. Two questions, both required:
 *   (1) with every integer masked, does any text differ? Nonzero is a statement, expression or datum.
 *   (2) is every numeric difference a line-stamp shift explained by a cumulative partial sum of the hunk nets?
 * JS: the non-comment @babel/parser token streams must be identical.
 * Every path carries a control (one injected statement) that must register, or its answer is VOID.
 * A header has no translation unit: pass a `.c` that includes it. The working tree is only read.
 *
 * Named residual: not covered — a `.h` path is declined rather than resolved to its includers; next diff —
 * parse includers' `#include` lines and report the union; absence shows as a header-only `--prose` claim that is
 * DECLINED. */
import { spawnSync } from "node:child_process";
import { mkdtempSync, writeFileSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, basename, dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { parse as babelParse } from "@babel/parser";

const ENGINE = dirname(fileURLToPath(import.meta.url));
const REPO = resolve(ENGINE, "..");
const die = (code, ...lines) => { for (const l of lines) process.stderr.write("[prosediff] " + l + "\n");
                                  process.exit(code); };
/* Every spawn uses a large buffer and has its `error` read: an exceeded default buffer truncates stdout
   silently, which shortens one side of the comparison and reads as a residue. */
const MAXBUF = 1 << 29;
const run = (cmd, args, opts = {}) => {
  const r = spawnSync(cmd, args, { cwd: REPO, encoding: "utf8", maxBuffer: MAXBUF, ...opts });
  if (r.error) die(4, "`" + cmd + " " + args.join(" ") + "` failed: " + r.error.message,
                     "A truncated or failed spawn silently shortens one side of the comparison, which reads as",
                     "a residue rather than as a missing operand. Nothing is published off it.");
  return r;
};

/* The include roots are derived from build.mjs's own `"-I" + …` line, resolving each name through that file's
   `const` definitions. An unparseable shape throws rather than falling back to a hand-kept list. */
function includeRoots(engineDir) {
  const src = readFileSync(join(engineDir, "build.mjs"), "utf8");
  const line = src.split("\n").find((l) => /^\s*"-I"\s*\+/.test(l));
  if (!line) die(3, "build.mjs has no `\"-I\" + …` flag line — its shape moved, so the include roots this",
                    "would preprocess with are no longer derivable. Fix THIS parser against that line; do not",
                    "paste a list here, which is the second copy the derivation exists to avoid.");
  /* Split on top-level commas only: `join(HOST, "browser")` carries a comma inside a call. */
  const exprs = [];
  { let depth = 0, cur = "";
    for (const ch of line) {
      if (ch === "(") depth++;
      else if (ch === ")") depth--;
      if (ch === "," && depth === 0) { exprs.push(cur.trim()); cur = ""; continue; }
      cur += ch;
    }
    exprs.push(cur.trim()); }
  const flags = exprs.filter((s) => s.startsWith('"-I"'));
  const defs = new Map();
  for (const m of src.matchAll(/^const\s+([A-Z_]+)\s*=\s*(.+?);\s*$/gm)) defs.set(m[1], m[2]);
  const resolveName = (id, depth = 0) => {
    if (depth > 4) die(3, "include-root name `" + id + "` resolves through more than four `const`s");
    if (id === "ENGINE") return engineDir;
    const rhs = defs.get(id);
    if (!rhs) die(3, "build.mjs defines no `const " + id + "` — the include-root derivation cannot resolve it");
    const j = /^join\(\s*([A-Za-z_]+)\s*,\s*"([^"]+)"\s*\)$/.exec(rhs);
    if (j) return join(resolveName(j[1], depth + 1), j[2]);
    const alias = /^([A-Z_]+)$/.exec(rhs);
    if (alias) return resolveName(alias[1], depth + 1);
    die(3, "cannot resolve `const " + id + " = " + rhs + "` to a directory");
  };
  const roots = [];
  for (const e of flags) {
    const tail = e.replace(/^"-I"\s*\+\s*/, "");
    const j = /^join\(\s*([A-Za-z_]+)\s*,\s*"([^"]+)"\s*\)$/.exec(tail);
    roots.push(j ? join(resolveName(j[1]), j[2]) : resolveName(tail));
  }
  if (!roots.length) die(3, "the `-I` line parsed to no roots at all");
  return roots;
}

/* Normalisation order matters: linemarkers and blank lines first (a blank line is not a numeric change), then
   paths globally. The integer mask is a second view used only by question (1). */
const stripMarkers = (s) => s.split("\n").filter((l) => !/^# [0-9]/.test(l) && l.trim() !== "").join("\n");
const normPaths = (s, paths) => { for (const p of paths) s = s.split(p).join("NORMALISED"); return s; };
const maskInts = (s) => s.replace(/[0-9]+/g, "N");

/* The files a translation unit included, read off its own linemarkers, as repo-relative paths. */
let lastIncludes = new Set();
function preprocess(file, roots, dev, paths) {
  const r = run("clang", ["-E", "-DAPICLIENT_DEV=" + dev,
                          ...roots.flatMap((x) => ["-I", x]), "-I", dirname(file), file],
                {});
  if (r.status !== 0)
    die(4, "clang -E failed on " + file + " at -DAPICLIENT_DEV=" + dev,
           (r.stderr || "").split("\n").slice(0, 6).join("\n"),
           "A residue check cannot be read off a file that does not preprocess; fix the compile first.");
  lastIncludes = new Set();
  for (const m of r.stdout.matchAll(/^# [0-9]+ "([^"]+)"/gm))
    if (m[1].startsWith(REPO + "/")) lastIncludes.add(m[1].slice(REPO.length + 1));
  return normPaths(stripMarkers(r.stdout), paths);
}

/* A line stamp below a hunk shifts by the sum of the hunk nets above it, so every cumulative partial sum is an
   admissible delta; the last one is the file's net. */
function hunkPartialSums(path, base) {
  const d = run("git", ["diff", "-U0", base, "--", path]).stdout || "";
  const sums = [];
  let add = 0, del = 0, seen = false, acc = 0;
  const flush = () => { if (seen) { acc += add - del; sums.push(acc); } add = del = 0; };
  for (const l of d.split("\n")) {
    if (l.startsWith("@@")) { flush(); seen = true; continue; }
    if (!seen) continue;
    if (l.startsWith("+") && !l.startsWith("+++")) add++;
    else if (l.startsWith("-") && !l.startsWith("---")) del++;
  }
  flush();
  return sums;
}

/* The diff is kept as `diff` commands, not two flat lists: a stamp shift is a change command whose `<` and `>`
   lines pair inside it, while an inserted statement is an append. Pairing flat lists by index mixes the two. */
function diffCommands(a, b) {
  const dir = mkdtempSync(join(tmpdir(), "prosediff-"));
  try {
    writeFileSync(join(dir, "a"), a); writeFileSync(join(dir, "b"), b);
    const d = run("diff", [join(dir, "a"), join(dir, "b")]).stdout || "";
    const cmds = [];
    let cur = null;
    for (const l of d.split("\n")) {
      const m = /^[0-9]+(?:,[0-9]+)?([acd])[0-9]+(?:,[0-9]+)?$/.exec(l);
      if (m) { cur = { op: m[1], lost: [], gained: [] }; cmds.push(cur); continue; }
      if (!cur) continue;
      if (l === "---") continue;
      if (l.startsWith("< ")) cur.lost.push(l.slice(2));
      else if (l.startsWith("> ")) cur.gained.push(l.slice(2));
      else if (l === "<") cur.lost.push("");
      else if (l === ">") cur.gained.push("");
    }
    return cmds;
  } finally { rmSync(dir, { recursive: true, force: true }); }
}
const flatten = (cmds) => ({ gained: cmds.flatMap((c) => c.gained), lost: cmds.flatMap((c) => c.lost) });

function main() {
  const argv = process.argv.slice(2);
  const bi = argv.indexOf("--base");
  const base = bi >= 0 ? argv[bi + 1] : "origin/main";
  const dev = argv.includes("--release") ? "0" : "1";
  const paths = argv.filter((a, i) => !a.startsWith("--") && argv[i - 1] !== "--base");
  if (!paths.length)
    die(2, "usage: node engine/prosediff.mjs <path.c|path.js|path.mjs>… [--base <rev>] [--release]",
           "  Answers whether a diff is PROSE-ONLY by preprocessing both sides and comparing.",
           "  A `.h` has no translation unit of its own — pass a `.c` that includes it (see the residual).",
           "  EXIT: 0 every named path cleared; 1 a FINDING (not prose-only); 5 VOID (a control did not speak);",
           "  6 NOT ASKED (every path DECLINED, so nothing was judged); 2 usage; 3 the include-root derivation;",
           "  4 a command failed.");
  const roots = includeRoots(ENGINE);
  /* The base side is preprocessed against the base tree's own headers, materialised whole, so a header-only change
     is visible from every includer. One include set for both sides would hide it. */
  const baseDir = mkdtempSync(join(tmpdir(), "prosediff-base-"));
  /* Cleanup is registered before anything is written, so every exit path, including `die`, removes it. */
  process.on("exit", () => rmSync(baseDir, { recursive: true, force: true }));
  {
    /* `--output`, not stdout: `run` decodes as UTF-8 and a tar is binary. */
    const tf = join(baseDir, "base.tar");
    const ar = run("git", ["archive", "--format=tar", "--output=" + tf, base]);
    if (ar.status !== 0)
      die(4, "`git archive " + base + "` failed, so the OLD side has no headers of its own to preprocess",
             "against. Not falling back to the current tree's roots: that is exactly the blind spot this",
             "materialisation exists to close.",
             (ar.stderr || "").split("\n").slice(0, 4).join("\n"));
    const tx = run("tar", ["-x", "-f", tf, "-C", baseDir]);
    if (tx.status !== 0)
      die(4, "extracting the base tree failed", (tx.stderr || "").split("\n").slice(0, 4).join("\n"));
  }
  const baseRoots = includeRoots(join(baseDir, "engine"));
  console.log("# include roots DERIVED from engine/build.mjs's own `-I` line, PER SIDE:");
  for (const r of roots) console.log("#   new  " + r);
  for (const r of baseRoots) console.log("#   base " + r);
  console.log("# base " + base + " (" + (run("git", ["rev-parse", "--short", base]).stdout || "?").trim() +
              ")  -DAPICLIENT_DEV=" + dev);

  let worst = 0, findings = 0, declined = 0, voided = 0, cleared = 0;
  for (const p of paths) {
        /* JavaScript: the program is its token stream, so a comment-only diff leaves the non-comment tokens identical.
       A control appends one statement and must change the stream, or the answer is not published. */
    if (/\.m?js$/.test(p)) {
      const sh = run("git", ["show", base + ":" + p]);
      if (sh.status !== 0 || !sh.stdout)
        die(4, "`git show " + base + ":" + p + "` returned nothing — the path does not exist at that revision");
      const toks = (src) => {
        const ast = babelParse(src, { sourceType: "unambiguous", errorRecovery: true, tokens: true,
                                      allowReturnOutsideFunction: true, allowAwaitOutsideFunction: true,
                                      plugins: ["importAttributes"] });
        if (ast.errors && ast.errors.length) return null;
        return ast.tokens.filter((k) => typeof k.type !== "string")
                         .map((k) => k.type.label + "\u0000" + (k.value === undefined ? "" : String(k.value)));
      };
      const cur = readFileSync(join(REPO, p), "utf8");
      const N = toks(cur), O = toks(sh.stdout), C = toks(cur + "\n;apiclient_prosediff_control_sentinel;\n");
      if (!N || !O || !C) {
        console.log("\n" + p + ": DECLINED — the parser reported errors on one side, so the token streams are not comparable.");
        declined++;
        continue;
      }
      const same = (a, b) => a.length === b.length && a.every((x, k) => x === b[k]);
      if (same(N, C)) {
        console.log("\n" + p + ": CONTROL DID NOT SPEAK — an appended statement left the token stream unchanged.");
        voided++; worst = Math.max(worst, 5);
        continue;
      }
      if (same(N, O)) {
        console.log("\n" + p + ": PROSE-ONLY — " + N.length + " non-comment tokens identical on both sides; control spoke.");
        cleared++;
        continue;
      }
      let k = 0; while (k < N.length && k < O.length && N[k] === O[k]) k++;
      console.log("\n" + p + ": NOT PROSE-ONLY — token streams differ at token " + k + " (" + O.length + " -> " + N.length +
                  "): old `" + (O[k] || "<end>").replace("\u0000", " ") + "` new `" + (N[k] || "<end>").replace("\u0000", " ") + "`");
      findings++; worst = 1;
      continue;
    }
    if (!p.endsWith(".c")) {
      /* Anything else is declined rather than attempted: clang -E on a non-C file returns text instead of refusing,
         and a verdict over it would model nothing. A decline exits nonzero, so a `--prose` claim naming it refuses. */
      const why = p.endsWith(".h")
        ? "NO TRANSLATION UNIT — a header emits nothing on its own, so this cannot answer about it. Pass a `.c`"
          + "\n  that includes it; a silence here is the instrument declining, never a prose-only verdict. See"
          + "\n  this file's named residual."
        : "NOT A C TRANSLATION UNIT — this measures what a C PREPROCESSOR emits, and `clang -E` over a non-C"
          + "\n  file does not refuse: it would return text and this would print a verdict over a pipeline that"
          + "\n  models nothing about how that file runs. Declined rather than attempted.";
      console.log("\n" + p + ": " + why);
      declined++;
      continue;
    }
    const dir = mkdtempSync(join(tmpdir(), "prosediff-src-"));
    try {
      const b = basename(p);
      const newF = join(dir, "new_" + b), oldF = join(dir, "old_" + b), ctlF = join(dir, "ctl_" + b);
      const cur = readFileSync(join(REPO, p), "utf8");
      const sh = run("git", ["show", base + ":" + p]);
      const old = sh.stdout;
      if (sh.status !== 0 || !old)
        die(4, "`git show " + base + ":" + p + "` returned nothing — the path does not exist at that revision");
      writeFileSync(newF, cur); writeFileSync(oldF, old);
      /* The control is a real statement appended to the real file, so it shares the subject's whole pipeline. */
      writeFileSync(ctlF, cur + "\nstatic int apiclient_prosediff_control_" + "sentinel = 1;\n");
      const nm = [newF, oldF, ctlF, join(REPO, p), join(baseDir, p), baseDir, REPO, b];
      const N = preprocess(newF, roots, dev, nm);
      const included = lastIncludes;
      /* The base side uses the base tree's roots (see above). */
      const O = preprocess(oldF, baseRoots, dev, nm);
      const C = preprocess(ctlF, roots, dev, nm);

      const ctl = flatten(diffCommands(maskInts(N), maskInts(C)));
      if (!ctl.gained.length) {
        console.log("\n" + p + ": CONTROL DID NOT SPEAK — an injected statement produced no masked difference,");
        console.log("  so a zero below would mean `my input never reached this check` and is not published.");
        voided++;
        worst = Math.max(worst, 5);
        continue;
      }

      const masked = flatten(diffCommands(maskInts(O), maskInts(N)));
      const rawCmds = diffCommands(O, N);
      const sums = hunkPartialSums(p, base);
      const net = sums.length ? sums[sums.length - 1] : 0;
      /* A stamp expanded inside an included header shifts by that header's own hunk sums, so each changed header
         this unit includes contributes its partial sums to the admissible set. */
      const changedHeaders = (run("git", ["diff", "--name-only", base]).stdout || "").split("\n")
        .filter((h) => h.endsWith(".h") && included.has(h));
      const headerSums = new Map(changedHeaders.map((h) => [h, hunkPartialSums(h, base)]));
      const admissible = new Set([...sums, ...[...headerSums.values()].flat()]);

      /* Numeric residue: for each changed line that differs only in its numbers, the delta of each number. */
      const deltas = new Set();
      for (const c of rawCmds) {
        if (c.op !== "c") continue;                 /* an append or a delete is not a stamp that moved */
        const n = Math.min(c.gained.length, c.lost.length);
        for (let i = 0; i < n; i++) {
          const g = c.gained[i], l = c.lost[i];
          if (maskInts(g) !== maskInts(l)) continue;
          const gn = (g.match(/[0-9]+/g) || []).map(Number), ln = (l.match(/[0-9]+/g) || []).map(Number);
          for (let k = 0; k < Math.min(gn.length, ln.length); k++)
            if (gn[k] !== ln[k]) deltas.add(gn[k] - ln[k]);
        }
      }
      const unexplained = Array.from(deltas).filter((d) => d !== 0 && !admissible.has(d));

      console.log("\n" + p);
      console.log("  PREPROCESSED LINES old=" + O.split("\n").length + " new=" + N.split("\n").length +
                  " ctl=" + C.split("\n").length + " — printed because a residue is a DIFFERENCE and a " +
                  "difference cannot say which operand is wrong; a truncated side was found this way");
      console.log("  CONTROL ARMED: an injected statement produced " + ctl.gained.length + " masked line(s)");
      console.log("  (1) NON-NUMERIC: gained " + masked.gained.length + ", lost " + masked.lost.length +
                  (masked.gained.length + masked.lost.length === 0
                    ? "  — nothing but numbers differs" : "  — A STATEMENT, EXPRESSION OR DATUM"));
      for (const l of masked.gained.slice(0, 6)) console.log("      + " + l.slice(0, 160));
      for (const l of masked.lost.slice(0, 6)) console.log("      - " + l.slice(0, 160));
      console.log("  (2) NUMERIC: delta set {" + Array.from(deltas).sort((x, y) => x - y).join(", ") +
                  "} against hunk partial sums {" + sums.join(", ") + "}, net " + net);
      for (const [h, s] of headerSums) console.log("      included changed header " + h + ": partial sums {" + s.join(", ") + "}");
      if (unexplained.length)
        console.log("      UNEXPLAINED: {" + unexplained.join(", ") + "} — no hunk boundary accounts for these,");
      if (masked.gained.length + masked.lost.length === 0 && !unexplained.length)
        { console.log("  VERDICT: PROSE-ONLY — every difference is a line stamp a hunk above it explains."); cleared++; }
      else {
        console.log("  VERDICT: NOT PROSE-ONLY.");
        findings++;
        worst = Math.max(worst, 1);
      }
    } finally { rmSync(dir, { recursive: true, force: true }); }
  }
  /* Bands are printed on every run. Exit precedence: a FINDING (about the diff), then VOID (a control did not
     speak), then NOT ASKED (every path declined — about this instrument's reach). */
  console.log("\nBANDS — printed every run, clean day and red day alike, because a precedence that hides a band" +
              " is the folded answer this file exists to refuse:");
  console.log("  cleared  " + cleared + "  prose-only: (1) at zero and (2) accounted");
  console.log("  findings " + findings + "  NOT prose-only — a statement, expression, datum or unexplained delta");
  console.log("  voided   " + voided + "  the control did not speak, so that path's answer is not published");
  console.log("  declined " + declined + "  this instrument CANNOT ANSWER about the path — a blind spot, not a finding");
  if (!findings && !voided && declined) worst = 6;
  console.log("VERDICT: exit " + worst + " — " +
              (worst === 0 ? "every named path cleared"
               : worst === 1 ? "a FINDING: at least one path is not prose-only"
               : worst === 5 ? "VOID: at least one path's control did not speak"
               : worst === 6 ? "NOT ASKED: every named path was DECLINED, so nothing was judged and nothing is cleared"
               : "see above"));
  /* The base tree is removed by the exit handler registered at its creation. */
  process.exit(worst);
}
main();
