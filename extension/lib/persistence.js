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
    apiKeys: Object.fromEntries(
      [...globalStore.apiKeys].map(([k, v]) => [
        k,
        {
          origin: v.origin,
          referer: v.referer,
          source: v.source,
          firstSeen: v.firstSeen,
          lastSeen: v.lastSeen,
          requestCount: v.requestCount || 0,
          services: [
            ...(v.services instanceof Set ? v.services : v.services || []),
          ],
          hosts: [...(v.hosts instanceof Set ? v.hosts : v.hosts || [])],
          endpoints: [
            ...(v.endpoints instanceof Set ? v.endpoints : v.endpoints || []),
          ],
          pageUrls: [
            ...(v.pageUrls instanceof Set ? v.pageUrls : v.pageUrls || []),
          ],
        },
      ]),
    ),
    endpoints: Object.fromEntries(globalStore.endpoints),
    discoveryDocs: Object.fromEntries(
      [...globalStore.discoveryDocs].map(([k, v]) => [
        k,
        {
          status: v.status,
          url: v.url || null,
          method: v.method || null,
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
    scriptCache: Object.fromEntries(globalStore.scriptCache),
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
  if (s.scriptCache) {
    var TTL = 30 * 24 * 60 * 60 * 1000; // 30 days
    var now = Date.now();
    var entries = Object.entries(s.scriptCache);
    // Evict expired entries
    entries = entries.filter(function([_, v]) { return now - v.timestamp < TTL; });
    // Cap at 500 entries (LRU by timestamp)
    if (entries.length > 500) {
      entries.sort(function(a, b) { return b[1].timestamp - a[1].timestamp; });
      entries = entries.slice(0, 500);
    }
    for (const [k, v] of entries)
      globalStore.scriptCache.set(k, v);
  }
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
