// lib/web-request.js — the browser's own record of a page's fetch() and XMLHttpRequest traffic, from
// chrome.webRequest, and its join to intercept.js's capture.
//
// The service worker relays every webRequest event of a document's `xmlhttprequest`-type requests here as an
// `__evt` (background.js). Those events are stamped by the browser, so the request line, the header lists,
// the request body bytes and the status a record carries cannot be stated by the page. The record is the
// observed request: every complete one is filed (lib/response-decode.js's httpRecordFile) but a CORS
// preflight, which is the browser's own act. intercept.js, which runs in the page's own realm, adds only what
// webRequest cannot state: the response body the page read, the call site and the request init. A request
// made from a realm intercept.js does not wrap, such as a fetch taken from another frame's window, has no
// capture and is filed from the record alone, its response body absent (null, never an empty "").

// A record is filed once: when its capture joins it, and otherwise when its document leaves, since the two
// halves reach this zone over separate relays and a capture can arrive after the record completes.

// The join key is (documentId, method, URL, ordinal), the ordinal counting that document's requests with that
// method and URL. webRequest's side is the order of onBeforeRequest, which Chrome delivers in call order
// (measured in Chrome 148, four runs: 60 identical-key fetch() POSTs, and 20 interleaved fetch() and XHR
// POSTs, issued in one task). intercept.js counts at the call. The documentId is the browser's on both sides,
// webRequest's `details.documentId` and the content script's `MessageSender.documentId` (measured equal for
// a top frame and two iframes). The capture's own observed status is checked against the record's.

// Named residual. Not covered: a request one side counts and the other does not shifts every later ordinal
// of its key in that document: a fetch the page's service worker answers itself (no webRequest event), a
// dedicated worker's fetch (stamped with the parent's documentId; intercept.js does not run in the worker),
// an EventSource connection (type `xmlhttprequest`), a request CSP or mixed content blocks. Next diff
// builds: intercept.js counting only calls that reach the network, from a fact the browser states. Absence
// shows as: a capture refused `status-mismatch`, or counted `wrapperOnly`, on a page whose worker, service
// worker or EventSource shares a (method, URL) with its own fetch() calls.

/* documentId -> the document's requests: `next` holds each key's request count in onBeforeRequest order (its
   next ordinal) and the last requestId counted; `slots`, by key and ordinal, what each side has delivered of
   one request: the browser's `record` and the page's `capture`, each null until it arrives. A slot leaves
   when its request is filed or set aside. A document that left keeps only `{tabId, left: true}`, so a late
   event or capture of it is told apart from a relay fault; its tab's close deletes that too. */
var _wrDocs = new Map();
/* requestId -> the record of a request whose events are still arriving. A redirect reuses the requestId. */
var _wrById = new Map();
/* Lifetime counts. Every record ends in exactly one of `joined` (filed with its capture), `webRequestOnly`
   (filed with no capture), `preflight` (the browser's CORS-preflight request), `failed`, `leftInFlight` and
   `unregistered` (complete, in a document that never registered with this zone), so `recorded` is their sum
   plus the records live now. Of the captures, `wrapperOnly` is one no record matched by the time its
   document left, and `statusMismatch` and `captureRefused` are refused by name. */
var webRequestCensus = { recorded: 0, joined: 0, webRequestOnly: 0, preflight: 0, failed: 0, leftInFlight: 0,
                         unregistered: 0, wrapperOnly: 0, statusMismatch: 0, captureRefused: 0,
                         captureAfterLeft: 0, eventAfterLeft: 0 };

function _wrDocOf(documentId, tabId) {
  var d = _wrDocs.get(documentId);
  if (d === undefined) {
    d = { tabId: tabId, left: false, next: new Map(), slots: new Map() };
    _wrDocs.set(documentId, d);
  }
  return d;
}

function _wrKey(method, url) {
  return method + " " + url;
}

/* The slot of `key` and `ordinal` in document `d`, made empty if absent. */
function _wrSlotOf(d, key, ordinal) {
  if (!d.slots.has(key)) d.slots.set(key, new Map());
  var keyS = d.slots.get(key);
  var slot = keyS.get(ordinal);
  if (slot === undefined) {
    slot = { key: key, ordinal: ordinal, record: null, capture: null };
    keyS.set(ordinal, slot);
  }
  return slot;
}

function _wrSlotDrop(d, slot) {
  DCHECK(d.slots.get(slot.key).get(slot.ordinal) === slot,
         "a webRequest slot was dropped from a document that does not hold it — each slot lives in one document");
  d.slots.get(slot.key).delete(slot.ordinal);
}

