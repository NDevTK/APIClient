/* THE REVISION A GATE'S NUMBER BELONGS TO — one place, every gate.
 *
 * CLAUDE.md §Testing states the rule and this file is the mechanism for it: "a gate runs from a FROZEN
 * SNAPSHOT … and the commit it measured is REPORTED WITH THE RESULT. A result quoted without the revision it
 * came from is not a measurement." Every gate in this tree obeyed the first half and none of them obeyed the
 * second, so every number this project has ever produced was a number about an unnamed tree.
 *
 * IT IS NOT A HYPOTHETICAL, AND THE INCIDENT IS THE REASON THIS FILE EXISTS. A whole-corpus WPT run reported
 * 1393 of 2000 aborts as one atom-census leak naming nine interned names — `type`,
 * `__fileSystemWritableSlots`, `data`, `position`, `top`, `headers`, `body`, `method`, `url` — and it was read
 * as the highest-value defect in the tree. Those nine are the exact nine that superproject 26a34533 had
 * already fixed at their three roots, and the run measured a tree from BEFORE it. Nothing in the report said
 * so. What finally said so was FORENSICS ON A DCHECK's PROSE: the pasted message read "went down with 9 ATOMS
 * still interned", which is submodule 88701bd's wording, superseded twenty-three minutes later by 2268e0f's
 * "above baseline, N of them BUILT-IN names". A gate whose revision has to be recovered by diffing the
 * spelling of an assertion is a gate that reported a number about nothing, and the cost is not the reading —
 * it is that the next reader is sent to build what is already built, which is exactly the stale-`DFAIL`
 * failure §Disposition names.
 *
 * THE SUBMODULE IS HALF THE IDENTITY AND IT IS THE HALF THAT MOVED. The engine is a superproject plus
 * `engine/qjs`, and a checkout can hold a submodule at a commit the superproject does not record — which is
 * precisely how the run above came to compile a quickjs.c older than the HEAD that "contained" the fix. So a
 * revision here is a PAIR, and the PINNED commit is reported beside the CHECKED-OUT one so a disagreement is a
 * line rather than a discovery.
 *
 * A DIRTY CONE IS NOT A FOOTNOTE, IT IS THE VERDICT ON THE MEASUREMENT. §Testing's worked example is a build
 * that read `idl_args.h` 33 seconds apart and produced a program no revision of the tree ever contained. This
 * checkout is edited by several agents continuously, so "HEAD is X" is only true of the run if nothing in the
 * cone the gate COMPILES differs from X. The cone is passed in by the gate, because the gates do not compile
 * the same sources: test262 links five quickjs files and not one host file, so a `solver/cow.c` edit does not
 * touch its program and must not be reported as if it did.
 *
 * A FAILED ASK IS KEPT AS THE IDENTITY, never replaced by a plausible value — the same rule engine/wpt.mjs
 * states for the corpus's own identity, and §Offensive-programming's rule about a consumer that defaults a
 * producer's field. A run that could not ask git and a run that asked and got an answer must not read alike.
 */
import { spawnSync } from "node:child_process";
import { statSync, readFileSync, writeFileSync, realpathSync } from "node:fs";
import { dirname, join } from "node:path";

const ENGINE = import.meta.dirname;
const ROOT = dirname(ENGINE);            /* the superproject */
const QJS = join(ENGINE, "qjs");         /* the submodule */
const SUBMODULE_PATH = "engine/qjs";     /* how the superproject's tree names it */

/* IS THIS DIRECTORY A REPOSITORY OF ITS OWN — asked once per directory per run, because a directory does not
   stop being one under a gate. `--show-toplevel` is the only question that answers it: `--is-inside-work-tree`
   says yes for every subdirectory of every repository, which is the exact confusion this guards against. Both
   sides are realpath'd because a snapshot reached through a symlink is still the same directory. */
const IS_REPO_ROOT = new Map();
function isRepoRoot(cwd) {
  if (!IS_REPO_ROOT.has(cwd)) {
    const real = (p) => { try { return realpathSync(p); } catch { return null; } };
    const r = spawnSync("git", ["rev-parse", "--show-toplevel"], { cwd, encoding: "utf8" });
    const top = r.status === 0 ? real(r.stdout.trim()) : null;
    const self = real(cwd);
    IS_REPO_ROOT.set(cwd, top !== null && self !== null && top === self);
  }
  return IS_REPO_ROOT.get(cwd);
}

/* THE ANSWER OR THE FAILURE, never neither. `status !== 0` is kept verbatim so it prints as itself.
 *
 * AND THE QUESTION MUST REACH THE REPOSITORY IT NAMES, WHICH IS A SECOND CONTRACT AND THE ONE THAT WAS BROKEN.
 * git resolves its repository by walking UPWARD from cwd, so a question put to a directory that is not itself a
 * repository is answered by whichever repository CONTAINS it — successfully, with status 0, about a different
 * program. `status !== 0` cannot see that, because there is no failure to keep: this is the "one fact answered
 * from the wrong place" defect, and it wears the shape of an answer rather than of a gap.
 * MEASURED, IN THE VERY ARRANGEMENT THIS FILE'S OWN TEXT RECOMMENDS. A snapshot made with
 * `git clone --shared <root> <dir>` has an EMPTY `engine/qjs` until the submodule is cloned too, and every ask
 * of it was then answered from the superproject: `rev-parse HEAD` returned the SUPERPROJECT's sha wearing the
 * submodule's name, so the banner printed THE SUBMODULE IS NOT THE ONE THIS COMMIT RECORDS against the real
 * pin; `status --porcelain -- .` answered clean, so the same banner called the cone "CLEAN at that revision"
 * about a directory with nothing in it; and `ls-tree -r HEAD` listed the superproject UNDER THAT PREFIX, which
 * is a gitlink rather than a tree, so it answered EMPTY and 590 `#include "quickjs.h"` lines were published as
 * includes "no commit provides" under THIS REVISION DOES NOT COMPILE. One wrong repository, three confident
 * false statements, and not one failed ask anywhere to notice. So the repository is ESTABLISHED before the
 * question is put, and a directory that is not one of its own returns the failure it always should have. */
