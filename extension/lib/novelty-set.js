// lib/novelty-set.js — the product's known identities, per agent-cluster origin, and what each engine instance
// is still owed of them.
//
// engine/host/solver/novelty.h defines an emission's identity and is its only speller: every result document
// carries `_novelty.identities`, one `<kind> <32 hex>` line per identity that instance emitted (a counted kind,
// a drive of uncalled code, adds ` <count>`, that instance's own drives of it), and this file keeps their union,
// verbatim, under the spelling the engine stated. An instance is handed the union of its own partition at boot
// and incrementally (`NoveltyLearn`): it pays a credit only for an identity absent from it, and lowers a drive's
// optimism by the count.
//
// The partition is the origin this zone stamped on the emitting cluster, never anything the engine wrote: an
// engine runs the attacker's bundle, so a set it could write into another origin's partition would let one page
// zero the reward of another's discoveries. The engine's own scope (inside every digest) is what keeps two
// sites' relative addresses apart; the partition is the security boundary. Loaded by extension/bridge.js and,
// verbatim, by engine/trusted.mjs and engine/noveltygate.mjs.

/* partition origin -> { spelling, lines: [line…] in arrival order, plain: Set of the uncounted lines,
   counts: Map from a counted line's `<kind> <hex>` to the product's total } */
const _noveltyParts = new Map();
/* Partitions whose lines grew since noveltyTakeDirty last ran, for the persistence edge. */
const _noveltyDirty = new Set();

/* Folds one result document's `_novelty` into `partition`; answers how many lines were new. `counted` is the
   emitting instance's own record (a Map, `<kind> <hex>` -> the count it last reported), because a result is
   re-composed whole on every partial: a counted line adds what the instance's count rose by since its last
   result, so one drive is counted once however many results repeat it. A result under another spelling than the
   partition holds is a build that digests differently, so the partition is replaced rather than mixed. */
function noveltyAbsorb(partition, nov, counted) {
  DCHECK(typeof partition === "string", "a novelty partition is not an origin string: " + String(partition));
  /* Asked by shape and not by `instanceof`: engine/trusted.mjs loads this file into a realm of its own and
     hands it a Map of its realm. */
  DCHECK(!!counted && typeof counted.has === "function" && typeof counted.set === "function",
         "a novelty absorb was handed no per-instance count record");
  DCHECK(!!nov && typeof nov === "object" && typeof nov.spelling === "string" && nov.spelling !== "" &&
         nov.spelling.indexOf("\n") < 0 && Array.isArray(nov.identities),
         "a result document's `_novelty` is not {spelling, identities[]} — solver/novelty.c's novelty_json " +
         "writes both on every document, so this is a wasm built before that commit (build it) or that " +
         "composition changed under this reader");
  let p = _noveltyParts.get(partition);
  if (p === undefined || p.spelling !== nov.spelling) {
    p = { spelling: nov.spelling, lines: [], plain: new Set(), counts: new Map() };
    _noveltyParts.set(partition, p);
  }
  let added = 0;
  for (const line of nov.identities) {
    const m = typeof line === "string" ? /^([a-z]+ [0-9a-f]{32})(?: ([1-9][0-9]*))?$/.exec(line) : null;
    DCHECK(m !== null, "a `_novelty.identities` entry is not `<kind> <32 hex>[ <count>]`: " + JSON.stringify(line));
    if (m === null) continue;
    if (m[2] === undefined) {
      if (p.plain.has(line)) continue;
      p.plain.add(line);
      p.lines.push(line);
      added++;
      continue;
    }
    const key = m[1], n = Number(m[2]);
    const last = counted.has(key) ? counted.get(key) : 0;
    DCHECK(n >= last, "an instance reported fewer drives of " + key + " than its previous result did");
    if (n <= last) continue;
    counted.set(key, n);
    const total = (p.counts.has(key) ? p.counts.get(key) : 0) + (n - last);
    p.counts.set(key, total);
    p.lines.push(key + " " + total);
    added++;
  }
  if (added) _noveltyDirty.add(partition);
  return added;
}

/* What an instance has not yet been handed of `partition`, as solver/novelty.h's set text, or null when it is
   owed nothing. `handed` is the instance's own record, `{spelling, n}`, `{spelling: null, n: 0}` before its
   first handover; it is advanced here, in the same turn the caller sends the text, so no line can be absorbed
   between the read and the record. `counted` is the same instance's absorb record: a counted line is handed
   less what that instance reported, because the engine adds its own drives to what it is handed, and a line
   left at zero is not handed. A count that rose is a later line; the engine keeps the largest. */
function noveltyOwed(partition, handed, counted) {
  DCHECK(!!handed && (handed.spelling === null || typeof handed.spelling === "string") &&
         Number.isInteger(handed.n) && handed.n >= 0,
         "an instance's novelty handover record is not {spelling, n}");
  DCHECK(!!counted && typeof counted.has === "function" && typeof counted.get === "function",
         "a novelty handover was given no per-instance count record");
  const p = _noveltyParts.get(partition);
  if (p === undefined) return null;
  const from = handed.spelling === p.spelling ? handed.n : 0;
  DCHECK(from <= p.lines.length, "an instance was handed more novelty lines than its partition holds");
  if (handed.spelling === p.spelling && from === p.lines.length) return null;
  handed.spelling = p.spelling;
  handed.n = p.lines.length;
  let text = p.spelling + "\n";
  for (let i = from; i < p.lines.length; i++) {
    const line = p.lines[i];
    if (p.plain.has(line)) { text += line + "\n"; continue; }
    const cut = line.lastIndexOf(" "), key = line.slice(0, cut);
    const others = Number(line.slice(cut + 1)) - (counted.has(key) ? counted.get(key) : 0);
    DCHECK(others >= 0, "an instance reported more drives of " + key + " than its partition counts in all");
    if (others > 0) text += key + " " + others + "\n";
  }
  return text;
}

/* The partitions that grew, as `{partition, spelling, lines}` records for the persistence edge, each counted
   identity once at its current total, and forgets that they grew. */
function noveltyTakeDirty() {
  const out = [];
  for (const partition of _noveltyDirty) {
    const p = _noveltyParts.get(partition);
    const lines = [...p.plain];
    for (const [key, total] of p.counts) lines.push(key + " " + total);
    out.push({ partition, spelling: p.spelling, lines });
  }
  _noveltyDirty.clear();
  return out;
}

/* One persisted record back into the set, at the zone's start. The record is this zone's own write; its counted
   lines carry totals, so each is folded as one reporter's whole count. */
function noveltyRestore(rec) {
  DCHECK(!!rec && typeof rec.partition === "string" && typeof rec.spelling === "string" &&
         Array.isArray(rec.lines), "a persisted novelty record is not {partition, spelling, lines}");
  noveltyAbsorb(rec.partition, { spelling: rec.spelling, identities: rec.lines }, new Map());
  _noveltyDirty.delete(rec.partition);
}

/* Every partition gone: the Clear button's contract. */
function noveltyClear() {
  _noveltyParts.clear();
  _noveltyDirty.clear();
}