function _wrHeaderMap(list) {
  DCHECK(Array.isArray(list), "a webRequest event carried no header list where its extraInfoSpec asked for one");
  var out = {};
  for (var i = 0; i < list.length; i++) {
    DCHECK(typeof list[i].name === "string" && typeof list[i].value === "string",
           "a webRequest header entry carries no string name or value; binaryValue is never asked for");
    out[list[i].name.toLowerCase()] = list[i].value;
  }
  return out;
}

/* The reason phrase of a status line ("HTTP/1.1 200 OK" -> "OK"); "" when the line states none, which is
   Fetch §2.2.6 "Responses"' status message for every HTTP/2 response. */
function _wrStatusText(line) {
  DCHECK(typeof line === "string" && /^HTTP\/\S+ \d{3}/.test(line),
         "a webRequest status line is not `HTTP/<version> <code>` — the browser composes it");
  var m = /^HTTP\/\S+ \d{3} ?(.*)$/.exec(line);
  return m[1];
}

/* Whether webRequest reports a request's body for `method`: for POST and PUT alone (measured in Chrome 148:
   PATCH, DELETE, OPTIONS and an extension method each sent a body and carried none). intercept.js states
   the body for every other method. */
function webRequestStatesBody(method) {
  return method === "POST" || method === "PUT";
}

/* The request body webRequest exposes, as bytes in base64, or null for none. `raw` concatenates its byte
   elements (base64 across the relay); a `file` element carries no bytes, so a multipart body states every
   part but a file's content.
   `formData` is what the browser reports in place of the bytes of a form body; for
   application/x-www-form-urlencoded it is serialized again (URL §5.2 application/x-www-form-urlencoded
   serializing), and for another type it states no bytes. `error` (a Blob or stream body) states none. */
function _wrRequestBody(rb, contentType) {
  if (!rb || rb.error !== undefined) return null;
  if (Array.isArray(rb.raw)) {
    var parts = [];
    var n = 0;
    for (var i = 0; i < rb.raw.length; i++) {
      if (rb.raw[i].b64 === undefined) continue;
      var u = base64ToUint8(rb.raw[i].b64);
      parts.push(u);
      n += u.length;
    }
    var all = new Uint8Array(n);
    for (var j = 0, at = 0; j < parts.length; j++) { all.set(parts[j], at); at += parts[j].length; }
    return uint8ToBase64(all);
  }
  DCHECK(rb.formData && typeof rb.formData === "object",
         "a webRequest requestBody has none of raw, formData and error");
  if (!/^application\/x-www-form-urlencoded/i.test(contentType)) return null;
  var params = new URLSearchParams();
  for (var name in rb.formData)
    for (var k = 0; k < rb.formData[name].length; k++) params.append(name, rb.formData[name][k]);
  return uint8ToBase64(new TextEncoder().encode(params.toString()));
}