function ask(cwd, ...a) {
  if (!isRepoRoot(cwd))
    return `<git ${a.join(" ")} failed: ${cwd} is not a git repository of its own, so git would have answered ` +
           `from whichever repository contains it, about a different tree>`;
  const r = spawnSync("git", a, { cwd, encoding: "utf8" });
  return r.status === 0 ? r.stdout.trim() : `<git ${a.join(" ")} failed: ${(r.stderr || "").trim()}>`;
}

/* DID AN ASK ANSWER. `ask` returns the failure verbatim rather than throwing, so every consumer that compares
   two of its answers must first know that they ARE answers — comparing two failure strings, or a failure
   against a sha, is how "could not ask" gets published as "differs". */
const answered = (v) => typeof v === "string" && v !== "" && !v.startsWith("<git ");

/* THE SAME CONTRACT AS `ask`, FOR A QUESTION PUT TO THE BUILD RATHER THAN TO GIT — the answer or the failure,
   never neither, and never a remembered answer standing in for a failed one. The build is asked because it is
   the only thing that knows what it hands the compiler; a parse error or a non-zero exit is returned as the
   finding it is, so a checker that cannot ask says so instead of quietly checking something else. */
function askJson(cwd, argv) {
  const r = spawnSync(process.execPath, argv, { cwd, encoding: "utf8" });
  if (r.status !== 0)
    return `<${argv.join(" ")} failed: ${((r.stderr || "") + (r.stdout || "")).trim().split("\n").slice(-3).join(" | ")}>`;
  try { return JSON.parse(r.stdout); }
  catch (e) { return `<${argv.join(" ")} did not answer JSON: ${String(e && e.message)}>`; }
}

/* THE PORCELAIN LINES FOR ONE CONE, as a list. `--porcelain` names each path with its two status columns, and
   both columns matter: a staged edit and an unstaged one are equally not-in-HEAD, which is the only question
   this file asks. An untracked `.c` under the cone is in the cone too — the walk that builds a gate's source
   list reads the DIRECTORY, so a file no revision contains still gets compiled. */
/* TWO FACTS, KEPT APART: the paths that DIFFER, and whether the question could be PUT AT ALL. This function
   used to return the failed ask as though it were a differing path, and the verdict downstream then counted it
   and announced "THE COMPILED CONE IS DIRTY — 2 path(s) below differ from that revision" about a tree it had
   never managed to inspect. That is this file's own header defeated inside its own body — "a run that could
   not ask git and a run that asked and got an answer must not read alike" — and the collapse is the worse
   direction, because the false verdict is the CONFIDENT one: it names a count, so it reads as a measurement.
   IT IS NOT HYPOTHETICAL AND IT FIRES ON THE RECIPE §Testing ASKS FOR. A frozen snapshot exported with
   `git archive` has no `.git` at all, so every ask fails and a gate run from the very snapshot the rule
   prescribes reported the snapshot as dirty — the one arrangement in which the cone is guaranteed clean. */
function dirtyIn(cwd, cone) {
  /* AN EMPTY PATHSPEC IS THE WHOLE REPOSITORY, which is the widest possible wrong answer wearing the shape of
     a right one: a cone naming only the submodule would report every edit anyone made anywhere as a reason to
     distrust this gate's number. An empty cone has nothing to be dirty. */
  if (!cone.length) return { lines: [], unasked: [] };
  const out = ask(cwd, "status", "--porcelain", "--", ...cone);
  if (out.startsWith("<git ")) return { lines: [], unasked: [out] };
  return { lines: out.split("\n").map((l) => l.trimEnd()).filter(Boolean), unasked: [] };
}

