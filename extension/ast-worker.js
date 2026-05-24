// Offscreen document — thin relay between the service worker and a Web Worker.
// Heavy libs (Babel, ast.js, sourcemap.js) load and run on the Worker thread,
// keeping both the service worker and this document's main thread responsive.

var _worker = null;
var _pending = new Map();
var _nextId = 0;

function _onWorkerMessage(e) {
  // Unsolicited durable-deep RESUME result (no pending _id): the worker
  // picked up an incomplete deep grind from IndexedDB after an eviction and
  // finished it. Forward to the service worker so the recovered endpoints
  // land in globalStore. The offscreen doc has chrome.runtime; the worker
  // doesn't, which is why it routes through here.
  if (e.data && e.data._resumed) {
    try {
      chrome.runtime.sendMessage({ type: "AST_RESUMED", sourceUrl: e.data.sourceUrl || "",
        result: e.data.response && e.data.response.result });
    } catch (_) {}
    return;
  }
  // Mid-grind partial: the deep grind streams what each batch learned so the
  // UI grows live instead of waiting for the whole run. Same merge path as a
  // resume result, just delivered repeatedly during the grind.
  if (e.data && e.data._partial) {
    try {
      chrome.runtime.sendMessage({ type: "AST_PARTIAL", sourceUrl: e.data.sourceUrl || "",
        result: e.data.response && e.data.response.result });
    } catch (_) {}
    return;
  }
  var cb = _pending.get(e.data._id);
  if (cb) {
    _pending.delete(e.data._id);
    cb(e.data.response);
  }
}

function _onWorkerError(e) {
  // Worker crashed — reject all pending callbacks
  _pending.forEach(function(cb) {
    cb({ success: false, error: "Worker error: " + (e.message || "unknown") });
  });
  _pending.clear();
}

function _spawnWorker() {
  _worker = new Worker("ast-thread.js");
  _worker.onmessage = _onWorkerMessage;
  _worker.onerror = _onWorkerError;
}
_spawnWorker();

function _rejectAllPending(reason) {
  _pending.forEach(function(cb) { try { cb({ success: false, error: reason }); } catch (e) {} });
  _pending.clear();
}

chrome.runtime.onMessage.addListener(function(msg, sender, sendResponse) {
  if (!msg || typeof msg.type !== "string" || !msg.type.startsWith("AST_")) return;

  if (msg.type === "AST_CLEAR") {
    // Hard reset for the Clear button. The worker may be mid-grind on a wasm
    // instance; a cooperative "please stop" flag would let it keep running on
    // already-deleted data. Instead TERMINATE it outright — that kills the
    // running wasm immediately — reject anything in flight (so the SW's awaits
    // don't hang on a worker we're killing), delete the resumable-grind DB now
    // that its only connection-holder is gone, then spin up a fresh worker so
    // the next navigation analyses from a clean slate.
    try { if (_worker) _worker.terminate(); } catch (e) {}
    _worker = null;
    _rejectAllPending("cleared");
    var done = false;
    var finish = function () {
      if (done) return;
      done = true;
      _spawnWorker();   // fresh worker; its _resumeIncompleteDeep finds the wiped DB empty → no resume
      try { sendResponse({ success: true, reset: true }); } catch (e) {}
    };
    try {
      var del = indexedDB.deleteDatabase("feDeepDB");
      del.onsuccess = finish;
      del.onerror = finish;
      // onblocked: a worker connection is still closing after terminate; the
      // delete proceeds to onsuccess once it does. Don't respawn yet (a new
      // worker could re-open the DB and re-block the delete) — the timeout
      // below is the backstop so the SW await can never hang.
    } catch (e) { /* fall through to the timeout backstop */ }
    setTimeout(finish, 3000);
    return true;
  }

  var id = _nextId++;
  _pending.set(id, sendResponse);
  try {
    _worker.postMessage({ _id: id, msg: msg });
  } catch (err) {
    _pending.delete(id);
    sendResponse({ success: false, error: "postMessage failed: " + (err && err.message) });
  }
  return true; // keep sendResponse alive for async Worker response
});
