// lib/document-record.js — the trusted zone's record of how each document it provisioned was reached.
//
// A document's record is three facts about the navigation that produced it: its reach grade (the three
// provenance words, CLAUDE.md's "a request carries the provenance of its values"), its drive mark (whether a
// drive of uncalled code made it) and its witness mark (whether its address rested on a value a fork pinned).
// The zone writes one per document at provisioning, from the provisioning notice joined with the creating
// instance's record, and never again. lib/safe-fetch.js reads it as a floor under the engine's per-request
// words, so an engine whose words are wrong can narrow what fires and never widen it.
//
// Records are frozen and minted only by docRecord; `_DOC_RECORDS` is how a reader tells a record from an
// object with the same keys. The wire form is engine.h's arrival word, `<reach>,<drive mark>,<witness mark>`,
// which is also what the engine starts the document's timelines with.

var _DOC_REACH = ["observed", "derived", "forced"];
var _DOC_DRIVEN = ["driven", "undriven"];
var _DOC_PINNED = ["pinned", "unpinned"];
/* How two reach grades compose: a child of a document this tool chose to open is itself such a document, so
   the composition is the weaker grade. The order is an explicit map, not array positions, so a word added to
   the vocabulary cannot silently re-rank the others. */
var _DOC_REACH_RANK = { observed: 0, derived: 1, forced: 2 };
var _DOC_RECORDS = new WeakSet();

function docRecord(reach, driven, pinned) {
  CHECK(_DOC_REACH.indexOf(reach) >= 0 && typeof _DOC_REACH_RANK[reach] === "number" &&
        _DOC_DRIVEN.indexOf(driven) >= 0 && _DOC_PINNED.indexOf(pinned) >= 0,
        "a document record was minted from " + JSON.stringify([reach, driven, pinned]) + " — a ranked reach " +
        "grade, `driven`/`undriven` and `pinned`/`unpinned` are the only words, because each row the record " +
        "floors reads one");
  /* A person names an observed document by no drive and no pin, so `observed` with a mark is two statements
     about one navigation that cannot both be true. */
  CHECK(reach !== "observed" || (driven === "undriven" && pinned === "unpinned"),
        "a document record states `observed` with a drive or witness mark — an observed document is one a " +
        "person named, which no drive navigated to and no pin addressed");
  var r = Object.freeze({ reach: reach, driven: driven, pinned: pinned });
  _DOC_RECORDS.add(r);
  return r;
}

function docRecordIs(r) {
  return _DOC_RECORDS.has(r);
}

/* The stronger-wins join: the weaker reach, and a mark either record carries. A child's record is its
   creator's joined with its own navigation's, so it inherits every mark above it, and an instance's record is
   the join of its documents'. The join can only narrow what either input permits. */
function docRecordJoin(a, b) {
  CHECK(_DOC_RECORDS.has(a) && _DOC_RECORDS.has(b),
        "two document records were joined and one was not minted by docRecord");
  var j = docRecord(_DOC_REACH_RANK[a.reach] >= _DOC_REACH_RANK[b.reach] ? a.reach : b.reach,
                    a.driven === "driven" || b.driven === "driven" ? "driven" : "undriven",
                    a.pinned === "pinned" || b.pinned === "pinned" ? "pinned" : "unpinned");
  /* Permanence, asserted at the one writer that derives a record from others: nothing a document does
     relaxes a mark its navigation carries, so the result is at least as strong as each input. */
  CHECK(_docRecordCovers(j, a) && _docRecordCovers(j, b),
        "a joined document record cleared a mark or strengthened the reach of " + JSON.stringify([a, b]) +
        " — the join can only narrow");
  return j;
}
/* Whether `r` carries every mark `of` carries and a reach no stronger. */
function _docRecordCovers(r, of) {
  return _DOC_REACH_RANK[r.reach] >= _DOC_REACH_RANK[of.reach] &&
         (of.driven !== "driven" || r.driven === "driven") && (of.pinned !== "pinned" || r.pinned === "pinned");
}

/* engine.h's arrival word, parsed and serialized. */
function docRecordOfArrival(arrival) {
  var w = typeof arrival === "string" ? arrival.split(",") : [];
  CHECK(w.length === 3, "a document record was asked of the arrival " + JSON.stringify(arrival) +
                        ", which is not engine.h's three comma-joined words");
  return docRecord(w[0], w[1], w[2]);
}
function docRecordArrival(r) {
  CHECK(_DOC_RECORDS.has(r), "an arrival was asked of something docRecord did not mint");
  return r.reach + "," + r.driven + "," + r.pinned;
}

if (typeof self !== "undefined") {
  self.docRecord = docRecord;
  self.docRecordIs = docRecordIs;
  self.docRecordJoin = docRecordJoin;
  self.docRecordOfArrival = docRecordOfArrival;
  self.docRecordArrival = docRecordArrival;
}