/* A REVISION THAT CANNOT COMPILE ITSELF IS NOT A MEASURABLE REVISION, and that is a question about the
 * REVISION, which is why it lives here beside the other two.
 *
 * IT HAPPENED TODAY AND IT COST EVERY AGENT THEIR BUILD. A commit published `solver/dom_cow.c` with
 * `#include "core/dom/node_heap.h"` while `node_heap.{c,h}` were still untracked — they existed only in the
 * shared working tree. `main` did not compile for anyone, and nothing said so until somebody ran a build four
 * minutes long. The cause is §Disposition's staging rule: the committing agent verified `git diff HEAD` and a
 * different agent's hunks landed in the shared worktree before its `git hash-object` two commands later, so an
 * include for a file it did not stage rode into its commit. That rule tells an agent how not to cause it; this
 * tells every gate how to SEE it, in 73 ms, without a compiler.
 *
 * §Testing already states the neighbouring half — "A TRANSLATION UNIT NO GATE COMPILES IS OUTSIDE THE GATE,
 * AND THE SHIPPED ENTRY POINT IS THE ONE THAT ROTS" — and the remedy there was that `build.mjs` compiles the
 * entry it did not link. This is that rule's dual and it is strictly cheaper: a HEADER no commit contains is
 * outside every gate too, and unlike an unbuilt entry it takes the whole program down with it rather than one
 * translation unit. Answered over the COMMITTED tree, never the working one — the working tree passes while
 * HEAD is broken, and HEAD is what another agent checks out.
 *
 * Resolution mirrors the build's own `-I` list (`engine/host`, `engine/host/browser`, `engine/qjs`) plus the
 * including file's own directory, because that is what the compiler will do. A path resolving into the
 * submodule is asked of the submodule: the superproject's tree does not list a gitlink's contents.
 *
 * ONE SHAPE ON EVERY PATH, AND THIS FUNCTION HAD THREE. The two-field split below — `dangling` for what the
 * audit FOUND, `unaudited` for the audit that could not be PUT — was introduced for the manifest ask and never
 * carried back to the git asks above it, which went on returning the pre-split BARE ARRAY. `gateRevision`
 * spreads this result into its record, and spreading an array into an object literal is silent: it contributes
 * an index key and NO `dangling` key at all, so the record came back missing the field its own reporter reads.
 * The cost was not a wrong number, it was that `revisionLines` threw `Cannot read properties of undefined` at
 * the `rev.dangling.length` line — BEFORE returning any of the lines it had already composed, including the
 * THE CONE'S STATE COULD NOT BE ASKED block written for precisely this case. So the one arrangement whose
 * diagnostic this file wrote out in full was the one arrangement in which it could not be printed: a
 * `git archive` snapshot has no `.git`, the first ask here fails, and every gate that names `engine/host` in
 * its cone died with a stack trace where its explanation should have been. A dead fallback path is worse than
 * an absent one, because the prose reads as though the case is handled. */
function auditUnputtable(why) { return { dangling: [], unaudited: [why] }; }
function danglingIncludes(rev) {
  const out = ask(ROOT, "grep", "-n", "-e", '^[[:space:]]*#[[:space:]]*include[[:space:]]*"',
                  rev, "--", "engine/host/*.c", "engine/host/*.h");
  if (out.startsWith("<git ")) return auditUnputtable(out);
  const listed = ask(ROOT, "ls-tree", "-r", "--name-only", rev, "--", "engine/host");
  if (listed.startsWith("<git ")) return auditUnputtable(listed);
  const tracked = new Set(listed.split("\n").filter(Boolean));
  /* THE SUBMODULE'S TREE IS A PRECONDITION OF THE ANSWER, NOT AN OPTIONAL ENRICHMENT, and treating it as one
     is what produced the 590-include false red above. This line used to substitute an EMPTY SET for a failed
     ask, which is the strongest possible negative claim — "the submodule provides no header at all" — derived
     from the one state in which nothing is known; 590 of this cone's includes resolve only through it. A
     failed ask is not a finding about the tree, so it leaves by the same door as the manifest's. */
  const qjsListed = ask(QJS, "ls-tree", "-r", "--name-only", "HEAD");
  if (qjsListed.startsWith("<git ")) return auditUnputtable(qjsListed);
  const inQjs = new Set(qjsListed.split("\n").filter(Boolean));
  /* NO `-h`, DELIBERATELY, AND THIS WAS WRONG ONCE. Searching a REVISION rather than the worktree, git grep
     prefixes `<rev>:<path>:<lineno>:`, and `-h` suppresses the path — which is the one field that names the
     OWNER of a bad include. With `-h` this parser matched nothing and the check passed for every tree there
     will ever be: the "diagnostic that always says yes" failure this file names two functions up, reproduced
     inside the fix for it. A report that some file somewhere includes a missing header is a search, not a
     finding, so the path is the deliverable and its absence is what makes the whole check inert. */
  /* THE ROOTS COME FROM THE BUILD, BECAUSE THE BUILD IS WHAT HANDS THEM TO THE COMPILER. This list used to be
     four paths written out here, and a second program's extra `-I` made the copy wrong the day it landed: the
     unit included a header its own compiler could see, the link succeeded, and this check announced that the
     revision "cannot be built by anyone who checks it out". That is the phantom §Testing describes — a
     confident false red costs more than a silent miss, because the next REAL dangling include arrives in a
     report already known to lie. A second copy of a build's configuration is the same defect as a second copy
     of its source list, which the paragraph above `--list-sources` in build.mjs was written about.
     PER SET, NOT A UNION, WHICH MATTERS MOST WHEN THERE IS ONE SET. The build answers a LIST of compiler
     invocations and this walks it; today that list has one entry, and the temptation is to flatten. A flat
     union answers "fine" to a unit including a header only some OTHER invocation's roots can reach — the
     diagnostic that always says yes — so the shape is kept as the build states it, and a source in two sets
     must satisfy BOTH, which is what its own two links require.
     IF THE BUILD CANNOT ANSWER, THAT IS THE FINDING. There is no fallback to a remembered list here: a stale
     list is exactly what produced the false red, and a checker that quietly reverts to one when the question
     fails is a checker whose answer nobody can interpret.
     BUT "COULD NOT ASK" IS NOT "ASKED AND FOUND A DANGLING INCLUDE", AND THIS RETURNED THE FIRST AS THE SECOND.
     The build's error string was handed back in the dangling list, so the reporter counted it as an include and
     announced THIS REVISION DOES NOT COMPILE — the exact phantom the paragraph above was written to kill,
     surviving on the one path that paragraph does not cover. Measured: a snapshot with no `engine/.work/emsdk`
     cannot run `--list-include-roots` at all, and every gate run from such a snapshot published that verdict
     over a revision whose full artifact had been built and linked an hour earlier. Absence of an answer and a
     negative answer are different facts (§Architecture), so they are returned as different fields and the
     reporter states them as different sentences. Refusing the fallback is still right; reporting the refusal
     as a compile failure was not. */
  const manifest = askJson(ROOT, ["engine/build.mjs", "--list-include-roots"]);
  if (typeof manifest === "string") return auditUnputtable(manifest);
  const bad = [];
  const setsFor = (owner) => {
    const named = manifest.filter((g) => g.sources.includes(owner));
    if (named.length) return named;
    /* A HEADER is compiled by whichever unit includes it, so it is checked against every set whose roots could
       reach it — the sets that can name it are the sets whose compilers could be looking at it. */
    /* NOT ONLY HEADERS. `engine/build.mjs` is not the whole build system — `engine/wpt.mjs` compiles
       `engine/host/wpt_runner.c` with its own flags — so a source absent from this manifest is a source whose
       roots this file does not know, NOT a source nobody builds. An earlier draft of this check said "compiled
       by NO build target, so nothing checks its includes or its syntax" about exactly that file, which is the
       same overreach it was written to remove. What can still be said soundly is the at-least-one question. */
    const reach = manifest.filter((g) => g.roots.some((r) => owner.startsWith(r + "/")));
    if (reach.length) return { any: reach };
    return null;
  };
  for (const line of out.split("\n")) {
    const m = /^([^:]+):([^:]+):(\d+):\s*#\s*include\s+"([^"]+)"/.exec(line);
    if (!m) continue;
    const [, , owner, no, inc] = m;
    const groups = setsFor(owner);
    if (!groups) {
      bad.push(`${owner}:${no} includes "${inc}", and the file sits under no include root this build declares`);
      continue;
    }
    const has = (c) => tracked.has(c) ||
                       (c.startsWith("engine/qjs/") && inQjs.has(c.slice("engine/qjs/".length)));
    /* A .c IS CHECKED AGAINST EVERY SET THAT COMPILES IT, because each of those links really does hand it that
       root list and all of them must succeed. A .h CANNOT BE — which unit includes a header is not a fact a
       grep for `#include` lines has, and requiring every set whose roots merely REACH it is provably wrong: a
       browser-only header sits under `engine/host`, which is a root of both sets, while the browser process
       never compiles a translation unit that includes it. So the header question is AT LEAST ONE, and it is an
       approximation stated as one rather than a stricter-looking answer that reports healthy files. */
    const gs = Array.isArray(groups) ? groups : groups.any;
    const resolves = (g) => [join(dirname(owner), inc), ...g.roots.map((r) => join(r, inc))].some(has);
    if (Array.isArray(groups)) {
      for (const g of gs)
        if (!resolves(g))
          bad.push(`${owner}:${no} includes "${inc}", which nothing provides on the ${g.name} include path`);
    } else if (!gs.some(resolves)) {
      bad.push(`${owner}:${no} includes "${inc}", which nothing provides on any include path that can reach it`);
    }
  }
  return { dangling: bad, unaudited: [] };
}

