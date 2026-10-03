/* WHAT A CACHED LEXBOR ARCHIVE WAS COMPILED FROM — one answer, for every compiler in this tree.
 *
 * WHY IT IS ITS OWN FILE. `engine/build.mjs` learned this and `engine/wpt.mjs` did not, and the two halves of
 * that sentence are the same defect: build.mjs's own paragraph says presence "now means 'some archive exists'"
 * and that an edit to the fork "would relink the PREVIOUS objects and every gate would attribute the result to
 * the edited revision" — while wpt.mjs cached its NATIVE archive on `existsSync` alone and stated, in a comment
 * of its own, that "this gate and the emscripten build compile the same bytes by construction rather than by
 * two clones agreeing". That claim was FALSE at the moment it was written, because construction was the one
 * thing missing. It did not stay quiet either: the fork grew `lxb_html_attribute_steps_*`,
 * `lxb_dom_attr_local_name` and `lxb_html_tree_dom_set`, and the WPT runner stopped LINKING — the browser
 * half's gate could not be run at all, on a clean tree, at a revision every other gate called green.
 *
 * A LINK ERROR IS THE LUCKY CASE, WHICH IS THE REAL ARGUMENT FOR THIS FILE. An ADDED function breaks the link
 * and says so; an EDITED function body does not. That archive links, the gate runs, and it measures a lexbor
 * the shipped build does not contain — a number about a program no revision holds, which is §Testing's
 * frozen-snapshot rule broken from inside the toolchain instead of from the working tree.
 *
 * SO THE IDENTITY IS COMPUTED IN ONE PLACE AND STAMPED BESIDE EVERY ARCHIVE. A second copy of it would be the
 * same shape as the defect it removes: two programs deciding independently whether the same source changed.
 *
 * AND COMPUTING THE IDENTITY IS NOT THE SAME AS CONSULTING IT, WHICH IS HOW THIS FILE'S OWN LESSON GOT LOST A
 * THIRD TIME. Exporting the answer made two callers agree about WHAT the id is and left each of them to
 * remember to ASK — and `engine/build.mjs`'s NATIVE arm never did: it took the archive on `existsSync` alone,
 * beside a stamp file that said, in the same directory, that the archive was not this source's. The skew it
 * linked is the one an added function would have caught in the linker and an edited struct cannot: this fork
 * gave `struct lxb_selectors` a host-callback table, so the header the host compiles against said 56 bytes
 * while the archive's `lxb_selectors_create` still callocated 40 — and `lxb_selectors_host_cb_set` is
 * `lxb_inline`, so every `dom_collect_scripts` wrote one pointer past the allocation into the next chunk's
 * header. `document_bundle_id` runs on EVERY document, so the native host aborted in `free()` before its first
 * line of output, three frames away from the write, with a message about an invalid pointer and nothing in it
 * naming a cache.
 * SO THE PROVISIONING IS HERE TOO, AND THERE IS NO STATE FOR A CALLER TO CHECK. `lexborNativeArchive` returns a
 * path to an archive that IS this source's or it does not return at all; a consumer cannot hold a stale one
 * because it is never handed one. That is the difference between a check every caller must remember and an
 * impossible state — and the check-shaped version had already been forgotten once per consumer added.
 *
 * AND AN ID COMPUTED HERE IS NOW A NAME, WHICH IS THE PROPERTY THE EMCC HALF HAD AND THIS ONE DID NOT. THE
 * RETIRED PARAGRAPH IS KEPT BECAUSE A READER WHO RE-DERIVES IT WILL RE-ADD THE SIDECAR. It said: "The archive
 * below is the remaining FIXED NAME plus SIDECAR, written IN PLACE in a per-snapshot directory, so every frozen
 * build still pays its cmake+make" — and that a sidecar beside a fixed name in a directory ONE snapshot owns is
 * a sound answer to "what was this compiled from"; "what it cannot do is let two snapshots share one archive,
 * which is a different question and the only one the name answers". Every clause of that was true, and it is
 * the name that answers, so the name is what this file writes: `liblexbor_static-<id>.a` in the shared object
 * store, published by `rename` from a temporary INSIDE it, with no sidecar left to disagree with it.
 *
 * WHAT THE NAME CARRIES THAT A `srcid` COULD NOT, which is the whole of why the sidecar's spelling was the
 * wrong one: an archive's bytes are decided by the COMPILER and the BUILD SYSTEM as much as by the source, and
 * leaving either out is not a MISS but a FALSE HIT — the one failure a content-addressed store may not have.
 * So the id folds in the compiler CMAKE ITSELF PICKED, asked for its own version text, cmake's own version, the
 * `-D` set cmake is driven with, and the 19 `CMakeLists.txt`/`.cmake` files that decide what cmake compiles.
 *
 * AND THE COMPILER IS READ FROM CMAKE'S CACHE RATHER THAN PREDICTED, which is the one thing a reader of this is
 * most likely to get wrong. cmake resolves its C compiler from `CMAKE_C_COMPILER`, then `$CC`, then a platform
 * candidate list; re-deriving that order here would be a second copy of a rule this file cannot be right about,
 * and being wrong about it is a false hit rather than a refusal. MEASURED: cmake picks `/usr/bin/cc` here,
 * which is `x86_64-linux-gnu-gcc-13`, and the native target compiles its OWN sources with `clang` — so the two
 * are different compilers in one build and a key naming the wrong one would let gcc's archive answer for
 * clang's. Reading it out of `CMakeCache.txt` is cmake's own answer about what it picked.
 */
