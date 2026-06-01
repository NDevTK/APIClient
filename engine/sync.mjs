#!/usr/bin/env node
// Upstream quickjs-ng sync — automate the mechanical, human-gate the judgment.
//
// The forced-execution engine is a DEEP fork of quickjs-ng (our patches woven
// through quickjs.c). Keeping it current is maintenance to PERFORM, not poll for
// by hand. This script collapses the safe mechanical steps into one command. It
// pushes only the FORK's master MIRROR (a pure copy of upstream — always safe);
// it does NOT commit, push apiclient-fork, bump the gitlink, resolve a conflict,
// or claim a red build is fine — those are the agent's post-verify job.
//
//   node engine/sync.mjs check      detection only: fetch upstream, report how far
//                                   behind + vendored vs upstream version. (cheap,
//                                   no build — safe for CI / a scheduled poll.)
//   node engine/sync.mjs apply      check, then FF master + 3-way merge into
//                                   apiclient-fork. ABORTS on conflict (prints the
//                                   conflicted files for a human). On a clean merge
//                                   it rebuilds + stages the worker and prints the
//                                   exact verify+land steps — but does NOT run them.
//
// After `apply` you MUST: run the FULL gate set + _idxdocs through the live Chrome
// harness, and only then commit (apiclient-fork + gitlink + worker) and push. A
// clean merge that builds can still misbehave on a real bundle (CLAUDE.md).

import { execSync } from "node:child_process";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const ENGINE = dirname(fileURLToPath(import.meta.url));
const QJS = join(ENGINE, "qjs");
const ROOT = dirname(ENGINE);
const g = (cmd, opts = {}) => execSync(cmd, { cwd: QJS, encoding: "utf8", stdio: ["ignore", "pipe", "pipe"], ...opts }).trim();
const gSafe = (cmd) => { try { return g(cmd); } catch (e) { return ((e.stdout || "") + (e.stderr || "")).trim(); } };

function vendoredVersion() {
  const h = readFileSync(join(QJS, "quickjs.h"), "utf8");
  const m = (re) => (h.match(re) || [])[1];
  return `${m(/QJS_VERSION_MAJOR\s+(\d+)/)}.${m(/QJS_VERSION_MINOR\s+(\d+)/)}.${m(/QJS_VERSION_PATCH\s+(\d+)/)}`;
}

// Resolve remotes by URL, NOT by name: the submodule's `origin` (per .gitmodules)
// is the FORK (APIClient-quickjs), so a name-based "origin/master" diffs against
// the fork's mirror, not real upstream — the bug that left the fork's master 2
// weeks stale while detection said "up to date".
function remoteByUrl(needle) {
  for (const line of (gSafe("git remote -v") || "").split("\n")) {
    const p = line.split(/\s+/);
    if (p.length >= 2 && p[1].includes(needle) && line.includes("(fetch)")) return p[0];
  }
  return null;
}
function upstreamRemote() {            // quickjs-ng (where new versions come FROM)
  let r = remoteByUrl("quickjs-ng/quickjs");
  if (!r) { gSafe("git remote add upstream https://github.com/quickjs-ng/quickjs.git"); r = "upstream"; }
  return r;
}
function forkRemote() {                // APIClient-quickjs (where master-mirror + apiclient-fork are PUSHED)
  return remoteByUrl("APIClient-quickjs") || "origin";
}

function check() {
  const up = upstreamRemote();
  console.log(`[sync] fetching upstream (${up} = quickjs-ng)…`);
  gSafe(`git fetch ${up} --tags --quiet`);
  // behind = upstream commits not yet in apiclient-fork (true upstream, via merge-base)
  const base = gSafe(`git merge-base apiclient-fork ${up}/master`);
  const behind = base ? parseInt(gSafe(`git rev-list --count ${base}..${up}/master`) || "0", 10) : -1;
  const upstream = gSafe(`git describe --tags ${up}/master`) || "(unknown)";
  const forkTip = gSafe("git describe --tags apiclient-fork") || "(unknown)";
  console.log(`[sync] vendored quickjs.h version : ${vendoredVersion()}`);
  console.log(`[sync] apiclient-fork git lineage : ${forkTip}`);
  console.log(`[sync] upstream ${up}/master      : ${upstream}`);
  console.log(`[sync] apiclient-fork is ${behind} upstream commit(s) behind`);
  if (behind === 0) console.log("[sync] UP TO DATE — nothing to do.");
  else if (behind > 0) console.log(`[sync] BEHIND by ${behind} — run \`node engine/sync.mjs apply\`.`);
  else console.log("[sync] could not compute (no merge-base?) — check remotes.");
  return { behind, upstream, up };
}

function apply() {
  const { behind, up } = check();
  if (behind <= 0) return;
  // Refuse to merge on a dirty TRACKED tree (untracked dumps are fine) — a human
  // should decide what to do with in-progress edits before a structural merge.
  const dirty = gSafe("git status --short --untracked-files=no");
  if (dirty) { console.error(`[sync] ABORT: engine/qjs has uncommitted tracked changes:\n${dirty}\n  Commit or stash them first.`); process.exit(2); }
  const fork = forkRemote();
  console.log(`[sync] FF master to ${up}/master + mirror to ${fork}…`);
  gSafe("git checkout master");
  gSafe(`git merge --ff-only ${up}/master`);
  // Keep the FORK's master a CURRENT mirror of upstream — the step that was
  // missing (only apiclient-fork was pushed), so the fork's master went stale.
  const pm = gSafe(`git push ${fork} master`);
  console.log(`[sync]   master mirror → ${fork}: ${(pm.split("\n").pop() || "ok").trim()}`);
  console.log("[sync] 3-way merge master into apiclient-fork…");
  gSafe("git checkout apiclient-fork");
  gSafe("git merge --no-edit master");
  const conflicts = gSafe("git diff --name-only --diff-filter=U");
  if (conflicts) {
    console.error(`[sync] MERGE CONFLICT — needs a human to re-apply patches:\n${conflicts}`);
    console.error("[sync] resolve, then: node engine/build.mjs worker && stage, run gates+_idxdocs, commit, push.");
    console.error("[sync] (to back out: git merge --abort)");
    process.exit(3);
  }
  console.log(`[sync] clean merge → ${gSafe("git describe --tags apiclient-fork")}`);
  console.log("[sync] rebuilding worker…");
  execSync("node engine/build.mjs worker && node engine/build.mjs stage", { cwd: ROOT, stdio: "inherit" });
  console.log("\n[sync] MERGED + BUILT (master mirror pushed). apiclient-fork NOT yet pushed (a clean merge can still break a real bundle). Next — the AGENT does these, not the user:");
  console.log("  1. node testing/harness.js restart");
  console.log("  2. FULL polarity gate set + _idxdocs --deep (spin 0, converged, _xss 4x REAL_EXPLOIT)");
  console.log("  3. a real vendor bundle (Apple/MS) — no freeze, grind completes");
  console.log(`  4. ONLY if all green:  git -C engine/qjs push ${fork} apiclient-fork`);
  console.log("     APIClient: git add engine/qjs extension/lib/qjs/qjs_worker.js extension/lib/qjs/hostedge.gen.js && git commit  (hook checks the worker is staged)");
  console.log("     then: git push origin main   (pushing verified changes to main is YOUR responsibility — don't leave it)");
}

const cmd = process.argv[2] || "check";
if (cmd === "check") check();
else if (cmd === "apply") apply();
else { console.error(`usage: node engine/sync.mjs [check|apply]`); process.exit(1); }
