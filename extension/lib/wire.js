// lib/wire.js — the trusted zone's one network, in the offscreen document. lib/safe-fetch.js decides every
// request and hands `wireSend` the platform fetch it took at load, which no other script of this document holds.
// Each send's URL carries a fragment naming it, `#t=<nonce>`, or `#t=<scope>.<nonce>` for a send that speaks as
// a document, whose declarativeNetRequest rule matches the URL up to the nonce (speak-as-rule.js); webRequest
// reports URLs with their fragment and a server never receives one. This document has no chrome.webRequest, so
// the service worker relays each webRequest event of this document's requests as `__evt WIRE`, in the browser's
// order (wire-events.js), and `wireEvent` binds it to its send by the nonce: the 3xx of a "manual" redirect, the
// reply's Set-Cookie fields, and the browser's reason for a network error.

/* nonce -> the live send {url, ruleId, requestId, redirect, setCookie, ended} with each promise's resolver. A send
   is live from before its fetch until webRequest reports its lifecycle's end, so every event of it finds it. */
var _wireLive = new Map();

function _wireNonce() {
  var b = new Uint8Array(16);
  crypto.getRandomValues(b);
  return Array.from(b, function (x) { return x.toString(16).padStart(2, "0"); }).join("");
}
function _wireNonceOf(url) {
  var m = /#t=(?:[0-9a-f]{32}\.[0-9]+\.)?([0-9a-f]{32})$/.exec(url);
  return m === null ? null : m[1];
}

function _wireRecord(url, nonce, ruleId) {
  var rec = { url: url, ruleId: ruleId, requestId: null };
  rec.redirect = new Promise(function (resolve) { rec.resolveRedirect = resolve; });
  rec.setCookie = new Promise(function (resolve) { rec.resolveSetCookie = resolve; });
  rec.ended = new Promise(function (resolve) { rec.resolveEnded = resolve; });
  _wireLive.set(nonce, rec);
  return rec;
}

/* `body` with `done` called once when it ends, errors or is cancelled; null for no body. */
function _wireBodyUntil(body, done) {
  if (body === null) { done(); return null; }
  var reader = body.getReader();
  return new ReadableStream({
    pull: function (c) {
      return reader.read().then(function (step) {
        if (step.done) { done(); c.close(); } else c.enqueue(step.value);
      }, function (e) { done(); c.error(e); });
    },
    cancel: function (why) { done(); return reader.cancel(why); }
  });
}

/* One send, answered {response, setCookie}: the reply as a Response, a 3xx as the record webRequest reported
   (its body none), and the reply's Set-Cookie fields, which a Response hides (Fetch §2.2.2 "Headers"' forbidden
   response-header names; a field webRequest reports as bytes is null). A rejection is a TypeError whose
   `netError` is the browser's reason, read from the lifecycle's end, or null for an aborted send or a data: URL,
   which has no webRequest event. A send that speaks as a document (`init.speakAs`) runs under its document's rule
   (lib/speak-as.js's speakAsAcquire), held until the send settles.
   Cache mode "no-store" (Fetch §2.2.5 "Requests": "Fetch behaves as if there is no HTTP cache at all"): the
   extension's one cache is keyed by the URL without its fragment, so it would answer one document's send with
   another's reply, and a stale-while-revalidate revalidation is a request of its own after the send ended. */
