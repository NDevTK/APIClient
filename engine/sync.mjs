#!/usr/bin/env node
// Upstream quickjs-ng sync — automate the mechanical, human-gate the judgment.
//
// The forced-execution engine is a DEEP fork of quickjs-ng (our patches woven
// through quickjs.c). Keeping it current is maintenance to PERFORM, not poll for
// by hand. This script collapses the safe mechanical steps into one command and
// REFUSES to land anything risky (it never commits, never pushes, never resolves
// a conflict, never claims a red build is fine — those need a human/agent).
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
const gSafe = (cmd) => { try { return g(cmd); } catch (e) { return (e.stdout || "") + (e.stderr || ""); } };

function vendoredVersion() {
  const h = readFileSync(join(QJS, "quickjs.h"), "utf8");
  const m = (re) => (h.match(re) || [])[1];
  return `${m(/QJS_VERSION_MAJOR\s+(\d+)/)}.${m(/QJS_VERSION_MINOR\s+(\d+)/)}.${m(/QJS_VERSION_PATCH\s+(\d+)/)}`;
}

function check() {
  console.log("[sync] fetching upstream (origin = quickjs-ng)…");
  gSafe("git fetch origin --tags");
  const behind = parseInt(gSafe("git rev-list --count master..origin/master") || "0", 10);
  const upstream = gSafe("git describe --tags origin/master") || "(unknown)";
  const vendored = vendoredVersion();
  const forkBase = gSafe("git describe --tags apiclient-fork") || "(unknown)";
  console.log(`[sync] vendored quickjs.h version : ${vendored}`);
  console.log(`[sync] apiclient-fork git lineage : ${forkBase}`);
  console.log(`[sync] upstream origin/master     : ${upstream}`);
  console.log(`[sync] master is ${behind} commit(s) behind origin/master`);
  if (behind === 0) console.log("[sync] UP TO DATE — nothing to do.");
  else console.log(`[sync] BEHIND by ${behind} — run \`node engine/sync.mjs apply\` to merge (then verify + land by hand).`);
  return { behind, upstream, vendored };
}

function apply() {
  const { behind } = check();
  if (behind === 0) return;
  // Refuse to merge on a dirty TRACKED tree (untracked dumps are fine) — a human
  // should decide what to do with in-progress edits before a structural merge.
  const dirty = gSafe("git status --short --untracked-files=no").trim();
  if (dirty) { console.error(`[sync] ABORT: engine/qjs has uncommitted tracked changes:\n${dirty}\n  Commit or stash them first.`); process.exit(2); }
  console.log("[sync] FF master to origin/master…");
  console.log(gSafe("git checkout master") , gSafe("git merge --ff-only origin/master"));
  console.log("[sync] 3-way merge master into apiclient-fork…");
  gSafe("git checkout apiclient-fork");
  const mergeOut = gSafe("git merge --no-edit master");
  const conflicts = gSafe("git diff --name-only --diff-filter=U").trim();
  if (conflicts) {
    console.error(`[sync] MERGE CONFLICT — needs a human to re-apply patches:\n${conflicts}`);
    console.error("[sync] resolve, then: node engine/build.mjs worker && stage, run gates+_idxdocs, commit, push.");
    console.error("[sync] (to back out: git merge --abort)");
    process.exit(3);
  }
  console.log(`[sync] clean merge → ${gSafe("git describe --tags apiclient-fork")}`);
  console.log("[sync] rebuilding worker…");
  execSync("node engine/build.mjs worker && node engine/build.mjs stage", { cwd: ROOT, stdio: "inherit" });
  console.log("\n[sync] MERGED + BUILT. NOT committed/pushed (human-gated). Next:");
  console.log("  1. node testing/harness.js restart");
  console.log("  2. run the FULL polarity gate set + _idxdocs --deep (spin 0, converged, _xss 4x REAL_EXPLOIT)");
  console.log("  3. verify a real vendor bundle (Apple/MS) — no freeze, grind completes");
  console.log("  4. ONLY if all green: git -C engine/qjs push fork apiclient-fork");
  console.log("     then in APIClient: git add engine/qjs extension/lib/qjs/qjs_worker.js && git commit  (the pre-commit hook checks both are staged)");
}

const cmd = process.argv[2] || "check";
if (cmd === "check") check();
else if (cmd === "apply") apply();
else { console.error(`usage: node engine/sync.mjs [check|apply]`); process.exit(1); }
