/* The revision a gate's number belongs to: one place, every gate.
 *
 * A gate runs from a frozen snapshot and reports the commit it measured with its result; a result quoted
 * without its revision is not a measurement. This file answers what that revision is: the commit, whether the
 * cone the gate compiles is clean at it, whether the commit can compile itself, and which revision a prebuilt
 * artifact was built from.
 *
 * The revision is one commit. `engine/qjs` is ordinary tracked content of this repository, so every question
 * about it is asked from the repository root with the path as a pathspec, and `dirty` answers for it as for
 * any other cone path. `engine/qjs/test262` is still a submodule, declared at the root `.gitmodules` with
 * `update = none`; it is a corpus and not a source, no cone names it, and `git ls-files` and `git ls-tree`
 * both stop at its gitlink.
 *
 * A dirty cone is the verdict on the measurement: the checkout is edited by several agents, so "HEAD is X" is
 * true of a run only if nothing in the cone the gate compiles differs from X. The gate passes its cone in,
 * because gates do not compile the same sources (test262 links quickjs files and no host file).
 *
 * A failed ask is kept as the identity and never replaced by a plausible value: a run that could not ask git
 * and a run that asked and got an answer must not read alike. */
import { spawnSync } from "node:child_process";
import { statSync, readFileSync, writeFileSync, realpathSync } from "node:fs";
import { dirname, join } from "node:path";

const ENGINE = import.meta.dirname;
const ROOT = dirname(ENGINE);            /* the superproject */
const QJS_PATH = "engine/qjs";           /* how this repository's tree names the engine sources */

/* Whether this directory is a repository of its own, asked once per directory per run. `--show-toplevel` is
   the question that answers it; `--is-inside-work-tree` says yes for every subdirectory of every repository.
   Both sides are realpath'd because a snapshot reached through a symlink is the same directory. */
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

/* The answer or the failure, never neither; a non-zero status is returned verbatim so it prints as itself.
 * git resolves its repository by walking upward from cwd, so a question put to a directory that is not itself
 * a repository would be answered, with status 0, by whichever repository contains it. The repository is
 * therefore established before the question is put, and a directory that is not one returns a failure. */
function ask(cwd, ...a) {
  if (!isRepoRoot(cwd))
    return `<git ${a.join(" ")} failed: ${cwd} is not a git repository of its own, so git would have answered ` +
           `from whichever repository contains it, about a different tree>`;
  const r = spawnSync("git", a, { cwd, encoding: "utf8" });
  return r.status === 0 ? r.stdout.trim() : `<git ${a.join(" ")} failed: ${(r.stderr || "").trim()}>`;
}

/* Whether an ask answered. `ask` returns a failure as a string, so a consumer comparing two of its answers
   first checks that both are answers. */
const answered = (v) => typeof v === "string" && v !== "" && !v.startsWith("<git ");

/* What this tree is landing on top of: the publication tip, for a gate scoped to what a checkout introduced.
   `origin/main` is that tip only in a checkout whose origin is the real remote. `git clone` builds
   refs/remotes/origin/* from the source repository's refs/heads/*, so inside a frozen snapshot origin/main is
   the source's local branch main, which nothing advances, and a stale base makes pre-existing work read as
   introduced by this checkout. So the clone chain is walked through remote.origin.url until it reaches a
   checkout whose origin is not a local repository, and that one's origin/<branch> is the base.

   The repository that can read the object is returned with the sha: a snapshot carries no alternates file and
   cannot name a commit that landed in its source after the clone, so the base is read from `repo`. No ancestry
   is asked: the repository is shallow, so the base is a revision whose files are read by content. Nothing is
   fetched and the network is not asked, since a fetch would move a ref other lanes read and would make a
   verdict depend on the hour. A base that cannot be resolved returns a null sha beside the reason.

   Named residual: not covered — the base is the tip at run time and not at freeze time, so two runs of one
   snapshot days apart compare the same subject against different bases, and a finding can retire because
   somebody else published. Next diff — frozen_snapshot.sh resolves the base in the parent at clone time,
   brings the object into the clone as a ref of its own, and records it where this resolver prefers it over the
   live ask. Absence shows as — two runs of one snapshot printing the same subject line and different resolved
   base shas. */