async function wireSend(platformFetch, href, init) {
  CHECK(typeof platformFetch === "function" && typeof init.method === "string" && init.credentials === "omit" &&
        init.redirect === "manual" && (init.headers === undefined || Array.isArray(init.headers)) &&
        (init.speakAs === undefined || init.speakAs === null || typeof init.speakAs === "object"),
        "a send reached the wire without the platform fetch, a method, credentials \"omit\", redirect \"manual\", a " +
        "header list of pairs and a speak-as or none — cookies are the speak-as rule's and every hop is this zone's");
  var u = new URL(href);
  u.hash = "";
  CHECK(u.protocol === "http:" || u.protocol === "https:" || u.protocol === "data:",
        "the wire was handed " + u.href + ", which is not an http(s) or data: URL");
  var data = u.protocol === "data:";
  var speak = init.speakAs === undefined ? null : init.speakAs;
  CHECK(!(data && speak !== null), "a data: URL speaks as no document — it reaches no other party");
  var held = speak === null ? null : await speakAsAcquire(u.href, init.method, speak);
  var released = false;
  function release() { if (!released && held !== null) held.release(); released = true; }
  /* A send whose signal is already aborted is never made: a redirect hop after the abort would otherwise go out,
     since an aborted signal fires no further `abort`. */
  if (init.signal && init.signal.aborted) {
    release();
    throw new TypeError("wire: the send of " + u.href + " was aborted before it was made");
  }
  var nonce = _wireNonce();
  var url = data ? u.href : u.href + "#t=" + (held === null ? "" : held.scope + ".") + nonce;
  var rec = data ? null : _wireRecord(url, nonce, held === null ? null : held.ruleId);
  var resp;
  try {
    resp = await platformFetch(url, { method: init.method, headers: init.headers === undefined ? [] : init.headers,
                                      body: init.body, credentials: "omit", redirect: "manual", cache: "no-store",
                                      signal: init.signal });
  } catch (e) {
    RETHROW_FATAL(e);
    var aborted = !!(init.signal && init.signal.aborted);
    var err = new TypeError("wire: " + (aborted ? "the send of " + u.href + " was aborted"
                                                : String((e && e.message) || e)));
    /* Every http(s) request of this document reaches webRequest, a refused one included (measured in Chrome 155:
       `net::ERR_UNSAFE_PORT` and an abort each ended in onErrorOccurred), so its end is awaited. */
    err.netError = rec === null || aborted ? null : await rec.ended;
    release();
    throw err;
  }
  if (resp.type === "opaqueredirect") {
    var r = await rec.redirect;
    var reason = /^HTTP\/\S+ \d{3} ?(.*)$/.exec(r.statusLine);
    var setCookie3xx = await rec.setCookie;
    release();
    return { response: new Response(null, { status: r.status, statusText: reason ? reason[1] : "", headers: r.headers }),
             setCookie: setCookie3xx };
  }
  var setCookie = data ? [] : await rec.setCookie;
  return { response: new Response(_wireBodyUntil(resp.body, release),
                                  { status: resp.status, statusText: resp.statusText, headers: resp.headers }),
           setCookie: setCookie };
}

/* One webRequest event of this document's requests, as wire-events.js relays it: {ev, requestId, url} and, by
   event, `matched` (onSendHeaders: the ids of the speak-as rules that matched), `setCookie` (onHeadersReceived),
   `statusCode`, `statusLine` and `headers` (onBeforeRedirect), `error` (onErrorOccurred). */
function wireEvent(ev) {
  DCHECK(typeof ev.ev === "string" && typeof ev.requestId === "string" && typeof ev.url === "string",
         "the service worker relayed a wire event with no name, request id or URL: " + JSON.stringify(ev));
  var nonce = _wireNonceOf(ev.url), rec = nonce === null ? undefined : _wireLive.get(nonce);
  CHECK(rec !== undefined, "this document sent " + ev.url + " (" + ev.ev + ") and it is not a live wire send — " +
        "only wireSend sends, every send carries its own nonce, and it stays live until its lifecycle ends");
  if (rec.requestId === null) rec.requestId = ev.requestId;
  CHECK(rec.requestId === ev.requestId, "the wire send of " + rec.url + " went out twice — a \"manual\" redirect " +
        "never sends its hop, so a second request carrying the send's fragment is a hop the platform followed");
  DCHECK(rec.url === ev.url, "webRequest reported the wire send of " + rec.url + " as " + ev.url);
  if (ev.ev === "onSendHeaders") {
    CHECK(Array.isArray(ev.matched) && (rec.ruleId === null ? ev.matched.length === 0
                                        : ev.matched.length === 1 && ev.matched[0] === rec.ruleId),
          "the wire send of " + rec.url + " matched the speak-as rules " + JSON.stringify(ev.matched) + ", not " +
          "exactly its own " + JSON.stringify(rec.ruleId) + " — a rule changed a request it was not installed for");
  } else if (ev.ev === "onHeadersReceived") {
    DCHECK(Array.isArray(ev.setCookie), "the service worker relayed a reply's head with no Set-Cookie list");
    rec.resolveSetCookie(ev.setCookie);
  } else if (ev.ev === "onBeforeRedirect") {
    DCHECK(Array.isArray(ev.headers) && typeof ev.statusLine === "string" && Number.isInteger(ev.statusCode),
           "the service worker relayed the redirect of " + rec.url + " without its status or headers — the hop is " +
           "judged on its Location, Referrer-Policy and CORS headers");
    rec.resolveRedirect({ status: ev.statusCode, statusLine: ev.statusLine, headers: ev.headers });
  } else if (ev.ev === "onCompleted" || ev.ev === "onErrorOccurred") {
    rec.resolveEnded(ev.ev === "onErrorOccurred" ? ev.error : null);
    _wireLive.delete(nonce);
  } else {
    DCHECK(ev.ev === "onBeforeRequest", "the service worker relayed the wire event " + ev.ev + ", which this " +
           "document does not take");
  }
}

if (typeof self !== "undefined") {
  self.wireSend = wireSend;
  self.wireEvent = wireEvent;
}
