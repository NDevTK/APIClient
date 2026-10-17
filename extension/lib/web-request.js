// lib/web-request.js — the browser's own record of a page's fetch() and XMLHttpRequest traffic, from
// chrome.webRequest, and its join to intercept.js's body capture.
//
// The service worker relays every webRequest event of a document's `xmlhttprequest`-type requests here as an
// `__evt` (background.js). Those events are stamped by the browser, so the request line, the header lists,
// the request body bytes and the status a record carries cannot be stated by the page. intercept.js, which
// runs in the page's own realm, supplies only what webRequest cannot: the response body and the call site.

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
// shows as: a capture refused `status-mismatch`, or left waiting, on a page whose worker, service worker or
// EventSource shares a (method, URL) with its own fetch() calls.

/* documentId -> the document's requests: `next` holds each key's request count in onBeforeRequest order (its
   next ordinal) and the last requestId counted; `records` the records no capture has taken yet, by key and
   ordinal; `waiting` the captures whose record is not complete yet, by key and ordinal. A document that left
   keeps only `{tabId, left: true}`, so a late event or capture of it is told apart from a relay fault; its
   tab's close deletes that too. */
var _wrDocs = new Map();
/* requestId -> the record of a request whose events are still arriving. A redirect reuses the requestId. */
var _wrById = new Map();
/* Lifetime counts of how each record and capture ended, by outcome. */
var webRequestCensus = { joined: 0, statusMismatch: 0, leftUnjoined: 0, leftFailed: 0, leftInFlight: 0,
                         leftWaiting: 0, eventAfterLeft: 0, captureAfterLeft: 0 };

function _wrDocOf(documentId, tabId) {
  var d = _wrDocs.get(documentId);
  if (d === undefined) {
    d = { tabId: tabId, left: false, next: new Map(), records: new Map(), waiting: new Map() };
    _wrDocs.set(documentId, d);
  }
  return d;
}