import { copyFileSync, existsSync, mkdirSync, readdirSync, readFileSync, renameSync, rmSync } from "node:fs";
import { join, relative } from "node:path";
import { createHash } from "node:crypto";
import { spawnSync } from "node:child_process";
import { cpus } from "node:os";
import { cmakeCacheVars, objStore, toolIdentity } from "./obj_store.mjs";

/* CONTENT, NOT MTIME: a fresh clone writes every file at checkout time, so mtimes would rebuild 213 sources on
   a tree that changed nothing, and a restored file would keep a stale hash. Reading 22 MB to hash it costs a
   fraction of the compile it guards. */
/* HEADERS COUNT. They are not compiled on their own, so a walk that collected only .c would call a tree with an
   edited html/tree.h unchanged — and that header is exactly where this fork's seam is declared. */
/* THE PATH IS RELATIVE TO THE SOURCE ROOT, because an absolute one makes the id a property of WHERE the tree is
   checked out rather than of what is in it. A gate runs from a frozen snapshot in a scratch directory, so an
   absolute path recompiled 213 sources on content byte-identical to the tree it was cloned from — never a wrong
   answer, but a per-build cost paid to learn nothing, and an id that cannot be compared between two checkouts
   of the same revision is not an identity. */
/* `srcDir` IS THE DIRECTORY HOLDING `lexbor/`, which is `engine/lexbor/source`. Taken as an argument rather
   than derived here, because the two callers reach it by different constants and a third one guessed at in
   this file would be a fourth place the layout is written down. */
export function lexborSourceId(srcDir) {
  const walk = (dir, out) => {
    for (const e of readdirSync(dir, { withFileTypes: true }).sort((a, b) => (a.name < b.name ? -1 : 1))) {
      const p = join(dir, e.name);
      if (e.isDirectory()) { if (p.includes("windows_nt")) continue; walk(p, out); }
      else if (e.name.endsWith(".c") || e.name.endsWith(".h")) out.push(p);
    }
    return out;
  };
  const h = createHash("sha256");
  const root = join(srcDir, "lexbor");
  for (const p of walk(root, [])) h.update(relative(root, p).replace(/\\/g, "/")).update(readFileSync(p));
  return h.digest("hex").slice(0, 16);
}

/* THE BUILD SYSTEM IS AN INPUT TO THE NATIVE ARCHIVE AND TO NOTHING ELSE, which is why it is its own id and
   not a widening of `lexborSourceId`. cmake reads 19 `CMakeLists.txt`/`.cmake` files under `engine/lexbor` and
   they decide which translation units exist, what macros they see and which port is compiled — so an edit to
   one changes the archive's bytes while leaving every `.c` and `.h` byte-identical, which is a FALSE HIT under
   a source-only name. The emcc half reads NONE of them: it is handed an explicit source list and an explicit
   flag list, so folding these into the shared source id would move that archive's name for a parameter that
   cannot distinguish two of its objects — the argument `engine/build.mjs` already makes for keeping `shell`
   out of `flagId`, and it costs a cold 213-source compile to get wrong.
   EVERY PORT'S CONFIG IS HASHED, `windows_nt` INCLUDED, where `lexborSourceId` skips that directory. Which
   ports cmake reads is cmake's decision and not this file's, and hashing one it ignores can only cost a MISS
   when it changes — which is a compile, where the inverse is an archive answering for a configuration that
   never produced it. */
