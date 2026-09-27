// A DECLARATION SWALLOWED BY A BLOCK COMMENT — the one defect in this project's JavaScript that
// `node --check` answers EXIT 0 on, in the zone that is deployed the instant a file is saved.
//
// THIS BANNER IS IN LINE COMMENTS AND THAT IS NOT A STYLE CHOICE. It has to spell the terminator that goes
// missing, and a block comment cannot hold one — the first draft of this file put the argument in a `/*` block,
// spelled the two characters in its third sentence, and terminated its own banner there. A file about a runaway
// comment is the one file that must not be able to open one, and every argument here that names the digraph
// therefore lives on a `//` line. The scanner below reads both kinds, so nothing about the check depends on it.
//
// WHAT `node --check` CAN AND CANNOT SEE, WHICH IS THE WHOLE REASON THIS FILE EXISTS. It is a PARSE, so it
// catches everything that makes a file not-JavaScript — including a block comment left unterminated AT END OF
// FILE, which is `SyntaxError: Unterminated comment` and exits 1 (MEASURED). What it cannot catch is a comment
// whose terminator is missing while a LATER comment's is not: the runaway block absorbs every declaration
// between the two and then closes, and the file that remains is perfectly valid JavaScript with less code in it.
// MEASURED on `extension/lib/safe-fetch.js` with ONE terminator deleted: `node --check` exits 0 while
// `_PINNED_MARKS` and `_pinnedOf` have ceased to exist. It also resolves no identifiers, so the
// read-with-no-writer that follows from the same edit is outside it too.
//
// WHY IT IS SEVERE HERE RATHER THAN UNTIDY. The trusted zone is INTERPRETED FROM THE TREE (CLAUDE.md
// §A-CROSS-BOUNDARY-DIFF), so a commented-out `_pinnedOf` is deployed on WRITE with no build in between — and
// what that function guards is the chokepoint's firing decision. These files are also mostly prose by design,
// because every recording rule in CLAUDE.md puts an argument at its site; so the population of edits that touch
// only comments is very large, and it is the same population as the edits that can commit this. It was
// committed TWICE in one lane's editing of one file, and `node --check` — the floor that lane and two peers
// reached for — passed both times.
//
// THE SIGNATURE, AND IT IS STRUCTURAL RATHER THAN A HEURISTIC. A runaway block ends at the next terminator, and
// to reach it the scan passes through the NEXT COMMENT'S OPENER — so the runaway block's body contains an
// opener, which nothing nests in JavaScript. Restricted to an opener at a LINE START, which is this tree's own
// convention for writing one (MEASURED at 4764ab99d45575d25ad8b77f527e8552adb7232d: 5499 block openers at a
// line start against 343 mid-line), the check answers ZERO over all 281 tracked `.js`/`.mjs` files — so a
// non-zero is a CHANGE and the change is the signal, which is the property CLAUDE.md
// §A-VERDICT-THAT-IS-RED-ON-EVERY-RUN says a gate needs to stay off the furniture pile. WITHOUT the line-start
// restriction it answers ONE, and that one is a false accusation: a URL glob inside a JSDoc paragraph in
// `extension/lib/protocol-parsers.js`, which is prose and not an opener.
//
// THE TREE ALREADY KNEW THIS BOUNDARY, IN THE OTHER DIRECTION, AND THAT IS WHY THIS GATE IS NARROW RATHER THAN
// NOVEL. `engine/host/browser/core/frame/csp_source_list.h` records that a grammar example spelling a star and
// then a slash "inside a block comment — which ENDS IT", and `testing/corpus/site.mjs` declines to write a glob
// pathspec for exactly that reason, in its own words: "a star followed by a slash ENDS A BLOCK COMMENT, so the
// obvious spelling of that filter does not compile and node --check cannot see it". Both are about a terminator
// arriving EARLY, and that direction needs no gate: comment prose becomes code, which almost never parses, so
// the floor catches it — `site.mjs` says "does not compile" and means it. The direction with no instrument is a
// terminator arriving LATE, where CODE BECOMES COMMENT and the file parses BETTER rather than worse. One
// boundary, two failures, and only one of them is loud.
//
// THE POPULATION IS A CONSTRUCT AND EXCLUDES NOTHING. `git ls-files` filtered to `.js`/`.mjs` — so no path is
// typed here, and the thing a reader would expect to see carved out is out because of what it IS rather than
// because somebody chose it: the engine's installed glue at `extension/lib/qjs/qjs.mjs` is BUILD OUTPUT and git
// does not track it (MEASURED: `git ls-files extension/lib/qjs/` is empty). Third-party page fixtures ARE in the
// population and answer clean, so there is no argument for carving them out either.
//
// Usage:  node engine/jscommentgate.mjs        (also a build stage — see engine/build.mjs)
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import vm from "node:vm";

