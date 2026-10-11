// wire.js — the trusted zone's one network: this service worker's fetch. lib/safe-fetch.js decides every
// request in the offscreen document and hands this worker only the call that reaches the network, one port
// per send ("wire"). The worker's fetch is the only platform fetch the extension spends on the network; the
// offscreen document's is sealed (lib/safe-fetch.js), and webRequest reports none of that cross-origin
// isolated document's requests to this worker, since Chromium's WebRequestEventRouter::ListenerMatchesRequest
// drops an extension process's request for a listener in another process and isolation puts the extension's
// pages in a process apart from this worker (measured in Chrome 148).
// A send is asked for with redirect "manual", which never sends the hop and answers an opaque-redirect with no
// headers; the 3xx itself is what webRequest reported here, and it is answered to the zone as the response, so
// the zone follows each hop itself (Fetch §4.5 "HTTP-redirect fetch"). lib/web-request.js keeps documents'
// traffic, joined in the offscreen document to a page's capture; this file keeps the worker's own, joined here.
// Messages carry JSON only (a Uint8Array arrives as an object of indices, an ArrayBuffer as {}, measured), so
// bodies cross as base64 chunks. The worker is ended after 30 s with no message even with a fetch pending
// (measured), so the zone pings each send; a worker that ends anyway disconnects the port, which the zone
// answers as a network error naming it.

const _WIRE_ORIGIN = new URL(chrome.runtime.getURL("")).origin;
const _WIRE_SENDER = chrome.runtime.getURL("ast-worker.html");
/* The header that names a send to webRequest. onBeforeSendHeaders reports a request's headers before
   declarativeNetRequest modifies them, and onSendHeaders after (measured in Chrome 148), so the session rule
   `_WIRE_NONCE_RULE` removes it from every request of this worker and neither the server nor a page sees it;
   a rule condition cannot match a request header ("Unexpected property: 'requestHeaders'", measured). */
const _WIRE_NONCE_HEADER = "x-apiclient-wire";
const _WIRE_NONCE_RULE = 1;
const _wireRuleReady = chrome.declarativeNetRequest.updateSessionRules({
  removeRuleIds: [_WIRE_NONCE_RULE],
  addRules: [{ id: _WIRE_NONCE_RULE, priority: 1,
               action: { type: "modifyHeaders", requestHeaders: [{ header: _WIRE_NONCE_HEADER, operation: "remove" }] },
               condition: { initiatorDomains: [chrome.runtime.id], tabIds: [chrome.tabs.TAB_ID_NONE] } }],
});
/* nonce -> the send's record {redirect: a promise of its 3xx, resolveRedirect}; requestId -> the same record,
   bound when onBeforeSendHeaders shows the nonce. The requestId holds across the request's life. */
const _wireByNonce = new Map();
const _wireByRequest = new Map();
/* The keys of the sends this worker has on the network. A speak-as rule matches by method and URL (a rule
   condition cannot match the nonce), so lib/safe-fetch.js sends one request per URL at a time
   (`_sendSerialized`), hops included, which this asserts. */
const _wireInFlight = new Set();

function _wireKey(method, url) {
  const u = new URL(url);
  u.hash = "";
  return method + " " + u.href;
}
function _wireNonce() {
  const b = new Uint8Array(16);
  crypto.getRandomValues(b);
  return Array.from(b, (x) => x.toString(16).padStart(2, "0")).join("");
}

/* Which context and which request. Every other extension context is sealed or has `connect-src 'none'`, so
   a request initiated by this extension in no tab and no document is this worker's; a page's service worker
   is also in no tab and no document, and its initiator is the page's origin. The nonce names the send. */
const _WIRE_FILTER = { urls: ["http://*/*", "https://*/*"], types: ["xmlhttprequest"], tabId: chrome.tabs.TAB_ID_NONE };
chrome.webRequest.onBeforeSendHeaders.addListener((d) => {
  const nonce = d.requestHeaders.filter((h) => h.name.toLowerCase() === _WIRE_NONCE_HEADER).map((h) => h.value);
  if (nonce.length === 0) return;
  CHECK(d.initiator === _WIRE_ORIGIN && d.tabId === chrome.tabs.TAB_ID_NONE && d.documentId === undefined,
        "a request carrying the wire's nonce was initiated by " + d.initiator + " in tab " + d.tabId +
        " document " + d.documentId + " — only this worker's own fetch carries it");
  const rec = _wireByNonce.get(nonce[0]);
  CHECK(nonce.length === 1 && !!rec, "webRequest reported a wire nonce no send of this worker holds");
  DCHECK(!_wireByRequest.has(d.requestId) || _wireByRequest.get(d.requestId) === rec,
         "webRequest's request " + d.requestId + " is bound to two sends");
  _wireByRequest.set(d.requestId, rec);
}, _WIRE_FILTER, ["requestHeaders", "extraHeaders"]);
chrome.webRequest.onBeforeRedirect.addListener((d) => {
  const rec = _wireByRequest.get(d.requestId);
  if (!rec) return;
  DCHECK(Array.isArray(d.responseHeaders) && typeof d.statusLine === "string",
         "webRequest reported the redirect of " + d.url + " without its headers or status line — the listener " +
         "asks for responseHeaders, and the hop is judged on its Location, Referrer-Policy and CORS headers");
  const headers = d.responseHeaders.map((h) => {
    DCHECK(typeof h.value === "string",
           "webRequest reported the redirect header " + h.name + " of " + d.url + " as bytes — a 3xx's fields " +
           "this zone reads are text, and a binary one would be dropped from the hop's judgement");
    return [h.name, h.value];
  });
  rec.resolveRedirect({ status: d.statusCode, statusLine: d.statusLine, headers });
}, _WIRE_FILTER, ["responseHeaders", "extraHeaders"]);