export function lexborBuildSystemId(lexborDir) {
  const walk = (dir, out) => {
    for (const e of readdirSync(dir, { withFileTypes: true }).sort((a, b) => (a.name < b.name ? -1 : 1))) {
      const q = join(dir, e.name);
      if (e.isDirectory()) walk(q, out);
      else if (e.name === "CMakeLists.txt" || e.name.endsWith(".cmake")) out.push(q);
    }
    return out;
  };
  const h = createHash("sha256");
  for (const q of walk(lexborDir, []))
    h.update(relative(lexborDir, q).replace(/\\/g, "/")).update(readFileSync(q));
  return h.digest("hex").slice(0, 16);
}

/* THE `-D` SET IS DECLARED ONCE AND IS PART OF THE NAME. The SOURCE DIRECTORY is deliberately not among these:
   it is the one absolute path in the invocation, it is a different string in every snapshot, and what is IN it
   is `lexborSourceId`'s answer — so leaving it in would re-create the absolute-path cache key `engine/build.mjs`
   measured as making two checkouts of ONE revision share nothing.
   `-j` IS NOT IN THE NAME EITHER, and it is not an omission: a core count decides how long a make takes and not
   what it emits, so hashing it would rename the archive on every box with a different one. */
const CMAKE_DEFS = ["-DCMAKE_BUILD_TYPE=Release", "-DLEXBOR_BUILD_SHARED=OFF", "-DLEXBOR_BUILD_STATIC=ON",
                    "-DLEXBOR_BUILD_TESTS=OFF", "-DLEXBOR_BUILD_EXAMPLES=OFF"];

/* A TOOL THAT IS NOT INSTALLED AND A TOOL THAT FAILED ARE DIFFERENT FACTS, and only one of them writes to
   stderr: a spawn that never started answers `status: null` with an empty `stderr` and puts the reason in
   `error`, so reporting the stream alone printed a bare "cmake FAILED" and a blank line for a box with no
   cmake on it.
   RESIDUAL — THIS EXITS THE PROCESS WHERE A CALLER SHOULD BE TOLD.
   WHAT IS NOT COVERED: `process.exit(1)` below, and therefore `lexborNativeArchive`'s whole failure surface. A
   library function that kills its caller cannot be used by a consumer that wants to report the failure as one
   stage of several, which is what `engine/build.mjs`'s stage list does with every other failure it meets.
   WHAT THE NEXT DIFF BUILDS: a thrown error carrying the tool, the argv and the spawn's own `error`/`status`,
   with both importers reporting it — `engine/build.mjs` already carries this residual at its native arm and it
   is ONE diff for both sites rather than one each.
   HOW ITS ABSENCE WOULD SHOW: a build whose stage table ends at the lexbor stage with no row for it, where
   every other failing stage contributes a row and an exit code the table names. The observation is the missing
   row beside a non-zero exit; it names no revision and no stage number. */
function runOrExit(tag, cmd, args, cwd) {
  const b = spawnSync(cmd, args, { cwd, encoding: "utf8" });
  if (b.status === 0) return;
  console.error(`[${tag}] lexbor ${cmd} FAILED — ` +
                (b.error ? `it could not be run at all (${b.error.code || b.error.message})`
                         : `it exited ${b.status === null ? "on signal " + b.signal : b.status}`) +
                "\n" + (b.stderr || ""));
  process.exit(1);
}

