// THE ONE PLACE A LIVE DRIVER LEARNS WHICH PROGRAM IT MEASURED.
//
// CLAUDE.md §Testing: "a result quoted without the revision it came from is not a
// measurement" — so every driver here prints an artifact line before its first row, and
// everything it prints afterwards is filed under that line. That makes the line itself
// load-bearing: a WRONG one is worse than none, because it reads as authoritative and
// files today's numbers under a program that did not produce them.
//
// WHY THIS IS A MODULE AND NOT A FUNCTION IN EACH DRIVER. It was a function in each
// driver, and only one of them had the refusal below — so `live-run.js` refused a
// mislabelled artifact while `live-why.js`, reading the same three fields out of the
// same file, printed them as fact. Two copies of one check is the shape where the copy
// nobody ran against reality is the one that drifts, so there is one copy.
//
// WHAT IT DOES NOT COVER, AND HOW ITS ABSENCE WOULD SHOW: the stamp's `head` is what the
// SOURCE TREE was called at build time, and this tree is edited continuously by other
// lanes, so a build of a dirty tree records a revision whose sources do not contain the
// program that ran. No mtime can see that; `dirty`/`unasked` are what say so, and
// `stampReading` below classifies them rather than folding them into a boolean. It would
// show as a row citing a `head` whose sources lack the very assert text that row printed.
"use strict";

const path = require("path");
const fs = require("fs");

const DEFAULT_EXT = path.join(path.resolve(__dirname, ".."), "extension");

/* A STAMP IS A CLAIM ABOUT BYTES, SO IT IS REFUSED THE MOMENT THE BYTES ARE NEWER THAN THE CLAIM.
   engine/build.mjs calls `stampArtifact` AFTER the link that produced the artifact, and says why at that
   line: "stamping after a failed link would mark whatever qjs.mjs a PREVIOUS build left on disk as
   belonging to this revision, which is §Testing's number about nothing with the stamp itself doing the
   lying." This is that failure arriving from the other direction — the artifact REPLACED without the stamp
   being rewritten — and it lies in exactly the same way, except that a copied-in build is the ordinary way
   a fresh wasm reaches this directory. Because the build stamps last, a legitimate stamp is never older
   than the artifact it describes by more than its own write, so the slack is small and a gap of minutes or
   days is not an ordering artifact.
   REFUSING IS THE WHOLE POINT AND A WARNING WOULD NOT DO. This is the first line of a driver's output and
   every row after it is filed under it, so a warning would be a line the reader scrolls past on their way
   to the numbers it invalidates.
   MEASURED: a fresh `qjs.wasm`/`qjs.mjs` pair was copied into this directory beside a `.build.json` two
   days older; every row that driver printed carried `head f6cbdd9b` while the bytes were built from
   7a7cd512, ~350 commits later. Nothing in the output could have revealed it — the stamp parsed, the
   fields were all present, and only the mtimes disagreed. */
const SLACK_MS = 5000;

/* THREE STATES, NOT TWO — the same reading testing/corpus/site.mjs makes, because it is the same field and
   an artifact does not answer differently depending on who asks. An EMPTY `dirty` carries both "asked git,
   nothing differs" and "could not ask git at all", and the second is the state in which the stamp knows
   NOTHING; folding it into a clean answer publishes the STRONGEST claim available out of the weakest
   evidence. A stamp predating a field has NO OPINION, which is a third thing again. */
function stampReading(b) {
  if (b.dirty === undefined) return "no-dirty-field(stamp predates it)";
  if (b.dirty.length) return "dirty(" + b.dirty.length + " path(s) differ)";
  if (b.unasked === undefined)
    return "unprovable(stamp predates the unasked field — an empty dirty list here is NOT proof of a clean tree)";
  if (b.unasked.length) return "unprovable(" + b.unasked.length + " path(s) git could not be asked about)";
  return "clean(asked, nothing differs)";
}

/* THE ARTIFACT IS NAMED IN THE OUTPUT, and this reads the build stamp the builder wrote, NEVER a git
   command — the working tree advances under a live session and the tree's HEAD is not the artifact.
   THROWS rather than returning a hole: a driver whose whole output is filed under a revision cannot report
   one without naming it, so an absent or unreadable stamp is a refusal like a mislabelled one. That is the
   opposite of the right answer inside testing/harness.js, where the stamp appears as a HINT within another
   error's text — there a missing stamp must not replace the failure being reported, and it is caught and
   labelled instead. The difference is whether the stamp is the thing being asserted or a note beside it. */
function artifactStamp(extDir) {
  const dir = path.join(extDir || process.env.HARNESS_EXT_DIR || DEFAULT_EXT, "lib", "qjs");
  const p = path.join(dir, "qjs.mjs.build.json");
  let j, stampAt;
  try {
    j = JSON.parse(fs.readFileSync(p, "utf8"));
    stampAt = fs.statSync(p).mtimeMs;
  } catch (e) {
    throw new Error(
      "REFUSING TO REPORT: no readable build stamp at " + p + " (" + e.message + "). Every row this driver " +
      "prints is filed under the revision that stamp names, and §Testing: a result quoted without the " +
      "revision it came from is not a measurement. Build through `node engine/build.mjs cow`, which stamps " +
      "after the link, or copy the builder's .build.json in beside the wasm it belongs to.");
  }
  for (const art of ["qjs.wasm", "qjs.mjs"]) {
    const ap = path.join(dir, art);
    const artAt = fs.statSync(ap).mtimeMs;
    if (artAt > stampAt + SLACK_MS) {
      throw new Error(
        "REFUSING TO REPORT: " + art + " is newer than the stamp that claims to describe it — " +
        art + " " + new Date(artAt).toISOString() + " vs " + path.basename(p) + " " +
        new Date(stampAt).toISOString() + ", which names head " + String(j.head).slice(0, 12) + ". " +
        "engine/build.mjs stamps AFTER the link, so a stamp older than its artifact means the artifact was " +
        "replaced without being re-stamped, and every number this driver prints would be filed under a " +
        "revision that did not produce it. Re-stamp the pair (a build through engine/build.mjs does it) or " +
        "copy the builder's .build.json in beside the wasm it belongs to.");
    }
  }
  return { head: j.head, qjsHead: j.qjsHead, at: j.at, treeAtBuild: stampReading(j) };
}

module.exports = { artifactStamp, stampReading };