/* THE ARTIFACT CARRIES ITS OWN IDENTITY, BECAUSE AN MTIME IS NOT ONE — and the mtime answer was wrong in
 * exactly the mode §Testing MANDATES. `git worktree add --detach` writes every tracked file at the checkout
 * instant, so in a frozen snapshot every source is newer than any artifact built before it, whatever revision
 * built it: the first snapshot run of this mechanism reported "THE ARTIFACT IS OLDER THAN THE TREE — 600
 * tracked source(s) have changed since" about a build of that very revision, three minutes after the checkout.
 * This file already names the failure while excluding `test262/` for one instance of it — "a diagnostic that
 * always says yes is the same as one that never fires" — and the general case is the same defect, because a
 * checkout re-materializes EVERYTHING, not just the corpus.
 *
 * So `build.mjs` STAMPS the artifact with the revision it was built from, through this same file, and the
 * comparison is between REVISIONS. That is a fact rather than a heuristic and it reads identically in a fresh
 * worktree and in the shared checkout. Three answers, and each is a different statement:
 *   - stamped and EQUAL to this tree      -> the artifact is a build of the revision above.
 *   - stamped and DIFFERENT               -> named, with whether the build's revision is an ancestor of this
 *                                            one (behind by N commits, the ordinary case) or unrelated.
 *   - stamped from a DIRTY tree           -> there IS no revision that describes it, and saying so is stronger
 *                                            than any timestamp: the build is the artifact of an edit.
 * An UNSTAMPED artifact predates this mechanism, and mtime is then all there is. It is still reported, and
 * reported AS a heuristic — §Offensive-programming's rule that a run which could not ask and a run which asked
 * must not read alike applies to the answer's KIND as much as to its value. */
const STAMP_SUFFIX = ".build.json";
const stampPath = (artifact) => artifact + STAMP_SUFFIX;

/* WRITTEN BY THE BUILD, THROUGH THE ONE PLACE THAT ANSWERS "what revision is this tree". `build.mjs` does not
   re-derive the pair — it calls gateRevision() and writes what a gate would have computed, so the stamp and
   the check are the same answer by construction rather than by two authors agreeing. */
export function stampArtifact(artifact, cone) {
  const rev = gateRevision(cone);
  writeFileSync(stampPath(artifact), JSON.stringify(
    /* `unasked` IS STAMPED BESIDE `dirty` FOR THE SAME REASON THE VERDICT PRINTS THEM APART. A build whose
       tree could not be asked has an empty `dirty`, and a later reader comparing only that field would read
       the artifact as built from a clean revision — the strongest claim this file can make, derived from the
       one state in which it knows nothing. */
    { head: rev.head, branch: rev.branch, qjsHead: rev.qjsHead, qjsPinned: rev.qjsPinned,
      dirty: rev.dirty, unasked: rev.unasked, cone, at: new Date().toISOString() }, null, 1));
  return rev;
}

