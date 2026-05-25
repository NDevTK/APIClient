// background.js — STATELESS service worker. The learning brain (globalStore,
// discovery, AST merge, request log, popup handlers) lives in the OFFSCREEN
// document (offscreen-brain.js, loaded by ast-worker.html) where it has a stable
// lifetime + IndexedDB. The SW holds NO state — it is evicted freely. Its only
// jobs:
//   1. own the offscreen document's lifecycle (create it; wake it on startup),
//   2. forward browser events the offscreen can't observe (webNavigation +
//      content-script messages + tab updates) to the brain,
//   3. perform privileged chrome.tabs.* calls the offscreen can't, as RPC.
// The popup talks to the brain directly over chrome.runtime.sendMessage (a
// broadcast reaches the offscreen); the SW stays out of those messages.

const EXT_ORIGIN = "chrome-extension://" + chrome.runtime.id;
const OFFSCREEN_URL = "ast-worker.html";

// ─── Offscreen lifecycle ──────────────────────────────────────────────────────
let _offscreenP = null;
async function ensureOffscreen() {
  try {
    const existing = await chrome.runtime.getContexts({ contextTypes: ["OFFSCREEN_DOCUMENT"] });
    if (existing.length > 0) return;
  } catch (_) { /* getContexts unavailable during teardown — fall through */ }
  if (_offscreenP) return _offscreenP;
  _offscreenP = (async () => {
    try {
      await chrome.offscreen.createDocument({
        url: OFFSCREEN_URL,
        reasons: ["WORKERS"],
        justification: "API discovery + JavaScript analysis brain (IndexedDB + Web Worker)",
      });
    } catch (e) { /* concurrent create / already exists */ }
    finally { _offscreenP = null; }
  })();
  return _offscreenP;
}

// Wake the brain on browser start / extension update so it resumes any incomplete
// deep grind from IndexedDB without waiting for a navigation.
try {
  chrome.runtime.onStartup.addListener(() => ensureOffscreen().catch(() => {}));
  chrome.runtime.onInstalled.addListener(() => ensureOffscreen().catch(() => {}));
} catch (e) {}

// Forward a message to the offscreen brain, ensuring it exists first.
function toBrain(m) { ensureOffscreen().then(() => chrome.runtime.sendMessage(m).catch(() => {})).catch(() => {}); }

// ─── Browser events the offscreen can't observe → forward to the brain ────────
chrome.webNavigation.onCommitted.addListener((d) => {
  toBrain({ __evt: "NAV", tabId: d.tabId, frameId: d.frameId, url: d.url || "", parentFrameId: d.parentFrameId });
});
chrome.tabs.onRemoved.addListener((tabId) => { toBrain({ __evt: "TAB_REMOVED", tabId: tabId }); });
chrome.tabs.onUpdated.addListener((tabId, changeInfo, tab) => {
  toBrain({ __evt: "TAB_UPDATED", tabId: tabId, changeInfo: changeInfo, tab: { id: tab.id, url: tab.url || "", status: tab.status || "" } });
});

// ─── Privileged chrome.* RPC for the offscreen brain ──────────────────────────
// The brain runs in a document and can't call chrome.tabs.*; it sends a {__rpc}
// message and the SW performs the call. (chrome.scripting.executeScript injects a
// function that can't cross a message boundary, so the exploit-probe — the only
// scripting user — must run in the SW; that orchestration is relocated here as a
// follow-up and is not yet present.)
// chrome.scripting.executeScript injects a FUNCTION, which can't cross a message
// boundary — so the exploit-probe's injected functions live HERE (the SW has
// chrome.scripting) and the brain selects one by name, passing only serializable
// args. These are the probe's fixed primitives; the orchestration (session state,
// payload shaping, PROBE_HIT correlation) stays in the brain.
const _PROBE_INJECTORS = {
  // Read the per-marker execution flag intercept.js's apiclientsink() populated —
  // proof the browser actually ran the payload (vs. taint merely reaching a sink).
  readExecFlag: (flag) => { try { return self[flag] || null; } catch (_) { return null; } },
  // Same-window postMessage to the bundle's own listener, retried to cover async
  // handler-registration races.
  postMessage: (shapedPayload) => {
    const deliveries = [200, 1500, 3500];
    for (const delay of deliveries) {
      setTimeout(() => { try { window.postMessage(shapedPayload, "*"); } catch (_) {} }, delay);
    }
  },
  // Pre-seed localStorage + cookies before the bundle reads them at module init.
  seedStorage: (storageItems, cookieItems) => {
    try {
      for (const it of (storageItems || [])) {
        if (it && it.key != null) localStorage.setItem(String(it.key), String(it.value == null ? "" : it.value));
      }
      for (const it of (cookieItems || [])) {
        if (it && it.value != null) document.cookie = String(it.value);
      }
    } catch (_) {}
  },
  // Dispatch a sequence of postMessage events with per-message delay (structured plan).
  dispatchEvents: (eventList, perMessageDelayMs) => {
    const baseDelay = 500;
    eventList.forEach((ev, i) => {
      const t = baseDelay + i * perMessageDelayMs;
      setTimeout(() => {
        try { if (ev && ev.kind === "postMessage") window.postMessage(ev.payload, "*"); } catch (_) {}
      }, t);
    });
  },
};

