// wire-events.js — the service worker's half of the zone's wire. The offscreen document sends every request
// (lib/wire.js) and has no chrome.webRequest, so this worker observes its requests and relays each event to it
// as `__evt WIRE`, one at a time in the browser's order. This worker sends nothing itself. webRequest reports the
// offscreen document's requests here, in no tab and with its documentId (measured in Chrome 155). Beside the
// relay it checks the one fact only it can: what declarativeNetRequest did to each request's headers.

const _WIRE_ORIGIN = new URL(chrome.runtime.getURL("")).origin;
speakAsInstalledReady();

/* Which context. Every other extension context is sealed or has `connect-src 'none'`, and this worker sends
   nothing, so a request initiated by this extension in no tab is the offscreen document's; a page's service
   worker is also in no tab, and its initiator is the page's origin. A filter's `tabId` of TAB_ID_NONE matches
   every tab's requests too (measured in Chrome 155: a tab's fetch reached such a listener), so each listener
   reads `tabId` itself. The types are every type a speak-as rule's condition names. */
const _WIRE_FILTER = { urls: ["http://*/*", "https://*/*"], types: ["xmlhttprequest", "other"] };
function _wireOwn(d) {
  if (d.tabId !== chrome.tabs.TAB_ID_NONE || d.initiator !== _WIRE_ORIGIN) return false;
  CHECK(typeof d.documentId === "string", "this extension sent " + d.url + " in no tab and no document — only " +
        "the offscreen document sends (lib/wire.js), and this worker sends nothing");
  return true;
}

/* The offscreen document's id as getContexts last answered it. A request of any other document is one of an
   offscreen document that has ended, whose sends ended with it, so its events are not relayed. */
let _wireOffscreenDoc = null;
async function _wireIsOffscreen(documentId) {
  if (documentId === _wireOffscreenDoc) return true;
  const c = await chrome.runtime.getContexts({ contextTypes: ["OFFSCREEN_DOCUMENT"] });
  _wireOffscreenDoc = c.length === 1 ? c[0].documentId : null;
  return documentId === _wireOffscreenDoc;
}
/* Every event runs through one chain, so the offscreen document receives them in the browser's order: a send's
   lifecycle end never overtakes its head. `fields` is the event's own fields, or a function answering them;
   `relay` false runs the step and sends nothing, for a request that is not the offscreen document's. */
let _wireChain = Promise.resolve();
function _wireRelay(name, d, fields, relay) {
  const ev = { ev: name, requestId: d.requestId, url: d.url };
  const documentId = d.documentId;
  const next = _wireChain.then(async () => {
    Object.assign(ev, typeof fields === "function" ? await fields() : fields);
    if (relay !== false && await _wireIsOffscreen(documentId)) await chrome.runtime.sendMessage({ __evt: "WIRE", ev });
  });
  _wireChain = next.then(() => undefined, () => undefined);
  next.catch((e) => {
    RETHROW_FATAL(e);
    console.warn("[wire-events] relay failed for %s %s: %s", name, d.url, e && e.message || e);
  });
}

function _wireSpeakAsHeaders(list) {
  const out = {};
  for (const h of list) {
    const n = h.name.toLowerCase();
    if (SPEAK_AS_HEADER_NAMES.indexOf(n) >= 0) out[n] = h.value;
  }
  return out;
}
/* requestId -> the speak-as headers onBeforeSendHeaders reported, which precede declarativeNetRequest's
   changes (onSendHeaders reports after them, measured in Chrome 148 and 155). */
const _wireBefore = new Map();

chrome.webRequest.onBeforeRequest.addListener((d) => {
  if (_wireOwn(d)) _wireRelay("onBeforeRequest", d, {});
}, _WIRE_FILTER);
chrome.webRequest.onBeforeSendHeaders.addListener((d) => {
  if (d.tabId === chrome.tabs.TAB_ID_NONE) _wireBefore.set(d.requestId, _wireSpeakAsHeaders(d.requestHeaders));
}, _WIRE_FILTER, ["requestHeaders", "extraHeaders"]);