function remoteOriginUrl(cwd) {
  const r = spawnSync("git", ["config", "--get", "remote.origin.url"], { cwd, encoding: "utf8" });
  return r.status === 0 ? r.stdout.trim() : "";
}
/* The source repository this checkout was cloned from, or null when origin is a real remote. A url carrying a
   scheme or an scp-like host is a remote and is not walked; file:// is the one scheme that names a path, so
   it is stripped. A path that exists and is not a repository root is also null, for `ask`'s reason. */
function localCloneSource(cwd) {
  let url = remoteOriginUrl(cwd);
  if (!url) return null;
  if (url.startsWith("file://")) url = url.slice("file://".length);
  else if (/^[a-z][a-z0-9+.-]*:\/\//i.test(url) || /^[^/\\]+@[^/\\]+:/.test(url)) return null;
  let p;
  try { p = realpathSync(url.startsWith("/") ? url : join(cwd, url)); } catch { return null; }
  return isRepoRoot(p) ? p : null;
}
export function publicationBase(treeRoot = ROOT, branch = "main") {
  const hops = [], seen = new Set();
  let repo = treeRoot;
  for (;;) {
    let real; try { real = realpathSync(repo); } catch { real = repo; }
    if (seen.has(real))
      return { sha: null, repo: null, hops, branch,
               why: `<the clone chain from ${treeRoot} revisits ${real}, so no checkout in it owns a ` +
                    `publication tip and the base cannot be recovered from inside>` };
    seen.add(real);
    if (!isRepoRoot(repo))
      return { sha: null, repo: null, hops, branch,
               why: `<${repo} is not a git repository of its own, so origin/${branch} there would be ` +
                    `answered by whichever repository contains it, about a different tree>` };
    const src = localCloneSource(repo);
    if (!src) break;
    hops.push(src);
    repo = src;
  }
  const sha = ask(repo, "rev-parse", "--verify", `refs/remotes/origin/${branch}^{commit}`);
  if (!answered(sha)) return { sha: null, repo, hops, branch, why: sha };
  return { sha, repo, hops, branch, why: null };
}

/* `ask`'s contract for a question put to the build: the answer or the failure, never neither. A parse error
   or a non-zero exit is returned as the finding it is. */
function askJson(cwd, argv) {
  const r = spawnSync(process.execPath, argv, { cwd, encoding: "utf8" });
  if (r.status !== 0)
    return `<${argv.join(" ")} failed: ${((r.stderr || "") + (r.stdout || "")).trim().split("\n").slice(-3).join(" | ")}>`;
  try { return JSON.parse(r.stdout); }
  catch (e) { return `<${argv.join(" ")} did not answer JSON: ${String(e && e.message)}>`; }
}

/* The porcelain lines for one cone, and whether the question could be put at all, as two fields: a failed
   ask goes in `unasked` and never in `lines`, so "could not ask" is not counted as a differing path. Both
   status columns matter, since a staged edit and an unstaged one are equally not in HEAD, and an untracked
   `.c` under the cone is in the cone too: the walk that builds a gate's source list reads the directory. */
function dirtyIn(cwd, cone) {
  /* An empty pathspec is the whole repository; an empty cone has nothing to be dirty. */
  if (!cone.length) return { lines: [], unasked: [] };
  const out = ask(cwd, "status", "--porcelain", "--", ...cone);
  if (out.startsWith("<git ")) return { lines: [], unasked: [out] };
  return { lines: out.split("\n").map((l) => l.trimEnd()).filter(Boolean), unasked: [] };
}

/* Whether a revision can compile itself: every `#include "…"` under engine/host must name a file the commit
 * tracks. It is answered over the committed tree and never the working one, because the working tree passes
 * while HEAD is broken and HEAD is what another agent checks out. One `ls-tree` at the revision over
 * `engine/host` and `engine/qjs` lists what the commit provides (`ls-tree -r` stops at the
 * `engine/qjs/test262` gitlink). Resolution mirrors the compiler: the build's own `-I` roots plus the
 * including file's directory.
 *
 * Every path returns one shape: `dangling` for what the audit found and `unaudited` for an audit that could
 * not be put. `gateRevision` spreads the result into its record, so a bare array would carry neither key. */
function auditUnputtable(why) { return { dangling: [], unaudited: [why] }; }
function danglingIncludes(rev) {
  const out = ask(ROOT, "grep", "-n", "-e", '^[[:space:]]*#[[:space:]]*include[[:space:]]*"',
                  rev, "--", "engine/host/*.c", "engine/host/*.h");
  if (out.startsWith("<git ")) return auditUnputtable(out);
  /* Both paths in one ask, at the revision. The engine's tree is a precondition of the answer, so a failure
     here leaves through `auditUnputtable`; an empty set would claim the engine provides no header. */
  const listed = ask(ROOT, "ls-tree", "-r", "--name-only", rev, "--", "engine/host", QJS_PATH);
  if (listed.startsWith("<git ")) return auditUnputtable(listed);
  const tracked = new Set(listed.split("\n").filter(Boolean));
  /* No `-h`: searching a revision, git grep prefixes `<rev>:<path>:<lineno>:`, and the path names the owner
     of a bad include. Without it the parser below matches nothing and the check passes for every tree. */
  /* The roots come from the build, which is what hands them to the compiler; a second copy here would go
     wrong the day a program gains an `-I`. They are kept per set, as the build states them, and not
     flattened: a union would pass a unit including a header only another invocation's roots can reach. If
     the build cannot answer, that is returned as `unaudited`; there is no fallback list, and "could not ask"
     is never reported as a dangling include. */
  const manifest = askJson(ROOT, ["engine/build.mjs", "--list-include-roots"]);
  if (typeof manifest === "string") return auditUnputtable(manifest);
  const bad = [];
  const setsFor = (owner) => {
    const named = manifest.filter((g) => g.sources.includes(owner));
    if (named.length) return named;
    /* A header is compiled by whichever unit includes it, so it is checked against every set whose roots
       could reach it. So is a source this manifest does not list: `engine/build.mjs` is not the whole build
       system (`engine/wpt.mjs` compiles `engine/host/wpt_runner.c` with its own flags), so absence from the
       manifest is not "nobody builds it". */
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
    const has = (c) => tracked.has(c);
    /* A .c is checked against every set that compiles it, since each of those links hands it that root list.
       A header cannot be: a grep for `#include` lines does not know which unit includes it, and requiring
       every set whose roots reach it would fail a browser-only header under `engine/host`. So the header
       question is at-least-one, an approximation stated as one. */
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

/* The artifact carries its own identity, because an mtime is not one: `git worktree add --detach` writes
 * every tracked file at the checkout instant, so in a frozen snapshot every source is newer than any artifact.
 * `build.mjs` stamps the artifact with the revision it was built from, through this file, and the comparison
 * is between revisions. Three answers, each a different statement:
 *   - stamped and equal to this tree      -> the artifact is a build of the revision above.
 *   - stamped and different               -> named, with whether the build's revision is an ancestor of this
 *                                            one (behind by N commits) or unrelated.
 *   - stamped from a dirty tree           -> no revision describes it.
 * An unstamped artifact predates the stamp; its mtime is then reported, as a heuristic. */
const STAMP_SUFFIX = ".build.json";
const stampPath = (artifact) => artifact + STAMP_SUFFIX;

/* Written by the build through `gateRevision`, so the stamp and a gate's check are the same answer.
   `assertRegime` is part of the identity: `-DAPICLIENT_DEV=0` compiles every DCHECK and DFAIL out, so a
   release artifact's absent abort confirms nothing, and a reader must be able to tell which it holds. It is
   required at the call site and never defaulted. It is a fact about the artifact and not the run: one
   `node engine/build.mjs release` links a DEV=0 wasm and compiles its native verdict host at DEV=1, so the
   caller reads the word from the flag list that compiled this artifact. A stamp written before the field
   carries none, and testing/artifact_stamp.js takes the `release` arm for that absence.
   `environment` is the artifact's answers to the questions ECMAScript and ECMA-402 leave to the
   implementation: the default locale and system time zone it pins, and the Intl host whose data answers the
   rest, as build.mjs's intlEnvironment reads them. */
export function stampArtifact(artifact, cone, assertRegime, environment) {
  if (!environment || typeof environment.systemTimeZone !== "string" ||
      typeof environment.defaultLocale !== "string" || !environment.intlHost ||
      typeof environment.intlHost.host !== "string")
    throw new Error(`[rev] stampArtifact was asked to stamp ${artifact} with the environment ` +
                    `${JSON.stringify(environment)}, which names no default locale, system time zone or Intl host`);
  if (assertRegime !== "dev" && assertRegime !== "release")
    throw new Error(`[rev] stampArtifact was asked to stamp ${artifact} with the assertion regime ` +
                    `${JSON.stringify(assertRegime)}. It is check.h's own two words and nothing else: "dev" ` +
                    `(-DAPICLIENT_DEV=1, every DCHECK and DFAIL compiled in) or "release" ` +
                    `(-DAPICLIENT_DEV=0, every one of them compiled out). A stamp that cannot say which one ` +
                    `describes an artifact whose silence means nothing, and every driver reading it would ` +
                    `file an absent abort as a scored absence.`);
  const rev = gateRevision(cone);
  /* Named residual: not covered — the stamp names a revision and not the bytes. It is a separate file from
     the artifact, so a copy, install or partial write can leave one newer than the other, and no consumer
     can establish that these bytes are what that tree produced; testing/artifact_stamp.js's mtime comparison
     answers only "the stamp is not older than the artifact". Next diff — a digest of the artifact's own bytes
     in this record, after deciding what the artifact is: the wasm is a pair (`qjs.mjs` and `qjs.wasm`) while
     the native link is one file, and the mtime comparison is deleted in the same diff. Absence shows as — a
     driver reporting a revision for bytes some other build produced: a stamp and an artifact copied or
     installed apart, every field of the record true and the pair wrong. */
  writeFileSync(stampPath(artifact), JSON.stringify(
    /* `unasked` is stamped beside `dirty`: a build whose tree could not be asked has an empty `dirty`, which
       alone would read as a clean revision. */
    /* A stamp written while `engine/qjs` was a submodule carries `qjsHead`; readers treat its presence as
       dating the artifact and compare nothing with it. */
    { head: rev.head, branch: rev.branch, assertRegime, environment,
      dirty: rev.dirty, unasked: rev.unasked, cone, at: new Date().toISOString() }, null, 1));
  return rev;
}

/* The stamp beside `artifact`, or null where there is none to read. */
export function readStamp(artifact) {
  try { return JSON.parse(readFileSync(stampPath(artifact), "utf8")); }
  catch { return null; }
}

/* How far behind, and in which direction: `--is-ancestor` separates a build some commits old from a build
   off a branch that is not in this history. A repository that cannot answer keeps the failure. */
function behindBy(oldSha, newSha) {
  if (!oldSha || !newSha || oldSha.startsWith("<") || newSha.startsWith("<")) return null;
  if (oldSha === newSha) return { same: true };
  /* A commit this repository does not have and a commit on another branch both fail `--is-ancestor`, so
     resolvability is asked first. An unresolvable stamp was built from a tree this checkout never fetched. */
  if (ask(ROOT, "rev-parse", "--verify", "--quiet", oldSha + "^{commit}").startsWith("<"))
    return { unresolvable: true };
  const r = spawnSync("git", ["merge-base", "--is-ancestor", oldSha, newSha], { cwd: ROOT, encoding: "utf8" });
  if (r.error) return { unknown: String(r.error.message || r.error) };
  if (r.status !== 0) return { unrelated: true };
  /* `--first-parent` counts this repository's own line. The subtree merge made the absorbed quickjs history
     reachable, which `rev-list --count A..B` would otherwise count; a stamp's `head` is on the first-parent
     line because `build.mjs` reads it from ROOT. */
  return { behind: ask(ROOT, "rev-list", "--count", "--first-parent", oldSha + ".." + newSha) };
}

/* The mtime fallback, for an artifact with no stamp. Its caller reports it as a heuristic. */
function stalerThan(artifact, cone) {
  let art;
  try { art = statSync(artifact).mtimeMs; }
  catch (e) { return { missing: String(e.message || e) }; }
  /* The same cone the revision is about, so "clean at HEAD" and "newer than the build" are about one set of
     files. */
  const asks = [[ROOT, "", cone]];
  /* The count is the verdict and the newest file is the evidence; the names are not printed, since every
     entry has the same remedy (`node engine/build.mjs`) and the count is exact. */
  let count = 0, newest = null, newest_at = art, broke = null;
  for (const [cwd, prefix, paths] of asks) {
    if (!paths.length) continue;
    const listed = ask(cwd, "ls-files", "--", ...paths);
    if (listed.startsWith("<git ")) { broke = listed; continue; }
    for (const rel of listed.split("\n").filter(Boolean)) {
      /* `git ls-files` stops at the `engine/qjs/test262` gitlink, so the corpus is not walked; the gitlink
         entry has no extension and the filter below drops it. */
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

/* The record's shape, asserted where it is built. The list is what `revisionLines` and `revisionMoved` read,
   so a field the record stops carrying fails here with its name. The record carries one commit and no
   separate engine sha: a tree hash in a field readers spell as a commit would fail `merge-base` as if the
   commit did not exist. */
const REVISION_FIELDS = {
  head: "string", branch: "string",
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

/* The revision this run's numbers belong to. `cone` is the set of repository-relative paths the calling gate
   compiles or reads: what its own source list is built from, not the whole tree and not a hand-written
   subset. `artifact` is the prebuilt program a non-building gate runs, or null for a gate that links its
   own. */
export function gateRevision(cone, artifact = null) {
  /* One `git status` over the whole cone. A drifted `engine/qjs/test262` gitlink shows as one line, which is
     a real difference from what the revision records and needs no predicate of its own. */
  const dirty = dirtyIn(ROOT, cone);
  const stamp = artifact ? readStamp(artifact) : null;
  return checkedRevision({
    head: ask(ROOT, "rev-parse", "HEAD"),
    branch: ask(ROOT, "rev-parse", "--abbrev-ref", "HEAD"),
    dirty: dirty.lines,
    /* The question that could not be put, carried beside the answer: "this cone differs from its revision"
       and "this gate does not know" are different statements. */
    unasked: dirty.unasked,
    artifact,
    /* The stamp is the answer and the mtime is the fallback; only one of the two is carried. */
    stamp,
    stale: artifact && !stamp ? stalerThan(artifact, cone) : null,
    /* Asked of HEAD whenever the host is in the cone; a gate that links only quickjs sources cannot be broken
       by a host include. Two fields: an audit that could not run is not one that found a broken include. */
    ...(cone.includes("engine/host") ? danglingIncludes("HEAD") : { dangling: [], unaudited: [] }),
    cone,
  });
}

const short = (h) => (h && !h.startsWith("<") ? h.slice(0, 8) : h);

/* The lines a gate prints. Each carries the `[rev]` tag, because a driver collecting them out of a mixed
   stream filters by line. They are returned and not printed, so a gate can emit the block at its start and
   again in its summary, which is the part that gets pasted. */
export function revisionLines(rev) {
  const out = [];
  /* One commit names the whole program. */
  out.push(`[rev] engine ${short(rev.head)} (${rev.branch})`);
  /* Order matters: a gate that could not ask has neither a dirty finding nor a clean one to report. */
  if (rev.unasked.length) {
    out.push(`[rev] THE CONE'S STATE COULD NOT BE ASKED — git did not answer for ${rev.unasked.length} of the ` +
             `paths below, so this gate does not know whether the sources it compiled match the revision ` +
             `above. That is NOT a finding that they differ and NOT a finding that they agree: the number ` +
             `below is unattributable either way until the ask succeeds.`);
    for (const l of rev.unasked) out.push(`[rev]   ${l}`);
    /* The hint names the cause it knows without asserting it; the failure itself is printed above. */
    out.push(`[rev] a snapshot exported with \`git archive\` carries no \`.git\` at all, so the one arrangement ` +
             `whose cone is guaranteed clean reports as unknown. Freeze with a repository — ` +
             `\`git clone --shared <root> <dir> && git -C <dir> checkout --detach <rev>\`, which populates ` +
             `\`${QJS_PATH}\` with everything else because it is tracked content of this repository — or run ` +
             `\`engine/frozen_snapshot.sh\`, which does that and provisions the toolchain beside it.`);
  } else if (rev.dirty.length) {
    out.push(`[rev] THE COMPILED CONE IS DIRTY — ${rev.dirty.length} path(s) below differ from that revision, ` +
             `so this run measures a tree NO REVISION CONTAINS and the number cannot be quoted against a ` +
             `commit. Run it from a frozen snapshot: \`git clone --shared <root> <dir> && git -C <dir> ` +
             `checkout --detach <rev>\`, or \`engine/frozen_snapshot.sh\`.`);
    for (const l of rev.dirty) out.push(`[rev]   ${l}`);
  } else {
    /* A positive statement: "clean" is the finding that makes the head above quotable. */
    out.push(`[rev] the compiled cone (${rev.cone.join(", ")}) is CLEAN at that revision — this number is ` +
             `quotable against it`);
  }
  /* Before the artifact: this says the revision named above cannot be compiled at all. */
  if (rev.dangling.length) {
    out.push(`[rev] THIS REVISION DOES NOT COMPILE — ${rev.dangling.length} include(s) below name a file no ` +
             `commit provides, so the tree as published cannot be built by anyone who checks it out. The usual ` +
             `cause is a commit that staged a source and not the header it added (CLAUDE.md §Disposition: the ` +
             `index is shared, so stage and commit as ONE uninterrupted operation).`);
    for (const l of rev.dangling) out.push(`[rev]   ${l}`);
  }
  /* The audit could not be put, which says nothing about whether the revision compiles. It prints after the
     verdict because it is not one. */
  if (rev.unaudited.length) {
    out.push(`[rev] THE DANGLING-INCLUDE AUDIT DID NOT RUN — the build could not be asked which include roots ` +
             `it hands the compiler, so NOTHING here says whether this revision compiles. It is not a finding ` +
             `about the tree; the reason is below and is usually a snapshot missing engine/.work/emsdk.`);
    for (const l of rev.unaudited) out.push(`[rev]   ${l}`);
  }
  /* The artifact, when it is not the program this run linked. Reported after the revision and never instead
     of it: a clean tree in front of an older artifact would otherwise read as quotable. */
  if (rev.stamp) {
    const s = rev.stamp;
    if (s.unasked && s.unasked.length) {
      /* The stamp's own unknown, asked first: a build whose tree could not be asked stamped an empty `dirty`.
         A stamp from before the field has no `unasked` key and falls through. */
      out.push(`[rev] THE ARTIFACT'S TREE COULD NOT BE ASKED WHEN IT WAS BUILT — ${rev.artifact} was stamped ` +
               `${s.at} at ${short(s.head)} with ${s.unasked.length} unanswered ask(s), so whether it was ` +
               `built from a clean revision was never established and the head it names is unconfirmed.`);
      for (const l of s.unasked) out.push(`[rev]   built-with ${l}`);
    } else if (s.dirty && s.dirty.length) {
      /* The build was made from a tree no revision contains, so there is nothing to compare it to. */
      out.push(`[rev] THE ARTIFACT WAS BUILT FROM A DIRTY TREE — ${rev.artifact} was stamped ${s.at} at ` +
               `${short(s.head)} with ${s.dirty.length} path(s) of its cone modified, so no revision describes ` +
               `the program this run measured. Rebuild from a frozen snapshot before quoting this number.`);
      for (const l of s.dirty) out.push(`[rev]   built-with ${l}`);
    } else {
      const eng = behindBy(s.head, rev.head);
      /* A stamp carrying `qjsHead` was written while `engine/qjs` was a submodule; its presence dates the
         artifact and is named. `head` still determines that artifact's engine, which the commit pinned. */
      const preSubtree = typeof s.qjsHead === "string" ? s.qjsHead : null;
      if (eng && eng.same) {
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
                 `${say("engine", s.head, eng)}` +
                 (preSubtree ? `. It was stamped BEFORE the subtree merge — it carries a submodule commit ` +
                               `(${short(preSubtree)}) for \`${QJS_PATH}\`, which this record no longer ` +
                               `produces because that path is tracked content now; the engine it was built ` +
                               `from is whatever ${short(s.head)} pinned` : "") +
                 `. This run measured that BUILD, not the revision above; rebuild ` +
                 `(\`node engine/build.mjs\`) or quote the number against the build instead.`);
      }
    }
  } else if (rev.stale) {
    /* No stamp: mtime is all there is, and it is reported as a heuristic, since in a frozen snapshot every
       file is newer than every artifact. */
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

/* Whether the tree moved under the run, asked at the end against the identity taken at the start: a shared
   checkout edited mid-run means the linked program and the tree on disk differ. Returns the description, or
   null when nothing moved. */
export function revisionMoved(before) {
  /* No artifact on the re-ask: the question is whether the sources moved. */
  const now = gateRevision(before.cone);
  if (now.head !== before.head)
    return `the superproject HEAD changed under this run (${short(before.head)} → ${short(now.head)})`;
  if (now.dirty.join("\n") !== before.dirty.join("\n"))
    return "the compiled cone was edited under this run — the sources that were linked are not the sources " +
           "on disk now, so `git diff` will not show the program this number is about";
  /* Asked separately: a run that could ask at the start and not at the end is not an edit. */
  if (now.unasked.join("\n") !== before.unasked.join("\n"))
    return "git answered about the compiled cone at one end of this run and not the other, so whether the " +
           "sources moved under it is now unknowable rather than merely unknown";
  return null;
}