/* THE NATIVE ARCHIVE THIS SOURCE COMPILES TO — NAMED for what produced it, published into the shared object
   store, and therefore compiled ONCE across every snapshot that shares that store rather than once per
   snapshot.
 *
 * ONE RECIPE, NOT A CHECK PLUS A REFUSAL. `engine/build.mjs`'s native arm used to refuse an absent archive and
 * name `node engine/wpt.mjs` as the command that makes one, which put the cmake invocation in one file and a
 * second consumer's correctness in a sentence a person has to read. That division is what let a stale archive
 * sit for two days: nothing was wrong with either file's own code, and the archive on disk was the one nobody
 * owned. A caller that gets a path back has an archive; a caller that would have got a wrong one gets a build.
 *
 * THE CONFIGURE IS RUN ON EVERY CALL AND IT IS BOUGHT RATHER THAN SUFFERED, because it is the only thing that
 * can say which compiler this make would use. The alternatives were measured against the one failure this
 * scheme may not have. Predicting cmake's choice is a second copy of cmake's resolution order, and a wrong
 * prediction is a FALSE HIT. REUSING a previous configure is sound for its directory and not for its content —
 * cmake re-runs itself when `CMakeLists.txt` changes and not when a glob's membership does, so a reused build
 * directory can emit an archive missing a translation unit and publish it under a name that claims this
 * source. So the configure is FRESH, in a PID-NAMED directory nothing else can be configuring, and removed
 * when this function returns; two concurrent callers in one snapshot cannot collide, which a shared build
 * directory plus an unconditional configure would have made possible for the first time.
 * ITS COST IS PRINTED RATHER THAN ARGUED ABOUT HERE. A number in a comment is unfalsifiable; the elapsed
 * seconds beside the hit are read off every log that holds one, by anyone, at today's cmake on today's box.
 *
 * THE BUILD DIRECTORY GOES WITH THE ARCHIVE, because a CMake cache records WHERE the source was as much as what
 * was in it — one was generated when lexbor lived at `.work/lexbor-src` and cmake REFUSES to reconfigure across
 * the move ("The source … does not match the source … used to generate cache"). A pid-named directory that is
 * never reused makes that state unreachable rather than detected, which is the same move the name makes for the
 * sidecar: there is nothing left to be stale.
 *
 * NOTHING HALF-NAMED SURVIVES A FAILURE, which is the sidecar's "STAMPED ONLY ON SUCCESS" argument with nothing
 * left to stamp: a failed make publishes no name, so the next call asks for a name nothing ever wrote and
 * builds. The pid directory is deliberately LEFT on a failure — it holds cmake's own log and the objects that
 * did compile, and a loud failure is the one moment that is worth reading.
 *
 * `tag` NAMES THE CALLER IN THE LOG because the two consumers' output is read by different people looking for
 * different things, and a line that says which gate is paying for a five-minute cmake is the difference between
 * a build that looks hung and one that is explaining itself. */