/* What a no-tab request finally carries, after declarativeNetRequest: exactly the speak-as headers it carried
   before, changed by the one rule that matched it, or by none. That the rule is its own send's is the offscreen
   document's check, against the `matched` ids relayed with this event. This is the boundary that a rule changes
   only the sends it was installed for, so it is a CHECK; a rule's condition names no tab. */
function _wireHeadersChecked(d, before) {
  const matched = speakAsInstalled.filter((r) => speakAsRuleMatches(r, d));
  CHECK(matched.length <= 1 && (before !== undefined || matched.length === 0),
        "the request " + d.method + " " + d.url + " (initiator " + d.initiator + ") matched the speak-as rules " +
        JSON.stringify(matched.map((r) => r.id)) + (before === undefined ? " with no onBeforeSendHeaders record" : ""));
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
  return matched.map((r) => r.id);
}
/* Checked in the chain, after the family's installed rules are read: a worker this request woke has not read
   them yet. A send's rule is held until the send settles, which waits on its head, so it is still installed. */
chrome.webRequest.onSendHeaders.addListener((d) => {
  if (d.tabId !== chrome.tabs.TAB_ID_NONE) return;
  const before = _wireBefore.get(d.requestId);
  _wireBefore.delete(d.requestId);
  _wireRelay("onSendHeaders", d, async () => {
    await speakAsInstalledReady();
    return { matched: _wireHeadersChecked(d, before) };
  }, _wireOwn(d));
}, _WIRE_FILTER, ["requestHeaders", "extraHeaders"]);

/* A reply's Set-Cookie fields, a 3xx's included: a Response hides them (Fetch §2.2.2 "Headers"' forbidden
   response-header names), and webRequest reports each one only with extraHeaders (measured in Chrome 148).
   A field webRequest reports as bytes is null, which the zone ignores. */
chrome.webRequest.onHeadersReceived.addListener((d) => {
  if (!_wireOwn(d)) return;
  _wireRelay("onHeadersReceived", d, { setCookie: d.responseHeaders.filter((h) => h.name.toLowerCase() === "set-cookie")
    .map((h) => (typeof h.value === "string" ? h.value : null)) });
}, _WIRE_FILTER, ["responseHeaders", "extraHeaders"]);

/* A "manual" redirect's 3xx, which the send's fetch answers as an opaque-redirect with no headers. */
chrome.webRequest.onBeforeRedirect.addListener((d) => {
  if (!_wireOwn(d)) return;
  DCHECK(Array.isArray(d.responseHeaders) && typeof d.statusLine === "string",
         "webRequest reported the redirect of " + d.url + " without its headers or status line — the listener " +
         "asks for responseHeaders, and the hop is judged on its Location, Referrer-Policy and CORS headers");
  const headers = d.responseHeaders.map((h) => {
    DCHECK(typeof h.value === "string",
           "webRequest reported the redirect header " + h.name + " of " + d.url + " as bytes — a 3xx's fields " +
           "this zone reads are text, and a binary one would be dropped from the hop's judgement");
    return [h.name, h.value];
  });
  _wireRelay("onBeforeRedirect", d, { statusCode: d.statusCode, statusLine: d.statusLine,
                                      headers: headers.filter((h) => !/^set-cookie2?$/i.test(h[0])) });
}, _WIRE_FILTER, ["responseHeaders", "extraHeaders"]);

/* A send's lifecycle ends in exactly one of onCompleted and onErrorOccurred; the error is the browser's reason
   (`net::ERR_NAME_NOT_RESOLVED`, a certificate error) for a send whose fetch rejects. */
chrome.webRequest.onCompleted.addListener((d) => {
  if (_wireOwn(d)) _wireRelay("onCompleted", d, {});
}, _WIRE_FILTER);
chrome.webRequest.onErrorOccurred.addListener((d) => {
  _wireBefore.delete(d.requestId);
  if (_wireOwn(d)) _wireRelay("onErrorOccurred", d, { error: d.error });
}, _WIRE_FILTER);