function _wrKey(method, url) {
  return method + " " + url;
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
          status: null, statusText: null, responseHeaders: null, redirects: [], done: false, failed: null,
          joined: false };
    n.count++;
    n.lastRequestId = ev.requestId;
    _wrById.set(ev.requestId, r);
    if (!d.records.has(key)) d.records.set(key, new Map());
    d.records.get(key).set(r.ordinal, r);
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
    _wrTryJoin(d, r);
  } else if (ev.ev === "onErrorOccurred") {
    r.done = true;
    r.failed = ev.error;
    _wrById.delete(r.requestId);
    _wrTryJoin(d, r);
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

/* Settles the complete record `r` of document `d` with the capture `w` that names its ordinal: the record
   leaves `d.records`, and the capture is delivered or refused by name. */
function _wrSettle(d, r, w) {
  DCHECK(!r.joined && r.done, "a webRequest record was settled twice, or before it completed — a settled " +
                              "record leaves its document's records, and only a complete one is settled");
  r.joined = true;
  d.records.get(_wrKey(r.method, r.url)).delete(r.ordinal);
  if (r.failed !== null) {
    console.warn("[web-request] " + r.method + " " + r.url + " capture refused: ordinal " + r.ordinal +
                 " is a request that failed (" + r.failed + ")");
    return;
  }
  /* The capture is compared with the status the page's own fetch exposed. Fetch §4.1 "Main fetch" hands a
     request whose response tainting is "opaque" an opaque filtered response, which Fetch §2.2.6 "Responses"
     gives status 0 whatever the browser's record of the internal response says; every other response the page
     reads exposes the internal status. The type is the page's Response's (intercept.js reads `clone.type`). */
  var exposed = w.capture.transport === "fetch" && w.capture.responseType === "opaque" ? 0 : r.status;
  if (exposed !== w.capture.observedStatus) {
    webRequestCensus.statusMismatch++;
    console.warn("[web-request] " + r.method + " " + r.url + " capture refused (status-mismatch): the page " +
                 "observed " + String(w.capture.observedStatus) + " and its response exposes " + exposed +
                 " (the browser reports " + r.status + ") for ordinal " + r.ordinal);
    return;
  }
  webRequestCensus.joined++;
  w.deliver({
    url: r.url,
    method: r.method,
    status: r.status,
    statusText: r.statusText,
    requestHeadersOnWire: r.requestHeaders,
    requestHeaders: _wrPageHeaders(r.requestHeaders),
    rawBodyB64: webRequestStatesBody(r.method)
      ? _wrRequestBody(r.requestBodyRaw, _wrHeader(r.requestHeaders, "content-type")) : w.capture.requestBodyB64,
    responseHeaders: _wrReadableResponseHeaders(r.responseHeaders),
    contentType: _wrHeader(r.responseHeaders, "content-type"),
    redirects: r.redirects,
  });
}

function _wrTryJoin(d, r) {
  var keyW = d.waiting.get(_wrKey(r.method, r.url));
  var w = keyW === undefined ? undefined : keyW.get(r.ordinal);
  if (w === undefined) return;
  keyW.delete(r.ordinal);
  _wrSettle(d, r, w);
}

/* intercept.js's capture of one fetch() or XHR, from document `documentId`, joined to the browser's record
   of the same request; `deliver(wr)` runs once with the record's browser-stated fields when both halves have
   arrived. The capture is the page's: a key or ordinal no record holds leaves it waiting, never asserted. */
function webRequestJoin(documentId, tabId, capture, deliver) {
  var key = _wrKey(capture.method, capture.url);
  var d = _wrDocs.get(documentId);
  if (d !== undefined && d.left) {
    webRequestCensus.captureAfterLeft++;
    console.warn("[web-request] " + key + " capture refused: its document left");
    return;
  }
  d = _wrDocOf(documentId, tabId);
  var keyR = d.records.get(key);
  var r = keyR === undefined ? undefined : keyR.get(capture.seq);
  if (r !== undefined && r.done) {
    _wrSettle(d, r, { capture: capture, deliver: deliver });
    return;
  }
  var n = d.next.get(key);
  if (r === undefined && n !== undefined && capture.seq < n.count) {
    console.warn("[web-request] " + key + " capture refused: ordinal " + capture.seq + " is already joined");
    return;
  }
  if (!d.waiting.has(key)) d.waiting.set(key, new Map());
  if (d.waiting.get(key).has(capture.seq)) {
    console.warn("[web-request] " + key + " capture refused: ordinal " + capture.seq + " was captured twice");
    return;
  }
  d.waiting.get(key).set(capture.seq, { capture: capture, deliver: deliver });
}

/* Reclaims the live document `documentId`: every record and waiting capture it holds is counted by how it
   ended, and its in-flight records leave _wrById. */
function _wrReclaim(documentId, d) {
  var left = { unjoined: 0, failed: 0, inFlight: 0, waiting: 0 };
  for (var keyR of d.records.values()) {
    for (var r of keyR.values()) {
      if (!r.done) { left.inFlight++; _wrById.delete(r.requestId); }
      else if (r.failed !== null) left.failed++;
      else left.unjoined++;
    }
  }
  for (var keyW of d.waiting.values()) left.waiting += keyW.size;
  for (var rest of _wrById.values())
    DCHECK(rest.documentId !== documentId, "request " + rest.requestId + " of document " + documentId +
           " is in flight in _wrById and in none of the document's records, so it would outlive the document");
  webRequestCensus.leftUnjoined += left.unjoined;
  webRequestCensus.leftFailed += left.failed;
  webRequestCensus.leftInFlight += left.inFlight;
  webRequestCensus.leftWaiting += left.waiting;
  if (left.unjoined + left.inFlight + left.waiting > 0)
    console.warn("[web-request] document " + documentId + " left with " + left.unjoined + " complete record(s) " +
                 "no capture joined, " + left.inFlight + " in flight and " + left.waiting + " capture(s) no " +
                 "record completed");
}

/* The document `documentId` was destroyed (see webRequestDocumentPort): its records and waiting captures
   are reclaimed, and it stays known as left until its tab closes. */
function webRequestDocumentLeft(documentId, tabId) {
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
    _wrDocs.delete(id);
  }
}
