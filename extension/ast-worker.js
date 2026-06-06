// ast-worker.js — owns the analysis Web Worker POOL inside the offscreen
// document. The learning brain (offscreen-brain.js) loads in the SAME document
// and calls self.astDispatch(msg) directly — chrome.runtime.sendMessage can't
// reach a same-document listener (the sender's own context is excluded from
// the broadcast). Deep-grind results streamed by any pool worker are merged
// by calling the brain's self._mergeDeepResult directly.
//
// POOL: by default ONE worker (serial, cool). The user's "max analyzer
// workers" UI setting (SET_ANALYSIS_OPTS.maxWorkers) grows or shrinks the
// pool — real OS-level parallelism on multi-core machines, paid for in
// heat + memory (each worker holds its own wasm linear memory). Sticky
// routing by pageKey: a page's reviews always land on the same worker, so
// per-page state (its _fiberQ, its in-flight grind) lives consistently in
// one worker. New pages round-robin onto the least-loaded worker so heat
// spreads across cores.

var _pool = [];                 // [{ worker, pending: Map, assignedPages: Set, busy: int }]
var _nextId = 0;
var _pageToWorker = new Map();  // pageKey → poolIndex (sticky routing)

function _makeWorker() {
  var w = new Worker("ast-thread.js");
  var slot = { worker: w, pending: new Map(), assignedPages: new Set(), busy: 0, liveness: null, livenessRecvTs: 0 };
  w.onmessage = function (e) {
    if (e.data && e.data._heartbeat) {
      // Liveness observability: record this slot's last heartbeat + when WE
      // received it. A growing receive-age (vs the worker's own ts) means the
      // worker's event loop is blocked in a sync C call (no macrotask can run
      // there); a fresh heartbeat with a stale lastGrindProgressTs means the
      // loop is alive but the grind fiber isn't being resumed. Read via
      // self._poolLiveness() from the offscreen harness command.
      slot.liveness = e.data;
      slot.livenessRecvTs = Date.now();
      return;
    }
    if (e.data && e.data._resumed) {
      try { if (typeof self._mergeDeepResult === "function") self._mergeDeepResult(e.data.documentId || null, e.data.sourceUrl || "", e.data.response && e.data.response.result, true); }
      catch (err) { console.warn("[ast-worker] _mergeDeepResult (resumed) failed:", err && err.message || err); }
      return;
    }
    if (e.data && e.data._partial) {
      try { if (typeof self._mergeDeepResult === "function") self._mergeDeepResult(e.data.documentId || null, e.data.sourceUrl || "", e.data.response && e.data.response.result, false); }
      catch (err) { console.warn("[ast-worker] _mergeDeepResult (partial) failed:", err && err.message || err); }
      return;
    }
    var entry = slot.pending.get(e.data._id);
    if (entry) {
      slot.pending.delete(e.data._id);
      if (slot.busy > 0) slot.busy--;
      var cb = (entry && typeof entry === "object" && typeof entry.resolve === "function") ? entry.resolve : entry;
      cb(e.data.response);
    }
  };
  w.onerror = function (e) {
    // A worker crash rejects every in-flight call on THAT slot; the brain
    // can re-queue. Other pool slots keep running.
    slot.pending.forEach(function (entry) {
      var cb = (entry && typeof entry === "object" && typeof entry.resolve === "function") ? entry.resolve : entry;
      try { cb({ success: false, error: "Worker error: " + (e.message || "unknown") }); }
      catch (err) { console.warn("[ast-worker] error callback throw:", err && err.message || err); }
    });
    slot.pending.clear();
    slot.busy = 0;
  };
  return slot;
}

function _ensurePoolSize(n) {
  if (!(n > 0)) n = 1;
  while (_pool.length < n) _pool.push(_makeWorker());
  while (_pool.length > n) {
    var slot = _pool.pop();
    try { slot.worker.terminate(); }
    catch (e) { console.warn("[ast-worker] terminate (shrink) failed:", e && e.message || e); }
    /* Re-route in-flight work to a remaining slot rather than rejecting.
       Shrinking the pool (user lowered the worker-count UI) should not
       lose pending analyses — the brain would fall back to per-script
       re-analysis on a "pool shrunk" rejection and flood the smaller
       pool right back. The slot.pending values are NOW {resolve, msg}
       so we can re-dispatch the original message onto a surviving slot
       via astDispatch. */
    slot.pending.forEach(function (entry) {
      if (entry && typeof entry === "object" && typeof entry.resolve === "function" && entry.msg) {
        if (_pool.length === 0) {
          /* All workers shrunk away (n was 0 — caller still gets 1 via the
             min above, so this branch is only the transient mid-shrink
             state). Reject with a clear message so the brain abandons
             rather than fallback-flooding. */
          try { entry.resolve({ success: false, error: "pool shrunk (no remaining workers)" }); }
          catch (err) { console.warn("[ast-worker] shrink reject throw:", err && err.message || err); }
          return;
        }
        try {
          self.astDispatch(entry.msg).then(entry.resolve, function (err) {
            entry.resolve({ success: false, error: "re-dispatch after shrink failed: " + (err && err.message || String(err)) });
          });
        } catch (err) {
          console.warn("[ast-worker] re-dispatch after shrink threw:", err && err.message || err);
          try { entry.resolve({ success: false, error: "re-dispatch after shrink threw: " + (err && err.message || String(err)) }); }
          catch (e2) { /* terminal — entry.resolve itself threw */ }
        }
      } else if (typeof entry === "function") {
        /* Legacy resolver-only entry (e.g. SET_ANALYSIS_OPTS broadcast).
           No msg to re-dispatch; reject with a clear marker. */
        try { entry({ success: false, error: "pool shrunk" }); }
        catch (err) { console.warn("[ast-worker] shrink legacy callback throw:", err && err.message || err); }
      }
    });
    // Pages assigned to a removed slot lose stickiness; the next dispatch
    // re-routes them via the least-busy selector.
    slot.assignedPages.forEach(function (k) { _pageToWorker.delete(k); });
  }
}
_ensurePoolSize(1);   // boot with one worker; popup SET_ANALYSIS_OPTS grows it