function readStamp(artifact) {
  try { return JSON.parse(readFileSync(stampPath(artifact), "utf8")); }
  catch { return null; }
}

/* HOW FAR BEHIND, and in which direction. `--is-ancestor` is the question that separates "this build is six
   commits old" from "this build is off a branch that is not in this history at all", and the two need
   different responses from whoever reads the line. A repository that cannot answer keeps the failure. */
function behindBy(oldSha, newSha) {
  if (!oldSha || !newSha || oldSha.startsWith("<") || newSha.startsWith("<")) return null;
  if (oldSha === newSha) return { same: true };
  /* A COMMIT THIS REPOSITORY DOES NOT HAVE IS NOT A COMMIT ON ANOTHER BRANCH, and `--is-ancestor` answers
     both with the same non-zero status. Reporting them alike is §Testing's own defect — the test262 kill that
     named "segfault/abort/timeout" through one message and distinguished none of them — so the resolvability
     is asked FIRST and separately. An unresolvable stamp means the artifact was built from a tree this
     checkout has never fetched (a stale clone, a dropped branch, a corrupt stamp), and the remedy is to fetch
     or rebuild; a resolvable non-ancestor means a real divergence, and the remedy is to find out whose. */
  if (ask(ROOT, "rev-parse", "--verify", "--quiet", oldSha + "^{commit}").startsWith("<"))
    return { unresolvable: true };
  const r = spawnSync("git", ["merge-base", "--is-ancestor", oldSha, newSha], { cwd: ROOT, encoding: "utf8" });
  if (r.error) return { unknown: String(r.error.message || r.error) };
  if (r.status !== 0) return { unrelated: true };
  return { behind: ask(ROOT, "rev-list", "--count", oldSha + ".." + newSha) };
}

/* THE MTIME FALLBACK, kept ONLY for an artifact with no stamp. Unchanged in what it computes and changed in
 * what it claims: its caller reports it as a heuristic, never as the identity. */
function stalerThan(artifact, cone) {
  let art;
  try { art = statSync(artifact).mtimeMs; }
  catch (e) { return { missing: String(e.message || e) }; }
  /* THE SAME CONE THE REVISION IS ABOUT, and it is passed in rather than guessed for the reason the revision's
     own cone is: the two answers must be about ONE set of files, or "clean at HEAD" and "newer than the build"
     are statements about two different programs printed as if they were about one. */
  const superCone = cone.filter((p) => p !== SUBMODULE_PATH);
  const asks = [[ROOT, "", superCone]];
  if (cone.includes(SUBMODULE_PATH)) asks.push([QJS, SUBMODULE_PATH + "/", ["."]]);
  /* THE COUNT IS THE VERDICT AND THE NEWEST FILE IS THE EVIDENCE — the 445 names between them are not a work
     queue, and printing them would be. This is NOT the truncation §Testing forbids: an unreadable corpus file
     is its own diagnosis, so that list IS the work, whereas every entry here has the identical single remedy
     (`node engine/build.mjs`) and the count is EXACT rather than clipped. A gate whose own report is buried
     under four hundred lines is the "one number in which encoding answers three quarters of a million
     subtests" failure wearing a different hat. */
  let count = 0, newest = null, newest_at = art, broke = null;
  for (const [cwd, prefix, paths] of asks) {
    if (!paths.length) continue;
    const listed = ask(cwd, "ls-files", "--", ...paths);
    if (listed.startsWith("<git ")) { broke = listed; continue; }
    for (const rel of listed.split("\n").filter(Boolean)) {
      /* THE CORPORA ARE NOT SOURCES. `engine/qjs/test262` is a checkout this gate's program is not built from,
         and a corpus re-materialized this morning would otherwise report every artifact in the tree as stale —
         a diagnostic that always says yes is the same as one that never fires. */
      if (rel.startsWith("test262/")) continue;
      if (!/\.(c|h|mjs|js|json)$/.test(rel)) continue;
      let m;
      try { m = statSync(join(cwd, rel)).mtimeMs; } catch { continue; }
      if (m <= art) continue;
      count++;
      if (m > newest_at) { newest_at = m; newest = prefix + rel; }
    }
  }
  return { at: new Date(art).toISOString(), count, newest,
           newest_at: newest ? new Date(newest_at).toISOString() : null, broke };
}

/* THE REVISION THIS RUN'S NUMBERS BELONG TO.
 *
 * `cone` is the set of SUPERPROJECT-RELATIVE paths the calling gate compiles or reads. Pass what the gate's
 * own source list is built from — not the whole tree, which would report another agent's popup edit as a
 * reason to distrust a JS-engine number, and not a hand-written subset of it either.
 *
 * `engine/qjs` in the cone is what asks the submodule about ITSELF: the superproject can only say "the
 * submodule differs", and which of its files differ is a question only the submodule can answer.
 *
 * `artifact` is the PREBUILT program a non-building gate runs, or null for a gate that links its own. */
/* THE RECORD'S SHAPE IS ASSERTED WHERE IT IS BUILT, WHICH IS THE ONLY PLACE THE ANSWER IS. §Offensive
   programming: a consumer never defaults a producer's field, it asserts the field's presence — and the assert
   goes at the ORIGIN, because the reader is a hundred lines away and its `.length` names the symptom rather
   than the producer. The list is exactly what `revisionLines` and `revisionMoved` read, so a field added to the
   record without being added here is a field nothing states a contract about, and a field the record stops
   carrying crashes HERE with its own name in the message instead of there with none. */
