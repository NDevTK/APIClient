// lib/novelty-set.js — the product's known identities, per agent-cluster origin, and what each engine instance
// is still owed of them.
//
// engine/host/solver/novelty.h defines an emission's identity and is its only speller: every result document
// carries `_novelty.identities`, one `<kind> <32 hex>` line per identity that instance emitted, and this file
// keeps their union, verbatim, under the spelling the engine stated. An instance is handed the union of its own
// partition at boot and incrementally (`NoveltyLearn`), and pays a credit only for an identity absent from it.
//
// The partition is the origin this zone stamped on the emitting cluster, never anything the engine wrote: an
// engine runs the attacker's bundle, so a set it could write into another origin's partition would let one page
// zero the reward of another's discoveries. The engine's own scope (inside every digest) is what keeps two
// sites' relative addresses apart; the partition is the security boundary. Loaded by extension/bridge.js and,
// verbatim, by engine/trusted.mjs and engine/noveltygate.mjs.

/* partition origin -> { spelling, lines: [line…] in arrival order, has: Set of the same lines } */
const _noveltyParts = new Map();
/* Partitions whose lines grew since noveltyTakeDirty last ran, for the persistence edge. */
const _noveltyDirty = new Set();

/* Folds one result document's `_novelty` into `partition`; answers how many lines were new. A result under
   another spelling than the partition holds is a build that digests differently, so the partition is replaced
   rather than mixed: lines of two spellings never match each other. */
function noveltyAbsorb(partition, nov) {
  DCHECK(typeof partition === "string", "a novelty partition is not an origin string: " + String(partition));
  DCHECK(!!nov && typeof nov === "object" && typeof nov.spelling === "string" && nov.spelling !== "" &&
         nov.spelling.indexOf("\n") < 0 && Array.isArray(nov.identities),
         "a result document's `_novelty` is not {spelling, identities[]} — solver/novelty.c's novelty_json " +
         "writes both on every document, so this is a wasm built before that commit (build it) or that " +
         "composition changed under this reader");
  let p = _noveltyParts.get(partition);
  if (p === undefined || p.spelling !== nov.spelling) {
    p = { spelling: nov.spelling, lines: [], has: new Set() };
    _noveltyParts.set(partition, p);
  }
  let added = 0;
  for (const line of nov.identities) {
    DCHECK(typeof line === "string" && /^[a-z]+ [0-9a-f]{32}$/.test(line),
           "a `_novelty.identities` entry is not `<kind> <32 hex>`: " + JSON.stringify(line));
    if (p.has.has(line)) continue;
    p.has.add(line);
    p.lines.push(line);
    added++;
  }
  if (added) _noveltyDirty.add(partition);
  return added;
}

/* What an instance has not yet been handed of `partition`, as solver/novelty.h's set text, or null when it is
   owed nothing. `handed` is the instance's own record, `{spelling, n}`, `{spelling: null, n: 0}` before its
   first handover; it is advanced here, in the same turn the caller sends the text, so no line can be absorbed
   between the read and the record. */
function noveltyOwed(partition, handed) {
  DCHECK(!!handed && (handed.spelling === null || typeof handed.spelling === "string") &&
         Number.isInteger(handed.n) && handed.n >= 0,
         "an instance's novelty handover record is not {spelling, n}");
  const p = _noveltyParts.get(partition);
  if (p === undefined) return null;
  const from = handed.spelling === p.spelling ? handed.n : 0;
  DCHECK(from <= p.lines.length, "an instance was handed more novelty lines than its partition holds");
  if (handed.spelling === p.spelling && from === p.lines.length) return null;
  handed.spelling = p.spelling;
  handed.n = p.lines.length;
  let text = p.spelling + "\n";
  for (let i = from; i < p.lines.length; i++) text += p.lines[i] + "\n";
  return text;
}

/* The partitions that grew, as `{partition, spelling, lines}` records for the persistence edge, and forgets
   that they grew. */
function noveltyTakeDirty() {
  const out = [];
  for (const partition of _noveltyDirty) {
    const p = _noveltyParts.get(partition);
    out.push({ partition, spelling: p.spelling, lines: p.lines.slice() });
  }
  _noveltyDirty.clear();
  return out;
}

/* One persisted record back into the set, at the zone's start. The record is this zone's own write. */
function noveltyRestore(rec) {
  DCHECK(!!rec && typeof rec.partition === "string" && typeof rec.spelling === "string" &&
         Array.isArray(rec.lines), "a persisted novelty record is not {partition, spelling, lines}");
  noveltyAbsorb(rec.partition, { spelling: rec.spelling, identities: rec.lines });
  _noveltyDirty.delete(rec.partition);
}

/* Every partition gone: the Clear button's contract. */
function noveltyClear() {
  _noveltyParts.clear();
  _noveltyDirty.clear();
}
