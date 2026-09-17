#!/usr/bin/env node
// Upstream quickjs-ng sync.
//
// THIS COMMAND REFUSES, AND THE REFUSAL IS THE HONEST STATE RATHER THAN A GAP TO ROUTE AROUND. The mechanism
// it automated does not exist any more, and what stood here did not stop working quietly — it began aiming
// every one of its git commands at the WRONG REPOSITORY while reporting success.
//
// WHAT IT WAS, AND THE ARGUMENT IS KEPT BECAUSE A READER WHO RE-DERIVES IT WILL RE-ADD IT. `engine/qjs` was a
// SUBMODULE: a repository of its own, with `master` mirroring quickjs-ng and `apiclient-fork` carrying this
// project's patches woven through quickjs.c. Syncing meant fetching upstream INSIDE that repository, fast-
// forwarding its master, three-way merging into apiclient-fork, pushing BOTH branches to the fork remote, and
// then bumping the superproject's gitlink. Every step of that was right, and every step of it named a place.
//
// A SUBTREE MERGE ABSORBED THE FORK INTO THIS REPOSITORY. There is no gitlink to bump, no second repository to
// push, and no `apiclient-fork` branch anywhere in this history — the fork's commits are in THIS history, and
// `engine/qjs` is ordinary tracked content under a prefix.
//
// WHY THIS FILE REFUSES RATHER THAN CARRYING ON. Every command here ran through `execSync(..., { cwd: QJS })`,
// and git resolves its repository by walking UPWARD: a question put to a directory that is not itself a
// repository is answered, successfully and with status 0, by whichever repository CONTAINS it. So each of
// these would now operate on the SUPERPROJECT, in a checkout several agents share:
//   * `git remote add upstream https://github.com/quickjs-ng/quickjs.git` — a WRITE to the shared repository's
//     config, performed as a side effect of the read-only-sounding `check` mode. MEASURED at 97711c25: this
//     checkout has exactly one remote (`origin` → NDevTK/APIClient), so `remoteByUrl("quickjs-ng/quickjs")`
//     answers null and that line is the one that runs.
//   * `git fetch upstream --tags` — pulling a foreign project's objects into the shared store.
//   * `git checkout master` and `git checkout apiclient-fork` — a BRANCH checkout in the shared working tree,
//     which rewrites every tracked file under every lane working in it. CLAUDE.md bans `git checkout --
//     <paths>` for destroying uncommitted work with no record; moving HEAD is that with a wider blast radius.
//   * `git merge --ff-only` / `git merge --no-edit` in the superproject.
// `gSafe` returned stderr as a STRING rather than throwing, so every one of those failures was swallowed and
// the run continued. What kept `apply` from reaching the checkouts today is that `merge-base apiclient-fork
// …` finds no such branch, so `behind` computes as -1 and the `behind <= 0` early return fires. That is LUCK
// and not a guard: it is one `git branch master` away from stopping being true, and `check` writes to the
// shared config before it gets there either way.
//
// WHAT THE NEXT DIFF BUILDS, IN ORDER. These are ordered by which has a CONSUMER, not by which is deepest:
//   1. `sync check` over the subtree — fetch quickjs-ng into THIS repository under its own remote, and report
//      how far `engine/qjs`'s content is behind. The comparison is between a subtree prefix and an upstream
//      tree, not between two branches, so `git merge-base` is the wrong instrument and the right one is a
//      decision to make rather than to guess: `git subtree` records its own sync points in commit messages
//      (`git-subtree-dir`/`git-subtree-split`), and whether to rely on those or to keep an explicit marker is
//      the first thing to settle. `vendoredVersion()` below already answers half of what `check` printed and
//      is kept because it reads `quickjs.h` and asks git nothing.
//   2. `sync apply` as `git subtree merge --prefix=engine/qjs <upstream-ref>` (or `git merge -s subtree`),
//      which lands in ONE commit in ONE repository. There is no push to a fork remote and no gitlink bump, so
//      the "apiclient-fork FIRST then master, so master is never ahead" ordering that used to live here is
//      retired with the two branches it ordered.
// HOW ITS ABSENCE SHOWS: `node engine/sync.mjs check` exits non-zero and names this file, so a lane that needs
// an upstream sync finds out here rather than by reading a plausible "UP TO DATE" computed from -1.

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const ENGINE = dirname(fileURLToPath(import.meta.url));
const QJS = join(ENGINE, "qjs");

/* STILL TRUE AND STILL USEFUL — it reads the vendored header and asks git nothing, so the subtree merge did
   not touch it. It is the one thing this file could always answer without naming a repository. */
export function vendoredVersion() {
  const h = readFileSync(join(QJS, "quickjs.h"), "utf8");
  const m = (re) => (h.match(re) || [])[1];
  return `${m(/QJS_VERSION_MAJOR\s+(\d+)/)}.${m(/QJS_VERSION_MINOR\s+(\d+)/)}.${m(/QJS_VERSION_PATCH\s+(\d+)/)}`;
}

const cmd = process.argv[2] || "check";
console.error(`[sync] REFUSING \`${cmd}\` — the upstream-sync mechanism this file automated does not exist.`);
console.error(`[sync] vendored quickjs.h version: ${vendoredVersion()}`);
console.error("[sync] `engine/qjs` was a SUBMODULE with its own `master` and `apiclient-fork` branches, and a");
console.error("[sync] subtree merge absorbed it into this repository. There is no gitlink to bump and no");
console.error("[sync] second repository to push. Every git command that stood here ran with `cwd: engine/qjs`,");
console.error("[sync] which git now resolves UPWARD to the superproject — so `check` would have written a");
console.error("[sync] remote into this SHARED checkout's config and `apply` would have run `git checkout` in");
console.error("[sync] a working tree several agents are editing. Read this file's header for what to build:");
console.error("[sync] a subtree-aware `check`, then `git subtree merge --prefix=engine/qjs`.");
process.exit(1);