const REVISION_FIELDS = {
  head: "string", branch: "string", qjsPinned: "string?", qjsHead: "string?",
  dirty: "array", unasked: "array", dangling: "array", unaudited: "array", cone: "array",
};
function checkedRevision(rev) {
  for (const [k, kind] of Object.entries(REVISION_FIELDS)) {
    const v = rev[k];
    const ok = kind === "array" ? Array.isArray(v)
             : kind === "string?" ? (v === null || typeof v === "string")
             : typeof v === "string";
    if (!ok)
      throw new Error(`[rev] gateRevision built a record whose \`${k}\` is ${JSON.stringify(v)} and not a ` +
                      `${kind} — a producer in engine/gate_revision.mjs returned a shape this record does not ` +
                      `state. Absence is not one of this contract's values: a question that could not be put ` +
                      `is an EMPTY \`${k}\` beside a non-empty \`unasked\`/\`unaudited\`, never a missing key.`);
  }
  return rev;
}

export function gateRevision(cone, artifact = null) {
  const superDirty = dirtyIn(ROOT, cone.filter((p) => p !== SUBMODULE_PATH));
  const wantsQjs = cone.includes(SUBMODULE_PATH);
  const qjsDirty = wantsQjs ? dirtyIn(QJS, ["."]) : { lines: [], unasked: [] };
  const inQjs = (l) => l + "   (in " + SUBMODULE_PATH + ")";
  const stamp = artifact ? readStamp(artifact) : null;
  return checkedRevision({
    head: ask(ROOT, "rev-parse", "HEAD"),
    branch: ask(ROOT, "rev-parse", "--abbrev-ref", "HEAD"),
    /* THE COMMIT THE SUPERPROJECT RECORDS versus THE COMMIT THAT IS CHECKED OUT. Two facts, and the whole
       incident this file was written for is the gap between them. */
    qjsPinned: wantsQjs ? ask(ROOT, "rev-parse", "HEAD:" + SUBMODULE_PATH) : null,
    qjsHead: wantsQjs ? ask(QJS, "rev-parse", "HEAD") : null,
    dirty: [...superDirty.lines, ...qjsDirty.lines.map(inQjs)],
    /* THE QUESTION THAT COULD NOT BE PUT, carried beside the answer and never folded into it. A reader must be
       able to tell "this cone differs from its revision" from "this gate does not know whether it does", and
       only the first of those is a statement about the tree. */
    unasked: [...superDirty.unasked, ...qjsDirty.unasked.map(inQjs)],
    artifact,
    /* THE STAMP IS THE ANSWER AND THE MTIME IS THE FALLBACK — asked in that order, and only ONE of the two is
       ever carried, so a reader cannot mistake which kind of answer arrived. */
    stamp,
    stale: artifact && !stamp ? stalerThan(artifact, cone) : null,
    /* ASKED OF HEAD, not of the working tree, and asked whenever the host is in the cone — a gate that links
       only quickjs sources cannot be broken by a host include, and reporting one at it would be the same
       category error as reporting a popup edit at a JS-engine number. */
    /* TWO FIELDS, NEVER ONE: an audit that could not RUN is not an audit that found a broken include.
       See danglingIncludes — collapsing them published "THIS REVISION DOES NOT COMPILE" over revisions
       that build and link. A cone without engine/host asks neither question, so both are empty. */
    ...(cone.includes("engine/host") ? danglingIncludes("HEAD") : { dangling: [], unaudited: [] }),
    cone,
  });
}

const short = (h) => (h && !h.startsWith("<") ? h.slice(0, 8) : h);

/* THE LINES A GATE PRINTS. Every one carries the `[rev]` tag for the reason the atom census's rows carry
   theirs: a driver collecting this out of a mixed stream filters by line, so a header with indented rows under
   it is a report whose rows are invisible to whatever quotes it. Returned rather than printed so a gate can
   emit the same block at its start and again inside its summary — the tail is what gets pasted, and the tail
   is exactly where the revision was missing. */
