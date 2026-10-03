/* WHERE A NAMED ARTIFACT IS PUBLISHED, AND WHAT THE NAME IS A FACT ABOUT — one answer, for every tool here.
 *
 * WHY IT IS ITS OWN FILE, AND IT IS THE SAME ARGUMENT `engine/lexbor_source.mjs` MAKES FOR ITSELF. That file
 * says one archive-identity answer must be imported by everything that caches an archive, because "a second
 * copy of it is the same shape as the defect it removes: two programs deciding independently whether the same
 * source changed". The two primitives below were `engine/build.mjs`'s alone — the `APICLIENT_OBJ_STORE`
 * resolution and the version-text identity every object in that store is named under — and the second consumer
 * of a cached archive (`engine/wpt.mjs`, through `lexborNativeArchive`) could reach NEITHER. So the native
 * lexbor archive stayed a FIXED NAME plus a SIDECAR in a per-snapshot directory and every frozen build paid its
 * cmake+make, for want of a place to put two helpers rather than for any reason about cmake.
 *
 * NOTHING HERE DERIVES THE LAYOUT, which is `lexbor_source.mjs`'s rule for `srcDir` applied to the store: the
 * engine directory is taken as an ARGUMENT, because the callers reach it by their own constants and a copy
 * guessed at here would be another place the layout is written down.
 *
 * AND NOTHING HERE RUNS AT IMPORT. Both exports are functions. `build.mjs`'s §OBJECT CACHE banner argues at
 * length that nothing may spawn a compiler above the branch that decides what to compile — so a module it
 * imports at the top of the file may not spawn one either, and a module-scope `mkdirSync` would create a store
 * for `--list-sources` to answer a question into.
 */
import { mkdirSync, readFileSync } from "node:fs";
import { join, resolve } from "node:path";
import { createHash } from "node:crypto";
import { spawnSync } from "node:child_process";

/* THE STORE. `APICLIENT_OBJ_STORE` is set by `engine/frozen_snapshot.sh` to one directory outside every
   snapshot; unset, the fallback is this tree's own `.work/obj` and nothing is shared. The caller is told WHICH
   it got rather than left to re-read the environment, because a second read of one variable is a second answer
   to one question — and the two answers would be the banner and the behaviour. */
export function objStore(engineDir) {
  const env = process.env.APICLIENT_OBJ_STORE || "";
  const dir = env ? resolve(env) : join(engineDir, ".work", "obj");
  mkdirSync(dir, { recursive: true });
  return { dir, shared: !!env };
}

/* WHAT A TOOL IS, AS A STRING THAT MOVES WHEN THE TOOL DOES — the whole of why a content name works.
 *
 * THE TOOLCHAIN IS ASKED FOR ITS VERSION, NOT POINTED AT, and the paragraph this came from in `build.mjs`
 * records what pointing at it cost: hashing the PATH `EMCC` under a comment claiming "the compiler binary is in
 * the hash for the same reason a header is" — and emsdk upgrades IN PLACE, so one path named two compilers that
 * emit different objects. That is a FALSE HIT rather than a miss, which is the one failure a content-addressed
 * store may not have.
 *
 * THE ACCEPTANCE TEST TRAVELS WITH THE TOOL because "did this report a version" is a different question per
 * tool: a compiler that cannot be identified may not name an artifact at all, since a name that does not change
 * when the compiler does is how a stale object is reported fresh. MEASURED, on this box, that the test cannot
 * be shared: `cc --version` is gcc here and its first line is `cc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0`,
 * which contains no `version` token at all, while `clang --version` opens `Ubuntu clang version 18.1.3` — so
 * one tight regex refuses the other tool, and the caller states its own.
 *
 * `pathIndependent` IS THE CALLER'S, NOT THIS FILE'S, and it is required rather than defaulted. WHERE this tree
 * sits is not part of any identity — `build.mjs` measured that four absolute `-I` roots alone made two
 * checkouts of ONE revision share nothing — but WHICH absolute prefixes may be collapsed is a claim about what
 * two checkouts share at that prefix, and only the caller knows: emsdk is one symlinked directory in every
 * frozen snapshot, so collapsing it is sound, and a path nobody shares collapsed anyway is a FALSE HIT. A
 * caller with nothing to collapse passes an identity function and says why at its own site.
 *
 * A TOOL THAT IS NOT INSTALLED AND A TOOL THAT FAILED ARE DIFFERENT FACTS, and only one of them writes to
 * stderr: a spawn that never started answers `status: null` with an empty `stderr` and puts the reason in
 * `error`. The refusal below reports which, because `lexbor_source.mjs` had already paid for printing the
 * stream alone — a bare failure line and a blank line, on a box with no cmake on it. */