/* One webRequest event, as background.js relays it. */
function webRequestEvent(ev) {
  DCHECK(ev && typeof ev.requestId === "string" && typeof ev.documentId === "string" && ev.documentId !== "" &&
         typeof ev.url === "string" && typeof ev.method === "string",
         "background.js relayed a webRequest event without its requestId, documentId, url or method — it relays " +
         "only events of a document's own requests");
  var r = _wrById.get(ev.requestId);
  var d = _wrDocs.get(ev.documentId);
  if (r !== undefined) {
    DCHECK(r.documentId === ev.documentId && d !== undefined && !d.left,
           "the record of request " + ev.requestId + " outlived its document — webRequestDocumentLeft and " +
           "webRequestTabRemoved take every in-flight record of the document out of _wrById");
  } else if (d === undefined ? ev.ev !== "onBeforeRequest" : d.left) {
    /* A request of a document that left (a keepalive fetch, or a request cancelled by the navigation whose
       event the relay delivers after the document's port closed), or of a tab that closed. */
    webRequestCensus.eventAfterLeft++;
    return;
  }
  if (ev.ev === "onBeforeRequest") {
    if (r !== undefined) return;   // a redirect's next hop: the record keeps the first request line and body
    DCHECK(ev.requestBody === undefined || webRequestStatesBody(ev.method),
           "webRequest reported a body for a " + ev.method + " request — this file takes the body from the " +
           "browser for POST and PUT alone and from intercept.js otherwise, so that split is now stale");
    d = _wrDocOf(ev.documentId, ev.tabId);
    var key = _wrKey(ev.method, ev.url);
    var n = d.next.get(key);
    if (n === undefined) { n = { count: 0, lastRequestId: null }; d.next.set(key, n); }
    DCHECK(n.lastRequestId === null || Number(n.lastRequestId) < Number(ev.requestId),
           "a webRequest onBeforeRequest arrived out of the browser's order for " + key + " (" + ev.requestId +
           " after " + n.lastRequestId + ") — background.js relays through one chain, and the ordinal a " +
           "capture joins on is this order");
    r = { requestId: ev.requestId, documentId: ev.documentId, tabId: ev.tabId, frameId: ev.frameId,
          method: ev.method, url: ev.url, ordinal: n.count, requestBodyRaw: ev.requestBody, requestHeaders: null,
          status: null, statusText: null, responseHeaders: null, redirects: [], done: false, failed: null };
    n.count++;
    n.lastRequestId = ev.requestId;
    _wrById.set(ev.requestId, r);
    webRequestCensus.recorded++;
    var slot = _wrSlotOf(d, key, r.ordinal);
    DCHECK(slot.record === null, "two webRequest records took ordinal " + r.ordinal + " of " + key +
                                 " — the ordinal is this document's count of the key, taken once per record");
    slot.record = r;
    return;
  }
  DCHECK(r !== undefined, "a webRequest " + ev.ev + " arrived for request " + ev.requestId + " of a live " +
         "document before its onBeforeRequest — the browser orders a request's events and the relay keeps that " +
         "order");
  if (ev.ev === "onSendHeaders") {
    if (r.requestHeaders === null) r.requestHeaders = _wrHeaderMap(ev.requestHeaders);
  } else if (ev.ev === "onHeadersReceived") {
    r.status = ev.statusCode;
    r.statusText = _wrStatusText(ev.statusLine);
    r.responseHeaders = _wrHeaderMap(ev.responseHeaders);
  } else if (ev.ev === "onBeforeRedirect") {
    r.redirects.push({ status: ev.statusCode, location: ev.redirectUrl });
  } else if (ev.ev === "onCompleted") {
    DCHECK(r.requestHeaders !== null && r.status === ev.statusCode && r.responseHeaders !== null,
           "a webRequest record completed without the request headers and the final response's status and " +
           "headers its earlier events state");
    r.done = true;
    _wrById.delete(r.requestId);
    _wrAdvance(d, d.slots.get(_wrKey(r.method, r.url)).get(r.ordinal));
  } else if (ev.ev === "onErrorOccurred") {
    r.done = true;
    r.failed = ev.error;
    _wrById.delete(r.requestId);
    _wrAdvance(d, d.slots.get(_wrKey(r.method, r.url)).get(r.ordinal));
  } else {
    DFAIL("background.js relayed the webRequest event `" + ev.ev + "`, which this file does not take");
  }
}

/* The page-settable request headers: the request's own list less Fetch §2.2.2 "Headers"' forbidden
   request-headers, which the browser sets itself (Cookie, Origin, Referer, sec-*). A replay of the record
   sends these, and the forbidden ones are what the zone's send refuses (lib/safe-fetch.js). */
function _wrPageHeaders(all) {
  var out = {};
  for (var k in all) if (fetchForbiddenRequestHeader([[k, all[k]]]) === null) out[k] = all[k];
  return out;
}

/* Fetch §2.2.6's forbidden response-header names, which no page reads and a log does not keep: their
   values are the person's session. */
function _wrReadableResponseHeaders(all) {
  var out = {};
  for (var k in all) if (k !== "set-cookie" && k !== "set-cookie2") out[k] = all[k];
  return out;
}

/* A header's value, or "" for a header the list does not hold (the content type a record states when the
   message declared none). */
function _wrHeader(map, name) {
  return map[name] !== undefined ? map[name] : "";
}

/* Whether a complete record is Fetch §4.8 "CORS-preflight fetch"'s request, which the browser makes on its
   own: an `OPTIONS` request carrying `Access-Control-Request-Method`, a header the page cannot set (Fetch
   §2.2.2 "Headers" lists it among the forbidden request-headers) and §4.8 appends to every preflight. */
function _wrIsPreflight(r) {
  return r.method === "OPTIONS" && r.requestHeaders["access-control-request-method"] !== undefined;
}

/* Files the complete record `r` as an observed request of its document, with what its capture `w` added,
   or with none (`w` null). `outcome` names the census count the record ends in. */
