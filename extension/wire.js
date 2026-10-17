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
// (measured), so the zone pings each send; a worker that ends anyway disconnects the port, and the zone
// sends an idempotent request again or answers a network error naming it (lib/safe-fetch.js's _wireRelay).

const _WIRE_ORIGIN = new URL(chrome.runtime.getURL("")).origin;
const _WIRE_SENDER = chrome.runtime.getURL("ast-worker.html");
speakAsInstalledReady();

/* A send's fragment, which names it to webRequest (whose URLs keep the fragment, measured in Chrome 155) and
   never reaches a server: `t=<nonce>`, or `t=<scope>.<nonce>` for a send that speaks as a document, whose
   rule (speak-as-rule.js) matches the URL up to the nonce. The zone's own fragment is not the wire's: Fetch
   never sends one, and the zone keeps its URLs. */
function _wireNonce() {
  const b = new Uint8Array(16);
  crypto.getRandomValues(b);
  return Array.from(b, (x) => x.toString(16).padStart(2, "0")).join("");
}
function _wireNonceOf(url) {
  const m = /#t=(?:[0-9a-f]{32}\.[0-9]+\.)?([0-9a-f]{32})$/.exec(url);
  return m === null ? null : m[1];
}

/* nonce -> the send's record {url, scope, redirect: a promise of its 3xx, resolveRedirect, setCookie: a promise
   of its reply's Set-Cookie fields, resolveSetCookie, requestId}; requestId -> the same record, bound when
   onBeforeSendHeaders reports the send's URL, once, since a "manual" redirect never sends its hop. */
const _wireBySend = new Map();
const _wireByRequest = new Map();
/* requestId -> the speak-as headers onBeforeSendHeaders reported, which precede declarativeNetRequest's
   changes (onSendHeaders reports after them, measured in Chrome 148 and 155). */
const _wireBefore = new Map();

/* Which context and which request. Every other extension context is sealed or has `connect-src 'none'`, so a
   request initiated by this extension in no tab and no document is this worker's; a page's service worker is
   also in no tab and no document, and its initiator is the page's origin. A filter's `tabId` of TAB_ID_NONE
   matches every tab's requests too (measured in Chrome 155: a tab's fetch reached such a listener), so each
   listener reads `tabId` itself. The types are every type a speak-as rule's condition names. */
const _WIRE_FILTER = { urls: ["http://*/*", "https://*/*"], types: ["xmlhttprequest", "other"] };
function _wireSpeakAsHeaders(list) {
  const out = {};
  for (const h of list) {
    const n = h.name.toLowerCase();
    if (SPEAK_AS_HEADER_NAMES.indexOf(n) >= 0) out[n] = h.value;
  }
  return out;
}
chrome.webRequest.onBeforeSendHeaders.addListener((d) => {
  const nonce = _wireNonceOf(d.url), rec = nonce === null ? undefined : _wireBySend.get(nonce);
  if (d.tabId === chrome.tabs.TAB_ID_NONE) _wireBefore.set(d.requestId, _wireSpeakAsHeaders(d.requestHeaders));
  if (d.tabId !== chrome.tabs.TAB_ID_NONE || d.initiator !== _WIRE_ORIGIN) {
    CHECK(rec === undefined, "a request of tab " + d.tabId + " initiated by " + d.initiator + " carries a live " +
          "wire send's nonce — only this worker's own fetch carries it");
    return;
  }
  CHECK(rec !== undefined && d.documentId === undefined,
        "this extension sent " + d.url + " in no tab and no document, and it is not a live wire send — only " +
        "this worker's fetch sends, and every send carries its own nonce");
  CHECK(rec.requestId === null, "the wire send of " + d.url + " went out twice — a \"manual\" redirect never " +
        "sends its hop, so a second request carrying the send's fragment is a hop the platform followed");
  DCHECK(rec.url === d.url, "webRequest reported the wire send of " + rec.url + " as " + d.url);
  rec.requestId = d.requestId;
  _wireByRequest.set(d.requestId, rec);
}, _WIRE_FILTER, ["requestHeaders", "extraHeaders"]);

/* What a no-tab request finally carries, after declarativeNetRequest: exactly the speak-as headers it carried
   before, changed by the one rule of its own send's scope and by no other. A request that is not a send
   speaking as a document matches no rule and carries its own headers unchanged. This is the boundary that
   a rule changes only the sends it was installed for, so it is a CHECK; a rule's condition names no tab. */
