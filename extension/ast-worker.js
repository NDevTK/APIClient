// ast-worker.js — owns the analysis Web Worker (ast-thread.js) inside the
// offscreen document. The learning brain (offscreen-brain.js) loads in the SAME
// document and calls self.astDispatch(msg) directly — chrome.runtime.sendMessage
// can't reach a same-document listener (the sender's own context is excluded from
// the broadcast). Deep-grind results streamed by the worker are merged by calling
// the brain's self._mergeDeepResult directly.

var _worker = null;
var _pending = new Map();
var _nextId = 0;

function _onWorkerMessage(e) {
  // Durable-deep RESUME result: the worker picked up an incomplete deep grind
  // from IndexedDB and finished it — merge into the brain's globalStore.
  if (e.data && e.data._resumed) {
    try { if (typeof self._mergeDeepResult === "function") self._mergeDeepResult(e.data.sourceUrl || "", e.data.response && e.data.response.result, true); } catch (_) {}
    return;
  }
  // Mid-grind partial: the deep grind streams what each batch learned so the UI
  // grows live instead of waiting for the whole run.
  if (e.data && e.data._partial) {
    try { if (typeof self._mergeDeepResult === "function") self._mergeDeepResult(e.data.sourceUrl || "", e.data.response && e.data.response.result, false); } catch (_) {}
    return;
  }
  var cb = _pending.get(e.data._id);
  if (cb) { _pending.delete(e.data._id); cb(e.data.response); }
}

function _onWorkerError(e) {
  _pending.forEach(function (cb) { try { cb({ success: false, error: "Worker error: " + (e.message || "unknown") }); } catch (_) {} });
  _pending.clear();
}

function _spawnWorker() {
  _worker = new Worker("ast-thread.js");
  _worker.onmessage = _onWorkerMessage;
  _worker.onerror = _onWorkerError;
}
_spawnWorker();

// The brain dispatches AST_* messages here (same-document direct call → Promise).
self.astDispatch = function (msg) {
  // AST_CLEAR (bin/Clear): terminate the worker mid-grind outright (kills the
  // running wasm), reject in-flight calls, delete the resumable-grind DB now that
  // its only connection-holder is gone, then spawn a fresh worker.
  if (msg && msg.type === "AST_CLEAR") {
    return new Promise(function (resolve) {
      try { if (_worker) _worker.terminate(); } catch (e) {}
      _worker = null;
      _pending.forEach(function (cb) { try { cb({ success: false, error: "cleared" }); } catch (e) {} });
      _pending.clear();
      var done = false;
      var finish = function () { if (done) return; done = true; _spawnWorker(); resolve({ success: true, reset: true }); };
      try { var del = indexedDB.deleteDatabase("feDeepDB"); del.onsuccess = finish; del.onerror = finish; } catch (e) {}
      setTimeout(finish, 3000);   // backstop so the brain's await can't hang
    });
  }
  return new Promise(function (resolve) {
    if (!_worker) { resolve({ success: false, error: "no worker" }); return; }
    var id = _nextId++;
    _pending.set(id, resolve);
    try { _worker.postMessage({ _id: id, msg: msg }); }
    catch (err) { _pending.delete(id); resolve({ success: false, error: "postMessage failed: " + (err && err.message) }); }
  });
};