function _wrFile(r, w, outcome) {
  DCHECK(r.done && r.failed === null, "a webRequest record was filed before it completed, or after it failed");
  if (!httpRecordDocumentRegistered(r.documentId)) {
    webRequestCensus.unregistered++;
    console.warn("[web-request] " + r.method + " " + r.url + " is not filed: document " + r.documentId +
                 " never registered with this zone");
    return;
  }
  webRequestCensus[outcome]++;
  httpRecordFile({
    documentId: r.documentId,
    tabId: r.tabId,
    frameId: r.frameId,
    url: r.url,
    method: r.method,
    status: r.status,
    statusText: r.statusText,
    requestHeadersOnWire: r.requestHeaders,
    requestHeaders: _wrPageHeaders(r.requestHeaders),
    rawBodyB64: webRequestStatesBody(r.method)
      ? _wrRequestBody(r.requestBodyRaw, _wrHeader(r.requestHeaders, "content-type"))
      : w === null ? null : w.requestBodyB64,
    responseHeaders: _wrReadableResponseHeaders(r.responseHeaders),
    contentType: _wrHeader(r.responseHeaders, "content-type"),
    redirects: r.redirects,
  }, w);
}

/* Moves `slot` of document `d` on as far as what has arrived allows: a failed or preflight record is set
   aside, a capture whose status the record contradicts is refused and the record filed alone, and a record
   with its capture is filed. Anything else waits for the other half, or the document's departure. */
function _wrAdvance(d, slot) {
  var r = slot.record;
  if (r === null || !r.done) return;
  if (r.failed !== null) {
    if (slot.capture !== null) {
      webRequestCensus.captureRefused++;
      console.warn("[web-request] " + slot.key + " capture refused: ordinal " + r.ordinal +
                   " is a request that failed (" + r.failed + ")");
    }
    webRequestCensus.failed++;
    _wrSlotDrop(d, slot);
    return;
  }
  if (_wrIsPreflight(r)) {
    if (slot.capture !== null) {
      webRequestCensus.captureRefused++;
      console.warn("[web-request] " + slot.key + " capture refused: ordinal " + r.ordinal + " is the " +
                   "browser's CORS preflight");
    }
    webRequestCensus.preflight++;
    _wrSlotDrop(d, slot);
    return;
  }
  var w = slot.capture;
  if (w === null) return;
  _wrSlotDrop(d, slot);
  /* The capture is compared with the status the page's own fetch exposed. Fetch §4.1 "Main fetch" hands a
     request whose response tainting is "opaque" an opaque filtered response, which Fetch §2.2.6 "Responses"
     gives status 0 whatever the browser's record of the internal response says; every other response the page
     reads exposes the internal status. The type is the page's Response's (intercept.js reads `clone.type`). */
  var exposed = w.transport === "fetch" && w.responseType === "opaque" ? 0 : r.status;
  if (exposed !== w.observedStatus) {
    webRequestCensus.statusMismatch++;
    console.warn("[web-request] " + r.method + " " + r.url + " capture refused (status-mismatch): the page " +
                 "observed " + String(w.observedStatus) + " and its response exposes " + exposed +
                 " (the browser reports " + r.status + ") for ordinal " + r.ordinal + "; the record is filed " +
                 "without it");
    _wrFile(r, null, "webRequestOnly");
    return;
  }
  _wrFile(r, w, "joined");
}

/* intercept.js's capture of one fetch() or XHR, from document `documentId`, joined to the browser's record
   of the same request by key and ordinal. The capture is the page's: a key or ordinal no record holds leaves
   it waiting, never asserted. */
function webRequestJoin(documentId, tabId, capture) {
  var key = _wrKey(capture.method, capture.url);
  var d = _wrDocs.get(documentId);
  if (d !== undefined && d.left) {
    webRequestCensus.captureAfterLeft++;
    console.warn("[web-request] " + key + " capture refused: its document left");
    return;
  }
  d = _wrDocOf(documentId, tabId);
  var keyS = d.slots.get(key);
  var n = d.next.get(key);
  if ((keyS === undefined || !keyS.has(capture.seq)) && n !== undefined && capture.seq < n.count) {
    webRequestCensus.captureRefused++;
    console.warn("[web-request] " + key + " capture refused: ordinal " + capture.seq + " is already settled");
    return;
  }
  var slot = _wrSlotOf(d, key, capture.seq);
  if (slot.capture !== null) {
    webRequestCensus.captureRefused++;
    console.warn("[web-request] " + key + " capture refused: ordinal " + capture.seq + " was captured twice");
    return;
  }
  slot.capture = capture;
  _wrAdvance(d, slot);
}