export function revisionLines(rev) {
  const out = [];
  out.push(`[rev] engine ${short(rev.head)} (${rev.branch})` +
           (rev.qjsHead ? `   qjs ${short(rev.qjsHead)}` : ""));
  /* AN UNANSWERED ASK IS NOT A MISMATCH, and the comparison below cannot tell them apart on its own: `ask`
     returns its failure as a string, and two strings are unequal whatever they say. Without this branch a
     snapshot whose submodule is not a repository of its own printed THE SUBMODULE IS NOT THE ONE THIS COMMIT
     RECORDS — the file's loudest line — with a failure message where a sha belongs. */
  if (rev.qjsHead && !(answered(rev.qjsHead) && answered(rev.qjsPinned)))
    out.push(`[rev] THE SUBMODULE'S IDENTITY COULD NOT BE ASKED — ${SUBMODULE_PATH} is in this gate's cone and ` +
             `one of the two commits that identify it did not answer, so whether the checkout matches what ` +
             `${short(rev.head)} pins is UNKNOWN. That is not a finding that they differ and not a finding ` +
             `that they agree. Checked out: ${rev.qjsHead}. Pinned: ${rev.qjsPinned}.`);
  else if (rev.qjsHead && rev.qjsPinned && rev.qjsHead !== rev.qjsPinned)
    out.push(`[rev] THE SUBMODULE IS NOT THE ONE THIS COMMIT RECORDS — ${SUBMODULE_PATH} is checked out at ` +
             `${short(rev.qjsHead)} and ${short(rev.head)} pins ${short(rev.qjsPinned)}. The program measured ` +
             `below is NOT the program this superproject revision describes, and a fix committed against ` +
             `either half alone will read as absent in the other.`);
  /* THE ORDER IS THE POINT: a gate that could not ask has no dirty finding to report, and printing the CLEAN
     line for it would be the original defect inverted — an unasked question answered in the reassuring
     direction instead of the alarming one. Both directions are the same error, so neither branch may run. */
  if (rev.unasked.length) {
    out.push(`[rev] THE CONE'S STATE COULD NOT BE ASKED — git did not answer for ${rev.unasked.length} of the ` +
             `paths below, so this gate does not know whether the sources it compiled match the revision ` +
             `above. That is NOT a finding that they differ and NOT a finding that they agree: the number ` +
             `below is unattributable either way until the ask succeeds.`);
    for (const l of rev.unasked) out.push(`[rev]   ${l}`);
    /* THE HINT NAMES THE CAUSES IT KNOWS AND CLAIMS NEITHER, because the reason is in the failure above and a
       hint that asserts one cause reads as a diagnosis. There are two, and they need different halves of the
       same remedy: a `git archive` export carries no `.git` at all, and a `git clone --shared` snapshot carries
       one for the superproject only until the submodule is cloned too — the second half of the recipe below,
       which is the half that gets skipped. */
    out.push(`[rev] a snapshot exported with \`git archive\` carries no \`.git\` at all, and a cloned one whose ` +
             `\`${SUBMODULE_PATH}\` was never cloned carries none for the submodule — either way the arrangement ` +
             `whose cone is guaranteed clean reports as unknown. Freeze with repositories on BOTH halves — ` +
             `\`git clone --shared <root> <dir> && git -C <dir> checkout --detach <rev>\`, plus the same for ` +
             `\`${SUBMODULE_PATH}\` at \`<rev>:${SUBMODULE_PATH}\` — which leaves the gate able to answer this ` +
             `question about itself.`);
  } else if (rev.dirty.length) {
    out.push(`[rev] THE COMPILED CONE IS DIRTY — ${rev.dirty.length} path(s) below differ from that revision, ` +
             `so this run measures a tree NO REVISION CONTAINS and the number cannot be quoted against a ` +
             `commit. Run it from a frozen snapshot: \`git clone --shared <root> <dir> && git -C <dir> ` +
             `checkout --detach <rev>\`, plus the same for \`${SUBMODULE_PATH}\` at \`<rev>:${SUBMODULE_PATH}\`.`);
    for (const l of rev.dirty) out.push(`[rev]   ${l}`);
  } else {
    /* A POSITIVE STATEMENT, not an absence. "Clean" is the finding that makes the head above quotable, and a
       reader who sees nothing here cannot tell it from a gate that forgot to ask. */
    out.push(`[rev] the compiled cone (${rev.cone.join(", ")}) is CLEAN at that revision — this number is ` +
             `quotable against it`);
  }
  /* THE PROGRAM, WHEN IT IS NOT THE ONE THIS RUN LINKED. Reported after the revision and never instead of it:
     both facts are needed, because a clean tree in front of a six-commit-old artifact is the most misleading
     pair there is — every line above says "quotable" about a program that is not the one that ran. */
  /* BEFORE THE ARTIFACT, because this one says the revision named above cannot be COMPILED at all, which
     makes every statement under it about a program that cannot exist. */
  if (rev.dangling.length) {
    out.push(`[rev] THIS REVISION DOES NOT COMPILE — ${rev.dangling.length} include(s) below name a file no ` +
             `commit provides, so the tree as published cannot be built by anyone who checks it out. The usual ` +
             `cause is a commit that staged a source and not the header it added (CLAUDE.md §Disposition: the ` +
             `index is shared, so stage and commit as ONE uninterrupted operation).`);
    for (const l of rev.dangling) out.push(`[rev]   ${l}`);
  }
  /* THE OTHER HALF OF THE SAME QUESTION, AND IT MUST NOT WEAR THE VERDICT ABOVE. This says the audit could not
     be PUT, which licenses nothing about whether the revision compiles — it is the `unasked` half of `dirtyIn`
     one function over, and the sentence this file's own header states: a run that could not ask and a run that
     asked and got an answer must not read alike. It prints AFTER the verdict because it is not one. */
  if (rev.unaudited.length) {
    out.push(`[rev] THE DANGLING-INCLUDE AUDIT DID NOT RUN — the build could not be asked which include roots ` +
             `it hands the compiler, so NOTHING here says whether this revision compiles. It is not a finding ` +
             `about the tree; the reason is below and is usually a snapshot missing engine/.work/emsdk.`);
    for (const l of rev.unaudited) out.push(`[rev]   ${l}`);
  }
  if (rev.stamp) {
    const s = rev.stamp;
    if (s.unasked && s.unasked.length) {
      /* THE STAMP'S OWN UNKNOWN, ASKED FIRST for the same reason the tree's is: a build whose tree could not
         be asked stamped an EMPTY `dirty`, so every branch below would read it as the clean case and compare
         revisions that were never established. An artifact stamped before this field existed has no `unasked`
         key at all, which is the ordinary case and correctly falls through — absence here is age, not a
         negative answer, and it is the `dirty` comparison below that then carries the weaker claim. */
      out.push(`[rev] THE ARTIFACT'S TREE COULD NOT BE ASKED WHEN IT WAS BUILT — ${rev.artifact} was stamped ` +
               `${s.at} at ${short(s.head)} with ${s.unasked.length} unanswered ask(s), so whether it was ` +
               `built from a clean revision was never established and the head it names is unconfirmed.`);
      for (const l of s.unasked) out.push(`[rev]   built-with ${l}`);
    } else if (s.dirty && s.dirty.length) {
      /* THE STRONGEST OF THE THREE ANSWERS, and the only one no timestamp could ever give: the build was made
         from a tree that no revision contains, so there is nothing to compare it TO. */
      out.push(`[rev] THE ARTIFACT WAS BUILT FROM A DIRTY TREE — ${rev.artifact} was stamped ${s.at} at ` +
               `${short(s.head)} with ${s.dirty.length} path(s) of its cone modified, so no revision describes ` +
               `the program this run measured. Rebuild from a frozen snapshot before quoting this number.`);
      for (const l of s.dirty) out.push(`[rev]   built-with ${l}`);
    } else {
      const eng = behindBy(s.head, rev.head), qjs = behindBy(s.qjsHead, rev.qjsHead);
      const both = (eng && eng.same) && (!s.qjsHead || (qjs && qjs.same));
      if (both) {
        out.push(`[rev] the artifact ${rev.artifact} was BUILT FROM THIS REVISION (stamped ${s.at}) — the ` +
                 `program measured and the tree named above are the same thing`);
      } else {
        const say = (what, from, d) =>
          !d ? `${what} ${short(from)} (could not be compared)`
             : d.same ? `${what} ${short(from)} (same)`
             : d.unresolvable ? `${what} ${short(from)}, a commit THIS CHECKOUT DOES NOT HAVE — fetch it or ` +
                                `rebuild; the stamp names a tree that was never here`
             : d.unrelated ? `${what} ${short(from)}, which is NOT an ancestor of this tree's`
             : d.unknown ? `${what} ${short(from)} (${d.unknown})`
             : `${what} ${short(from)}, ${d.behind} commit(s) behind`;
        out.push(`[rev] THE ARTIFACT IS NOT A BUILD OF THIS REVISION — ${rev.artifact} was stamped ${s.at} at ` +
                 `${say("engine", s.head, eng)}` + (s.qjsHead ? `, ${say("qjs", s.qjsHead, qjs)}` : "") +
                 `. This run measured that BUILD, not the revision above; rebuild ` +
                 `(\`node engine/build.mjs\`) or quote the number against the build instead.`);
      }
    }
  } else if (rev.stale) {
    /* NO STAMP: the artifact predates the stamping, so mtime is all there is — and it is reported AS a
       heuristic, because in a frozen snapshot every file is newer than every artifact and the answer is
       meaningless there. A reader must be able to tell this line from the stamped ones above. */
    if (rev.stale.missing)
      out.push(`[rev] THE ARTIFACT THIS GATE RUNS IS NOT THERE — ${rev.artifact}: ${rev.stale.missing}`);
    else if (rev.stale.broke)
      out.push(`[rev] THE ARTIFACT'S AGE COULD NOT BE DECIDED — ${rev.stale.broke}`);
    else
      out.push(`[rev] THE ARTIFACT CARRIES NO BUILD STAMP — ${rev.artifact} was built ${rev.stale.at} by a ` +
               `build that did not record its revision, so its identity is unknown and only its TIMESTAMP can ` +
               `be compared: ${rev.stale.count} tracked source(s) of this gate's cone are newer` +
               (rev.stale.newest ? `, most recently ${rev.stale.newest} at ${rev.stale.newest_at}` : "") +
               `. In a frozen snapshot every file is newer than every artifact, so treat this as a HINT and ` +
               `rebuild (\`node engine/build.mjs\`) to get an answer.`);
  }
  return out;
}