function _pickSlot(pageKey) {
  // Sticky: if this page has already been routed, keep it on the same slot
  // so its in-worker state (fiberQ, grind cursor) stays consistent.
  if (pageKey && _pageToWorker.has(pageKey)) {
    var idx = _pageToWorker.get(pageKey);
    if (idx >= 0 && idx < _pool.length) return _pool[idx];
    _pageToWorker.delete(pageKey);   // stale assignment after a pool shrink
  }
  // Least-busy: pick the slot with the fewest pending requests.
  var bestI = 0, best = _pool[0];
  for (var i = 1; i < _pool.length; i++) {
    if (_pool[i].busy < best.busy) { best = _pool[i]; bestI = i; }
  }
  if (pageKey) { _pageToWorker.set(pageKey, bestI); best.assignedPages.add(pageKey); }
  return best;
}

self.astDispatch = function (msg) {
  // AST_CLEAR (bin/Clear): terminate EVERY pool worker mid-grind (kills any
  // running wasm), reject in-flight calls across the pool, delete the
  // resumable-grind DB now that no worker holds an IDB connection, then
  // re-spawn a single fresh worker (the user's previous pool size is
  // re-applied on the next SET_ANALYSIS_OPTS).
  if (msg && msg.type === "AST_CLEAR") {
    return new Promise(function (resolve) {
      for (var i = 0; i < _pool.length; i++) {
        var slot = _pool[i];
        try { slot.worker.terminate(); }
        catch (e) { console.warn("[ast-worker] clear-terminate failed:", e && e.message || e); }
        slot.pending.forEach(function (entry) {
          var cb = (entry && typeof entry === "object" && typeof entry.resolve === "function") ? entry.resolve : entry;
          try { cb({ success: false, error: "cleared" }); }
          catch (err) { console.warn("[ast-worker] clear callback throw:", err && err.message || err); }
        });
        slot.pending.clear();
      }
      _pool = [];
      _pageToWorker.clear();
      var done = false;
      var finish = function () { if (done) return; done = true; _ensurePoolSize(1); resolve({ success: true, reset: true }); };
      try { var del = indexedDB.deleteDatabase("feDeepDB"); del.onsuccess = finish; del.onerror = finish; }
      catch (e) { console.warn("[ast-worker] clear IDB delete failed:", e && e.message || e); finish(); }
    });
  }
  // SET_ANALYSIS_OPTS: cooling + pool-size knobs. maxWorkers grows/shrinks
  // the pool. yieldThrottleMs is forwarded to every worker (each one's
  // scheduler sleeps independently).
  if (msg && msg.type === "SET_ANALYSIS_OPTS") {
    var opts = msg.opts || {};
    if (typeof opts.maxWorkers === "number" && opts.maxWorkers > 0) {
      _ensurePoolSize(opts.maxWorkers | 0);
    }
    for (var i = 0; i < _pool.length; i++) {
      try { _pool[i].worker.postMessage({ msg: { type: "SET_ANALYSIS_OPTS", opts: opts } }); }
      catch (err) { console.warn("[ast-worker] SET_ANALYSIS_OPTS postMessage failed:", err && err.message || err); }
    }
    return Promise.resolve({ success: true });
  }
  return new Promise(function (resolve) {
    if (_pool.length === 0) { resolve({ success: false, error: "no workers in pool" }); return; }
    var pageKey = msg && msg.documentId ? msg.documentId : null;   // sticky routing by documentId, NEVER url (same url != same content)
    var slot = _pickSlot(pageKey);
    var id = _nextId++;
    /* Store {resolve, msg} so a pool-shrink can re-dispatch the SAME
       message onto a surviving worker instead of failing the analysis.
       The worker message handler reads the .resolve part; the shrink
       path reads .msg. */
    slot.pending.set(id, { resolve: resolve, msg: msg });
    slot.busy++;
    try { slot.worker.postMessage({ _id: id, msg: msg }); }
    catch (err) {
      slot.pending.delete(id);
      slot.busy--;
      resolve({ success: false, error: "postMessage failed: " + (err && err.message) });
    }
  });
};

// Liveness snapshot for the harness `offscreen` command — distinguishes a
// BLOCKED worker event loop (livenessAgeMs grows without bound: the wasm
// thread is parked in one long sync C call) from a LIVE loop with a stalled
// grind (livenessAgeMs stays small but grindStuckMs grows). Pure read-only
// observability; never mutates pool state.
self._poolLiveness = function () {
  var now = Date.now();
  return _pool.map(function (s, i) {
    var lv = s.liveness || null;
    return {
      slot: i,
      busy: s.busy,
      pending: s.pending.size,
      livenessAgeMs: s.livenessRecvTs ? (now - s.livenessRecvTs) : -1,
      fiberQ: lv ? lv.fiberQ : -1,
      grindRunning: lv ? lv.grindRunning : -1,
      grindStuckMs: (lv && lv.lastGrindProgressTs) ? (lv.ts - lv.lastGrindProgressTs) : -1,
      resumeCount: lv ? lv.resumeCount : -1,
      deepSteps: lv ? lv.deepSteps : -1,
      deepRem: lv ? lv.deepRem : -1,
      deepTotal: lv ? lv.deepTotal : -1,
      stuckOrphan: (lv && lv.currentOrphan) ? lv.currentOrphan : null,
      activePageKey: lv ? lv.activePageKey : null,
    };
  });
};
