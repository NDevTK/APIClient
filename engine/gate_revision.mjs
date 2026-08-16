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
import { statSync } from "node:fs";
import { dirname, join } from "node:path";

const ENGINE = import.meta.dirname;
const ROOT = dirname(ENGINE);            /* the superproject */
const QJS = join(ENGINE, "qjs");         /* the submodule */
const SUBMODULE_PATH = "engine/qjs";     /* how the superproject's tree names it */

/* THE ANSWER OR THE FAILURE, never neither. `status !== 0` is kept verbatim so it prints as itself. */
function ask(cwd, ...a) {
  const r = spawnSync("git", a, { cwd, encoding: "utf8" });
  return r.status === 0 ? r.stdout.trim() : `<git ${a.join(" ")} failed: ${(r.stderr || "").trim()}>`;
}

/* THE PORCELAIN LINES FOR ONE CONE, as a list. `--porcelain` names each path with its two status columns, and
   both columns matter: a staged edit and an unstaged one are equally not-in-HEAD, which is the only question
   this file asks. An untracked `.c` under the cone is in the cone too — the walk that builds a gate's source
   list reads the DIRECTORY, so a file no revision contains still gets compiled. */
function dirtyIn(cwd, cone) {
  /* AN EMPTY PATHSPEC IS THE WHOLE REPOSITORY, which is the widest possible wrong answer wearing the shape of
     a right one: a cone naming only the submodule would report every edit anyone made anywhere as a reason to
     distrust this gate's number. An empty cone has nothing to be dirty. */
  if (!cone.length) return [];
  const out = ask(cwd, "status", "--porcelain", "--", ...cone);
  if (out.startsWith("<git ")) return [out];
  return out.split("\n").map((l) => l.trimEnd()).filter(Boolean);
}

/* A GATE THAT DOES NOT BUILD MEASURES AN ARTIFACT, AND THE ARTIFACT'S AGE IS THEN THE REAL IDENTITY.
 * engine/solvergate.mjs runs `extension/lib/qjs/qjs.mjs` — the shipped wasm — and never compiles anything, so
 * a revision line about the TREE would be a confident false statement about the program: the tree can be six
 * commits ahead of the .wasm that answers every question in the run. That is the same defect this whole file
 * exists for, one layer over: a number attributed to a revision that never produced it. So an artifact-driven
 * gate reports which SOURCES ARE NEWER THAN THE THING IT RAN, by mtime, over the git-tracked files of its own
 * cone — tracked, because an untracked scratch file under engine/host is not what `build.mjs` compiled either.
 * The list is the work item: every one of those is a change the run did not contain. */
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
export function gateRevision(cone, artifact = null) {
  const superDirty = dirtyIn(ROOT, cone.filter((p) => p !== SUBMODULE_PATH));
  const wantsQjs = cone.includes(SUBMODULE_PATH);
  return {
    head: ask(ROOT, "rev-parse", "HEAD"),
    branch: ask(ROOT, "rev-parse", "--abbrev-ref", "HEAD"),
    /* THE COMMIT THE SUPERPROJECT RECORDS versus THE COMMIT THAT IS CHECKED OUT. Two facts, and the whole
       incident this file was written for is the gap between them. */
    qjsPinned: wantsQjs ? ask(ROOT, "rev-parse", "HEAD:" + SUBMODULE_PATH) : null,
    qjsHead: wantsQjs ? ask(QJS, "rev-parse", "HEAD") : null,
    dirty: [...superDirty, ...(wantsQjs ? dirtyIn(QJS, ["."]).map((l) => l + "   (in " + SUBMODULE_PATH + ")")
                                        : [])],
    artifact,
    stale: artifact ? stalerThan(artifact, cone) : null,
    cone,
  };
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
  if (rev.qjsHead && rev.qjsPinned && rev.qjsHead !== rev.qjsPinned)
    out.push(`[rev] THE SUBMODULE IS NOT THE ONE THIS COMMIT RECORDS — ${SUBMODULE_PATH} is checked out at ` +
             `${short(rev.qjsHead)} and ${short(rev.head)} pins ${short(rev.qjsPinned)}. The program measured ` +
             `below is NOT the program this superproject revision describes, and a fix committed against ` +
             `either half alone will read as absent in the other.`);
  if (rev.dirty.length) {
    out.push(`[rev] THE COMPILED CONE IS DIRTY — ${rev.dirty.length} path(s) below differ from that revision, ` +
             `so this run measures a tree NO REVISION CONTAINS and the number cannot be quoted against a ` +
             `commit. Run it from a frozen snapshot (CLAUDE.md §Testing: \`git worktree add --detach\` plus a ` +
             `worktree of the submodule at \`HEAD:${SUBMODULE_PATH}\`).`);
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
  if (rev.stale) {
    if (rev.stale.missing)
      out.push(`[rev] THE ARTIFACT THIS GATE RUNS IS NOT THERE — ${rev.artifact}: ${rev.stale.missing}`);
    else if (rev.stale.broke)
      out.push(`[rev] THE ARTIFACT'S AGE COULD NOT BE DECIDED — ${rev.stale.broke}`);
    else if (rev.stale.count) {
      out.push(`[rev] THE ARTIFACT IS OLDER THAN THE TREE — ${rev.artifact} was built ${rev.stale.at} and ` +
               `${rev.stale.count} tracked source(s) of this gate's cone have changed since, most recently ` +
               `${rev.stale.newest} at ${rev.stale.newest_at}. This run measured that BUILD, not the revision ` +
               `above; rebuild (\`node engine/build.mjs\`) or quote the number against the build instead.`);
    } else {
      out.push(`[rev] the artifact ${rev.artifact} (built ${rev.stale.at}) is NEWER than every tracked source ` +
               `of this gate's cone — it is a build OF the tree above`);
    }
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
  return null;
}
