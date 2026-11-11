// lib/send-coalesce.js — the sends one document's exploration has made, so a byte-identical send is made once.
//
// An exploration runs one document's code in many forced worlds, and each world makes the requests its path
// makes. A request byte-identical to one the same document already sent is the same act by the same document,
// so it is sent once and every later asker is answered from the recorded reply: its head, and its body replayed
// from the first byte and then followed live while it is still arriving. Each asker receives the reply as its
// own; whatever the asker grades or stores (provenance, cookies, the CORS check) is applied by its own caller.
// This sits beside the HTTP cache (lib/http-cache.js), not inside it: the cache reuses a response across time
// under RFC 9111 freshness and Vary, while this ignores the server's cache headers, because its premise is one
// act and not reuse.
//
// Scope: one table per document key (lib/request-identity.js's `docKey`, this zone's name for the document a
// send speaks as), ended with the document by sendCoalesceDocumentEnded; nothing crosses documents. The caller
// composes the send's key from the bytes it puts on the wire (lib/safe-fetch.js's `_wireSendKey`), and this
// file compares keys and nothing else.

var _sendCoalesceDocs = new Map();

/* The record of one send: its head once answered, every body chunk received so far, `end` (null while the
   body is arriving, then {error}), and the controllers of the askers' bodies still following it. */
function _sendCoalesceRecord(send) {
  var rec = { controller: new AbortController(), head: null, hasBody: false, chunks: [], end: null,
              readers: new Set(), requestTime: Date.now(), responseTime: 0, answered: null };
  rec.answered = send(rec.controller.signal).then(function (sent) {
    DCHECK(!!sent && !!sent.head && typeof sent.head.status === "number" && Array.isArray(sent.head.headers) &&
           Array.isArray(sent.head.setCookie) && (sent.body === null || typeof sent.body.getReader === "function"),
           "a coalesced send answered no {head: {status, statusText, headers, setCookie}, body} — lib/safe-fetch.js's " +
           "_wireSend states one for every reply the wire gives");
    rec.head = sent.head;
    rec.responseTime = Date.now();
    rec.hasBody = sent.body !== null;
    if (rec.hasBody) _sendCoalescePump(rec, sent.body.getReader());
    else rec.end = { error: null };
  });
  return rec;
}

/* Reads the reply's body to its end whether or not an asker still reads it, so a later asker is answered
   from the whole of it. The body is read until it ends or the document ends (which aborts the send). */
async function _sendCoalescePump(rec, reader) {
  var step;
  try {
    for (;;) {
      step = await reader.read();
      if (step.done) break;
      DCHECK(ArrayBuffer.isView(step.value),
             "the wire's body reader yielded a chunk that is not a byte view — the carrier is this host's contract");
      rec.chunks.push(step.value);
      rec.readers.forEach(function (c) { c.enqueue(step.value.slice()); });
    }
  } catch (e) {
    RETHROW_FATAL(e);
    /* A body that fails mid-way failed for every asker; a later asker reads the same bytes and the same end. */
    rec.end = { error: e };
    rec.readers.forEach(function (c) { c.error(e); });
    rec.readers.clear();
    return;
  }
  rec.end = { error: null };
  rec.readers.forEach(function (c) { c.close(); });
  rec.readers.clear();
}

/* One asker's body: a fresh stream over the recorded chunks that then follows the arriving ones. Each asker
   gets its own copies, so no consumer can change the bytes another reads. `signal` is the asker's own abort,
   which errors this stream alone. */
function _sendCoalesceBody(rec, signal) {
  var ctl = null;
  var stream = new ReadableStream({
    start: function (c) {
      ctl = c;
      rec.chunks.forEach(function (chunk) { c.enqueue(chunk.slice()); });
      if (rec.end === null) rec.readers.add(c);
      else if (rec.end.error !== null) c.error(rec.end.error);
      else c.close();
    },
    cancel: function () { rec.readers.delete(ctl); }
  });
  if (signal) signal.addEventListener("abort", function () {
    if (!rec.readers.has(ctl)) return;
    rec.readers.delete(ctl);
    ctl.error(new TypeError("the asker's read of a coalesced reply was aborted"));
  });
  return stream;
}

