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
   `ENDPOINT_ABSENT`/`_ENDPOINT_STATED` or lib/discovery-entry.js's `_DISCOVERY_STATED` move, and nowhere
   else. `1` is the first stamped shape; a store with NO stamp is every store written before this line. */
const _STORE_SHAPE = 1;

/* WHAT THE RESTORE DID, AS NUMBERS — because a shed nobody can count is the silent truncation this whole
   section exists to end, and §NO BOUNDS is explicit that discarding work "with nothing to say so" is a cap
   however good the reason for discarding it was. The vocabulary is bridge.js's cold-tier residency, which
   makes the identical decision over the identical categories one store along: `shed` is work traded for
   recomputation, `stranded` is the only copy of itself and is the overage a preference about disk is not
   worth. `strandedKeys` carries the NAMES for the stranded half alone, because that is the half nothing can
   recompute — a shed record is named by the document that mints it again. */
const storeRestoreStats = { shape: null, kept: 0, shed: 0, stranded: 0, strandedKeys: [] };

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

/* SHED OR STRAND ONE RECORD AN EARLIER SHAPE WROTE — the ONE accountant, shared by both record kinds,
   because the law is one law: a record this build cannot read is either re-derivable, in which case its
   bytes are a copy and dropping them costs a re-visit, or it is the only copy of itself, in which case
   dropping them costs the work. Spelled per record kind it would be two laws free to disagree about which. */
function _shedByShape(key, recipe) {
  if (recipe !== null) { storeRestoreStats.shed++; return; }
  storeRestoreStats.stranded++;
  storeRestoreStats.strandedKeys.push(key);
}

