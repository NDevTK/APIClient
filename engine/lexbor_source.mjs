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
 */
import { existsSync, mkdirSync, readdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { join, relative } from "node:path";
import { createHash } from "node:crypto";
import { spawnSync } from "node:child_process";
import { cpus } from "node:os";

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

/* THE NATIVE ARCHIVE THIS SOURCE COMPILES TO — built if it is not there, REBUILT if the one that is there was
   compiled from something else, and returned only when the two agree.
 *
 * ONE RECIPE, NOT A CHECK PLUS A REFUSAL. `engine/build.mjs`'s native arm used to refuse an absent archive and
 * name `node engine/wpt.mjs` as the command that makes one, which put the cmake invocation in one file and a
 * second consumer's correctness in a sentence a person has to read. That division is what let a stale archive
 * sit for two days: nothing was wrong with either file's own code, and the archive on disk was the one nobody
 * owned. A caller that gets a path back has an archive; a caller that would have got a wrong one gets a build.
 *
 * THE BUILD DIRECTORY GOES WITH THE ARCHIVE, because a CMake cache records WHERE the source was as much as what
 * was in it — this one was generated when lexbor lived at `.work/lexbor-src` and cmake REFUSES to reconfigure
 * across the move ("The source … does not match the source … used to generate cache"). Same stale-cache defect
 * one level down, so it is one decision: if the archive is not this source's, nothing cached beside it is.
 *
 * THE STAMP IS WRITTEN ONLY AFTER A SUCCESSFUL BUILD. An id recorded for a failed compile makes the next run
 * skip the build and link whatever object was there before, which is this defect reached through its own cache.
 *
 * `tag` NAMES THE CALLER IN THE LOG because the two consumers' output is read by different people looking for
 * different things, and a line that says which gate is paying for a five-minute cmake is the difference between
 * a build that looks hung and one that is explaining itself. */
export function lexborNativeArchive(engineDir, tag) {
  const src = join(engineDir, "lexbor");
  const build = join(engineDir, ".work", "lexbor-native");
  const lib = join(build, "liblexbor_static.a");
  const stamp = join(build, "liblexbor_static.srcid");
  const id = lexborSourceId(join(src, "source"));
  const cached = existsSync(stamp) ? readFileSync(stamp, "utf8").trim() : null;

  if (existsSync(lib) && cached === id) return lib;
  console.log(existsSync(lib)
    ? `[${tag}] the cached native lexbor archive was compiled from ${cached || "an unrecorded source"} and ` +
      `this tree's is ${id} — it is rebuilt, because an archive whose headers disagree with the ones its ` +
      "callers compiled against is a program no revision of this tree contains"
    : `[${tag}] building lexbor natively (once, cmake + make)…`);
  rmSync(build, { recursive: true, force: true });
  mkdirSync(build, { recursive: true });
  for (const [cmd, args] of [["cmake", ["-DCMAKE_BUILD_TYPE=Release", "-DLEXBOR_BUILD_SHARED=OFF",
                                        "-DLEXBOR_BUILD_STATIC=ON", "-DLEXBOR_BUILD_TESTS=OFF",
                                        "-DLEXBOR_BUILD_EXAMPLES=OFF", src]],
                             ["make", ["-j" + (cpus().length || 4)]]]) {
    const b = spawnSync(cmd, args, { cwd: build, encoding: "utf8" });
    /* A TOOL THAT IS NOT INSTALLED AND A TOOL THAT FAILED ARE DIFFERENT FACTS, and only one of them writes to
       stderr: a spawn that never started answers `status: null` with an empty `stderr` and puts the reason in
       `error`, so reporting the stream alone printed a bare "cmake FAILED" and a blank line for a box with no
       cmake on it. */
    if (b.status !== 0) {
      console.error(`[${tag}] lexbor ${cmd} FAILED — ` +
                    (b.error ? `it could not be run at all (${b.error.code || b.error.message})`
                             : `it exited ${b.status === null ? "on signal " + b.signal : b.status}`) +
                    "\n" + (b.stderr || ""));
      process.exit(1);
    }
  }
  writeFileSync(stamp, id + "\n");
  return lib;
}
