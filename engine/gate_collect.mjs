/* WHO COLLECTS THIS FILE — the accounting that makes "the gate is green" a statement about the whole
 * directory rather than about whatever the runner happened to enumerate.
 *
 * CLAUDE.md §Testing: "A TEST FILE THE GATE DOES NOT COLLECT IS AN EXCLUDED TEST, AND AN EXCLUDED TEST IS A
 * FAILURE … worse, because the total LOOKS complete." A driver that hands a tree to a runner and prints the
 * runner's own count is asserting nothing about the tree: the count is the runner's opinion of its own input,
 * and every file the runner passed over is invisible in exactly the same way a file that passed is.
 *
 * SO THE EXPECTED SET IS DERIVED FROM DISK AND THE RUNNER'S NUMBER IS COMPARED AGAINST IT. That comparison is
 * the caller's; what lives here is the half that decides WHAT THE DIRECTORY CONTAINS AND WHO ANSWERS FOR IT,
 * because that half is a pure function of a path and can therefore be exercised with one fixture tree and no
 * compiler at all. It was not, while it sat inline in a driver whose first act is to invoke clang — a
 * mechanism reachable only behind a build is a mechanism the lanes that cannot build cannot check, and this
 * project splits exactly along that line.
 *
 * THE STRUCTURAL PROPERTY: a new file dropped into the tree either RAISES the expected count (so a runner that
 * did not run it cannot report green) or FAILS NAMING ITSELF (so content this driver cannot run cannot sit
 * there unclaimed). Nobody maintains a list — the list IS the directory. An exemption is a POSITIVE STATEMENT
 * and never silence: `_FIXTURE.js` says so in the filename (test262's own convention, enforced in
 * run-test262's add_test_file), and anything else declares its owner in a `GATE` file in its own directory.
 *
 * Usage:  import { collectFixtures } from "./gate_collect.mjs";
 *         const { files, failures } = collectFixtures(TESTS, ROOT);
 */
import { readdirSync, readFileSync, existsSync } from "node:fs";
import { join, relative } from "node:path";

/* run-test262's add_test_file, restated. A MIRROR, and stated as one: if the two rules drift the gate reddens
   on a difference that is about this file rather than about the corpus, which is the honest failure of the
   pair and the one that gets fixed. */
export const isTest = (name) => name.endsWith(".js") && !name.endsWith("_FIXTURE.js");

/* A `GATE` file is one claim per line: `<runner path from the repo root>  <suffix-or-exact-filename>`, with
   `#` starting a comment. It is read as a POSITIVE statement about content the calling gate does not run, so
   the runner it names is required to EXIST — a claim pointing at a deleted gate is the stale-`DFAIL` shape, a
   sentence that reads as authoritative while the thing it names is gone, and it is one `existsSync` to catch. */
function readGate(dir, root, fail) {
  const path = join(dir, "GATE");
  if (!existsSync(path)) return [];
  const claims = [];
  for (const raw of readFileSync(path, "utf8").split("\n")) {
    const line = raw.replace(/#.*/, "").trim();
    if (!line) continue;
    const [runner, claim] = line.split(/\s+/);
    if (!runner || !claim) {
      fail(`${relative(root, path)}: "${raw.trim()}" is not "<runner> <suffix-or-filename>". A GATE line that ` +
           "cannot be parsed claims nothing, which is the silence it exists to replace.");
      continue;
    }
    if (!existsSync(join(root, runner)))
      fail(`${relative(root, path)} names ${runner} as the gate that collects "${claim}", and ${runner} does ` +
           "not exist. Either the runner moved and this claim must follow it, or the content it claims is now " +
           "collected by nobody.");
    claims.push({ runner, claim });
  }
  return claims;
}

/* Recursive walk. `files` is what the caller's runner is expected to have RUN; everything else must be
   claimed, by the filename convention or by a GATE in ITS OWN directory.
   A CLAIM DOES NOT REACH INTO A SUBDIRECTORY, and that is the whole difference between a positive statement
   and a silent widening. The walk here is RECURSIVE because run-test262's is; the runners a GATE names need
   not be, and the one standing in this tree is not — engine/solvergate.mjs enumerates with
   `readdirSync(CORPUS).filter(f => f.endsWith(".html"))`, a single directory. Let a parent's claim inherit and
   a `solver/sub/x.html` reads as COLLECTED while solvergate never opens the directory: an excluded test whose
   exclusion is stated nowhere, arrived at THROUGH the accounting that exists to end exactly that. So each
   directory answers for itself, and a new one fails NAMING ITS OWN PATH — the GATE to write is the one the
   message names. */
export function collectFixtures(dir, root) {
  const failures = [];
  const fail = (m) => failures.push(m);
  const walk = (d) => {
    const claims = readGate(d, root, fail);
    const found = [];
    for (const e of readdirSync(d, { withFileTypes: true }).sort((a, b) => (a.name < b.name ? -1 : 1))) {
      const path = join(d, e.name);
      if (e.isDirectory()) { found.push(...walk(path)); continue; }
      if (isTest(e.name)) { found.push(path); continue; }
      if (e.name === "GATE") continue;                                 /* the statement itself */
      if (e.name.endsWith("_FIXTURE.js")) continue;                    /* test262's own convention, in the name */
      if (!claims.find((c) => e.name === c.claim || e.name.endsWith(c.claim)))
        fail(`${relative(root, path)} is under ${relative(root, dir)} and NO GATE COLLECTS IT — the driver ` +
             "here runs .js only and nothing says who runs the rest. Either it is a fixture (say so in the " +
             `filename), or the gate that owns it declares itself in ${relative(root, join(d, "GATE"))} — ` +
             "that file and not an ancestor's, because the runner it names may not descend.");
    }
    return found;
  };
  const files = walk(dir);
  /* AN EMPTY CORPUS REPORTS THE SAME GREEN AS A PASSING ONE, which is the exact thing this accounting exists
     to make impossible, so it is a failure here rather than a zero the caller has to notice. */
  if (files.length === 0)
    fail(`${relative(root, dir)} holds no runnable fixture at all — a gate with an empty corpus cannot fail, ` +
         "so its green says nothing about anything.");
  return { files, failures };
}