export function toolIdentity(name, cc, versionArgv, versionOk, flags, pathIndependent) {
  const v = spawnSync(cc, versionArgv, { encoding: "utf8" });
  /* `InstalledDir` NAMES WHERE THE TOOL IS, WHICH IS NOT WHAT IT IS — clang prints an absolute one and it moves
     with the box rather than with the compiler. */
  const text = pathIndependent(((v.stdout || "") + (v.stderr || "")).split("\n")
                                 .filter((l) => !l.startsWith("InstalledDir")).join("\n"));
  if (!versionOk.test(text)) {
    console.error("[identity] `" + name + " " + versionArgv.join(" ") + "` did not report a version — " +
                  (v.error ? "it could not be run at all (" + (v.error.code || v.error.message) + ")"
                           : "it exited " + (v.status === null ? "on signal " + v.signal : v.status)) + "\n" +
                  "[identity]   an artifact may not be named for a tool this cannot identify — a name that\n" +
                  "[identity]   does not change when the tool does is how a stale object is reported fresh.");
    process.exit(1);
  }
  return createHash("sha256").update(text + "\0" + pathIndependent(flags.join("\0")))
                             .digest("hex").slice(0, 12);
}

/* A CMAKE CACHE IS CMAKE'S OWN ANSWER ABOUT WHAT IT PICKED, which is why it is read rather than predicted.
   `CMAKE_C_COMPILER` is resolved by cmake from `CMAKE_C_COMPILER`, then `$CC`, then a platform candidate list,
   and re-deriving that order here would be a second copy of a rule this file cannot be right about — and being
   wrong about it is a FALSE HIT, since an archive gcc emitted would keep the name clang's asks for. MEASURED
   from a real frozen build's cache: `CMAKE_C_COMPILER:FILEPATH=/usr/bin/cc`, which resolves to
   `x86_64-linux-gnu-gcc-13` on this box and is NOT the `clang` the native target compiles its own sources with.
   `CMAKE_HOME_DIRECTORY` IS WHY A CACHE MAY BE REUSED OR MAY NOT: it records WHERE the source was, and cmake
   REFUSES to reconfigure across a move ("The source … does not match the source … used to generate cache"), so
   a cache naming another source directory answers about another configuration.
   THE KEYS ARE `:TYPE=VALUE`, and the type is dropped: the caller asks for a variable and a cache that records
   it as `FILEPATH` today and something else tomorrow is the same answer. Absent is returned as null, which is a
   positive answer — "this cache does not record that" — and never an empty string a caller could hash. */
export function cmakeCacheVars(cacheFile, names) {
  let text;
  try { text = readFileSync(cacheFile, "utf8"); } catch { return null; }
  const out = Object.fromEntries(names.map((n) => [n, null]));
  for (const line of text.split("\n")) {
    const m = /^([A-Za-z0-9_]+):[A-Za-z]+=(.*)$/.exec(line.trim());
    if (m && Object.prototype.hasOwnProperty.call(out, m[1]) && out[m[1]] === null) out[m[1]] = m[2];
  }
  return out;
}