function _serializeGlobalStore() {
  return {
    /* ONE PROJECTION, SHARED WITH THE POPUP'S (lib/serialize.js `serializeApiKeyEntry`). This was a second
       copy of that function's body, and the copies disagreed about one field: it dropped `name`, the key
       TYPE lib/keys.js matched, so a key that survived a save/load came back with no type at all and the
       popup labelled it with its own `|| "API Key"` default. Two hand-maintained projections of one record
       is how a field goes missing on exactly one path; there is now one. */
    apiKeys: Object.fromEntries(
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
    probeResults: Object.fromEntries(globalStore.probeResults),
    scopes: Object.fromEntries(globalStore.scopes),
    securityFindings: Object.fromEntries(globalStore.securityFindings),
    discoveryChanges: Object.fromEntries(globalStore.discoveryChanges),
    /* THE STORE STATES WHAT SHAPE ITS RECORDS ARE. Written LAST because it is a claim about everything above
       it, and it is only true because every branch of `_deserializeIntoGlobalStore` either restored a record
       at this shape or did not restore it at all — a migration that skipped a record would leave one of an
       older shape under this stamp, which is the one way a stamp is worse than no stamp. */
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

function _deserializeIntoGlobalStore(s) {
  /* THE SHAPE IS READ ONCE, BEFORE ANY RECORD IS, because it decides the question every checked record below
     is asked — asserted against this build's shape where the store states one, asked where it states none. */
  const shape = _storedRecordShape(s);
  storeRestoreStats.shape = shape;
  if (s.apiKeys) {
    for (const [k, v] of Object.entries(s.apiKeys)) {
      globalStore.apiKeys.set(k, {
        ...v,
        services: new Set(v.services || []),
        hosts: new Set(v.hosts || []),
        endpoints: new Set(v.endpoints || []),
        pageUrls: new Set(v.pageUrls || []),
      });
    }
  }
  /* THE STORE IS THE OTHER PLACE A RECORD CAN ARRIVE SHORT OF A NAME. Everything else in the extension gets
     an endpoint from lib/merge.js's constructor in this same session; these came out of IndexedDB, possibly
     written by a build whose record shape differed, and from here they are indistinguishable from freshly
     minted ones. Checked on the way IN rather than at each surface, because the alternative is every reader
     carrying a `||` for a gap only this door can produce — which is exactly how `e.method || "GET"` came to
     stand between the cumulative moat and `netdiff --unused`. */
  if (s.endpoints) {
    for (const [k, v] of Object.entries(s.endpoints)) {
      const where = "lib/persistence.js restoring the cumulative store, key " + JSON.stringify(k);
      /* THE MIGRATION, AND IT RUNS FOR EXACTLY ONE POPULATION. An UNSTAMPED store is the only one whose
         records' shapes are not stated, so it is the only one where "this record lacks a name" has a cause
         that is not our producer broken. `endpointRecordMissingNames` derives that question from the same
         two declarations `makeEndpointRecord` builds every record from, so it answers for a name added
         tomorrow with nothing edited here — and it answers `[]` for a record that carries every name and
         holds a wrong VALUE, which is the producer broken and still aborts on the line below.
         A STAMPED STORE DOES NOT REACH THIS AT ALL: `shape` is a number, the assert stands whole, and the
         moment this session's first save restamps the store the unstamped path is dead for it for ever. */
      if (shape === null && endpointRecordMissingNames(v, where).length > 0) {
        _shedByShape(k, endpointRecordRecipe(v));
        continue;
      }
      checkEndpointRecord(v, where);
      globalStore.endpoints.set(k, v);
      storeRestoreStats.kept++;
    }
  }
  if (s.discoveryDocs) {
    for (const [k, v] of Object.entries(s.discoveryDocs)) {
      /* THE SAME DOOR THE ENDPOINT RECORDS ARE CHECKED AT, for the same reason: from here a store written by
         a build whose record shape differed is indistinguishable from an entry this session's producers
         minted, and every surface downstream reads `grouping` off it. Checked once on the way IN rather than
         by each surface carrying a `||` for a gap only this door can produce. */
      const dwhere = "lib/persistence.js restoring the cumulative store, service " + JSON.stringify(k);
      /* THE SAME MIGRATION OVER THE OTHER RECORD KIND, and it is here because the defect is the door's and
         not the endpoint record's: `grouping` was optional until the day every producer was made to state
         it, so a store written on the earlier side of that day aborts this check exactly as one written
         before `pathParamsForced` aborts the one above. Covering one door and not the other would leave the
         whole cumulative store taken by the second — the restore is one operation and it fails whole. */
      if (shape === null && discoveryEntryMissingNames(v).length > 0) {
        _shedByShape(k, discoveryEntryRecipe(v));
        continue;
      }
      checkDiscoveryGrouping(v, dwhere);
      globalStore.discoveryDocs.set(k, {
        ...v,
        pageUrls: new Set(v.pageUrls || []),
        frameOrigins: new Set(v.frameOrigins || []),
      });
    }
  }
  if (s.probeResults) {
    for (const [k, v] of Object.entries(s.probeResults))
      globalStore.probeResults.set(k, v);
  }
  if (s.scopes) {
    for (const [k, v] of Object.entries(s.scopes))
      globalStore.scopes.set(k, v);
  }
  if (s.securityFindings) {
    for (const [k, v] of Object.entries(s.securityFindings))
      globalStore.securityFindings.set(k, v);
  }
  /* NO scriptCache REHYDRATION. The replay cache it restored is deleted (see globalStore): it was a
     document-identity seen-set whose hit skipped the engine entirely, and its TTL + 500-entry LRU trim here
     are what a cache needs and a frontier must never have — the frontier drops nothing and is aged only by
     value. A persisted store written before this change simply carries a key nothing reads. */
  /* discoveryChanges IS REHYDRATED, unlike the line above, and the difference is that it HAS A WRITER. It is
     the record of every time a service's published API surface changed between two discovery fetches
     (lib/discovery-probe.js), which is a finding and not a cache: nothing about it decides whether work runs,
     so restoring it restores an observation rather than a skip. */
  if (s.discoveryChanges) {
    for (const [k, v] of Object.entries(s.discoveryChanges))
      globalStore.discoveryChanges.set(k, v);
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