const ENGINE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(ENGINE, "..");
const BASE_REV = "4764ab99d45575d25ad8b77f527e8552adb7232d";

// A `/` OPENS A REGEX OR DIVIDES, AND WHICH ONE DECIDES WHETHER THIS READER CAN READ THE FILE AT ALL — so the
// previous SIGNIFICANT token is tracked, which is the only part of this scanner that is not a two-character
// test. A regex literal may hold a quote, and reading one as division sends the scan into STRING mode at that
// quote and past the end of the line: MEASURED at 127 such mis-lexes over this tree's own gate scripts before
// this arm existed, against 2 after it. The other direction is free — a regex can never OPEN with an opener,
// which would be a quantifier with nothing to quantify — so no regex is ever mistaken for a comment, and the
// signature above is sound whatever this arm decides.
export const REGEX_KEYWORDS = new Set(["return", "typeof", "case", "in", "of", "new", "delete", "void", "do",
                                       "else", "yield", "await", "throw", "instanceof"]);

// THE THREE KINDS THIS RETURNS ARE NOT ONE POPULATION, and they are banded rather than summed for
// engine/idlgen.mjs's reason: a FINDING is a disagreement about the subject and a BLIND SPOT is a construct this
// run could not read, so a verdict that added them would be red about its own eyesight.
//   SWALLOWED-OPENER  a finding, and the one `node --check` cannot see.
//   UNTERMINATED      a finding, and `node --check` sees it too (it is a SyntaxError). Kept because this reader
//                     reaches it first, and naming it is cheaper than a reader wondering why a gate about
//                     comment boundaries is silent on the loudest one.
//   MIS-LEX           a blind spot: this scanner lost the token stream, so it found nothing about the region
//                     rather than nothing wrong with it.
export function scanJs(src) {
  const out = []; const n = src.length;
  let i = 0, prev = "", prevWord = "";
  const lineAt = (k) => src.slice(0, k).split("\n").length;
  const noteSig = (ch, word) => { prev = ch; prevWord = word === undefined ? "" : word; };
  while (i < n) {
    const c = src[i];
    if (c === " " || c === "\t" || c === "\r" || c === "\n") { i++; continue; }
    if (c === "/" && src[i + 1] === "/") { while (i < n && src[i] !== "\n") i++; continue; }
    if (c === "/" && src[i + 1] === "*") {
      const line = lineAt(i);
      const ls = src.lastIndexOf("\n", i - 1) + 1;
      const atLineStart = src.slice(ls, i).trim() === "";
      i += 2; const bs = i;
      while (i + 1 < n && !(src[i] === "*" && src[i + 1] === "/")) i++;
      if (i + 1 >= n) { out.push({ kind: "UNTERMINATED", line, ctx: "" }); return out; }
      const body = src.slice(bs, i); i += 2;
      if (atLineStart) {
        const m = /(?:^|\n)[ \t]*\/\*/.exec(body);
        if (m) {
          const at = m.index + m[0].length;
          out.push({ kind: "SWALLOWED-OPENER", line,
                     ctx: body.slice(Math.max(0, at - 70), at + 30).replace(/\n/g, " | ") });
        }
      }
      // A COMMENT IS NOT A SIGNIFICANT TOKEN, so `prev` is deliberately unchanged here: a regex written after a
      // comment is decided off the token BEFORE that comment, which is what the language does.
      continue;
    }
    if (c === "'" || c === '"') {
      const line = lineAt(i); const q = c; i++;
      while (i < n && src[i] !== q && src[i] !== "\n") { if (src[i] === "\\") i++; i++; }
      if (i >= n || src[i] === "\n") {
        out.push({ kind: "MIS-LEX", line, ctx: src.slice(Math.max(0, i - 60), i).replace(/\n/g, " | ") });
      }
      i++; noteSig(q); continue;
    }
    if (c === "`") {
      i++;
      // A template's hole may contain a quote, an operator or another template, so the run is walked with a
      // nesting depth rather than to the next backtick. NAMED RESIDUAL — this is not a parse. What is not
      // covered: a closing brace inside a STRING inside a hole closes the hole here, after which the scan reads
      // code as template text until it re-syncs. What the next diff builds: a hole walked by this same function
      // recursively, so a string inside a hole is a string. How its absence would show, as an OBSERVATION: a
      // MIS-LEX reported against a file whose only unusual construct is a nested template, which is what the
      // ones this run lists are. It is left narrow because a MIS-LEX is BANDED as a blind spot and never as a
      // finding, so the cost is coverage this gate states rather than an accusation it makes.
      let depth = 0;
      while (i < n) {
        if (src[i] === "\\") { i += 2; continue; }
        if (src[i] === "$" && src[i + 1] === "{") { depth++; i += 2; continue; }
        if (src[i] === "}" && depth > 0) { depth--; i++; continue; }
        if (src[i] === "`" && depth === 0) break;
        i++;
      }
      i++; noteSig("`"); continue;
    }
    if (c === "/") {
      const isRegex = prev === "" || "(,=:[!&|?{};+-*%~^<>".includes(prev) || REGEX_KEYWORDS.has(prevWord);
      if (!isRegex) { noteSig("/"); i++; continue; }
      i++; let inClass = false;
      while (i < n && src[i] !== "\n") {
        if (src[i] === "\\") { i += 2; continue; }
        if (src[i] === "[") inClass = true;
        else if (src[i] === "]") inClass = false;
        else if (src[i] === "/" && !inClass) break;
        i++;
      }
      if (i >= n || src[i] === "\n") {
        out.push({ kind: "MIS-LEX", line: lineAt(i),
                   ctx: src.slice(Math.max(0, i - 60), i).replace(/\n/g, " | ") });
      }
      i++; while (i < n && /[a-z]/.test(src[i])) i++;
      noteSig("/"); continue;
    }
    if (/[A-Za-z0-9_$]/.test(c)) {
      let j = i; while (j < n && /[A-Za-z0-9_$]/.test(src[j])) j++;
      noteSig(src[j - 1], src.slice(i, j)); i = j; continue;
    }
    noteSig(c); i++;
  }
  return out;
}