export function lexborNativeArchive(engineDir, tag) {
  const src = join(engineDir, "lexbor");
  const srcId = lexborSourceId(join(src, "source"));
  const bsId = lexborBuildSystemId(src);
  const store = objStore(engineDir);
  const build = join(engineDir, ".work", "lexbor-cmake-" + process.pid);

  rmSync(build, { recursive: true, force: true });
  mkdirSync(build, { recursive: true });
  /* SAID BEFORE IT IS PAID, which is this file's own rule for the make one paragraph down: a build that goes
     quiet for a tool it did not name reads as hung. */
  console.log(`[${tag}] configuring lexbor (cmake, which is what says WHICH compiler this make would use)…`);
  const t0 = Date.now();
  runOrExit(tag, "cmake", [...CMAKE_DEFS, src], build);
  const cfgMs = Date.now() - t0;

  /* CMAKE'S OWN ANSWER ABOUT WHAT IT PICKED. `cmakeCacheVars` returns null for a variable the cache does not
     record, which is a positive answer and never an empty string that could be hashed — an id computed over a
     compiler nobody identified is exactly the name that cannot change when the compiler does. */
  const V = cmakeCacheVars(join(build, "CMakeCache.txt"),
                           ["CMAKE_C_COMPILER", "CMAKE_CACHE_MAJOR_VERSION",
                            "CMAKE_CACHE_MINOR_VERSION", "CMAKE_CACHE_PATCH_VERSION"]);
  const cmakeVer = V && [V.CMAKE_CACHE_MAJOR_VERSION, V.CMAKE_CACHE_MINOR_VERSION,
                         V.CMAKE_CACHE_PATCH_VERSION].every((x) => x)
                     ? V.CMAKE_CACHE_MAJOR_VERSION + "." + V.CMAKE_CACHE_MINOR_VERSION + "." +
                       V.CMAKE_CACHE_PATCH_VERSION
                     : null;
  if (!V || !V.CMAKE_C_COMPILER || !cmakeVer) {
    console.error(`[${tag}] cmake configured lexbor and its cache records no ` +
                  (!V ? "readable cache at all" : !V.CMAKE_C_COMPILER ? "CMAKE_C_COMPILER" : "cmake version") +
                  ` in ${join(build, "CMakeCache.txt")}\n` +
                  `[${tag}]   an archive may not be named for a configuration this cannot read — a name that\n` +
                  `[${tag}]   does not change when the compiler does is how a stale archive is linked.`);
    process.exit(1);
  }
  /* THE VERSION TEST IS LOOSE BECAUSE THE TOOL IS NOT KNOWN IN ADVANCE, and that is MEASURED rather than
     assumed: cmake picks `cc` here, which is gcc, whose first line is `cc (Ubuntu 13.3.0-6ubuntu2~24.04.1)
     13.3.0` and contains no `version` token — so `clang`'s own `/clang version [0-9]/` would REFUSE the
     compiler this build actually uses. A dotted number is the whole of what can be required of a compiler
     whose identity cmake chose: the contract is that the text moves when the tool does, not that it is worded
     any particular way.
     NOTHING IS COLLAPSED OUT OF THESE STRINGS, which is the opposite of the emcc half and is deliberate. There
     a path under the checkout names the SAME symlinked emsdk and the SAME relative headers in every snapshot,
     so collapsing it is what makes two snapshots share; here no string carries a path this tree owns — the
     `-D` set is five literals and a compiler outside the checkout describes itself — and collapsing a prefix
     two checkouts do NOT share would be a false hit, where leaving one in costs a miss and a miss is a make. */
  const ccId = toolIdentity(`cc(cmake's pick, ${tag})`, V.CMAKE_C_COMPILER, ["--version"],
                            /[0-9]+\.[0-9]+/, CMAKE_DEFS, (s) => s);
  const id = createHash("sha256")
               .update("apiclient-lexbor-native-v1\0" + ccId + "\0" + cmakeVer + "\0" + bsId + "\0" + srcId)
               .digest("hex").slice(0, 32);
  const lib = join(store.dir, "liblexbor_static-" + id + ".a");
  const made = join(build, "liblexbor_static.a");
  const what = `source ${srcId}, build system ${bsId}, cmake ${cmakeVer}, cc+flags ${ccId}`;

  if (existsSync(lib)) {
    console.log(`[${tag}] lexbor  ${lib}  (${what}; configure ${cfgMs} ms` +
                (store.shared ? ", shared store" : ", this tree's own store") + ")");
    rmSync(build, { recursive: true, force: true });
    return lib;
  }
  console.log(`[${tag}] building lexbor natively -> ${lib}  (${what}; configure ${cfgMs} ms, now make, ` +
              "~minutes)");
  runOrExit(tag, "make", ["-j" + (cpus().length || 4)], build);
  /* A SUCCESSFUL MAKE THAT PRODUCED NO ARCHIVE IS A SHOULD-NEVER-HAPPEN AND IS SAID AS ONE, because the
     alternative is an ENOENT out of `copyFileSync` naming a path and no diagnosis — the target name and the
     build directory are what a reader needs and only this line has both. */
  if (!existsSync(made)) {
    console.error(`[${tag}] make exited 0 and ${made} does not exist — the static target is named ` +
                  "`liblexbor_static` by `-DLEXBOR_BUILD_STATIC=ON`, so either that flag no longer selects " +
                  "it or lexbor's cmake writes it elsewhere; the configured tree is left at " + build);
    process.exit(1);
  }
  /* A PRIVATE TEMPORARY INSIDE THE STORE AND A PUBLISH BY `rename`, exactly as `buildLexbor` does and for both
     of its reasons. The pid keeps two concurrent builds out of each other's way, and the rename makes the
     publish atomic — so a snapshot producing this archive cannot overwrite bytes another snapshot is linking,
     which under a fixed name it could. Under a content name both arrive at the same name with the same bytes,
     so arriving twice is harmless, and a reader holding an open fd keeps the inode it opened. INSIDE THE STORE
     because a rename is atomic only within one filesystem and a lane may point `APICLIENT_OBJ_STORE` at
     another one, which is why the make's output is COPIED there first rather than renamed from the build
     directory. */
  const tmp = join(store.dir, ".tmp-" + process.pid + "-liblexbor_static.a");
  copyFileSync(made, tmp);
  renameSync(tmp, lib);
  rmSync(build, { recursive: true, force: true });
  console.log(`[${tag}] lexbor OK -> ${lib}  (${what})`);
  return lib;
}