/* Reclaims the live document `documentId`. No capture can arrive for it any more, so each complete record is
   filed without one; an in-flight record leaves _wrById; and a capture no record matched is counted. */
function _wrReclaim(documentId, d) {
  var inFlight = 0, wrapperOnly = 0, alone = 0;
  for (var keyS of d.slots.values()) {
    for (var slot of keyS.values()) {
      var r = slot.record;
      if (r === null) { wrapperOnly++; continue; }
      if (!r.done) { inFlight++; _wrById.delete(r.requestId); continue; }
      DCHECK(r.failed === null && !_wrIsPreflight(r) && slot.capture === null,
             "a complete record of document " + documentId + " is still held with a failure, as a preflight " +
             "or with its capture — _wrAdvance settles each of those when it arrives");
      alone++;
      _wrFile(r, null, "webRequestOnly");
    }
  }
  for (var rest of _wrById.values())
    DCHECK(rest.documentId !== documentId, "request " + rest.requestId + " of document " + documentId +
           " is in flight in _wrById and in none of the document's slots, so it would outlive the document");
  webRequestCensus.leftInFlight += inFlight;
  webRequestCensus.wrapperOnly += wrapperOnly;
  if (inFlight + wrapperOnly + alone > 0)
    console.warn("[web-request] document " + documentId + " left with " + alone + " complete record(s) no " +
                 "capture joined, " + inFlight + " in flight and " + wrapperOnly + " capture(s) no record matched");
}

/* The document `documentId` was destroyed (see webRequestDocumentPort): its requests are filed and its
   captures reclaimed, it stays known as left until its tab closes, and the speak-as rules of sends that spoke as
   it go (lib/speak-as.js). */
function webRequestDocumentLeft(documentId, tabId) {
  speakAsDocumentEnded("frame:" + documentId);
  var d = _wrDocs.get(documentId);
  if (d !== undefined && d.left) return;
  if (d !== undefined) _wrReclaim(documentId, d);
  _wrDocs.set(documentId, { tabId: d === undefined ? tabId : d.tabId, left: true });
}

/* The reason Chrome gives the receiving end of a content script's port it closes because the document
   entered the back/forward cache (measured in Chrome 148). A port closed by the document's destruction has
   no reason. */
var _WR_PORT_CLOSED_FOR_BFCACHE =
  "The page keeping the extension port is moved into back/forward cache, so the message channel is closed.";

/* A document's lifetime port, opened by doc-lifetime.js in every frame and authenticated by the caller
   (offscreen-brain.js). Chrome closes it when the document is destroyed, and when the document enters the
   back/forward cache with the reason above; webNavigation tells neither apart (getFrame() answers null for
   both). A destroyed document is reclaimed. A cached one is not, since it can come back with the same
   documentId, and a restored document opens a new port. The page sends nothing on the port.
   Named residual. Not covered: a cached document the browser evicts, which no event reports, keeps its
   records until its tab closes. Next diff builds: reclaiming every cached document at the evictions this
   extension causes itself, since a declarativeNetRequest session-rule update and a webRequest listener's
   registration each flush the whole cache (measured, reason CacheFlushed). Absence shows as: the records of
   a tab's cached documents counted only at the tab's close. */
function webRequestDocumentPort(port, documentId, tabId) {
  port.onMessage.addListener(function () {
    console.warn("[web-request] document " + documentId + " sent a message on its lifetime port, which " +
                 "carries none");
  });
  port.onDisconnect.addListener(function () {
    var reason = chrome.runtime.lastError === undefined ? null : chrome.runtime.lastError.message;
    if (reason === _WR_PORT_CLOSED_FOR_BFCACHE) return;
    if (reason !== null) {
      DFAIL("Chrome closed document " + documentId + "'s lifetime port for a reason this file does not know (" +
            reason + ") — learn whether it is the document's destruction, and add it");
      return;
    }
    webRequestDocumentLeft(documentId, tabId);
  });
}

/* A tab closed, and every document of it with it: a live one is reclaimed, and the tab's left documents
   are forgotten. */
function webRequestTabRemoved(tabId) {
  for (var [id, d] of _wrDocs) {
    if (d.tabId !== tabId) continue;
    if (!d.left) _wrReclaim(id, d);
    speakAsDocumentEnded("frame:" + id);
    _wrDocs.delete(id);
  }
}