chrome.webRequest.onSendHeaders.addListener((d) => {
  if (d.tabId !== chrome.tabs.TAB_ID_NONE) return;
  const before = _wireBefore.get(d.requestId);
  _wireBefore.delete(d.requestId);
  const rec = _wireByRequest.get(d.requestId);
  const matched = speakAsInstalled.filter((r) => speakAsRuleMatches(r, d));
  /* A request already on the network when this worker started has no record before its rules ran, so what
     is checked is that no rule matched it; this worker's own sends end with the worker that made them. */
  if (before === undefined) {
    CHECK(rec === undefined && matched.length === 0, "the request " + d.method + " " + d.url + " (initiator " +
          d.initiator + ") reached onSendHeaders with no onBeforeSendHeaders record and matched the speak-as " +
          "rules " + JSON.stringify(matched.map((r) => r.id)));
    return;
  }
  // A rule's prefix holds its scope, so the one rule that matches a send's URL is its own scope's.
  CHECK(matched.length === (rec !== undefined && rec.scope !== null ? 1 : 0),
        "the request " + d.method + " " + d.url + " (initiator " + d.initiator + ") matched the speak-as rules " +
        JSON.stringify(matched.map((r) => r.id)) + ", not exactly its own send's — a rule changed a request it " +
        "was not installed for");
  const expect = Object.assign({}, before);
  if (matched.length === 1)
    for (const h of matched[0].action.requestHeaders) {
      const n = h.header.toLowerCase();
      if (h.operation === "remove") delete expect[n]; else expect[n] = h.value;
    }
  const after = _wireSpeakAsHeaders(d.requestHeaders);
  for (const n of SPEAK_AS_HEADER_NAMES)
    CHECK(after[n] === expect[n], "the request " + d.method + " " + d.url + " carries " + n + ": " +
          JSON.stringify(after[n]) + " where its own headers and its rule make " + JSON.stringify(expect[n]));
}, _WIRE_FILTER, ["requestHeaders", "extraHeaders"]);
chrome.webRequest.onErrorOccurred.addListener((d) => { _wireBefore.delete(d.requestId); }, _WIRE_FILTER);

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
  rec.resolveRedirect({ status: d.statusCode, statusLine: d.statusLine,
                       headers: headers.filter((h) => !/^set-cookie2?$/i.test(h[0])) });
}, _WIRE_FILTER, ["responseHeaders", "extraHeaders"]);
/* A reply's Set-Cookie fields, a 3xx's included: the worker's Response hides them (Fetch §2.2.2 "Headers"' forbidden
   response-header names), and webRequest reports each one only with extraHeaders (measured in Chrome 148).
   A field webRequest reports as bytes is null, which the zone ignores. */
chrome.webRequest.onHeadersReceived.addListener((d) => {
  const rec = _wireByRequest.get(d.requestId);
  if (!rec) return;
  rec.resolveSetCookie(d.responseHeaders.filter((h) => h.name.toLowerCase() === "set-cookie")
    .map((h) => (typeof h.value === "string" ? h.value : null)));
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

/* One send: `m` is {href, scope, init: {method, headers: [[name, value]] | null, body: null | {text} | {b64}}},
   `href` fragment-less and `scope` its speak-as rule's or null. The port is answered {head: {status,
   statusText, headers, setCookie}}, then {chunk: base64} per body chunk, then {end}; or {error} for Fetch
   §5.6's network error. A 3xx's head is the record webRequest reported and its body is none. A data: URL has
   no webRequest event, no fragment and no Set-Cookie. No answer carries the send's fragment. */
async function _wireSend(port, m, signal) {
  CHECK(typeof m.href === "string" && /^(https?|data):/.test(m.href) && !m.href.includes("#") && !!m.init &&
        typeof m.init.method === "string",
        "a wire send names no fragment-less http(s) or data: URL and method — lib/safe-fetch.js's _wire states both");
  CHECK(m.scope === null || (typeof m.scope === "string" && SPEAK_AS_SCOPE.test(m.scope) && !m.href.startsWith("data:")),
        "a wire send names the scope " + JSON.stringify(m.scope) + ", which is not a document token and an index");
  const data = m.href.startsWith("data:");
  const nonce = _wireNonce();
  const url = data ? m.href : m.href + "#t=" + (m.scope === null ? "" : m.scope + ".") + nonce;
  const rec = { url, scope: m.scope, nonce, requestId: null };
  rec.redirect = new Promise((resolve) => { rec.resolveRedirect = resolve; });
  rec.setCookie = new Promise((resolve) => { rec.resolveSetCookie = resolve; });
  if (!data) _wireBySend.set(nonce, rec);
  const post = (msg) => {
    CHECK(!JSON.stringify(msg).includes(nonce), "a wire answer carries the send's fragment nonce");
    try { port.postMessage(msg); } catch (e) { /* the zone ended the send */ }
  };
  try {
    await speakAsInstalledReady();
    /* Cache mode "no-store" (Fetch §2.2.5 "Requests": "Fetch behaves as if there is no HTTP cache at all"):
       the worker's one cache is keyed by the URL without its fragment, so it would answer one document's
       send with another's reply, and "default" revalidates a stale-while-revalidate entry with a request of
       its own, which carries the send's fragment after the send has ended (in Chrome 155 such a request of a
       `stale-while-revalidate` reply reached the header CHECK below as no live send). */
    const init = { method: m.init.method, headers: m.init.headers || [], credentials: "omit", redirect: "manual",
                   cache: "no-store", signal };
    if (m.init.body !== null)
      init.body = m.init.body.text !== undefined ? m.init.body.text : _wireBytes(m.init.body.b64);
    let resp;
    try { resp = await fetch(url, init); }
    catch (e) { post({ error: String((e && e.message) || e) }); return; }
    if (resp.type === "opaqueredirect") {
      const r = await rec.redirect;
      const reason = /^HTTP\/\S+ \d{3} ?(.*)$/.exec(r.statusLine);
      post({ head: { status: r.status, statusText: reason ? reason[1] : "", headers: r.headers,
                     setCookie: await rec.setCookie } });
      post({ end: true });
      return;
    }
    const out = [];
    resp.headers.forEach((v, k) => out.push([k, v]));
    const setCookie = data ? [] : await rec.setCookie;
    post({ head: { status: resp.status, statusText: resp.statusText, headers: out, setCookie } });
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
    _wireBySend.delete(nonce);
    if (rec.requestId !== null) _wireByRequest.delete(rec.requestId);
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