/* The reply to the send `key` of the document `docKey`. The first ask calls `send(signal)`, which makes the
   send and answers {head: {status, statusText, headers (pairs), setCookie}, body: a ReadableStream or null};
   `signal` is aborted only when the document ends. Every ask, the first included, answers
   {head, body, requestTime, responseTime}: its own body stream and the record's times (RFC 9111 §4.2.3
   "Calculating Age": "request_time" and "response_time" are when the send was made and its reply received,
   so a replay is no fresher than the reply it repeats). A send that rejects rejects every ask. `signal` is this
   asker's abort, which ends this asker's answer and never the send. */
function sendCoalesce(docKey, key, signal, send) {
  CHECK(typeof docKey === "string" && docKey !== "" && typeof key === "string" && typeof send === "function",
        "a send was coalesced with no document key, no send key or no send — the table is one document's, and " +
        "a send with no document to scope it would be answered from another document's acts");
  var doc = _sendCoalesceDocs.get(docKey);
  if (doc === undefined) {
    doc = { replies: new Map(), sends: 0, answers: 0 };
    _sendCoalesceDocs.set(docKey, doc);
  }
  var rec = doc.replies.get(key);
  if (rec === undefined) {
    rec = _sendCoalesceRecord(send);
    doc.replies.set(key, rec);
    doc.sends++;
  } else {
    doc.answers++;
  }
  return new Promise(function (resolve, reject) {
    var aborted = function () { reject(new TypeError("the asker of a coalesced send aborted before its head")); };
    if (signal) {
      if (signal.aborted) { aborted(); return; }
      signal.addEventListener("abort", aborted);
    }
    rec.answered.then(function () {
      if (signal) {
        if (signal.aborted) return;
        signal.removeEventListener("abort", aborted);
      }
      resolve({ head: rec.head, body: rec.hasBody ? _sendCoalesceBody(rec, signal) : null,
                requestTime: rec.requestTime, responseTime: rec.responseTime });
    }, function (e) {
      if (signal) signal.removeEventListener("abort", aborted);
      reject(e);
    });
  });
}

/* The document `docKey` ended (HTML §7.5.10 "Destroying documents", or its instance ended): its table is
   forgotten and every send still arriving is aborted, as lib/speak-as.js's speakAsDocumentEnded ends the
   scopes a key is part of. */
function sendCoalesceDocumentEnded(docKey) {
  for (var [k, doc] of [..._sendCoalesceDocs]) {
    if (k.split("+").indexOf(docKey) < 0) continue;
    _sendCoalesceDocs.delete(k);
    doc.replies.forEach(function (rec) { rec.controller.abort(); });
  }
}

/* Whether the document `docKey` has made the send `key`, so an ask of it now is answered from the record and
   reaches no other party. */
function sendCoalesceHolds(docKey, key) {
  var doc = _sendCoalesceDocs.get(docKey);
  return doc !== undefined && doc.replies.has(key);
}

/* The census of the document `docKey`'s table: `sends` made, `answers` given from a record (counted at the
   ask), and `identities`, the distinct keys recorded; or null for a document that coalesced nothing. */
function sendCoalesceCensus(docKey) {
  var doc = _sendCoalesceDocs.get(docKey);
  if (doc === undefined) return null;
  DCHECK(doc.sends === doc.replies.size,
         "a document's coalescing table made " + doc.sends + " sends and records " + doc.replies.size +
         " identities — every send is recorded under its key and none is dropped while the document lives");
  return { sends: doc.sends, answers: doc.answers, identities: doc.replies.size };
}

if (typeof self !== "undefined") {
  self.sendCoalesce = sendCoalesce;
  self.sendCoalesceDocumentEnded = sendCoalesceDocumentEnded;
  self.sendCoalesceCensus = sendCoalesceCensus;
  self.sendCoalesceHolds = sendCoalesceHolds;
}