async function _swRpc(msg) {
  const api = msg.api, args = msg.args || [];
  switch (api) {
    case "tabs.sendMessage": return chrome.tabs.sendMessage(...args);
    case "tabs.create": return chrome.tabs.create(...args);
    case "tabs.remove": return chrome.tabs.remove(...args);
    case "tabs.query": return chrome.tabs.query(...args);
    case "tabs.get": return chrome.tabs.get(...args);
    case "webNavigation.getAllFrames": return chrome.webNavigation.getAllFrames(...args);
    // The request log is the "network traffic" exception that lives in
    // chrome.storage.session — but chrome.storage isn't exposed to the offscreen
    // document, so the brain reads/writes it through the SW.
    case "storage.session.set": return chrome.storage.session.set(...args);
    case "storage.session.get": return chrome.storage.session.get(...args);
    case "storage.session.remove": return chrome.storage.session.remove(...args);
    case "storage.session.clear": return chrome.storage.session.clear(...args);
    // Cross-origin fetch on the brain's behalf. The offscreen document is under
    // COEP require-corp, so a cross-origin fetch there fails unless the host
    // happens to send CORP — the SW (host_permissions: <all_urls>) is not, so its
    // fetch works for ANY host. Security: cookies are ALWAYS omitted (these are
    // uncredentialed public-resource fetches — scripts/chunks/specs — never the
    // user's authenticated session; credentialed replay goes through the page
    // renderer instead) and only http(s) is allowed. Returns a serializable
    // response (status + headers object + text body).
    case "fetch": {
      const o = args[0] || {};
      let u;
      try { u = new URL(o.url); } catch (e) { throw new Error("bad fetch url"); }
      if (u.protocol !== "http:" && u.protocol !== "https:") throw new Error("blocked: non-http(s) fetch url");
      const init = {
        method: o.method || "GET",
        credentials: "omit",
        redirect: o.redirect || "follow",
        cache: o.cache || "default",
      };
      if (o.headers) init.headers = o.headers;
      if (o.body != null) init.body = o.body;
      const resp = await fetch(u.href, init);
      const headers = {};
      resp.headers.forEach((v, k) => { headers[k] = v; });
      return { ok: resp.ok, status: resp.status, statusText: resp.statusText, headers, body: await resp.text() };
    }
    // Exploit-probe injection: run a named, predefined injector (above) in the
    // target tab's MAIN world. Returns the per-frame InjectionResult array (its
    // `.result` values are serializable) so the brain can merge them.
    case "scripting.exec": {
      const o = args[0] || {};
      const fn = _PROBE_INJECTORS[o.op];
      if (!fn) throw new Error("unknown scripting op: " + o.op);
      const target = { tabId: o.tabId };
      if (o.allFrames) target.allFrames = true;
      const results = await chrome.scripting.executeScript({ target, world: "MAIN", func: fn, args: o.args || [] });
      return (results || []).map((r) => ({ frameId: r && r.frameId, result: r ? r.result : undefined }));
    }
    default: throw new Error("unknown rpc api: " + api);
  }
}

chrome.runtime.onMessage.addListener((msg, sender, sendResponse) => {
  // The SW services EXTENSION-PAGE senders only (popup, offscreen). Our content
  // scripts message the offscreen brain DIRECTLY — chrome.runtime.sendMessage
  // broadcasts to every extension context, so the offscreen receives them with
  // the real, browser-verified sender (tab/frame/url). The SW never sees, relays,
  // or launders page data, so it can't turn a web renderer's message into a
  // trusted extension-origin one. onMessage only ever delivers from this
  // extension's own contexts (external senders go to onMessageExternal), so
  // sender.id is not a trust signal — sender.url is.
  if (sender.id !== chrome.runtime.id) return;
  if (!msg || typeof msg !== "object") return;
  const fromExtPage = sender.url && sender.url.startsWith(EXT_ORIGIN + "/");
  if (!fromExtPage) return;   // content-script (web renderer) message: handled by the offscreen directly

  // Privileged chrome.* RPC — accepted ONLY from the offscreen brain. (The popup
  // is also extension-origin but never uses __rpc; pinning it to the offscreen
  // URL means even a popup-context compromise can't reach chrome.tabs.* /
  // storage.session.* through this relay.)
  if (msg.__rpc) {
    if (sender.url.startsWith(EXT_ORIGIN + "/" + OFFSCREEN_URL)) {
      _swRpc(msg).then((r) => sendResponse({ ok: true, result: r }))
        .catch((e) => sendResponse({ ok: false, error: String((e && e.message) || e) }));
      return true;
    }
    return; // __rpc from a non-offscreen extension page: refuse
  }

  // Any other extension-page message (the popup's GET_STATE etc.) is handled by
  // the brain, which receives the same broadcast directly. The SW's only job is
  // to make sure the brain is alive to receive it (and to resume any grind).
  ensureOffscreen().catch(() => {});
});