/* DID THE TREE MOVE UNDER THE RUN. Asked at the end against the identity taken at the start, the way
   engine/wpt.mjs already asks it of the corpus: a shared checkout edited mid-run means the binary that was
   linked and the tree that is on disk are two different programs, and the second one is what a reader will
   `git show` when they go looking. Returns the description, or null when nothing moved — a POSITIVE null, read
   by the caller as "did not move", never as a hole to fill. */
export function revisionMoved(before) {
  /* NO ARTIFACT ON THE RE-ASK, and deliberately: the question here is whether the SOURCES moved, and the
     artifact cannot have been rebuilt by this run — a gate that runs a prebuilt program does not build one. */
  const now = gateRevision(before.cone);
  if (now.head !== before.head)
    return `the superproject HEAD changed under this run (${short(before.head)} → ${short(now.head)})`;
  if (now.qjsHead !== before.qjsHead)
    return `${SUBMODULE_PATH} changed under this run (${short(before.qjsHead)} → ${short(now.qjsHead)})`;
  if (now.dirty.join("\n") !== before.dirty.join("\n"))
    return "the compiled cone was edited under this run — the sources that were linked are not the sources " +
           "on disk now, so `git diff` will not show the program this number is about";
  /* ASKED SEPARATELY, because a run that could ask at the start and not at the end learned something real —
     the repository moved out from under it — and folding that into the `dirty` comparison would have made it
     look like an edit, which is a different remedy. */
  if (now.unasked.join("\n") !== before.unasked.join("\n"))
    return "git answered about the compiled cone at one end of this run and not the other, so whether the " +
           "sources moved under it is now unknowable rather than merely unknown";
  return null;
}
