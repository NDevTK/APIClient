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
  chrome.runtime.onStartup.addListener(() => ensureOffscreen().catch((e) => console.warn("[bg:onStartup] ensureOffscreen failed:", e && e.message || e)));
  chrome.runtime.onInstalled.addListener(() => ensureOffscreen().catch((e) => console.warn("[bg:onInstalled] ensureOffscreen failed:", e && e.message || e)));
} catch (e) {
  // chrome.runtime.onStartup / onInstalled missing — the only way this catch fires
  // is if the MV3 API surface changed. Surface so the boot-time hook gap is visible.
  console.warn("[bg] failed to register onStartup/onInstalled listeners:", e && e.message || e);
}

// Forward a message to the offscreen brain, ensuring it exists first.
function toBrain(m) {
  ensureOffscreen()
    .then(() => chrome.runtime.sendMessage(m).catch((e) => {
      // The brain isn't listening (race during offscreen-document boot, or it
      // crashed). The dispatch silently failing leaves the SW thinking the
      // message was delivered. Surface the message type + error so a lost
      // event (NAV, TAB_REMOVED, content-shipped script) is diagnosable.
      console.debug("[bg:toBrain] sendMessage failed type=%s: %s", m && m.type, e && e.message || e);
    }))
    .catch((e) => console.warn("[bg:toBrain] ensureOffscreen failed type=%s: %s", m && m.type, e && e.message || e));
}

// ─── Browser events the offscreen can't observe → forward to the brain ────────
// Only tab-close: it frees per-tab transient state and is carried by no document
// message. Navigation/activation/update are NOT forwarded — the brain prioritizes
// by each document's own CONTENT_HTML arrival, not a tab-level main-frame guess.
// (webNavigation is still used on demand — getAllFrames via __rpc for GET_FRAMES.)
chrome.tabs.onRemoved.addListener((tabId) => { toBrain({ __evt: "TAB_REMOVED", tabId: tabId }); });

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
  // A throw here means the page tampered with `self` (frozen, proxied, etc.) so
  // the probe result is genuinely unknown; surface so a bundle that defeats the
  // flag read is visible rather than silently flipping to "not reproduced".
  readExecFlag: (flag) => { try { return self[flag] || null; } catch (e) { console.warn("[probe:readExecFlag] flag=%s threw: %s", flag, e && e.message || e); return null; } },
  // Same-window postMessage to the bundle's own listener, retried to cover async
  // handler-registration races.
  postMessage: (shapedPayload) => {
    const deliveries = [200, 1500, 3500];
    for (const delay of deliveries) {
      setTimeout(() => {
        try { window.postMessage(shapedPayload, "*"); }
        catch (e) {
          // postMessage of structured-cloneable data should never throw under
          // a normal page; a throw means the payload couldn't be cloned (e.g.
          // contains a function / DOM node) or the window is detached. Both
          // are real failures the probe needs to know about.
          console.warn("[probe:postMessage] threw at delay=%dms: %s", delay, e && e.message || e);
        }
      }, delay);
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
    } catch (e) {
      // localStorage / document.cookie can throw (quota, third-party cookie
      // policy, sandboxed iframe). Surface so the probe knows the seed didn't
      // land — otherwise the exploit reproducibility looks like a code-path
      // bug when it's actually a storage-policy block.
      console.warn("[probe:seedStorage] threw: %s", e && e.message || e);
    }
  },
  // Dispatch a sequence of postMessage events with per-message delay (structured plan).
  dispatchEvents: (eventList, perMessageDelayMs) => {
    const baseDelay = 500;
    eventList.forEach((ev, i) => {
      const t = baseDelay + i * perMessageDelayMs;
      setTimeout(() => {
        try { if (ev && ev.kind === "postMessage") window.postMessage(ev.payload, "*"); }
        catch (e) {
          // Same diagnostic rationale as the single-postMessage injector above —
          // a structured-clone failure or detached window must be visible so
          // the structured-plan probe doesn't report NOT_REPRODUCED for a
          // delivery failure rather than a real bundle behavior.
          console.warn("[probe:dispatchEvents] postMessage threw at i=%d t=%dms: %s", i, t, e && e.message || e);
        }
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
    // chrome.storage.session.* RPC handlers removed — the brain no longer
    // mirrors request logs to session storage (the offscreen document's
    // stable lifetime makes the in-memory log authoritative). If a caller
    // accidentally still issues a storage.session.* RPC the unknown-api
    // throw below will surface it as a diagnostic — better than silently
    // doing nothing.
    // NOTE: cross-origin fetch is NOT a SW RPC. External fetches go DIRECTLY from
    // the offscreen document / its Worker through lib/safe-fetch.js (GET only,
    // cookies omitted, http(s) only) — COEP does not block fetch, so the SW relay
    // was pointless indirection and is gone.
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
  // sender.id is not a trust signal — sender.ORIGIN is (browser-set + unforgeable,
  // and "null" for a sandboxed extension page, which must NOT be trusted as one).
  // An exact origin match beats a URL prefix: chrome-extension://<id> is the origin,
  // while sender.url is the document URL (kept below only to PIN the offscreen doc).
  if (sender.id !== chrome.runtime.id) return;
  if (!msg || typeof msg !== "object") return;
  const fromExtPage = sender.origin === EXT_ORIGIN;
  if (!fromExtPage) return;   // content-script (web renderer) message: handled by the offscreen directly

  // Privileged chrome.* RPC — accepted ONLY from the offscreen brain. (The popup
  // is also extension-origin but never uses __rpc; pinning it to the offscreen
  // URL means even a popup-context compromise can't reach chrome.tabs.* /
  // chrome.scripting.exec / chrome.* fetch through this relay.)
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
  ensureOffscreen().catch((e) => {
    // The popup just asked the brain for state and we couldn't even WAKE the
    // offscreen document — the popup will time out waiting for the broadcast
    // reply. Surface so the boot gap is diagnosable; the popup user otherwise
    // sees "loading…" forever with no console signal.
    console.warn("[bg:onMessage] ensureOffscreen failed for popup request:", e && e.message || e);
  });
});
