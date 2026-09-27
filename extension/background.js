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
/* THE OFFSCREEN DOCUMENT'S ADDRESS, ASKED OF THE API THAT OWNS IT rather than concatenated here.
   `chrome.runtime.getURL()` is documented as the conversion of a packaged resource's relative path to its
   fully-qualified URL, and `MessageSender.url` is documented as "the URL of the page or frame that opened
   the connection" — so these are the same string from the same authority, and the __rpc pin below can be an
   EQUALITY. It was a `startsWith` of a hand-built `EXT_ORIGIN + "/" + OFFSCREEN_URL`, which is a prefix
   standing where an identity belongs: every future packaged page whose name merely BEGINS with this one's
   would inherit the most privileged surface this extension has (chrome.tabs.*, chrome.scripting in the MAIN
   world of any tab), and nothing about adding such a file would say so. */
const OFFSCREEN_HREF = chrome.runtime.getURL(OFFSCREEN_URL);

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
// by each document's own CONTENT_SEED arrival, not a tab-level main-frame guess.
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
// @security-finding  NOTHING CALLS `scripting.exec` — grep the whole extension for it and the only hit is the
// RPC arm below. These injectors are therefore an UNUSED privilege, and the privilege is the largest one this
// extension has: arbitrary MAIN-world execution in any tab (`world: "MAIN"`, `target.allFrames`), i.e. full
// same-origin control of every site the user has open, plus the `scripting` permission that exists only for
// them. The live PoC verification took a different route — the popup embedded the sandboxed poc-sandbox.html and
// the payload is delivered by the PoC itself (window.open / postMessage), with intercept.js's apiclientsink
// relaying the hit — so `seedStorage`, `postMessage`, `dispatchEvents` and `readExecFlag` have no caller and
// `tabs.create` / `tabs.remove` / `tabs.get` below have none either.
// It is gated to the offscreen document (the only trusted zone). THE PARAGRAPH THAT STOOD HERE SAID THIS TABLE
// HAD NO CALLER AND NAMED THE DECISION THAT WOULD SETTLE IT, AND THAT DECISION HAS NOW BEEN MADE, so the
// sentence is kept in its own words rather than deleted — a reader who greps for a caller and finds few will
// re-derive it. It read: "so it is not reachable today; what it is, is the blast radius of any future hole in
// that gate, held open for a caller that does not exist. Either the probe orchestration this file's comment
// promises lands and uses it, or this arm, the injectors, the three unused tabs.* arms and the `scripting`
// permission are DELETED together. It is named here rather than deleted in a security review because removing
// it decides that the injected-probe design is not coming back — that is the probe design's call, not this
// review's."
// THE PROBE DESIGN'S CALL IS IN: `pocAttackerButton` below is the caller, and it is the one §LIVE-VERIFY needs.
// The live PoC used to run in `poc-sandbox.html`, a `manifest.sandbox.pages` document whose origin is OPAQUE —
// so every cross-document delivery it performed arrived at the victim with `event.origin === "null"`, an
// identity no real handler's origin check accepts, and a no-hit through that channel was indistinguishable from
// the engine-fidelity divergence §LIVE-VERIFY reads a no-hit as. The attacker document is now a REAL
// REGISTRABLE ORIGIN the trusted zone opens as an ordinary tab, with the payload EVALUATED there rather than
// served from there, so nothing is hosted and `event.origin` reads that document's own address.
// WHICH WORLD EACH HALF RUNS IN IS MEASURED AND NOT CHOSEN. In real Chrome at `https://example.com`, with an
// invented-capability control beside each reading: the ISOLATED world reports `self.origin` as the document's
// and has `chrome.runtime.sendMessage`, and `new Function` THROWS there — `EvalError`, naming this EXTENSION's
// own `script-src`, which governs an isolated world and carries no `unsafe-eval`. The MAIN world evaluates
// fine (the page's CSP governs, and this one has none) and has no `chrome.runtime` at all. So the two
// capabilities the delivery needs are in two different worlds, and the split below is forced rather than
// preferred: the MAIN world evaluates and dispatches a CustomEvent, and content.js's ISOLATED world relays it —
// the same cross-world transport `__uasr_probe_hit` already uses, which is a `document` event rather than a
// `window.postMessage` precisely so no page handler sees a `message` event that was not the page's.
const _PROBE_INJECTORS = {
  /* THE ATTACKER DOCUMENT'S RUN BUTTON, IN THE MAIN WORLD OF A REGISTRABLE ORIGIN. The user's click is the
     whole reason a button exists: HTML §7.2.2.1 "Opening and closing windows" step 12 reaches the rules for
     choosing a navigable, and a popup blocker is that algorithm answering with no navigable — which a real
     click prevents and nothing this extension can fake does. A `debugger`-permission synthetic gesture was
     never on the table and is not needed.
     IT CARRIES CONSTANTS AND THE ENGINE'S OWN PAYLOAD, NOTHING COMPOSED HERE. `pocJs` is what the engine
     fire-verified and what `buildLiveDelivery` shaped for this source's real browser transform; this function
     does not alter it, because altering it would make the thing verified different from the thing run, and
     §LIVE-VERIFY's whole content is that the two are the same artifact.
     THE MARKER IS THE CORRELATION AND IS CHECKED FOR PRESENCE ONLY. It rides INSIDE the payload as
     apiclientsink('<id>'), and this document is an untrusted third-party page, so nothing here may assert on
     what comes back through it — the trusted zone decides delivery from the browser's own facts
     (offscreen-brain.js `_recordProbeDelivery`), and everything this function states is PAGE-CLAIMED.
     IT IS IDEMPOTENT PER MARKER because the offscreen may re-arm a session, and two buttons for one marker
     would let one click report twice and the other never. A second injection for the SAME marker replaces its
     own overlay; one for a DIFFERENT marker replaces it too, since a tab holds one live verify at a time.
     AN `EvalError` IS A DISTINCT STATE AND IS REPORTED AS ONE. The MAIN world is governed by the PAGE's CSP,
     so an attacker origin that ships `script-src` without `'unsafe-eval'` cannot evaluate the payload at all —
     which is a refusal naming that policy, and not a delivery that failed to fire. Folding the two together
     would hand §LIVE-VERIFY a no-hit for a reason that is not an engine divergence, which is the exact defect
     the sandboxed attacker page was retired for. */
  pocAttackerButton: (pocJs, marker) => {
    const ID = "__apiclient_poc_overlay";
    const prev = document.getElementById(ID);
    if (prev) prev.remove();
    const box = document.createElement("div");
    box.id = ID;
    box.setAttribute("style", "position:fixed;inset:auto 12px 12px auto;z-index:2147483647;max-width:420px;"
      + "background:#0d1117;color:#c9d1d9;border:1px solid #30363d;border-radius:8px;padding:10px 12px;"
      + "font:12px/1.45 -apple-system,'Segoe UI',sans-serif;box-shadow:0 6px 24px rgba(0,0,0,.5)");
    const hdr = document.createElement("div");
    hdr.setAttribute("style", "font-size:10px;text-transform:uppercase;letter-spacing:.4px;color:#8b949e;margin-bottom:6px");
    hdr.textContent = "attacker page — live verify (" + String(self.origin) + ")";
    const btn = document.createElement("button");
    btn.setAttribute("style", "background:#238636;color:#fff;border:0;border-radius:4px;padding:5px 12px;"
      + "font-size:12px;cursor:pointer");
    btn.textContent = "\u25B6 Run PoC";
    const status = document.createElement("span");
    status.setAttribute("style", "margin-left:8px;font-size:11px;color:#8b949e");
    status.textContent = "your click is the user gesture window.open needs";
    const row = document.createElement("div");
    row.appendChild(btn); row.appendChild(status);
    box.appendChild(hdr); box.appendChild(row);
    (document.body || document.documentElement).appendChild(box);
    btn.addEventListener("click", () => {
      btn.disabled = true;
      status.textContent = "running\u2026";
      let threw = null, errName = null, opened;
      try { opened = (0, eval)(pocJs); }
      catch (err) { threw = (err && err.message) || String(err); errName = (err && err.name) || null; }
      /* READ BY IDENTITY ONLY. A successful open answers a CROSS-ORIGIN WindowProxy whose members are
         HTML §7.2.1 "Security infrastructure for Window, WindowProxy, and Location objects"' fixed
         cross-origin list and whose anything-else is a SecurityError, so touching it would throw here and
         report a delivery that happened as one that broke. `undefined` is not a shape the window open steps
         return: it is this function's statement that the payload it was handed is no longer a single
         expression whose value says anything. */
      const handle = opened === undefined ? "none" : opened === null ? "null" : "window";
      status.textContent = threw === null
        ? "delivery ran \u2014 the extension's panel reports whether a document received it"
        : "the delivery threw before it completed: " + threw;
      document.dispatchEvent(new CustomEvent("__uasr_poc_ran", { detail: {
        marker: marker, ran: threw === null, error: threw, errorName: errName,
        handle: handle, attackerOrigin: String(self.origin),
      } }));
    });
    return { armed: true, attackerOrigin: String(self.origin) };
  },
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
    if (sender.url === OFFSCREEN_HREF) {
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