function _wireB64(u8) {
  let bin = "";
  for (let i = 0; i < u8.length; i += 8192) bin += String.fromCharCode.apply(null, u8.subarray(i, i + 8192));
  return btoa(bin);
}
function _wireBytes(b64) {
  const bin = atob(b64);
  const u = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) u[i] = bin.charCodeAt(i);
  return u;
}

/* One send: `m` is {href, init: {method, headers: [[name, value]] | null, body: null | {text} | {b64}}}. The
   port is answered {head: {status, statusText, headers}}, then {chunk: base64} per body chunk, then {end}; or
   {error} for Fetch §5.6's network error. A 3xx's head is the record webRequest reported and its body is none. */
async function _wireSend(port, m, signal) {
  CHECK(typeof m.href === "string" && /^(https?|data):/.test(m.href) && !!m.init && typeof m.init.method === "string",
        "a wire send names no http(s) or data: URL and method — lib/safe-fetch.js's _wire states both");
  CHECK(m.init.headers === null || m.init.headers.every((h) => String(h[0]).toLowerCase() !== _WIRE_NONCE_HEADER),
        "a wire send states the wire's own nonce header");
  const key = _wireKey(m.init.method, m.href);
  DCHECK(!_wireInFlight.has(key),
         "two sends of " + key + " are on the network at once — lib/safe-fetch.js sends one per URL at a time, " +
         "and a speak-as rule names its send by method and URL alone");
  _wireInFlight.add(key);
  const nonce = _wireNonce();
  const rec = {};
  rec.redirect = new Promise((resolve) => { rec.resolveRedirect = resolve; });
  _wireByNonce.set(nonce, rec);
  const post = (msg) => { try { port.postMessage(msg); } catch (e) { /* the zone ended the send */ } };
  try {
    await _wireRuleReady;
    const headers = (m.init.headers || []).concat([[_WIRE_NONCE_HEADER, nonce]]);
    const init = { method: m.init.method, headers, credentials: "omit", redirect: "manual", signal };
    if (m.init.body !== null)
      init.body = m.init.body.text !== undefined ? m.init.body.text : _wireBytes(m.init.body.b64);
    let resp;
    try { resp = await fetch(m.href, init); }
    catch (e) { post({ error: String((e && e.message) || e) }); return; }
    if (resp.type === "opaqueredirect") {
      const r = await rec.redirect;
      const reason = /^HTTP\/\S+ \d{3} ?(.*)$/.exec(r.statusLine);
      post({ head: { status: r.status, statusText: reason ? reason[1] : "", headers: r.headers } });
      post({ end: true });
      return;
    }
    const out = [];
    resp.headers.forEach((v, k) => out.push([k, v]));
    post({ head: { status: resp.status, statusText: resp.statusText, headers: out } });
    if (resp.body) {
      const reader = resp.body.getReader();
      for (;;) {
        const step = await reader.read();
        if (step.done) break;
        post({ chunk: _wireB64(step.value) });
      }
    }
    post({ end: true });
  } catch (e) {
    post({ error: String((e && e.message) || e) });
  } finally {
    _wireInFlight.delete(key);
    _wireByNonce.delete(nonce);
    for (const [id, r] of _wireByRequest) if (r === rec) _wireByRequest.delete(id);
  }
}

chrome.runtime.onConnect.addListener((port) => {
  if (port.name !== "wire") return;
  CHECK(!!port.sender && port.sender.url === _WIRE_SENDER,
        "a wire port was opened by " + JSON.stringify(port.sender && port.sender.url) + " — only the offscreen " +
        "document's lib/safe-fetch.js sends, after every gate it holds");
  const controller = new AbortController();
  let started = false;
  port.onDisconnect.addListener(() => controller.abort());
  port.onMessage.addListener((m) => {
    if (m.ping || m.ack) return;
    CHECK(!started, "a wire port carried a second send — one port is one send");
    started = true;
    _wireSend(port, m, controller.signal);
  });
});
