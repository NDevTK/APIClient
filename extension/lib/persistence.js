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
       THE FIVE REMAINING `|| null`s ARE OPTIONALITY, NOT DEFAULTS, and this is the positive statement they
       read as: a LEARNED entry has no fetched document behind it. lib/learn.js mints a virtual `status:"found"`
       entry from a request it observed — grouping + doc only — so `url`/`apiKey`/`fetchedAt` are absent for
       exactly that entry and their absence means "this service was never fetched from an address", while a
       FETCHED entry (lib/discovery-probe.js) writes all three. `grouping` is the mirror: present on a learned
       entry, absent on a fetched one. */
    discoveryDocs: Object.fromEntries(
      [...globalStore.discoveryDocs].map(([k, v]) => [
        k,
        {
          status: v.status,
          url: v.url || null,
          apiKey: v.apiKey || null,
          fetchedAt: v.fetchedAt || null,
          doc: v.doc || null,
          grouping: v.grouping || null,
          isVirtual: !!v.isVirtual,
          pageUrls: [...(v.pageUrls instanceof Set ? v.pageUrls : v.pageUrls || [])],
          frameOrigins: [...(v.frameOrigins instanceof Set ? v.frameOrigins : v.frameOrigins || [])],
        },
      ]),
    ),
    probeResults: Object.fromEntries(globalStore.probeResults),
    scopes: Object.fromEntries(globalStore.scopes),
    securityFindings: Object.fromEntries(globalStore.securityFindings),
    discoveryChanges: Object.fromEntries(globalStore.discoveryChanges),
    savedAt: Date.now(),
  };
}

function _deserializeIntoGlobalStore(s) {
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
  if (s.endpoints) {
    for (const [k, v] of Object.entries(s.endpoints))
      globalStore.endpoints.set(k, v);
  }
  if (s.discoveryDocs) {
    for (const [k, v] of Object.entries(s.discoveryDocs)) {
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
  } catch (_) {
    console.error("[Storage] Save failed:", _);
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
  } catch (_) {
    console.error("[Storage] Load failed:", _);
  }
}

// UNION a page's discovery doc INTO the existing global doc so the cumulative
// cross-site frontier ACCUMULATES methods (and their path-param example values)
// across pages/sessions. Previously mergeToGlobal set `doc: v.doc`, so the last
// same-host page's doc REPLACED the prior one — silently dropping every earlier
// page's learned endpoints + concrete values (the moat's whole cumulative point).
