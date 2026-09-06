// lib/persistence.js — IndexedDB persistence for the cross-session global store (the offscreen brain's stable
// home). Open/get/set/clear the uasr_store DB, plus (de)serialize + debounced save/load of globalStore.
// Extracted from the offscreen-brain.js monolith (one problem per file); loaded before it, functions resolve
// globalStore at call-time. chrome.storage.local stays banned (SECURITY.md) — IDB only.

// ─── Persistent Storage (IndexedDB) ─────────────────────────────────────────

const _IDB_NAME = "uasr_store";
const _IDB_VERSION = 1;
const _IDB_STORE = "global";

function _openIDB() {
  return new Promise((resolve, reject) => {
    const req = indexedDB.open(_IDB_NAME, _IDB_VERSION);
    req.onupgradeneeded = (e) => {
      const db = e.target.result;
      if (!db.objectStoreNames.contains(_IDB_STORE)) {
        db.createObjectStore(_IDB_STORE);
      }
    };
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}

function _idbGet(key) {
  return _openIDB().then(
    (db) =>
      new Promise((resolve, reject) => {
        const tx = db.transaction(_IDB_STORE, "readonly");
        const store = tx.objectStore(_IDB_STORE);
        const req = store.get(key);
        req.onsuccess = () => resolve(req.result);
        req.onerror = () => reject(req.error);
        tx.oncomplete = () => db.close();
      }),
  );
}

function _idbSet(key, value) {
  return _openIDB().then(
    (db) =>
      new Promise((resolve, reject) => {
        const tx = db.transaction(_IDB_STORE, "readwrite");
        const store = tx.objectStore(_IDB_STORE);
        store.put(value, key);
        tx.oncomplete = () => {
          db.close();
          resolve();
        };
        tx.onerror = () => {
          db.close();
          reject(tx.error);
        };
      }),
  );
}

function _idbClear() {
  return _openIDB().then(
    (db) =>
      new Promise((resolve, reject) => {
        const tx = db.transaction(_IDB_STORE, "readwrite");
        const store = tx.objectStore(_IDB_STORE);
        store.clear();
        tx.oncomplete = () => {
          db.close();
          resolve();
        };
        tx.onerror = () => {
          db.close();
          reject(tx.error);
        };
      }),
  );
}

let _saveTimer = null;

function scheduleSave() {
  if (_saveTimer) clearTimeout(_saveTimer);
  _saveTimer = setTimeout(saveGlobalStore, 2000);
}

/* ─── THE STORE'S OWN SHAPE ──────────────────────────────────────────────────────────────────────────────
   THE ONE FACT THIS STORE COULD NOT STATE, AND EVERY OTHER DEFECT HERE WAS DOWNSTREAM OF NOT STATING IT.

   The cumulative frontier is CLAUDE.md's ONE global, continuous, cross-session store — "the UNION of every
   flow from every origin ever visited, persisted in IndexedDB, never reset". Its records' shapes MOVE: this
   record gained `pathParamsForced` on 2026-08-31, and a discovery entry's `grouping` went from a field five
   consumers read through `|| null` to a field every producer states. Both changes are right and both are
   RETROACTIVE — a store written the day before either one holds records the day after cannot read.

   Until this stamp there was nothing anywhere that could tell "written by an earlier shape of this record"
   from "the producer in this session went silent", so the restore door asserted the second against records
   that were the first, and lib/endpoint-record.js's own text for `pathParamsForced` says what that costs in
   as many words: a store written before the field "carries no such key, and it must crash rather than be
   read as the absence". It is right that it must not be read as the absence — "stored by an older build" and
   "this run forced nothing" are different facts and only one of them is something the moat observed. What
   was missing is the third outcome between crashing and lying, and it is the one CLAUDE.md's §OOM/paging
   already names: a record whose RECIPE outlives its bytes is RE-DERIVABLE, so shedding it converts storage
   into recomputation and truncates nothing, and a record with no recipe is a REPORTED OVERAGE.

   BUMP THIS WHENEVER ANY RECORD IN THE STORE GAINS OR LOSES A STATED NAME. The number is not a version of
   this file — it is the shape of what the store HOLDS, so it moves when lib/endpoint-record.js's
   `ENDPOINT_ABSENT`/`_ENDPOINT_STATED`, lib/discovery-entry.js's `_DISCOVERY_STATED` or lib/store-record.js's
   `STORE_RECORD_KINDS` move, and nowhere else. `1` is the first stamped shape; a store with NO stamp is every
   store written before this line.

   AND THE STAMP IS A CLAIM ABOUT THE MAPS THE DOOR CHECKED WHEN IT WAS WRITTEN, WHICH IS WHY EACH KIND STATES
   ITS OWN. Shape `1` was written by a door that checked TWO of the seven maps: `apiKeys`, `probeResults`,
   `scopes`, `securityFindings` and `discoveryChanges` were copied straight into the live maps with nothing
   asked of them, so a store stamped `1` holds five maps' worth of records of whatever shape their producers
   had at the time — the failure this comment names one paragraph up, "a migration that skipped a record would
   leave one of an older shape under this stamp". Reading the stamp as a store-wide promise would therefore
   assert those five against a population nobody ever checked, and the first refusal would take the whole
   cumulative frontier: the exact defect the stamp exists to end, re-created by trusting it too widely. So
   lib/store-record.js states a `statedFrom` per kind and the door asks `storeRecordShapeStates`. `2` is the
   shape at which the other five began being described.

   `3` IS THE SHAPE AT WHICH `securityFindings` BEGAN DESCRIBING WHAT IS INSIDE `securitySinks`, and the reason
   is a hole this comment's own rule could not see. The rule above is to bump whenever a record GAINS OR LOSES A
   STATED NAME, and at shape `2` this kind stated `securitySinks: _srArr` — "is an Array", and nothing about the
   parked @S entries in it. So the ENGINE's serialized grammar for those entries moved twice, correctly owing no
   bump under the letter of that rule, while lib/popup-security.js began asserting the new names on every card
   it renders; a store stamped `2` then handed the card an entry from an older engine and the popup aborted.
   THE GENERAL FACT, WHICH OUTLIVES THIS BUMP: A STAMP CANNOT COVER A GRAMMAR ANOTHER COMPONENT OWNS. This
   number moves on an edit to this file and solve.h's parked grammar moves on a BUILD of the engine, so nothing
   binds them and no discipline about bumping can — the two releases are independent. What closes it is that
   lib/store-record.js's `_srParkedSinkCurrent` asks the stored BYTES for the names rather than trusting this
   number, and states those names as member READS, which is the one form engine/fieldgate.mjs audits against
   solve.c in both directions. The stamp still decides ASSERT-versus-ASK; it is no longer the only thing
   standing between an older engine's record and a card that asserts today's. */
const _STORE_SHAPE = 3;

/* WHAT THE RESTORE DID, AS NUMBERS — because a shed nobody can count is the silent truncation this whole
   section exists to end, and §NO BOUNDS is explicit that discarding work "with nothing to say so" is a cap
   however good the reason for discarding it was. The vocabulary is bridge.js's cold-tier residency, which
   makes the identical decision over the identical categories one store along: `shed` is work traded for
   recomputation, `stranded` is the only copy of itself and is the overage a preference about disk is not
   worth. `strandedKeys` carries the NAMES for the stranded half alone, because that is the half nothing can
   recompute — a shed record is named by the document that mints it again.
   `byKind` IS NOT A REFINEMENT OF THE TOTALS, IT IS WHAT KEEPS THEM FROM BEING AN AVERAGE. The store holds
   seven kinds of record and a shed endpoint, a shed API key and a shed drift history are three different
   losses; summed into one number they read as one, and CLAUDE.md §MEASURE is explicit that facts of different
   kinds must never be averaged into one figure. The totals stay because a stranded ANYTHING is the overage,
   whatever kind it was. */
const storeRestoreStats = {
  shape: null, kept: 0, shed: 0, stranded: 0, strandedKeys: [],
  byKind: Object.fromEntries(Object.keys(STORE_RECORD_KINDS).map((k) => [k, { kept: 0, shed: 0, stranded: 0 }])),
};

/* WHICH SHAPE THE STORE WE ARE READING STATES. Three answers and they are three different facts:
     a number ≤ _STORE_SHAPE — the store says what it is, so every record in it IS that shape and a record
       short of a name is OUR PRODUCER BROKEN. The assert stands, unsoftened, for the whole of this case.
     no stamp at all       — every store written before this stamp existed. Its records' shapes are not
       stated, so this is the one population where the door must ASK rather than assert. It is CLOSED (this
       build never writes an unstamped store), SELF-IDENTIFYING (a record states which names it carries), and
       TERMINATING (the first save after a restore restamps it), which is what separates it from the
       `if (bad) continue` §Architecture forbids — that one has no closing condition and reports nothing.
     a number > _STORE_SHAPE — a store written by a LATER build, met after a downgrade. Reading records of a
       shape nothing here describes and re-saving them under OUR stamp would write a store that lies about
       itself, which is data integrity, so it is a CHECK and fatal in release too (§Offensive programming). */
function _storedRecordShape(s) {
  if (!Object.prototype.hasOwnProperty.call(s, "recordShape")) return null;
  CHECK(typeof s.recordShape === "number" && Number.isInteger(s.recordShape) && s.recordShape >= 1,
        "the cumulative store states a record shape of " + JSON.stringify(s.recordShape) + ", which is not a " +
        "shape number — this zone is the store's only writer and it writes `_STORE_SHAPE`, so anything else " +
        "is the stored blob corrupt and every record under it is of an unknown shape");
  CHECK(s.recordShape <= _STORE_SHAPE,
        "the cumulative store was written by a LATER build (shape " + s.recordShape + ", this build reads " +
        _STORE_SHAPE + ") — its records may state names this build's record does not carry, and restoring " +
        "them would re-save them under this build's stamp, leaving a store whose declared shape and actual " +
        "record shape disagree with nothing left to detect it. The frontier is never reset, so proceeding " +
        "here corrupts every session after this one");
  return s.recordShape;
}

/* SHED OR STRAND ONE RECORD AN EARLIER SHAPE WROTE — the ONE accountant, shared by every record kind the
   store holds, because the law is one law: a record this build cannot read is either re-derivable, in which
   case its bytes are a copy and dropping them costs a re-visit, or it is the only copy of itself, in which
   case dropping them costs the work. Spelled per record kind it would be seven laws free to disagree about
   which — and WHICH of the two a given kind is stays with the kind, in lib/store-record.js's `recipe`, beside
   the reason it is or is not re-derivable. Two of the seven are re-derivable from nothing and say so there. */
function _shedByShape(kind, key, recipe, why) {
  const per = storeRestoreStats.byKind[kind];
  DCHECK(!!per, "a record was shed as the store kind `" + kind + "`, which the census does not hold a column " +
                "for — both are built from lib/store-record.js's one table, so a miss is that table having " +
                "been read twice from two places");
  if (recipe !== null) { storeRestoreStats.shed++; per.shed++; return; }
  storeRestoreStats.stranded++;
  per.stranded++;
  /* THE KIND TRAVELS WITH THE KEY, AND SO DOES THE REASON. A bare key names nothing a reader can act on once
     seven maps can produce one — two maps legitimately key on the same service name — and a stranded record's
     whole worth to the next reader is knowing WHAT this build could not read in it. */
  storeRestoreStats.strandedKeys.push(kind + " " + key + " — " + why);
}

/* THE OTHER SIDE OF THE SAME DOOR, AND IT IS WHAT MAKES THE STAMP TRUE RATHER THAN HOPEFUL. `recordShape` is
   a claim that every record under it is the shape lib/store-record.js describes, and until this the claim
   rested entirely on the RESTORE having refused to bring anything else in — which says nothing whatever about
   a record a producer in THIS session minted wrong and handed straight to the save. A record checked only on
   the way in is a store that can be corrupted by its own writer and re-read as authoritative for ever after,
   because the next restore trusts the stamp this save wrote. Same table, same predicates, same `where`
   discipline; the abort travels out through `saveGlobalStore`'s RETHROW_FATAL rather than being swallowed. */
function _projectChecked(kind, pairs) {
  const out = {};
  for (const [k, v] of pairs) {
    checkStoreRecord(kind, k, v, "lib/persistence.js writing the cumulative store's `" + kind + "` map to " +
                                 "IndexedDB");
    out[k] = v;
  }
  return out;
}

function _serializeGlobalStore() {
  return {
    /* ONE PROJECTION, SHARED WITH THE POPUP'S (lib/serialize.js `serializeApiKeyEntry`). This was a second
       copy of that function's body, and the copies disagreed about one field: it dropped `name`, the key
       TYPE lib/keys.js matched, so a key that survived a save/load came back with no type at all and the
       popup labelled it with its own `|| "API Key"` default. Two hand-maintained projections of one record
       is how a field goes missing on exactly one path; there is now one. */
    apiKeys: _projectChecked(
      "apiKeys",
      [...globalStore.apiKeys].map(([k, v]) => [k, serializeApiKeyEntry(v)]),
    ),
    endpoints: Object.fromEntries(globalStore.endpoints),
    /* NO `method`. It held "PROBE"/"HYBRID" for a document that was probed rather than fetched; that producer
       was DELETED (lib/discovery-probe.js records why: nothing projected the field to the popup, nothing merged
       it, nothing rehydrated it) and this `v.method || null` outlived it — a default over a field with no
       producer, written into IndexedDB every save. The one writer left is the popup's OpenAPI import
       (`method: "IMPORT"`, lib/popup-handlers.js), and no surface in this extension reads it either, so it is
       not carried across a session.
       THE REMAINING `|| null`s ARE OPTIONALITY, NOT DEFAULTS, and this is the positive statement they read
       as: a LEARNED entry has no fetched document behind it. lib/learn.js mints a virtual `status:"found"`
       entry from a request it observed — grouping + doc only — so `url`/`apiKey`/`fetchedAt` are absent for
       exactly that entry and their absence means "this service was never fetched from an address", while a
       FETCHED entry (lib/discovery-probe.js) writes all three.
       `grouping` IS NO LONGER AMONG THEM, AND THE SENTENCE THAT STOOD HERE ABOUT IT WAS WRONG. It said the
       field was "the mirror: present on a learned entry, absent on a fetched one", and that read the DROP as
       the design: a fetched entry's bucket is named by exactly the same classifier as a learned one's
       (lib/response-decode.js hands `extractInterfaceName`'s answer to the fetch), so what an absent field
       there recorded was the published-document fetch REPLACING lib/learn.js's record and stating nothing in
       its place. Every producer states it now, and this hop copies the statement (lib/discovery-entry.js). */
    discoveryDocs: Object.fromEntries(
      [...globalStore.discoveryDocs].map(([k, v]) => [
        k,
        (checkDiscoveryGrouping(v, "lib/persistence.js writing the cumulative store to IndexedDB, service " +
                                   JSON.stringify(k)),
        {
          status: v.status,
          url: v.url || null,
          apiKey: v.apiKey || null,
          fetchedAt: v.fetchedAt || null,
          doc: v.doc || null,
          grouping: v.grouping,
          isVirtual: !!v.isVirtual,
          pageUrls: [...(v.pageUrls instanceof Set ? v.pageUrls : v.pageUrls || [])],
          frameOrigins: [...(v.frameOrigins instanceof Set ? v.frameOrigins : v.frameOrigins || [])],
        }),
      ]),
    ),
    probeResults: _projectChecked("probeResults", globalStore.probeResults),
    scopes: _projectChecked("scopes", globalStore.scopes),
    securityFindings: _projectChecked("securityFindings", globalStore.securityFindings),
    discoveryChanges: _projectChecked("discoveryChanges", globalStore.discoveryChanges),
    /* THE STORE STATES WHAT SHAPE ITS RECORDS ARE. Written LAST because it is a claim about everything above
       it, and it is true from BOTH sides: `_deserializeIntoGlobalStore` either restored a record at this shape
       or did not restore it at all (a migration that skipped a record would leave one of an older shape under
       this stamp, which is the one way a stamp is worse than no stamp), and every line above has just asserted
       what it is about to write, so a producer in this session cannot put one under the stamp either. */
    recordShape: _STORE_SHAPE,
    /* NO `savedAt`. It was written into IndexedDB on every save and read by NOTHING — one occurrence in the
       whole tree, this write. §Architecture's rule names the mirror of the wrong-number defect exactly: "a
       producer emits a field nothing reads: a measurement that has never once been looked at". It is not the
       stamp's earlier form and cannot become one — a timestamp says WHEN a store was written and the door
       needs to know WHAT SHAPE it is, and dating the shape change is the archaeology `recordShape` exists to
       replace. (engine/fieldgate.mjs reports 0 write-no-reader for the extension area: the auditor anchors on
       the message and ABI seams, and a record that leaves this zone through IndexedDB rather than through a
       seam is outside what it can see — which is why this one was found by reading and not by the gate.) */
  };
}

/* THE DOOR. ONE LOOP OVER lib/store-record.js's ONE TABLE, and that is the change rather than a tidy-up of
   seven blocks that did the same thing seven ways. Five of those blocks did NOTHING but copy — `globalStore
   .probeResults.set(k, v)` and four lines exactly like it — so a corrupt, half-written or stale record in any
   of them restored in silence and was then read as a measurement, which is the mirror of the defect that took
   this whole store: `endpoints` asserted so hard one unreadable record discarded the frontier, while these
   five asserted nothing at all. CLAUDE.md §Offensive programming decides which way that error runs — "too FEW
   asserts is the failure" — and it also decides that the refusal must have somewhere to go, which is what the
   three-way split below is.

   THE STORE IS WHERE A RECORD CAN ARRIVE SHORT OF A NAME WITH NO PRODUCER IN THIS SESSION HAVING GONE WRONG.
   Everything else in the extension gets its records from a producer in this same run; these came out of
   IndexedDB, possibly written by a build whose record shape differed, and from here they are indistinguishable
   from freshly minted ones. Checked once on the way IN rather than at each surface, because the alternative is
   every reader carrying a `||` for a gap only this door can produce — which is exactly how `e.method || "GET"`
   came to stand between the cumulative moat and `netdiff --unused`. */
function _deserializeIntoGlobalStore(s) {
  /* THE SHAPE IS READ ONCE, BEFORE ANY RECORD IS, because it decides the question every checked record below
     is asked — asserted against this build's shape where the store states one, asked where it states none. */
  const shape = _storedRecordShape(s);
  storeRestoreStats.shape = shape;
  /* A MAP IN THE BLOB THAT NO KIND DESCRIBES IS THE ONE THING THIS LOOP CANNOT SILENTLY DO RIGHT. The loop
     restores what the table names, so a map added to `_serializeGlobalStore` without a declaration would be
     WRITTEN every save and never read back — the write-with-no-reader defect, produced by the very structure
     that ended the read-with-no-writer one. Asked here rather than left to a reader, and `STORE_RETIRED_KEYS`
     is what keeps the question answerable: a name a store legitimately still carries after its map was
     deleted is a different fact from a name nobody declared. */
  for (const name of Object.keys(s)) {
    DCHECK(name === "recordShape" || Object.prototype.hasOwnProperty.call(STORE_RECORD_KINDS, name) ||
           STORE_RETIRED_KEYS.indexOf(name) >= 0,
           "the cumulative store holds a `" + name + "` this build's record table does not declare and does " +
           "not list as retired — either `_serializeGlobalStore` writes a map lib/store-record.js has no " +
           "shape for (in which case it is saved every session and restored never), or a map was deleted " +
           "without its name joining STORE_RETIRED_KEYS (in which case every store written before that " +
           "deletion aborts here)");
  }
  for (const kind of Object.keys(STORE_RECORD_KINDS)) {
    const stored = s[kind];
    if (!stored) continue;
    const target = globalStore[kind];
    DCHECK(target instanceof Map,
           "the cumulative store declares the record kind `" + kind + "` and globalStore has no Map of that " +
           "name — the table names the live map as well as the stored one, so a miss is the two having been " +
           "renamed apart and every record of that kind would restore into nothing");
    /* THE THREE-WAY SPLIT, ASKED PER KIND. A stamped store's records ARE the shape it names for the kinds it
       was describing when it was written, so a record short of one is OUR producer broken and the assert
       stands whole. Where the store does not state THIS kind's shape — no stamp at all, or a stamp from
       before this kind was described — the door ASKS, and a record it cannot read is disposed of by
       §OOM/paging's third category rather than dropped: shed where a recipe outlives its bytes, STRANDED and
       named where the bytes are the only copy. That population is closed (this build writes a stamp that
       states every kind), self-identifying, terminating (the first save restamps it) and REPORTED, which is
       what separates it from the `if (bad) continue` §Architecture forbids. */
    const states = storeRecordShapeStates(kind, shape);
    for (const [k, v] of Object.entries(stored)) {
      if (!states) {
        const why = storeRecordUnreadable(kind, k, v);
        if (why !== null) { _shedByShape(kind, k, storeRecordRecipe(kind, k, v), why); continue; }
      }
      checkStoreRecord(kind, k, v,
                       "lib/persistence.js restoring the cumulative store's `" + kind + "` map");
      target.set(k, storeRecordAdopt(kind, v));
      storeRestoreStats.kept++;
      storeRestoreStats.byKind[kind].kept++;
    }
  }
}

async function saveGlobalStore() {
  _saveTimer = null;
  try {
    await _idbSet("gapiStore", _serializeGlobalStore());
  } catch (e) {
    /* AN INVARIANT ABORT TRAVELS ON THROUGH HERE (extension/check.js RETHROW_FATAL). `_serializeGlobalStore`
       asserts every discovery entry it projects, and this catch is otherwise the place those assertions are
       locally disabled — check.h aborts the process, so a C `goto fail` cannot swallow a DCHECK, and on this
       side an assertion is a THROW, which any catch converts into a plausible answer. The catch keeps its
       real job: an IndexedDB write can fail for reasons that are not our logic (quota, a closing document),
       and that is a state rather than a bug. */
    RETHROW_FATAL(e);
    console.error("[Storage] Save failed:", e);
  }
}

async function loadGlobalStore() {
  // Learned data persists in IndexedDB (the offscreen has it + a stable
  // lifetime). chrome.storage is NOT used: chrome.storage.local is banned, and
  // chrome.storage is not even exposed to the offscreen document — a previous
  // chrome.storage.local read here THREW before the IndexedDB restore ran, so a
  // recreated offscreen came up with an empty store and the popup saw nothing.
  try {
    const s = await _idbGet("gapiStore");
    if (s) _deserializeIntoGlobalStore(s);
  } catch (e) {
    /* AN INVARIANT ABORT TRAVELS ON THROUGH HERE, AND THIS IS THE CATCH THAT COST THE MOST. Every record
       check in `_deserializeIntoGlobalStore` throws, and this swallowed all of them into a console line —
       so a single record the current shape could not read abandoned the restore WHERE IT STOOD. The order in
       that function is the blast radius: apiKeys, endpoints, discoveryDocs, probeResults, scopes,
       securityFindings, discoveryChanges, so a refusal at the second took the other five ENTIRELY, plus
       every endpoint after the offending key. `loadGlobalStore` then RESOLVED, `_globalStoreReady` resolved
       with it, every handler proceeded against the truncated store, and lib/merge.js's `scheduleSave` wrote
       it back over `gapiStore` two seconds later — the cumulative frontier, which CLAUDE.md says is never
       reset, permanently replaced by whatever survived, with a console line as the only record.
       The catch keeps its real job: an IndexedDB OPEN or READ can fail for reasons that are not our logic. */
    RETHROW_FATAL(e);
    console.error("[Storage] Load failed:", e);
  }
}

// UNION a page's discovery doc INTO the existing global doc so the cumulative
// cross-site frontier ACCUMULATES methods (and their path-param example values)
// across pages/sessions. Previously mergeToGlobal set `doc: v.doc`, so the last
// same-host page's doc REPLACED the prior one — silently dropping every earlier
// page's learned endpoints + concrete values (the moat's whole cumulative point).
