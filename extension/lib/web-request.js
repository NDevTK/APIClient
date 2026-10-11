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

/* requestId -> the request's record as its events arrive. A redirect reuses the requestId. */
var _wrById = new Map();
/* documentId -> (key -> the document's records of that key in onBeforeRequest order). */
var _wrByDoc = new Map();
/* documentId -> {tabId, keys: key -> (ordinal -> {capture, deliver})} for captures whose record is not
   complete yet. */
var _wrWaiting = new Map();

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
  if (ev.ev === "onBeforeRequest") {
    if (r) return;   // a redirect's next hop: the record keeps the first request line and body
    DCHECK(ev.requestBody === undefined || webRequestStatesBody(ev.method),
           "webRequest reported a body for a " + ev.method + " request — this file takes the body from the " +
           "browser for POST and PUT alone and from intercept.js otherwise, so that split is now stale");
    r = { requestId: ev.requestId, documentId: ev.documentId, tabId: ev.tabId, frameId: ev.frameId,
          method: ev.method, url: ev.url, requestBodyRaw: ev.requestBody, requestHeaders: null,
          status: null, statusText: null, responseHeaders: null, redirects: [], done: false, failed: null,
          joined: false };
    _wrById.set(ev.requestId, r);
    var byKey = _wrByDoc.get(ev.documentId);
    if (!byKey) { byKey = new Map(); _wrByDoc.set(ev.documentId, byKey); }
    var key = _wrKey(ev.method, ev.url);
    if (!byKey.has(key)) byKey.set(key, []);
    var prev = byKey.get(key)[byKey.get(key).length - 1];
    DCHECK(!prev || Number(prev.requestId) < Number(ev.requestId),
           "a webRequest onBeforeRequest arrived out of the browser's order for " + key + " (" + ev.requestId +
           " after " + (prev && prev.requestId) + ") — background.js relays through one chain, and the ordinal " +
           "a capture joins on is this order");
    r.ordinal = byKey.get(key).length;
    byKey.get(key).push(r);
    return;
  }
  DCHECK(!!r, "a webRequest " + ev.ev + " arrived for request " + ev.requestId + " before its onBeforeRequest — " +
              "the browser orders a request's events and the relay keeps that order");
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
    _wrTryJoin(r);
  } else if (ev.ev === "onErrorOccurred") {
    r.done = true;
    r.failed = ev.error;
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

function _wrDeliver(r, w) {
  DCHECK(!r.joined, "a webRequest record was joined to a second capture — each capture names one ordinal and " +
                    "each ordinal holds one record");
  r.joined = true;
  if (r.status !== w.capture.observedStatus) {
    console.warn("[web-request] " + r.method + " " + r.url + " capture refused (status-mismatch): the page " +
                 "observed " + String(w.capture.observedStatus) + " and the browser reports " + r.status +
                 " for ordinal " + r.ordinal);
    return;
  }
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

function _wrTryJoin(r) {
  var docW = _wrWaiting.get(r.documentId);
  var keyW = docW && docW.keys.get(_wrKey(r.method, r.url));
  var w = keyW && keyW.get(r.ordinal);
  if (!w) return;
  keyW.delete(r.ordinal);
  _wrDeliver(r, w);
}

/* intercept.js's capture of one fetch() or XHR, from document `documentId`, joined to the browser's record
   of the same request; `deliver(wr)` runs once with the record's browser-stated fields when both halves have
   arrived. The capture is the page's: a key or ordinal no record holds leaves it waiting, never asserted. */
function webRequestJoin(documentId, tabId, capture, deliver) {
  var key = _wrKey(capture.method, capture.url);
  var byKey = _wrByDoc.get(documentId);
  var r = byKey && byKey.get(key) && byKey.get(key)[capture.seq];
  if (r && r.done) {
    if (r.failed !== null || r.joined) {
      console.warn("[web-request] " + key + " capture refused: ordinal " + capture.seq + " is " +
                   (r.joined ? "already joined" : "a request that failed (" + r.failed + ")"));
      return;
    }
    _wrDeliver(r, { capture: capture, deliver: deliver });
    return;
  }
  var docW = _wrWaiting.get(documentId);
  if (!docW) { docW = { tabId: tabId, keys: new Map() }; _wrWaiting.set(documentId, docW); }
  if (!docW.keys.has(key)) docW.keys.set(key, new Map());
  if (docW.keys.get(key).has(capture.seq)) {
    console.warn("[web-request] " + key + " capture refused: ordinal " + capture.seq + " was captured twice");
    return;
  }
  docW.keys.get(key).set(capture.seq, { capture: capture, deliver: deliver });
}

/* A tab closed: its documents' requests and waiting captures go with it. */
function webRequestTabRemoved(tabId) {
  var docs = new Set();
  for (var [id, r] of _wrById) if (r.tabId === tabId) { docs.add(r.documentId); _wrById.delete(id); }
  for (var [d, w] of _wrWaiting) if (w.tabId === tabId) docs.add(d);
  for (var doc of docs) { _wrByDoc.delete(doc); _wrWaiting.delete(doc); }
}