// THE POPULATION, DERIVED — see the banner for why nothing is excluded by hand.
export function trackedJs(root) {
  return execFileSync("git", ["-C", root, "ls-files"], { encoding: "utf8" })
    .split("\n").filter((p) => /\.(js|mjs)$/.test(p));
}

// THE ARMED CONTROL — CLAUDE.md §THE-ORDER-IS-FIXED-AND-IT-IS-TWO-RUNS: a control that has never produced a
// finding is not a control, and a zero read without one is a statement about the probe. The green line below is
// the one a reader will quote, so the corrupt input is DERIVED FROM A FILE IN THE POPULATION rather than
// committed as a fixture — a fixture is a copy that drifts, and a control built from the real file cannot.
//
// IT ASSERTS FOUR THINGS AND FAILS IF ANY IS ABSENT. (a) the scanner FINDS the swallowed opener; (b)
// `node --check` ACCEPTS the same bytes, which is the claim this whole file rests on and is therefore measured
// on every run rather than asserted in prose; (c) `vm.Script` AGREES with `node --check` about it, so the two
// readers cannot have been calibrated against each other's bug; (d) the PRISTINE file scans clean, so (a) is
// about the corruption and not about the file. The control source comes from the `.js` half because `vm.Script`
// parses a CLASSIC SCRIPT and every tracked `extension/*.js` is one (MEASURED: 48 of 48).
export function armControl(files, root) {
  const fail = [];
  let armed = null, attempts = 0;
  const candidates = files.filter((p) => p.endsWith(".js"))
    .map((p) => ({ p, src: readFileSync(join(root, p), "utf8") }))
    .map((e) => ({ ...e, opens: (e.src.match(/(?:^|\n)[ \t]*\/\*/g) || []).length }))
    .sort((a, b) => b.opens - a.opens || (a.p < b.p ? -1 : 1));
  const tmp = mkdtempSync(join(tmpdir(), "jscommentgate-"));
  try {
    for (const cand of candidates) {
      if (armed || fail.length) break;
      if (scanJs(cand.src).some((f) => f.kind !== "MIS-LEX")) continue;   // not a clean control source
      const re = /\*\//g; let m;
      while ((m = re.exec(cand.src)) !== null) {
        if (++attempts > 400) break;
        const corrupt = cand.src.slice(0, m.index) + cand.src.slice(m.index + 2);
        if (!scanJs(corrupt).some((f) => f.kind === "SWALLOWED-OPENER")) continue;
        const file = join(tmp, "control.js");
        writeFileSync(file, corrupt);
        let nodeOk = true, vmOk = true;
        try { execFileSync(process.execPath, ["--check", file], { stdio: "ignore" }); } catch { nodeOk = false; }
        try { new vm.Script(corrupt); } catch { vmOk = false; }
        // A corruption `node --check` REJECTS is not a counterexample to anything — it is the state that tool
        // already catches — so the search passes over it and says how many it tried.
        if (!nodeOk) continue;
        if (nodeOk !== vmOk) {
          fail.push("the two parsers DISAGREE on the corrupt control derived from " + cand.p +
                    " (node --check accepts=" + nodeOk + ", vm.Script accepts=" + vmOk + ") — one of them is " +
                    "not the reader this gate's claim is about, and a control they do not agree on calibrates " +
                    "nothing");
          break;
        }
        armed = { p: cand.p, at: cand.src.slice(0, m.index).split("\n").length };
        break;
      }
    }
  } finally { rmSync(tmp, { recursive: true, force: true }); }
  if (!armed && !fail.length)
    fail.push("no corruption of any of the " + candidates.length + " classic-script file(s) in the population " +
              "both fired this scanner and still parsed, in " + attempts + " attempt(s) — so this run has NOT " +
              "shown that the check can fail, and its green line would be a statement about the probe");
  return { armed, attempts, fail, candidates: candidates.length };
}

