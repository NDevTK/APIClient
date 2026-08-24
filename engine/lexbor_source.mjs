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
 */
import { readdirSync, readFileSync } from "node:fs";
import { join, relative } from "node:path";
import { createHash } from "node:crypto";

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