if (process.argv[1] && process.argv[1].endsWith("jscommentgate.mjs")) {
  const files = trackedJs(ROOT);
  const findings = [], blind = [];
  for (const rel of files) {
    let src;
    try { src = readFileSync(join(ROOT, rel), "utf8"); } catch { continue; }
    for (const f of scanJs(src)) (f.kind === "MIS-LEX" ? blind : findings).push({ rel, ...f });
  }
  const ctrl = armControl(files, ROOT);

  console.log("[jscommentgate] " + files.length + " tracked .js/.mjs file(s) — the population is " +
              "`git ls-files` filtered by extension, so no path is typed and the engine's installed glue is " +
              "out because the build writes it and git does not track it.");
  if (ctrl.armed)
    console.log("[jscommentgate] CONTROL ARMED — deleting one terminator at " + ctrl.armed.p + ":" +
                ctrl.armed.at + " makes this scanner report SWALLOWED-OPENER while BOTH `node --check` and " +
                "`vm.Script` ACCEPT the bytes, and the pristine file scans clean (" + ctrl.attempts +
                " candidate(s) tried). That is the blind spot this gate exists for, demonstrated on this run " +
                "rather than claimed.");
  console.log("[jscommentgate] BLIND SPOTS — this run found NOTHING about any of these, on the clean day too:");
  console.log("  * A COMMENTED-OUT DECLARATION WHOSE RUNAWAY BLOCK SWALLOWED NO LATER OPENER. The signature is " +
              "the runaway block reaching the next comment's opener; a file whose swallowed region contains no " +
              "further block comment closes at its own next terminator and is invisible here. Nothing measured " +
              "says how large that population is.");
  console.log("  * WHETHER AN IDENTIFIER THE CODE READS IS DECLARED ANYWHERE. This reads comment boundaries and " +
              "resolves nothing, so the read-with-no-writer half of the same defect — and the one a peer hit " +
              "reading one file's constant from another that does not load it — is outside it.");
  console.log("  * " + blind.length + " REGION(S) THIS SCANNER COULD NOT READ (MIS-LEX), in " +
              new Set(blind.map((b) => b.rel)).size + " file(s): a closing brace inside a string inside a " +
              "template hole closes the hole here. Banded as a blind spot and never as a finding — see the " +
              "residual at the template arm.");
  for (const b of blind) console.log("      " + b.rel + ":" + b.line + "  ..." + b.ctx + "...");

  if (ctrl.fail.length) {
    console.error("[jscommentgate] THE CONTROL IS NOT ARMED — this run cannot show that the check can fail, so " +
                  "its finding count is not evidence either way:");
    for (const c of ctrl.fail) console.error("  " + c);
    process.exit(1);
  }
  if (!findings.length) {
    console.log("[jscommentgate] PASS (findings) — no block comment in the " + files.length + " file(s) above " +
                "swallows a later comment's opener, and none is unterminated. That verdict is over that " +
                "population and over nothing else; the blind spots above are not part of it.");
    process.exit(0);
  }
  console.error("[jscommentgate] FINDINGS — a block comment is holding code:");
  for (const f of findings)
    console.error("  " + f.rel + ":" + f.line + "  " + f.kind +
                  (f.kind === "SWALLOWED-OPENER"
                     ? " — this block runs past a later comment's opener, so everything between them is a " +
                       "comment and `node --check` will not say so. The text it is holding: ..." + f.ctx + "..."
                     : " — a block comment is never closed. `node --check` reports this one too."));
  console.error("[jscommentgate] FAILED — " + findings.length + " finding(s). The repair is a TERMINATOR and " +
                "never a word. There is no baseline to update and no allowlist: this population answered ZERO " +
                "at " + BASE_REV + ", so a finding is a change and the change is the signal.");
  process.exit(1);
}
