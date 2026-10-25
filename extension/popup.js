// Popup controller: renders captured data, security findings, and sends requests.

/** Extract a flat deduplicated method list from a discovery doc. */
function getDocMethods(doc) {
  if (!doc || !doc.resources) return [];
  const seen = new Set();
  const methods = [];
  function walk(res) {
    for (const rName in res) {
      const r = res[rName];
      if (r.methods) {
        for (const mName in r.methods) {
          const m = r.methods[mName];
          /* THE VERB IS THE HALF OF THE IDENTITY THAT A DEFAULT ERASES. `(m.httpMethod || "GET")` made every
             record without one collapse onto the GET of the same id, so two endpoints would have been
             rendered as one and the second silently dropped by `seen` — and it could never fire, because
             every producer of a method record writes the verb: lib/learn.js's four registrations take it from
             the call site or the request, lib/openapi-import.js from the operation, and a probed discovery
             document declares it. A missing one is that producer broken, which is a thing to see rather than
             a thing to guess a verb for. */
          DCHECK(typeof m.httpMethod === "string" && m.httpMethod,
                 "a discovery-doc method carries no httpMethod — every producer writes it (lib/learn.js from " +
                 "the call site or the live request, lib/openapi-import.js from the operation, a probed " +
                 "document from the service's own declaration), so an absent one would be filed under GET " +
                 "and merged onto whatever GET shares its id (" + String(m.id) + ")");
          const key = m.httpMethod + " " + m.id;
          if (seen.has(key)) continue;
          seen.add(key);
          methods.push(m);
        }
      }
      if (r.resources) walk(r.resources);
    }
  }
  // Walk probed first so learned wins on dedup (learned is added second, probed filtered by seen)
  if (doc.resources.probed) walk({ probed: doc.resources.probed });
  if (doc.resources.learned) walk({ learned: doc.resources.learned });
  const skip = new Set(["probed", "learned"]);
  const rest = {};
  for (const k in doc.resources) { if (!skip.has(k)) rest[k] = doc.resources[k]; }
  walk(rest);
  return methods;
}

let currentTabId = null;
let tabData = null;
let currentSchema = null;
/* THE ADDRESS TEMPLATE THIS PANEL COMPOSES AGAINST — `base + pathTemplate` from the endpoint dropdown, or a
   logged request's own concrete address on the replay path. It is NOT the address a send used: that is
   `lastSentUrl` below, and the two were ONE SLOT until this comment was written.
   THE COST OF THAT WAS THREE DEFECTS OFF ONE ROOT, all measured on the live panel. `sendRequest` wrote its
   composed address back over this slot, which DESTROYS THE HOLES — so after the first Send (a) every later
   edit of a PATH parameter was discarded, the panel holding `SUBJOWNER` in the field and the request going
   to the `CTLOWNER` of the previous send, because `applyPathParams` had no `{owner}` left to substitute;
   (b) a query parameter the operator CLEARED could never be removed, since `composeSendUrl` reaches its
   `searchParams.set` only for a parameter the form still states and an absent one leaves the baked-in copy
   standing; and (c) `renderSendUrl`'s address row — the surface SECURITY.md's operator-tier exemption
   is named after, "a human at a surface that shows them the bytes" — showed one answer while the operator's
   own form field showed another. A panel giving two answers about one quantity does not weaken that
   authorization, it FORGES it, which is the argument `composeSendUrl` already makes one file over.
   It is the §A-PREDICATE-THAT-ANSWERS-TWO-QUESTIONS shape arriving in a variable: one slot answering both
   "what is this endpoint's template" and "what address did the last send use", decided by the stricter
   consumer (the send needs a resolved address) with the looser one (every composer, which needs the holes)
   losing silently — and silently is exact, because the address it then produced was a REAL address that a
   server answered. */
let currentRequestUrl = "";
/* THE ADDRESS THE LAST SEND ACTUALLY USED. A fact about a past ACT, so it is written by `sendRequest` alone
   and read by whoever is describing that act's RESULT — never by a composer, which must compose from the
   template. `""` is the positive statement that this panel's current subject has not been sent to; every
   reader already answers that case rather than defaulting past it. */
let lastSentUrl = "";
/* THE ONE WRITER OF THE TEMPLATE, WHICH IS WHAT KEEPS THE PAIR FROM DRIFTING. Re-pointing the panel at a
   different request necessarily retires the address the last send used, and the four sites that do the
   re-pointing used to be free to update one without the other — a hand-copied obligation, which is the shape
   this file's own per-realm-intrinsics rule warns about. Going through here makes the pairing structural:
   there is no spelling of "change the template" that leaves a stale sent address behind it. */
function setRequestTemplate(url) {
  currentRequestUrl = url;
  lastSentUrl = "";
}
let currentRequestMethod = "POST";
let currentContentType = "application/json";
let currentBodyMode = "form"; // "form" | "raw" | "graphql" | "multipart" | "msgconsole"
// mpState: when currentBodyMode === "multipart", tracks one editor per part.
// Each part: { partNumber, method, path, headers{}, contentType, bodyEditor }
// where bodyEditor is { kind: "json" | "graphql" | "form-urlencoded" | "raw", value, meta? }.
// Reassembled into a multipart/mixed envelope on Send.
let mpState = { boundary: null, parts: [] };
// Tab ID for the currently-open Message Console channel. Initially null.
// Set when initMsgConsole() opens a captured WS/PM/MC entry — the channel
// lives in the TAB THAT CAPTURED IT, which may be different from the
// popup's own tab (`currentTabId`) when browsing cross-tab logs. All
// PM_GET_STATUS / MC_SEND_MSG / WS_SEND_MSG calls route through this tab.
let currentChannelTabId = null;
let currentChannelId = null;
let currentChannelType = null; // the log record's own `kind`: "websocket" | "postmessage" | "msgchannel"
let currentTargetOrigin = null; // For postMessage send
let currentChannelFrameId = null; // Frame where the channel lives (legacy routing fallback)
let currentChannelDocumentId = null; // Document the channel lives on — preferred routing target (stable across navigations)
let logFilter = "active"; // "active" | "all" | tabId (number)
let logSearchQuery = ""; // text filter for request log
let allTabsData = null; // { tabId: { meta, requestLog } }
let lastSendResult = null; // Last rendered response for re-render after rename
let gqlState = { ops: [], batched: false, activeIdx: 0 }; // GraphQL operation state
let currentFrameId = 0; // Target frame for sending (0 = main frame)
let availableFrames = []; // Cached frame list for current tab
/* WHETHER THE BROWSER RESOLVES THIS TAB AT ALL, which is NOT the question `availableFrames.length` answers.
   lib/popup-handlers.js reports the two facts separately because webNavigation reports them separately (an
   invalid tab id is null; a tab holding no web document is []), and an empty frame list means opposite things
   under them: "this tab is a chrome:// / New Tab / extension page" versus "this tab is gone". `null` here is
   the third state and the only one that is about US — GET_FRAMES has not answered yet in this popup's life —
   which is why it is not a boolean defaulting to either answer. */
let tabResolved = null;   // null = not asked yet; true/false = the browser's own answer
// ── Send target: PINNED to a documentId (the "pin to documentid" model) ───────
// The send target is a specific documentId, NOT the reused frameId. A navigation
// that swaps a frame's document therefore can't silently retarget a send: the
// pinned id stops being LIVE, readiness drops, and Send gates OFF until a new
// document is ready — there is no arbitrary timer that re-enables it. Same-origin
// swaps are followed automatically (identical credential principal); a confirmed
// cross-origin swap goes _pinStale and needs an explicit re-confirm.
let _pinnedDocId = null;   // pinned send-target documentId (null = not yet pinned)
let _pinnedOrigin = "";    // its principal origin (the credentials origin), from the documentId→origin mapping
let _pinStale = false;     // pinned doc swapped to a DIFFERENT, known origin — re-confirm required
let _staleOrigin = "";     // the new origin (for the warning)
function _origOf(f) {
  return (f && typeof f.origin === "string" && f.origin.indexOf("://") > 0) ? f.origin : "";
}
// The pinned documentId while it is LIVE (ready) in the frame tree; null when the
// document has gone/changed -> Send gates off on READINESS (not a timer).
function currentDocumentId() {
  if (_pinStale) return null;
  return (_pinnedDocId && availableFrames.some((x) => x.documentId === _pinnedDocId)) ? _pinnedDocId : null;
}
// Principal origin shown for the live pin. From the documentId→origin mapping
// (GET_FRAMES → _originForDoc → MessageSender.origin), NEVER url-derived; "" when
// opaque/unknown (sandboxed/about:blank) or not yet reported, shown fail-safe.
function currentPrincipalOrigin() {
  if (_pinnedDocId && availableFrames.some((x) => x.documentId === _pinnedDocId)) return _pinnedOrigin;
  const f = availableFrames.find((x) => x.frameId === currentFrameId)
         || availableFrames.find((x) => x.isMain) || availableFrames[0];
  return _origOf(f);
}
// Validate/advance the pin against the live frame tree (called by loadState after
// GET_FRAMES). First load pins to the main frame. A pinned doc that NAVIGATED is:
//   • followed if its replacement is SAME-ORIGIN (same principal) — seamless;
//   • left NOT-READY while the replacement's origin is unknown (not yet reported)
//     — Send stays off until it is ready, no false "changed" warning;
//   • marked STALE only once the replacement is a KNOWN, DIFFERENT origin.
function _resolvePin() {
  if (availableFrames.length === 0) return;            // keep last pin; currentDocumentId() -> null (not ready)
  if (_pinnedDocId === null) {                         // first load — default to the main frame
    const def = availableFrames.find((f) => f.isMain) || availableFrames[0];
    _pinnedDocId = def.documentId; _pinnedOrigin = _origOf(def); _pinStale = false;
    currentFrameId = def.frameId || 0;
    return;
  }
  if (availableFrames.some((f) => f.documentId === _pinnedDocId)) { _pinStale = false; return; }  // still live
  const repl = availableFrames.find((f) => f.frameId === currentFrameId)
            || availableFrames.find((f) => f.isMain) || availableFrames[0];
  const ro = _origOf(repl);
  if (!ro) { _pinStale = false; return; }              // replacement origin unknown -> NOT ready (not stale)
  if (ro === _pinnedOrigin) { _pinnedDocId = repl.documentId; _pinStale = false; }  // same principal — follow
  else { _pinStale = true; _staleOrigin = ro; }        // known different principal — fail closed + flag
}
function _repinTo(frameId) {                            // explicit (re)selection of a frame's CURRENT document
  const f = availableFrames.find((x) => x.frameId === frameId);
  currentFrameId = frameId;
  _pinnedDocId = f ? f.documentId : null;
  _pinnedOrigin = _origOf(f);
  _pinStale = false;
}
// Single source of truth for the Send button. Enabled IFF a valid send target is
// READY (a live pinned documentId, or a replay's captured documentId) AND we are
// not within the brief security settle right after a documentId change AND not
// already sending. The settle EXPIRING does not force-enable — readiness still
// governs, so a not-ready/stale document keeps Send off with NO arbitrary timer.
function _refreshSendEnabled() {
  const b = document.getElementById("btn-send");
  if (!b) return;
  const target = sendPanelDocumentId();
  const settling = Date.now() < _navBlockUntil;
  b.disabled = _sendInProgress || _pinStale || !target || settling;
  /* "WAITING FOR THE PAGE'S DOCUMENT TO BE READY…" IS TRUE OF EXACTLY ONE OF THE THREE WAYS THERE IS NO
     TARGET, and it used to be said for all three. Two of them are the browser's own settled answers — this
     tab holds no web document, or this tab id no longer resolves — and under those nothing is coming, so the
     tooltip promised a readiness that would never arrive. It is the same fabricated-pending state the frame
     picker showed, one element over; leaving it would have kept half the surface lying. `tabResolved === null`
     is the third and only real wait: GET_FRAMES has not answered once yet in this popup's life, which is
     reachable here because the navigation settle calls this before loadState returns. */
  const noFrameTreeYet = tabResolved === null;
  const noDocumentInTab = !noFrameTreeYet && availableFrames.length === 0;
  b.title = _sendInProgress ? "Sending…"
          : _pinStale ? "Document changed origin — re-select to send"
          : !target ? (noFrameTreeYet ? "Waiting for the browser's frame tree…"
                     : noDocumentInTab ? (tabResolved ? "No web document in this tab — there is nothing to send from."
                                                      : "This tab no longer exists — the browser does not resolve it.")
                     : "Waiting for the page's document to be ready…")
          : settling ? "Document just changed — settling…"
          : "";
}

// Virtual scroll state for request log
const _vs = {
  entries: [],       // full sorted entry list
  estHeight: 85,     // estimated row height (px)
  buffer: 5,         // extra rows above/below viewport
  scrollHandler: null,
  lastStart: -1,     // last rendered start index (skip no-op re-renders)
  lastEnd: -1,       // last rendered end index
  rendering: false,  // prevent re-entrant renders
};

function setSendPanelVisible(visible) {
  var els = [
    document.getElementById("send-headers-section"),
    document.getElementById("send-body-section"),
    document.querySelector(".send-actions"),
  ];
  for (var i = 0; i < els.length; i++) {
    if (!els[i]) continue;
    if (visible) {
      els[i].classList.remove("hidden");
      els[i].style.display = "";
    } else {
      els[i].classList.add("hidden");
      els[i].style.display = "none";
    }
  }
}

function setBodyMode(mode) {
  currentBodyMode = mode;
  const isConsole = mode === "msgconsole";
  // Show the body/actions sections whenever a mode is set
  setSendPanelVisible(true);
  // Toggle each mode panel by BOTH class and inline display. The initial
  // HTML marks these panels with class="hidden" for the static default;
  // leaving the class in place while only flipping `style.display` is
  // inconsistent state that breaks class-based visibility checks. Keep
  // class and display in sync.
  const panels = [
    ["send-form-fields", mode === "form"],
    ["send-raw-body", mode === "raw"],
    ["send-graphql-fields", mode === "graphql"],
    ["send-multipart-fields", mode === "multipart"],
    ["send-ws-console", isConsole],
  ];
  for (const [id, active] of panels) {
    const el = document.getElementById(id);
    if (!el) continue;
    if (active) {
      el.classList.remove("hidden");
      el.style.display = "block";
    } else {
      el.classList.add("hidden");
      el.style.display = "none";
    }
  }
  document.getElementById("send-frame-row").style.display =
    isConsole ? "none" : "";
  document.getElementById("send-headers-section").style.display =
    isConsole ? "none" : "";
  document.getElementById("btn-send").style.display =
    isConsole ? "none" : "";
  document.querySelector(".export-row").style.display =
    isConsole ? "none" : "";
  /* THE ADDRESS ROW IS RECOMPUTED, NEVER LEFT STANDING. A display taken once is stale the moment the panel
     changes, and a stale address at THIS surface is the defect the row exists to end — SECURITY.md scopes
     the operator tier out of the destructive-path deny list on the ground that a human saw the bytes,
     and bytes that were true a mode ago are not the bytes. This is the mode hook; the endpoint hook is at
     loadVirtualSchema's tail and the typing hook is the delegated listener in init(). */
  renderSendUrl();
}



// ─── Init ────────────────────────────────────────────────────────────────────

let renderTimer = null;
function throttledLoadState() {
  if (renderTimer) return;
  renderTimer = setTimeout(async () => {
    renderTimer = null;
    await loadState();
  }, 100);
}

// Change-detection fingerprints — skip DOM rebuild when data hasn't changed
let _lastKeysFp = "";
let _lastSecFp = "";
let _lastLogFp = "";
let _lastSendFp = "";
// Navigation race-guard (security delay). Sends are routed by the authoritative
// documentId→origin pinning, so a send racing a navigation is already REFUSED at
// the routing layer; this adds a short Send block + Request-Context refresh so the
// user never fires at (or sees a confusing failure from) a freshly-navigated doc.
let _navBlockUntil = 0;
let _sendInProgress = false;
let _navReenableTimer = null;

document.addEventListener("DOMContentLoaded", async () => {
  const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
  currentTabId = tab?.id ?? null;

  // Security delay: on any navigation in this tab (origin change → new
  // documentId), block Send for a short settle window and refresh the Request
  // Context (frame picker + per-document origins, from the documentId→origin
  // mapping) so the displayed origin and the send target track the live document.
  if (chrome.webNavigation) {
    // A frame changing its document is the trigger for the security settle: gate
    // Send off immediately, re-fetch the frame tree (→ _resolvePin re-validates the
    // pin), then re-evaluate after a short settle. Re-eval goes through
    // _refreshSendEnabled, so Send only returns when a documentId is actually
    // READY — the settle expiring never force-enables a not-ready/stale document.
    const SETTLE_MS = 1000;
    const _markNav = (refresh) => {
      _navBlockUntil = Date.now() + SETTLE_MS;
      _refreshSendEnabled();
      clearTimeout(_navReenableTimer);
      _navReenableTimer = setTimeout(_refreshSendEnabled, SETTLE_MS + 30);
      if (refresh) loadState();
    };
    chrome.webNavigation.onBeforeNavigate.addListener((d) => { if (d.tabId === currentTabId) _markNav(false); });
    chrome.webNavigation.onCommitted.addListener((d) => { if (d.tabId === currentTabId) _markNav(true); });
  }

  for (const btn of document.querySelectorAll(".tab")) {
    btn.addEventListener("click", () => {
      document.querySelector(".tab.active").classList.remove("active");
      document.querySelector(".panel.active").classList.remove("active");
      btn.classList.add("active");
      document
        .getElementById(`panel-${btn.dataset.panel}`)
        .classList.add("active");
    });
  }

  document.getElementById("btn-clear").addEventListener("click", clearState);
  // The engine host's own readings live on the diagnostics page (dev.html), not in this popup.
  document.getElementById("btn-dev").addEventListener("click", () => {
    chrome.tabs.create({ url: chrome.runtime.getURL("dev.html") });
  });

  /* THE STORED-PAGE-CACHE SETTING. It is wired, it reaches a command the host actually answers, and the
     answer it renders is the value now IN FORCE rather than the value that was typed — which is the whole of
     what the deleted settings panel got wrong (see popup.html): a control whose refusal every sender
     swallowed, so it moved and meant nothing. There is no catch on this path for the same reason. */
  document.getElementById("btn-frontier-share").addEventListener("click", async () => {
    const el = document.getElementById("frontier-share-mb");
    const mb = Number(el.value);
    /* THE INPUT IS THE ONE PLACE A NON-NUMBER CAN ENTER, so it is refused HERE rather than sent on as a
       plausible byte count. `<input type=number>` answers "" for text it could not parse, and Number("") is
       0 — a real setting, and the wrong one to make out of a typo. */
    if (!(Number.isFinite(mb) && mb >= 0) || el.value === "") { await renderFrontierShare(); return; }
    await renderFrontierShare(Math.round(mb * 1024 * 1024));
  });
  renderFrontierShare();

  /* This extension's third-party-cookie setting: sent only on a change, and re-rendered from the answer. */
  document.getElementById("third-party-cookies").addEventListener("change", async (ev) => {
    await renderThirdPartyCookies({ allowed: !!ev.target.checked });
  });
  document.getElementById("third-party-cookies-exception").addEventListener("change", async (ev) => {
    await renderThirdPartyCookies({ exception: ev.target.value === "" ? null : ev.target.value });
  });
  renderThirdPartyCookies();

  /* THE EGRESS CONTROL. It sends only on a CLICK and it re-renders from the answer, so what is shown is the
     table now in force rather than the act that was attempted — the same rule the setting above it keeps and
     for the same reason: a control that reported its own success could be permitting nothing. */
  document.getElementById("btn-egress-widen").addEventListener("click", async () => {
    await renderEgressPolicy({ grant: currentPrincipalOrigin() });
  });
  /* AND ONE DELEGATED PATH FOR THE PER-SIGNAL ROWS, for the reason the revocation list has one: the rows are
     re-rendered from the policy's answer on every change, so a listener bound per checkbox would be bound to
     a node the next render replaces. The checkbox is read for the DIRECTION and the row is read for WHICH —
     the surface states a person's answer and the chokepoint decides what it means, which is why nothing here
     computes whether the combination fires. */
  document.getElementById("egress-signals").addEventListener("change", async (ev) => {
    const t = ev.target.closest("[data-egress-signal]");
    if (!t) return;
    await renderEgressPolicy({ permit: { origin: currentPrincipalOrigin(), signal: t.dataset.egressSignal,
                                         value: t.dataset.egressValue, allow: !!t.checked } });
  });
  /* ONE DELEGATED PATH FOR THE REVOCATIONS, because the list is re-rendered on every answer and a listener
     bound per entry would be bound to a node the next render replaces. */
  document.getElementById("egress-allowed").addEventListener("click", async (ev) => {
    const t = ev.target.closest("[data-egress-revoke]");
    if (!t) return;
    await renderEgressPolicy({ revoke: t.dataset.egressRevoke });
  });
  /* NO FIRST RENDER HERE, AND ITS ABSENCE IS THE POINT RATHER THAN AN OMISSION — the setting above it does
     have one, and this row is not the same kind of control. Its subject is the PINNED DOCUMENT'S origin,
     which is unknown until GET_FRAMES has answered, so a render at wiring time can only ever paint "(no
     origin)". `loadState` (called at the end of this block) renders it the moment the frames are in and on
     every re-load after, which is also what keeps it following a navigation.
     AND IT WOULD HAVE BEEN A RACE, NOT MERELY A WASTED PAINT: two concurrent reads of one table settle in
     whichever order they settle, so the wiring-time one could land AFTER the frame-aware one and leave the
     row saying this document has no origin when it has one — a permission surface reporting the wrong
     subject, which is the one thing it may not do. */

  // Discovery panel (lib/popup-discovery.js): one delegated click path for the three active-discovery probes.
  initDiscoveryPanel();

  /* NO SETTINGS PANEL WIRING. Its two controls (yield throttle, analyzer workers) reached
     SET_ANALYSIS_OPTS, which the bridge answered "unknown type" to while every sender on the path wrapped
     the call in a catch — so the sliders moved, the value was persisted to IDB, and nothing whatsoever
     changed about how the engine ran. The panel, the message types and the record are deleted rather than
     built: the working set is bounded by resident WASM memory and never by a user-set instance count, and a
     wall-clock throttle over the cooperative quantum is a step cap. */

  // Data panel
  document
    .getElementById("btn-export-data")
    .addEventListener("click", () => copyToClipboard("data", tabData));

  // Send panel
  document
    .getElementById("send-ep-select")
    .addEventListener("change", onSendEndpointSelected);
  document.getElementById("btn-send").addEventListener("click", sendRequest);
  document.getElementById("ws-console-send").addEventListener("click", sendConsoleMessage);
  document.getElementById("ws-console-input").addEventListener("keydown", (e) => {
    if (e.key === "Enter" && !e.shiftKey) { e.preventDefault(); sendConsoleMessage(); }
  });
  document
    .getElementById("btn-add-header")
    .addEventListener("click", () => addHeaderRow("", ""));
  document.getElementById("btn-gql-add-op").addEventListener("click", () => {
    gqlSaveCurrentOp();
    gqlState.ops.push({ query: "", variables: null, operationName: null, extensions: null });
    gqlState.batched = true;
    gqlState.activeIdx = gqlState.ops.length - 1;
    gqlRenderAll();
  });
  const btnAddPart = document.getElementById("btn-mp-add-part");
  if (btnAddPart) {
    btnAddPart.addEventListener("click", () => {
      const num = (mpState.parts[mpState.parts.length - 1]?.partNumber || 0) + 1;
      mpState.parts.push({
        partNumber: num,
        method: null,
        path: null,
        contentType: "application/json",
        extraHeaders: {},
        contentId: null,
        editor: { kind: "json", value: "{}", meta: null },
      });
      mpRenderAll();
    });
  }

  // Frame selector
  document.getElementById("send-frame-select").addEventListener("change", (e) => {
    _repinTo(parseInt(e.target.value, 10) || 0);  // pin to the chosen frame's CURRENT document
    loadState();  // re-load the selected document's per-document analysis (documentId-keyed)
  });

  wireKeyExamples();

  // Service origin hint: "Open" link
  document.getElementById("send-service-origin-hint").addEventListener("click", (e) => {
    if (e.target.classList.contains("service-origin-open")) {
      var url = e.target.dataset.url;
      if (url) chrome.tabs.create({ url: url });
    }
  });

  // Export buttons
  document.getElementById("btn-copy-curl").addEventListener("click", () => copyAsFormat("curl"));
  document.getElementById("btn-copy-fetch").addEventListener("click", () => copyAsFormat("fetch"));
  document.getElementById("btn-copy-python").addEventListener("click", () => copyAsFormat("python"));

  // Service filter + spec export/import
  document.getElementById("spec-service-select").addEventListener("change", () => {
    renderMethodDropdown();
    _renderServiceGrouping();
  });
  document.getElementById("btn-export-spec").addEventListener("click", exportOpenApiSpec);
  document.getElementById("btn-import-spec").addEventListener("click", () => {
    document.getElementById("import-spec-file").click();
  });
  document.getElementById("import-spec-file").addEventListener("change", importOpenApiSpec);

  // Request log tab filter
  document.getElementById("log-tab-filter").addEventListener("change", async (e) => {
    const val = e.target.value;
    logFilter = val === "active" ? "active" : val === "all" ? "all" : parseInt(val, 10);
    _lastLogFp = "";
    await loadRequestLog();
    renderResponsePanel();
  });
  populateTabFilter();

  // Request log search filter
  document.getElementById("log-search").addEventListener("input", (e) => {
    logSearchQuery = e.target.value.toLowerCase().trim();
    _lastLogFp = "";
    renderResponsePanel();
  });

  /* THE ADDRESS THE OPERATOR IS LOOKING AT TRACKS WHAT THEY TYPE. A path parameter can move the request's
     ORIGIN (a hole spans the run of example segments it occupied, scheme and authority included), so a row
     computed once and left standing would show a host the request no longer goes to — which is worse than
     showing nothing. Delegated on the panel rather than bound per input because buildFormFields replaces
     those inputs wholesale on every schema load, and a per-input binding would silently stop covering the
     fields it was written for. `input` carries typing, `change` carries selects and radios. */
  document.getElementById("panel-send").addEventListener("input", renderSendUrl);
  document.getElementById("panel-send").addEventListener("change", renderSendUrl);
  wireSendIdentityExamples();

  // HAR export
  document.getElementById("btn-export-har").addEventListener("click", exportHar);

  // Global rename handler
  document.addEventListener("click", async (e) => {
    if (e.target.classList.contains("btn-rename")) {
      const { schema, key } = e.target.dataset;
      const currentName = e.target.previousElementSibling.textContent;
      const newName = prompt(`Rename "${currentName}" to:`, currentName);
      if (newName && newName !== currentName) {
        const select = document.getElementById("send-ep-select");
        const svc = select.dataset.svc;
        const methodId = select.dataset.discoveryId; // This is the ID of the selected method/endpoint
        const url = currentRequestUrl;

        // Rename targeting the GraphQL-variables schema keeps the wire key
        // intact — alias is display-only. The regular form path below
        // rebuilds the whole body form, which would blow away the current
        // tree state; for GQL we just re-render the active op panel.
        const isGqlVarsRename = typeof schema === "string" && schema.startsWith("__gqlVars_");

        // Snapshot GQL op state BEFORE the await below — STATE_UPDATED
        // listeners, render() calls, or content script lifecycles can
        // mutate gqlState mid-await and we'd otherwise re-render from
        // empty defaults.
        const savedGqlOp = isGqlVarsRename && gqlState.ops[gqlState.activeIdx]
          ? JSON.parse(JSON.stringify(gqlState.ops[gqlState.activeIdx]))
          : null;

        const currentData = isGqlVarsRename
          ? null
          : formValuesToInitialData(collectFormValues());

        await chrome.runtime.sendMessage({
          type: "RENAME_FIELD",
          service: svc,
          methodId,
          schemaName: schema,
          fieldKey: key,
          newName,
          url,
        });

        // Refresh tabData so re-renders pick up the new schema
        tabData = await chrome.runtime.sendMessage({
          type: "GET_STATE",
          tabId: currentTabId,
          documentId: currentDocumentId(),
        });

        if (isGqlVarsRename) {
          // Restore the snapshot if something clobbered gqlState during
          // the await, then rebuild the full op panel so rename surfaces
          // everywhere (Query, Persisted Operation, Variables tree).
          if (savedGqlOp) {
            if (gqlState.ops.length === 0) {
              gqlState.ops = [savedGqlOp];
              gqlState.activeIdx = 0;
            } else {
              gqlState.ops[gqlState.activeIdx] = savedGqlOp;
            }
          }
          gqlRenderAll();
        } else {
          // Reload form to reflect change, preserving current body mode
          const savedBodyMode = currentBodyMode;
          await loadVirtualSchema(svc, select.dataset.discoveryId, currentData);
          if (currentBodyMode !== savedBodyMode) setBodyMode(savedBodyMode);
          // Re-render response tree so renamed field is immediately visible
          if (lastSendResult) {
            /* SAID WITH THE RECORD'S OWN WORD FOR IT. The rename just reloaded this panel's schema, so the
               doc the result was resolved against is stale and lib/popup-response.js must re-read the
               popup's copy — which is what `discovery: null` MEANS there. `delete` made the same statement
               in the one spelling the record cannot carry: an ABSENT key, which is what a producer that had
               stopped writing the field also produces, and which that file now asserts against. */
            lastSendResult.discovery = null;
            renderResponse(lastSendResult);
          }
        }
      }
    }
  });

  // ─── Export Functions ──────────────────────────────────────────────────────

  async function buildCurrentRequest() {
    const bodyMode = currentBodyMode;
    if (!currentRequestUrl) return null;
    /* THE EXPORTED REQUEST IS COMPOSED BY THE FUNCTIONS THAT COMPOSE THE SENT ONE, never by a second
       spelling here. This read `currentRequestUrl` and applied the form's QUERY parameters through a local
       `_applyFormParamsToUrl` — `composeSendUrl`'s query half written a second time — and it never applied the
       form's PATH parameters at all: `collectFormValues` fills a `pathParams` bucket and no reader of one
       existed anywhere in this file, so an endpoint whose address is a template exported as curl carrying a
       literal `{owner}` the panel had the operator's value for. `currentRequestUrl` is the TEMPLATE
       (`base + pathTemplate`, where the endpoint dropdown assigns it), and it USED TO STOP BEING ONE: this
       clause read "until `sendRequest` writes the composed address back over it, so the snippet was right or
       wrong depending on whether the operator had pressed Send first — which is what made it survive", which
       was an accurate account of the write-back and read it as the reason a bug SURVIVED rather than as a bug.
       It was the larger one. That write destroyed the holes, so after one Send no later edit of a path
       parameter could reach this snippet OR the wire, and the address row disagreed with the operator's own
       field; the slot is split at its declaration now and nothing writes a composed address over a template.
       THE HEADER HALF DIVERGED THE OTHER WAY (see `sendPanelHeaders`, lib/popup-form.js), so the snippet the
       operator copies and the request the button fires were two different requests in two directions, and the
       one that was wrong on the wire was the one that actually went out. SECURITY.md scopes
       the operator tier out of the credentialed destructive-path deny list on the ground that the request is
       "authorized by a human at a surface that shows them the bytes"; an export that shows bytes the send does
       not carry does not weaken that authorization, it FORGES it, which is the argument `composeSendUrl`
       already makes for the address row. One speller each, so the two cannot disagree. */
    const url = sendPanelAddress();

    const httpMethod = currentRequestMethod;
    const contentType = currentContentType;
    const epKey = document.getElementById("send-ep-select").value;
    const sel = document.getElementById("send-ep-select");
    const selectedOpt = sel.options[sel.selectedIndex];

    const headers = {};
    for (const row of document.querySelectorAll(
      "#send-headers-list .header-row",
    )) {
      const key = row.querySelector(".header-key").value.trim();
      const val = row.querySelector(".header-val").value.trim();
      if (key) headers[key] = val;
    }

    let body;
    if (httpMethod === "GET" || httpMethod === "DELETE") {
      // The form's path and query parameters are already in `url` — `sendPanelAddress` applied them under
      // the same `bodyMode === "form"` gate both of these arms used to spell for themselves.
      body = null;
    } else if (bodyMode === "form") {
      body = {
        mode: "form",
        formData: { fields: collectFormValues().fields },
      };
    } else if (bodyMode === "graphql") {
      body = {
        mode: "graphql",
        operations: gqlCollectAllOps(),
        batched: gqlState.batched,
      };
    } else if (bodyMode === "multipart") {
      body = mpCollectBody();
    } else {
      body = {
        mode: "raw",
        rawBody: document.getElementById("send-raw-body").value,
      };
    }

  /* A dataset entry an option does not carry reads as `undefined`, and the consumer branches on "did the
     operator choose a discovery method". `undefined` is not a statement of that — `null` is, and the offscreen
     then has a fact to test rather than a hole to survive. An option with no selection at all is the same
     answer, so `selectedOpt` being absent is not a second case. */
  const _svc = selectedOpt && typeof selectedOpt.dataset.svc === "string" && selectedOpt.dataset.svc !== ""
    ? selectedOpt.dataset.svc : null;
  const _mid = selectedOpt && typeof selectedOpt.dataset.discoveryId === "string" &&
    selectedOpt.dataset.discoveryId !== "" ? selectedOpt.dataset.discoveryId : null;
    try {
      return await chrome.runtime.sendMessage({
        type: "BUILD_REQUEST",
        endpointKey: epKey,
        service: _svc,
        methodId: _mid,
        url,
        httpMethod,
        contentType,
        headers: sendPanelHeaders(headers),
        body,
        requestInit: sendPanelRequestInit(),
        stated: sendPanelStatedHeaders(),
      });
    } catch (e) {
      /* An invariant abort travels ON (extension/check.js RETHROW_FATAL): `null` here is the ONE state the
         caller reads as "the background did not answer", and a broken record assertion arriving as that
         would report a contract failure as an unreachable service worker. */
      RETHROW_FATAL(e);
      return null;
    }
  }

  /* THE BUILT REQUEST, ASSERTED WHERE IT ARRIVES, so a consumer never renders a gap as the server's
     answer. `buildExportRequest` answers with EITHER an `error` string or the four fields the
     three formatters below turn into curl, fetch and python, and those formatters were reading all four
     through `||`: `req.headers || {}` in four places and `req.body` inside a swallowing try. What that
     concealed is precise — a curl command with no `-H` lines is a VALID command, and so is one with no
     `--data`, so a build that had stopped carrying the operator's headers or their body produced a snippet
     that runs, that they paste into a terminal, and that makes a different request than the panel showed. */
  function _checkExportRequestReply(reply) {
    if ("error" in reply) {
      DCHECK(typeof reply.error === "string" && reply.error !== "",
             "BUILD_REQUEST answered with an `error` that names nothing — the reason IS the whole of what " +
             "that arm carries, and an empty one tells the operator their request could not be built for no " +
             "stated cause");
      return;
    }
    DCHECK(typeof reply.url === "string" && reply.url !== "" &&
           typeof reply.method === "string" && reply.method !== "" &&
           typeof reply.headers === "object" && reply.headers !== null &&
           (reply.body === null || typeof reply.body === "string"),
           "BUILD_REQUEST answered with an incomplete request record — buildExportRequest writes " +
           "url/method/headers/body on its one success return, and a formatter that finds one missing emits " +
           "a curl or fetch snippet that RUNS and sends something else");
  }

  function formatCurl(req) {
    const sq = (s) => s.replace(/'/g, "'\\''");
    const parts = [`curl -X '${sq(req.method)}'`];
    for (const [k, v] of Object.entries(req.headers)) {
      parts.push(`  -H '${sq(k)}: ${sq(v)}'`);
    }
    if (req.body) {
      // The header map is always present; whether it NAMES a content type is the question this asks, and ""
      // is the built request carrying none.
      const ct = req.headers["Content-Type"] || "";
      if (ct.includes("protobuf") || ct.includes("grpc")) {
        // Binary body is base64-encoded — pipe through base64 decode
        parts.push(`  --data-binary @- <<< $(echo '${sq(req.body)}' | base64 -d)`);
      } else {
        parts.push(`  -d '${sq(req.body)}'`);
      }
    }
    parts.push(`  '${sq(req.url)}'`);
    return parts.join(" \\\n");
  }

  function formatFetch(req) {
    const opts = { method: req.method };
    if (Object.keys(req.headers).length) opts.headers = req.headers;
    if (req.body) opts.body = req.body;
    return `fetch(${JSON.stringify(req.url)}, ${JSON.stringify(opts, null, 2)});`;
  }

  function formatPython(req) {
    const lines = ["import requests", ""];
    const kwargs = [];
    const ct = req.headers["Content-Type"] || "";
    const isBinaryBody = ct.includes("protobuf") || ct.includes("grpc");
    const isJson = ct.includes("application/json");
    // For JSON content types, use json= with parsed object.
    // For binary (protobuf/gRPC-Web), body is base64 — decode it.
    // Otherwise use data= with raw string.
    if (isBinaryBody && req.body) {
      lines[0] = "import requests\nimport base64";
      kwargs.push(`    data=base64.b64decode(${JSON.stringify(req.body)})`);
    } else if (isJson && req.body) {
      try {
        const parsed = JSON.parse(req.body);
        kwargs.push(`    json=${JSON.stringify(parsed)}`);
      } catch (e) {
        /* THIS CATCH HAS A REAL JOB, WHICH IS EXACTLY WHY IT NEEDS THE FIRST LINE. A body whose Content-Type
           says JSON and whose bytes are not is a real state — servers misreport content types constantly —
           and `data=` is the right export for it. But on this side an assertion is a THROW (extension/
           check.js), so every legitimate catch is also a place an invariant abort becomes a plausible answer:
           without this line a broken record contract reaching the request builder would have been exported as
           a Python snippet posting the raw string, which is the concealment shape wearing a legitimate
           purpose. RETHROW_FATAL is not a second assertion mechanism — it is what stops the one mechanism
           being locally disabled. */
        RETHROW_FATAL(e);
        kwargs.push(`    data=${JSON.stringify(req.body)}`);
      }
    } else if (req.body) {
      kwargs.push(`    data=${JSON.stringify(req.body)}`);
    }
    const headers = { ...req.headers };
    // json= sets Content-Type automatically — remove to avoid conflict
    if (isJson && req.body) delete headers["Content-Type"];
    if (Object.keys(headers).length) {
      kwargs.push(`    headers=${JSON.stringify(headers)}`);
    }
    const fn = req.method === "GET" ? "get" : req.method === "POST" ? "post" : req.method === "PUT" ? "put" : req.method === "DELETE" ? "delete" : "request";
    if (fn === "request") {
      kwargs.unshift(`    ${JSON.stringify(req.method)}`);
    }
    const url = `${JSON.stringify(req.url)}`;
    if (kwargs.length) {
      lines.push(`resp = requests.${fn}(`);
      lines.push(`    ${url},`);
      lines.push(kwargs.join(",\n") + ",");
      lines.push(")");
    } else {
      lines.push(`resp = requests.${fn}(${url})`);
    }
    lines.push("print(resp.status_code, resp.text)");
    return lines.join("\n");
  }

  async function copyAsFormat(format) {
    const btn = document.getElementById(`btn-copy-${format}`);
    const req = await buildCurrentRequest();
    if (req !== null) _checkExportRequestReply(req);
    if (!req || req.error) {
      btn.textContent = "No request";
      setTimeout(() => { btn.textContent = format === "python" ? "Python" : format; }, 1500);
      return;
    }

    let text;
    if (format === "curl") text = formatCurl(req);
    else if (format === "fetch") text = formatFetch(req);
    else text = formatPython(req);

    try {
      await navigator.clipboard.writeText(text);
      btn.textContent = "Copied!";
    } catch (e) {
      // A clipboard write CAN legitimately reject (no permission, no user gesture, a headless profile), and
      // "Failed" on the button is what that state looks like. An invariant abort travels ON through it: a
      // formatter that asserted its way out of building `text` must not be reported to the user as the
      // clipboard having refused a snippet that was never built.
      RETHROW_FATAL(e);
      btn.textContent = "Failed";
    }
    setTimeout(() => { btn.textContent = format === "python" ? "Python" : format; }, 1500);
  }

  function exportHar() {
    /* A HAR IS A LOG OF HTTP ROUND TRIPS, AND HALF THIS LOG IS NOT ONE. HAR §4.2.6 entries defines the array
       as "all exported HTTP requests", so a WebSocket connection, a postMessage stream and a MessageChannel
       stream have no representation in it — and they were being exported as HTTP entries anyway, every field
       of them supplied by a default: method "WEBSOCKET", status 0, statusText "", no headers, no content. A
       reviewer opening that file read a round trip that never happened. Those records are the network view's,
       and the console is where they are read; the export takes the kind the format is for. */
    const entries = _vs.entries.filter((e) => e.kind === "http");
    if (entries.length === 0) {
      alert("No HTTP requests to export.");
      return;
    }

    const harEntries = entries.map((r) => {
      /* NO `if (r.requestHeaders)` / `if (r.responseHeaders)` GUARD. Both are written as objects on every
         record of this kind and `_pushGlobalLog` asserts it, so the empty object is a request that carried no
         headers — which is what an EventSource GET and a GET form both truthfully have — and the guard made
         that indistinguishable from a producer that had stopped writing the list at all. */
      const reqHeaders = Object.entries(r.requestHeaders).map(([k, v]) => ({ name: k, value: v }));
      const respHeaders = Object.entries(r.responseHeaders).map(([k, v]) => ({ name: k, value: v }));

      const request = {
        method: r.method,
        url: r.url,
        httpVersion: "HTTP/1.1",
        cookies: [],
        headers: reqHeaders,
        queryString: [],
        headersSize: -1,
        bodySize: r.rawBodyB64 ? Math.ceil((r.rawBodyB64.length * 3) / 4) : 0,
      };

      // Parse query string from URL — canParse guard so HAR export doesn't
      // catch parse-validity throws. A malformed request.url means the request
      // log captured something unparseable; preserve the entry but skip the
      // queryString breakdown (HAR spec allows empty queryString array).
      if (URL.canParse(r.url)) {
        const u = new URL(r.url);
        u.searchParams.forEach((v, k) => {
          request.queryString.push({ name: k, value: v });
        });
      } else {
        console.debug("[popup:HAR] skipping queryString for unparseable url:", r.url);
      }

      // Request body
      if (r.rawBodyB64) {
        request.postData = {
          /* HAR §4.2.12 postData's `mimeType` is the type of the POSTED DATA, so the request's own content
             type is the only value that answers it and "" is the request having stated none. It read
             `|| "application/octet-stream"`, which names a type the request did not send. */
          mimeType: r.contentType,
          text: r.rawBodyB64,
          encoding: "base64",
        };
      }

      const response = {
        status: r.status,
        /* HAR §4.2.8 response's `statusText` is the "Response status description" — the SERVER'S. It was
           `r.statusText || (r.status === 200 ? "OK" : "")` against a field no producer of this record had
           ever written (cb0061b0 carries it end to end now), so every entry this export has ever produced
           stated a reason phrase nothing observed, in the one field of a HAR a reader takes as verbatim. */
        statusText: r.statusText,
        httpVersion: "HTTP/1.1",
        cookies: [],
        headers: respHeaders,
        content: {
          size: r.responseBody ? r.responseBody.length : 0,
          /* THE RESPONSE'S OWN TYPE AND NOT THE REQUEST'S. HAR §4.2.14 content's `mimeType` is the type of
             the response text, so `|| r.contentType` answered it with the Content-Type the CLIENT sent —
             right by accident for gRPC-Web, where both ends carry the same string, and a fabrication
             everywhere else. "" is the server having stated no type, which is the fact this field has. */
          mimeType: r.mimeType,
        },
        redirectURL: "",
        headersSize: -1,
        // HAR 1.2's response bodySize is "-1 if the info is not available": a body nothing observed.
        bodySize: r.responseBody === null ? -1 : r.responseBody.length,
      };

      // Response body
      if (r.responseBody) {
        response.content.text = r.responseBody;
        if (r.responseBase64) {
          response.content.encoding = "base64";
        }
      }

      return {
        startedDateTime: new Date(r.timestamp).toISOString(),
        time: 0,
        request,
        response,
        cache: {},
        timings: { send: 0, wait: 0, receive: 0 },
      };
    });

    const har = {
      log: {
        version: "1.2",
        creator: { name: "API Security Researcher", version: "1.0" },
        entries: harEntries,
      },
    };

    // Download as file
    const blob = new Blob([JSON.stringify(har, null, 2)], { type: "application/json" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = "api-security-researcher.har";
    a.click();
    URL.revokeObjectURL(url);
  }

  const EXTENSION_ORIGIN = `chrome-extension://${chrome.runtime.id}`;

  // Threat model: popup runs in the extension process (trusted), but broadcast
  // messages are received by all listeners including content scripts. The real
  // gate is sender.ORIGIN — browser-set, unforgeable by the renderer, and "null"
  // for a sandboxed extension page (which must NOT be trusted as one). An exact
  // origin match beats a sender.url prefix. See SECURITY.md.
  chrome.runtime.onMessage.addListener((msg, sender) => {
    if (sender.id !== chrome.runtime.id) return;

    const isExtensionPage = sender.origin === EXTENSION_ORIGIN;
    if (!isExtensionPage) return;

    if (msg.type === "STATE_UPDATED") {
      // Endpoints, security findings and keys are GLOBAL (cross-tab
      // globalStore), so re-render on ANY tab's update — not just the active
      // tab's. The background deep unused-feature grind and cross-session
      // resumes learn endpoints (e.g. github's login-gated `preheat`) MINUTES
      // after the page loaded and may carry a different tabId than the open
      // popup's active tab; gating on `msg.tabId === currentTabId` hid those,
      // so they only showed on reopen. loadState re-fetches the global store
      // plus the active tab's per-tab request log, so a blanket refresh is
      // correct (and throttled).
      throttledLoadState();
      if (currentBodyMode === "msgconsole" && currentChannelId) {
        refreshMsgConsole();
      }
    }
  });
  loadState();
});

// ─── State ───────────────────────────────────────────────────────────────────

async function loadState() {
  // Fetch available frames FIRST so currentDocumentId() can resolve the selected
  // frame's documentId for the per-document GET_STATE below.
  /* THE FRAME TREE IS THE BROWSER'S ANSWER AND AN EMPTY ONE IS NOW A FACT, NOT A FAILURE. The assert here
     used to be `frames.length > 0`, justified by lib/popup-handlers.js substituting a one-element outermost
     frame "when webNavigation reports nothing" — so what it actually asserted was that the SUBSTITUTION had
     run. It could never fire: the fabricated frame satisfied it, carrying `documentId: null`, and that null
     is what made `currentDocumentId()` answer null and every per-document panel render its empty state as a
     fact about the page. With the fabrication deleted the reply carries the browser's own two answers, and
     BOTH are asserted here rather than defaulted.
     The `catch { availableFrames = [] }` goes with it, and for the same reason it was kept: the rejection IS
     real — the offscreen document is not there to answer — but `[]` is no longer a spare value to park that
     in. It now MEANS "the browser resolves this tab and it holds no web document", which the view says in
     words, so writing it on a transport failure would re-collapse the two facts the handler was just fixed to
     separate. A rejection therefore propagates: it is the same statement the GET_STATE assert below makes one
     line down, and a popup that cannot reach the trusted zone must paint nothing rather than paint a page. */
  const frameTree = await chrome.runtime.sendMessage({ type: "GET_FRAMES", tabId: currentTabId });
  DCHECK(frameTree && typeof frameTree === "object" && typeof frameTree.tabResolved === "boolean"
         && Array.isArray(frameTree.frames),
         "GET_FRAMES did not answer with a {tabResolved, frames} record — lib/popup-handlers.js builds one on " +
         "every path off webNavigation's own two answers, so another shape is that handler broken and this " +
         "view would have no way to tell 'this tab holds no web document' from 'the tab is gone'");
  DCHECK(frameTree.tabResolved || frameTree.frames.length === 0,
         "GET_FRAMES reported frames for a tab id the browser does not resolve — the frames come FROM that " +
         "resolution, so the two disagreeing means the record was assembled from something other than one " +
         "getAllFrames answer");
  tabResolved = frameTree.tabResolved;
  availableFrames = frameTree.frames;
  _resolvePin();   // validate/advance the pinned send-target against the live frame tree
  await loadSendJarDomain();   // the open frames the Send panel offers as jars and origins
  tabData = await chrome.runtime.sendMessage({
    type: "GET_STATE",
    tabId: currentTabId,
    documentId: currentDocumentId(),
  });
  /* EVERY PANEL IS MADE OF THIS ONE REPLY, AND NOTHING ASKED WHETHER IT ARRIVED. lib/popup-handlers.js
     answers GET_STATE unconditionally — a document it does not know still gets `_emptyDocView()` through
     `serializeTabData`, so the global cumulative moat is rendered either way — which means a null here is
     never "this tab has nothing"; it is the brain not answering at all. And the render path takes that
     silently: `tabData?.apiKeys`/`tabData?.discoveryDocs` read past it and every panel paints its own empty
     state, so the popup tells the user "No data captured yet" and offers an empty method list for a session
     whose store is full. MEASURED: with the offscreen document wedged, GET_STATE resolved null and the popup
     rendered exactly that — a clean-looking view of a page the engine had already learned an endpoint from. */
  DCHECK(tabData && typeof tabData === "object",
         "GET_STATE did not answer with a state object — lib/popup-handlers.js answers it unconditionally " +
         "(an unknown document still gets the global moat through _emptyDocView), so a null reply is the " +
         "offscreen brain not reachable, and every panel would render its empty state as a fact about the page");
  // The recorded API drift (lib/popup-discovery.js). Fetched only when it has not been read yet or an action
  // just re-fetched a document — a re-render never needs it again, and it is the whole history, not a window.
  if (discoveryChanges === null) await loadDiscoveryChanges();
  await loadRequestLog();
  render();
  _refreshSendEnabled();   // readiness may have changed (pin became live / stale / not-ready)
  /* AND THE EGRESS ROW FOLLOWS THE PIN. Its subject is the PINNED DOCUMENT'S origin, so a frame change or a
     navigation makes the origin it names — and the refusal it reports — a statement about a document that is
     no longer the one in front of the person. It is re-asked here, from the host, rather than re-labelled
     from anything this surface remembers. */
  await renderEgressPolicy();
  /* The third-party-cookie exception names the pinned document's site, so it follows the pin too. */
  await renderThirdPartyCookies();
}

/* THE SHARE, AND THE DECISIONS RESIDENCY MADE UNDER IT. Called with a byte count it SETS the share first;
   either way it renders what the host reports back, so the control can never show a number the store is not
   using. Everything here is a producer field the host writes on every reply — none of it is defaulted,
   because a `|| 0` on `stranded` would render "this policy has cost nothing" for a host that stopped
   reporting, which is the one thing this row exists to say. */
async function renderFrontierShare(bytes) {
  const r = await chrome.runtime.sendMessage(
    bytes === undefined ? { type: "FRONTIER_SHARE" } : { type: "FRONTIER_SHARE", bytes });
  DCHECK(r && typeof r.share === "number" && typeof r.docBytes === "number" &&
         typeof r.overShare === "number" && typeof r.shed === "number" &&
         typeof r.stranded === "number" && typeof r.rederived === "number" && typeof r.entries === "number" &&
         typeof r.crashed === "number",
         "the frontier-share command answered without the store's own numbers — this row is the only place " +
         "the user can see what the setting is doing, and a partial answer would render a policy that looks " +
         "as if it had never had to act");
  const mb = (n) => (n / (1024 * 1024)).toFixed(1);
  document.getElementById("frontier-share-mb").value = String(Math.round(r.share / (1024 * 1024)));
  document.getElementById("frontier-share-status").textContent =
    mb(r.docBytes) + " MB stored across " + r.entries + " parked page(s)" +
    (r.overShare > 0 ? "; " + mb(r.overShare) + " MB over the share is the only copy there is and was kept" : "") +
    "; " + r.shed + " shed, " + r.rederived + " fetched back, " + r.stranded + " could not be" +
    (r.crashed > 0 ? "; " + r.crashed + " stopped because resuming it crashed this engine" : "") +
    (r.persistError ? "; NOT SAVED for the next session (" + r.persistError + ")" : "");
}

/* This extension's third-party-cookie setting and the exception for the shown document's site, as the host
   holds them: no `act` reads them; `{allowed}` sets the setting and `{exception}` the site's exception. The
   status line says what it covers and that it is not the browser's setting. */
async function renderThirdPartyCookies(act) {
  const subject = currentPrincipalOrigin();
  const r = await chrome.runtime.sendMessage(act === undefined ? { type: "THIRD_PARTY_COOKIES", subject }
    : Object.assign({ type: "THIRD_PARTY_COOKIES", initiator: "user-initiated", subject }, act));
  DCHECK(r && typeof r.allowed === "boolean" && typeof r.defaultAllowed === "boolean" &&
         (r.site === null || typeof r.site === "string") && (r.exception === null || typeof r.exception === "string") &&
         (r.persistError === null || typeof r.persistError === "string"),
         "the third-party-cookie command answered without the setting and exception now in force");
  document.getElementById("third-party-cookies").checked = r.allowed;
  document.getElementById("third-party-cookies-site").textContent =
    r.site === null ? "no site (opaque origin)" : r.site;
  const ex = document.getElementById("third-party-cookies-exception");
  ex.value = r.exception === null ? "" : r.exception;
  ex.disabled = r.site === null;
  document.getElementById("third-party-cookies-status").textContent =
    (r.allowed ? "On" : "Off") + (r.allowed === r.defaultAllowed ? " (default)" : "") +
    ": when off, a request this tool sends with your cookies to a site other than the page's (not a page " +
    "navigation) carries and stores only partitioned cookies. This is the extension's own setting; Chrome's " +
    "third-party cookie setting is separate and is not read." +
    (r.persistError ? " NOT SAVED for the next session (" + r.persistError + ")." : "");
}

/* ─── THE PER-ORIGIN EGRESS WIDENING ─────────────────────────────────────────────────────────────────────
   THE SUBJECT IS THE BROWSER'S ORIGIN FOR THE PINNED DOCUMENT AND IS NEVER PARSED OUT OF A URL.
   `currentPrincipalOrigin` reads the documentId→origin mapping the offscreen built from `MessageSender.origin`
   — SECURITY.md's credentialed-read principal, opaque-unique per document — so a page that sandboxes its own
   iframe cannot have this control name its EMBEDDER's origin. That accessor answers `""` for an opaque or
   not-yet-reported document, which is a POSITIVE state of the web and is rendered as one: there is no origin
   to permit, and the button is off rather than aimed at something invented.
   `initiator` IS STATED HERE BECAUSE THIS IS WHERE THE PERSON IS. The bridge CHECKs it and an absent or
   unknown grade takes the refusing arm there, so this is a claim rather than a formality: a widening is a
   person's sentence and the ONE thing this command may not be is something the tool decided on its own. The
   constant is `lib/schema.js`'s `PAGE_CONTEXT_USER_INITIATED` and its VALUE is spelled as a literal here
   because the popup realm does not load that file — the vocabulary is owned and asserted on the other side,
   which is the same shape every other word that crosses this boundary has (the engine states `forced` and
   `derived` as strings and safe-fetch.js asserts them). A rename there makes this control start being
   REFUSED, loudly, which is the direction a vocabulary may drift in.
   NOTHING IS DEFAULTED OUT OF THE REPLY. Every field is one the host writes on every answer, and the failure
   an `|| []` would hide is the worst one this surface has: an empty origin list reads as "you have permitted
   nothing", which is exactly what a person would see if this command stopped being answered at all. */
async function renderEgressPolicy(act) {
  const subject = currentPrincipalOrigin();
  const r = await chrome.runtime.sendMessage({
    type: "EGRESS_POLICY", initiator: "user-initiated", subject,
    grant: act && act.grant, revoke: act && act.revoke, permit: act && act.permit });
  /* EVERY FIELD THIS FUNCTION READS IS ASSERTED, AND THE THREE THAT CAN BE `null` ARE ASSERTED AS
     NULL-OR-STRING RATHER THAN AS PRESENT. `null` is a POSITIVE answer on each of them — "this origin can be
     widened", "a forced data request here fires", "nothing was refused" — so an ABSENT one is not a missing
     detail: every test below reads `!== null`, which `undefined` passes, so a field that stopped being
     written would render "this origin cannot be permitted (undefined)" for an origin that can. */
  DCHECK(r && Array.isArray(r.origins) && typeof r.subject === "string" && typeof r.changed === "boolean" &&
         (r.subjectUsable === null || typeof r.subjectUsable === "string") &&
         (r.subjectRefusal === null || typeof r.subjectRefusal === "string") &&
         (r.refused === null || typeof r.refused === "string") &&
         Array.isArray(r.signals) && Array.isArray(r.defaults) && Array.isArray(r.legacyDropped) &&
         (r.vector === null || (r.vector !== null && typeof r.vector === "object")),
         "the egress command answered without the table now in force — this row is the only place a person " +
         "can see or change which signals this tool may fetch data under, and a partial answer would render " +
         "a permission surface that permits nothing while looking as if it had never been asked. The four " +
         "fields added with the per-signal control are asserted for the same reason the three above are: " +
         "`signals` empty renders NO ROWS, which is the same pixels as a policy that offers no choices");
  const originEl = document.getElementById("egress-origin");
  const btn = document.getElementById("btn-egress-widen");
  const status = document.getElementById("egress-status");
  const allowed = r.origins.indexOf(r.subject) >= 0;
  originEl.textContent = r.subject === "" ? "(no origin)" : r.subject;
  btn.disabled = r.subjectUsable !== null || allowed;
  /* WHAT THE PERSON IS TOLD, AND IT IS THE POLICY'S OWN ANSWER RATHER THAN THIS SURFACE'S GUESS AT IT.
     `subjectRefusal` is what `safeFetchFiringRefusal` — the SAME function the chokepoint refuses requests
     with — answers for a forced data request at this origin right now. So the sentence below changes because
     the policy changed, and a control that claimed to have granted something the policy did not would say so
     on its next line rather than reporting its own success. */
  let msg;
  if (r.subject === "")
    msg = "this document has an opaque origin (a sandboxed frame, or about:blank) and is same-origin with "
        + "nothing, so there is no host to permit";
  else if (r.subjectUsable === "not-http")
    msg = "only http(s) origins can be permitted — this tool issues no other scheme, so an entry here would "
        + "be a permission that matched nothing";
  else if (r.subjectUsable !== null)
    msg = "this origin cannot be permitted (" + r.subjectUsable + ")";
  else if (r.subjectRefusal === null)
    msg = "allowed — a forced data request at this origin fires under the values ticked below";
  else
    /* THE REFUSAL NAMES A SIGNAL AND A VALUE, WHICH IS THE WHOLE OF WHAT THIS CONTROL BUYS OVER A SCORE: the
       person is told WHICH ROW below holds it rather than that something did, and the row is right there. */
    msg = "refused on " + r.subjectRefusal + " — the row named there is the one holding it; this app's own "
        + "scripts, modules and lazy chunks load at every setting";
  if (r.refused) msg = "not permitted (" + r.refused + ") — " + msg;
  if (r.persistError) msg = "NOT SAVED for the next session (" + r.persistError + ") — " + msg;
  status.textContent = msg;
  /* WHAT THE PERSON IS DECIDING ABOUT. Every row is the POLICY'S, read back — this surface computes no value
     and decides no combination, because a control that worked out for itself what would fire would be the
     second copy of the rule that CLAUDE.md says two zones answering one question always becomes.
     THE CERTAINTY TRAVELS WITH THE ROW AND IS NOT A DECORATION. A person ticking `lineage: unknown` is
     permitting requests whose lineage NOBODY HAS ESTABLISHED, which is a different sentence from permitting
     requests known to have none — and the difference is invisible unless the row says it. That is
     §A-FIELD-A-CONSUMER-DEFAULTS arriving where the consumer is a person, and the words below are the fix.
     A NON-GATING ROW IS SHOWN AND CANNOT BE TICKED, because it is a FACT about this transport rather than a
     choice: `method` has one value because `safe-fetch.js` sends GET and can compose no other, and
     `invalidity` has one because no caller here can state such an intent through a closed option set. Hiding
     them would leave a person reading the rows that remain as though they were the whole question. */
  const CERTAINTY_SAYS = {
    certain: "this tool computed it",
    stated: "the engine said it; only the vocabulary is checked",
    partial: "a match is a fact — NO match is not a statement that there is none",
    undetermined: "nothing here can work this out yet; permitting it permits the unknown",
    intent: "an intent this engine holds and cannot verify" };
  // Joins node lists with a text separator, the node form of `Array.prototype.join`.
  const joinNodes = (lists, sep) => lists.map((n, i) => (i === 0 ? n : [sep, n]));
  domReplaceChildren(document.getElementById("egress-signals"), r.signals.length === 0 ? [] : [
    domElement("div", { class: "card-label" }, ["What may be fetched here, signal by signal"]),
    /* THE DEFAULTS FIRST, BECAUSE THE FIRST QUESTION A PERSON HAS LOOKING AT A CONTROL THAT PERMITS NOTHING
       IS WHY THEIR APP STILL WORKS. They are read off the policy rather than written here for the reason
       every other line in this function is: a surface that explained them in its own words would be a second
       copy of the list that could stop agreeing with it. */
    domElement("div", { class: "egress-default" }, ["Fires without any of this: ",
    /* AN ARM IS A CONJUNCTION AND EVERY CONDITION IS RENDERED, which is not a formatting choice: an arm
       fires only where ALL of its conditions hold, so showing one of them would tell a person that a
       population fires which does not — the same "one row, two populations" the arm list itself was changed
       to stop. `join(" and ")` is the sentence, and an arm of one condition reads exactly as it did. */
      joinNodes(r.defaults.map((d) => [
        joinNodes(d.when.map((c) => domElement("code", {}, [c.signal + "=" + c.value])), " and "),
        " (" + d.why + ")",
      ]), "; "),
    ]),
    r.signals.map((sig) => {
      const now = r.vector && r.vector[sig.name] !== undefined ? r.vector[sig.name] : null;
      return domElement("div", { class: "egress-signal" }, [
        domElement("code", {}, [sig.name]),
        domElement("span", { class: "egress-certainty" }, [CERTAINTY_SAYS[sig.certainty] || sig.certainty]),
        sig.values.map((v) => domElement("label", { class: "egress-value" }, [
          sig.gates
            ? domElement("input", Object.assign({ type: "checkbox", "data-egress-signal": sig.name,
                                                  "data-egress-value": v.value }, v.permitted ? { checked: "" } : {}))
            : [],
          domElement("span", now === v.value ? { class: "egress-now" } : {}, [v.value]),
          now === v.value ? domElement("span", { class: "egress-now-tag" }, ["this request"]) : [],
        ])),
        sig.gates ? [] : domElement("span", { class: "egress-fixed" }, ["fixed — not a choice here"]),
      ]);
    }),
  ]);
  /* AND A PERMISSION THAT COULD NOT BE CARRIED FORWARD IS SAID OUT LOUD. The previous control was one switch
     per origin and its store was a list of origin names; a bare origin in that shape meant "fire everything
     here", including signals that did not exist when the person said it, so it is DROPPED rather than read
     as a permission over them. A grant that silently stops existing is the one failure this row must not
     have — a person who is never told concludes it is still granted. */
  const dropped = document.getElementById("egress-dropped");
  /* ASSERTED AND NOT `if (dropped)`, WHICH IS THE SAME DEFECT ONE ZONE OVER FROM THE ONE THIS WHOLE ROW IS
     ABOUT. A guard there would make a MISSING ELEMENT indistinguishable from a person with nothing to be
     told, and the thing that goes unsaid is a permission of theirs that stopped existing. The node is this
     extension's own markup, so its absence is this surface broken rather than anything a page did. */
  DCHECK(dropped !== null,
         "the popup has no `egress-dropped` element — it is where a person is told that a standing " +
         "permission could not be carried forward, and with no node the sentence is simply never said while " +
         "the grant is simply gone");
  dropped.textContent = r.legacyDropped.length === 0 ? "" :
    "Permissions made with the previous single-switch control were cleared and must be re-made signal by "
    + "signal: " + r.legacyDropped.join(", ");
  /* THE ALLOWED LIST IS THE CHOKEPOINT'S TABLE READ BACK, so what is shown is what the policy is using — and
     the way OUT is beside every entry, because a permission with no withdrawal is a one-way door and this
     one spends somebody else's server under this person's session. */
  domReplaceChildren(document.getElementById("egress-allowed"), r.origins.length === 0 ? [] : [
    domElement("div", { class: "card-label" }, ["Data requests permitted at"]),
    r.origins.map((o) => domElement("div", { class: "egress-entry" }, [
      domElement("code", {}, [o]),
      domElement("button", { type: "button", class: "btn-small", "data-egress-revoke": o }, ["Revoke"]),
    ])),
  ]);
}

async function clearState() {
  await chrome.runtime.sendMessage({ type: "CLEAR_TAB", tabId: currentTabId });
  tabData = null;
  allTabsData = null;
  discoveryChanges = null;   // the drift history is part of "delete ALL extension data"
  _lastKeysFp = _lastSecFp = _lastLogFp = _lastSendFp = "";
  render();
}

async function loadRequestLog() {
  // The request log is a GLOBAL array (each entry carries its own tabId) — there
  // is NO per-document log. So every scope is served by GET_ALL_LOGS: "active" =
  // the global log filtered to the active tab, "all" = every tab, a number = that
  // tab. (Previously "active" nulled allTabsData and the render fell back to the
  // per-document tabData.requestLog, which is always empty after the
  // tabId->documentId migration — so the "Active Tab" option showed nothing.)
  const filter = logFilter === "active" ? currentTabId : logFilter;
  if (filter == null) {
    allTabsData = null;
    return;
  }
  let logs;
  try {
    logs = await chrome.runtime.sendMessage({
      type: "GET_ALL_LOGS",
      filter,
    });
  } catch (e) {
    // An invariant abort travels ON through here (check.js RETHROW_FATAL): every legitimate catch in this
    // view is also a place the assertions below could become the plausible "no logs for this scope" state,
    // which is the one thing a broken reply must not be able to say. A sendMessage rejection IS that state —
    // the background is not answering — so it keeps the null.
    RETHROW_FATAL(e);
    allTabsData = null;
    return;
  }
  /* THE LOG SLICES, ASSERTED AT THE ONE PLACE THEY ARRIVE. lib/popup-handlers.js's GET_ALL_LOGS builds every
     tab bucket as `{ meta, requestLog: [] }` before it pushes the first entry, so a bucket without an array
     is that handler broken — and the readers (replayRequest below, renderResponsePanel) walk
     `data.requestLog` straight, so what a `|| []` would have produced is a tab whose traffic reads as "this
     tab made no requests": a measurement about a producer that did not produce, indistinguishable from a tab
     the user simply had not touched. It is asserted HERE, on arrival, rather than re-asked at each `.find()`
     — one arrival, one contract — and `allTabsData` is not assigned until it has passed, so a reply that is
     not a bucket map can never be mistaken for an empty one. */
  DCHECK(logs && typeof logs === "object",
         "GET_ALL_LOGS answered with something that is not a bucket map — lib/popup-handlers.js sends a " +
         "plain object keyed by tabId and answers this command unconditionally, so anything else is that " +
         "command not reaching the background at all");
  for (const _tid of Object.keys(logs)) {
    DCHECK(Array.isArray(logs[_tid].requestLog),
           "tab " + _tid + " came back from GET_ALL_LOGS without a requestLog array — the handler creates " +
           "the bucket with an empty one and only then pushes, so a missing array is that projection broken " +
           "and this view would report the tab as having made no requests at all");
    /* THE TAB LABEL IS ALWAYS NAMED, WHICH IS WHY NOTHING DOWNSTREAM MAY RE-NAME IT. The handler's last act
       before answering is `if (!result[tid].meta.title) result[tid].meta.title = "Tab " + tid`, so a title
       is a settled fact by the time it arrives — and renderResponsePanel and _renderLogCard each carried
       their OWN `|| ("Tab " + id)` copy of that same fallback. Three writers of one string is how the two
       downstream ones stayed live long after the first made them unreachable, and it is why a handler that
       stopped naming tabs would have gone on rendering perfect labels here with nothing to say so. */
    DCHECK(!!logs[_tid].meta && typeof logs[_tid].meta.title === "string" && logs[_tid].meta.title !== "",
           "tab " + _tid + " came back from GET_ALL_LOGS with no title — the handler names every bucket " +
           "before it answers, so an empty one is that naming broken and the request-log cards would label " +
           "a real tab's traffic with a tab id this popup invented");
  }
  allTabsData = logs;
}

async function populateTabFilter() {
  const select = document.getElementById("log-tab-filter");
  if (!select) return;
  try {
    const tabList = await chrome.runtime.sendMessage({ type: "GET_TAB_LIST", tabId: currentTabId });
    // Remove old dynamic options (keep "Active Tab" and "All Tabs")
    while (select.options.length > 2) {
      select.remove(2);
    }
    if (tabList && tabList.length > 1) {
      const divider = document.createElement("option");
      divider.disabled = true;
      divider.textContent = "──────────";
      select.appendChild(divider);

      for (const t of tabList) {
        const opt = document.createElement("option");
        opt.value = String(t.tabId);
        let label = t.title
          ? (t.title.length > 30 ? t.title.slice(0, 30) + "\u2026" : t.title)
          : `Tab ${t.tabId}`;
        if (t.closed) label += " (closed)";
        opt.textContent = `${label} (${t.count})`;
        if (t.tabId === currentTabId) opt.textContent += " \u2605";
        select.appendChild(opt);
      }
    }
    // Restore selection
    if (logFilter !== "active" && logFilter !== "all") {
      select.value = String(logFilter);
    }
  } catch (e) {
    // GET_TAB_LIST RPC failure or DOM manipulation throw — the popup's tab
    // filter dropdown will be stale. Surface so the user-visible gap (only
    // "Active Tab" / "All Tabs" available, no per-tab choices) is diagnosable.
    console.warn("[popup:populateTabFilter] failed:", e && e.message || e);
  }
}

// ─── Render ──────────────────────────────────────────────────────────────────

function render() {
  renderEndpointsPanel();
  renderInputsPanel();
  renderDeepStatus();
  renderDataPanel();
  renderSecurityPanel();
  renderSendPanel();
  renderFrameSelector();
  renderResponsePanel();
  renderDiscoveryPanel();
  populateTabFilter();
}

// THE PAGE'S OWN UNCAUGHT ERRORS, which the engine calls `pageErrors` and this view reaches as
// `resolverErrors`. The relay is engine result.c `pageErrors` -> bridge.js linesToAnalysis (which renames it,
// and whose comment records that reading the OTHER name here dropped every one of them) -> offscreen-brain
// `tab._resolverErrors` -> serialize.js. A script that throws names a capability the engine has not built, so
// these are P1 and MUST be visible rather than console-only.
//
// EVERY FIELD ON A ROW IS WRITTEN ON EVERY ROW, so none of them is defaulted here. bridge.js builds each entry
// as {context, message} — TWO non-empty strings and nothing else — and offscreen-brain copies both across
// verbatim. (This sentence used to name `snippet` and `replyExample` beside them; bridge.js wrote those as
// constant nulls, they were read nowhere, and they are gone from the record. A comment that keeps naming a
// field the producer stopped writing is the same lie as a reader that keeps reading it.) The `|| "gap"` that stood on `context` was the worse half: "gap" is a label no
// producer has ever emitted, so a relay that stopped carrying the context would have printed a plausible
// English word in its place on every row — a fabricated attribution, indistinguishable from a real one.
function renderDeepStatus() {
  const el = document.getElementById("deep-status");
  if (!el) return;
  // A null tabData is "GET_STATE has not answered yet, or Clear just emptied the view" — a real state, and a
  // different statement from a tabData whose serializer dropped the array.
  if (!tabData) { el.style.display = "none"; return; }
  DCHECK(Array.isArray(tabData.resolverErrors),
         "GET_STATE answered without a resolverErrors array — serializeTabData writes one on every path " +
         "(lib/serialize.js), so its absence is that serializer broken and every error the engine recorded " +
         "while running this page would vanish from the only surface that shows them");
  const rerrs = tabData.resolverErrors;
  const sections = [];
  const deepRow = (children, attrs = {}) =>
    domElement("div", Object.assign({ class: "deep-row" }, attrs), [domElement("span", { class: "deep-label" }, children)]);
  /* THE RUN THIS PAGE'S FINDINGS CAME OUT OF, SAID FIRST, because it decides how to read everything else on
     the screen. A crashed engine's findings are REAL — it recorded them before it died and the moat has them
     — but the run is not a complete analysis of the page, and nothing must be able to read the panels below
     as one. The three states are three different facts and none of them is defaulted: `null` is "no run has
     returned for this document yet" (the popup opened over a page still being explored, or over one the
     engine never ran), which is not the same as a run that returned having found nothing. */
  DCHECK(tabData.analysisRun === null || typeof tabData.analysisRun === "string",
         "GET_STATE answered without an analysisRun field — serializeTabData writes it on every path " +
         "(lib/serialize.js), so its absence is that serializer broken and a page whose engine crashed would " +
         "render exactly like one that was analysed and found clean");
  /* SAID BEFORE THE RUN, BECAUSE A RUN THAT NEVER STARTED IS NOT A RUN THAT FOUND NOTHING. `analysisRun`
     answers what the engine did; this answers whether the engine was ever given anything to do. The reader's
     question on opening the popup over an empty panel is "did this fail, or is there nothing here", and until
     this row existed the popup had no way to distinguish them — the whole of reddit.com rendered as a clean
     page with zero endpoints. `undefined` is refused rather than skipped: serializeTabData writes the field on
     every path, so its absence is that relay broken, which is exactly when a page would go back to reading
     clean. */
  DCHECK(tabData.pageSource === null || (tabData.pageSource && typeof tabData.pageSource.state === "string"),
         "GET_STATE answered without a pageSource field — serializeTabData writes it on every path " +
         "(lib/serialize.js), so its absence is that serializer broken and a page whose bundle could never be " +
         "fetched would render exactly like one that was analysed and found clean");
  if (tabData.pageSource && tabData.pageSource.state === "unavailable") {
    const ps = tabData.pageSource;
    DCHECK(ps.kind === "status" || ps.kind === "empty" || ps.kind === "network",
           "an unavailable pageSource reached the popup with kind `" + ps.kind + "` — serialize.js admits " +
           "exactly three and asserts them, so a fourth here is that seam broken and this row would state " +
           "that the page could not be analysed without saying why, which is the silence it replaces");
    /* `detail` IS THE CHOKEPOINT'S OWN REFUSAL REASON and it is shown verbatim, not summarised. It is the one
       string that distinguishes a server that would not answer (`Failed to fetch`) from a request this tool
       REFUSED to make (`blocked-scheme:about:`, `blocked-local-network:local-from-public`), and the two need opposite
       things from the person reading this row. It was a content script's own exception text before the load
       moved to the chokepoint; it is a named rule now. */
    const reason = ps.kind === "status" ? `the server answered ${ps.status} to it`
                 : ps.kind === "empty"  ? "the server answered with an empty body"
                 : `the load did not happen: ${String(ps.detail).slice(0, 160)}`;
    sections.push(deepRow([domElement("strong", {}, ["This page was NOT analysed."]),
      ` The analyser loaded this page's address itself — the only way to get the bundle as SHIPPED, with `
      + `its real response headers, rather than this tab's already-executed DOM — and ${reason}. Nothing `
      + `below was learned by running this page's code. A single-use URL (a bot challenge, a one-time `
      + `token) cannot be fetched twice, and neither can a document that was reached by anything other `
      + `than a plain GET.`]));
  }
  if (tabData.analysisRun === "crashed") {
    sections.push(deepRow([domElement("strong", {}, ["The engine crashed while analysing this page."]),
      ` What is shown below is what it had already learned when it died — a PARTIAL `
      + `result, not a clean bill. See the engine run record on the Diagnostics page for the abort.`]));
  }
  if (rerrs.length) {
    const rows = rerrs.map((r) => {
      DCHECK(typeof r.context === "string" && typeof r.message === "string",
             "a page-error row reached the popup without a context/message pair — bridge.js writes both as " +
             "strings on every entry it builds, so a row missing one is that relay broken");
      return deepRow([r.context + ": " + r.message.slice(0, 140)], { title: r.message });
    });
    /* APPENDED, NOT ASSIGNED. `html =` here silently dropped whatever section had been built above it — which
       was nothing while this was the first section, and was the crashed-run notice the moment one existed.
       A section that overwrites the panel instead of adding to it is a defect waiting for the next section. */
    sections.push(domElement("details", { class: "deep-cross-tab" }, [
      domElement("summary", {}, [`Analyzer gaps — reached-but-unresolved / host-model (${rerrs.length})`]), rows,
    ]));
  }
  if (sections.flat().length === 0) { el.style.display = "none"; return; }
  domReplaceChildren(el, sections);
  el.style.display = "block";
}

// ─── Data Panel ──────────────────────────────────────────────────────────────

// A LIST OF URLs AS LINKS, one spelling: each a link when it is http(s), its short form with the full address
// as the title either way, comma-separated.
function _urlListNodes(list) {
  return list.map((u, i) => [i === 0 ? "" : ", ", domLink(u, { title: u }, [_shortUrl(u)])]);
}

function renderDataPanel() {
  const keysContainer = document.getElementById("data-keys");
  const empty = document.getElementById("data-empty");

  // A null tabData is "GET_STATE has not answered yet, or Clear just emptied the view" — a real state this
  // panel renders as its empty text, and a different statement from a snapshot whose serializer dropped a
  // field. Under a real snapshot every field read below is one serializeTabData writes on EVERY path
  // (GET_STATE falls back to _emptyDocView, which carries them too), so each is asserted, never defaulted.
  const keys = tabData?.apiKeys ? Object.entries(tabData.apiKeys) : [];
  let scopeRows = [];
  let auth = null;
  if (tabData) {
    DCHECK(tabData.apiKeys && typeof tabData.apiKeys === "object",
           "GET_STATE answered without an apiKeys record — serializeTabData builds one on every path " +
           "(lib/serialize.js), so its absence is that serializer broken and every key this extension has " +
           "learned would render as a page that leaked none");
    /* THE REQUIRED OAUTH SCOPES, WHICH REACHED NO READER AT ALL. lib/response-decode.js pulls them out of a
       403's `WWW-Authenticate`, lib/discovery-probe.js and lib/popup-handlers.js out of the gapi error
       envelope, mergeToGlobal carries them into the cumulative store, lib/persistence.js saves them across
       sessions and serializeTabData puts them on this snapshot — and grep found no consumer of
       `tabData.scopes` anywhere. That is the exact mirror of a reader with no writer and it hides the same
       way: every hop of the path is live, so the path looks live. What a scope states is what a replay of
       this service NEEDS, which is one of the things §What-the-tool-produces exists to report. */
    DCHECK(tabData.scopes && typeof tabData.scopes === "object",
           "GET_STATE answered without a scopes record — serializeTabData builds one on every path from " +
           "globalStore.scopes overlaid with the document's, so its absence is that serializer broken and the " +
           "scope list a 403 named would be dropped");
    /* WHAT EACH ENTRY IS, IS NOT THIS READER'S OPINION ANY MORE. A loop here asserted "a non-empty array of
       scope strings" and named the three writers it believed in — a correct rule, standing in the wrong
       place: one caller's transcription of a contract lib/store-record.js's `_SR_SCOPES` states for every
       surface, including the IndexedDB door a session later. It is now asked ONCE, in lib/serialize.js, as
       this map is projected onto the reply below, so what arrives here has already answered it and a second
       spelling of the question cannot drift from the first. `_list.length` in the fingerprint is therefore a
       count of a list, not a guess about one. */
    scopeRows = Object.entries(tabData.scopes).sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0));
    /* THE CREDENTIAL CONTEXT, in the same state. lib/response-decode.js writes it ONLY when a request from
       this document carried an Authorization header or a cookie, so `null` is the POSITIVE statement that
       none did — and that is the first thing a reader of a learned surface needs, because §What-the-tool-
       produces is about learning the LOGGED-IN surface while logged out. It was serialized to nobody. */
    auth = tabData.authContext;
    DCHECK(auth === null || (auth && typeof auth === "object" && !Array.isArray(auth)),
           "a document's authContext is neither null nor a record — lib/response-decode.js writes " +
           "{hasAuthorization?, hasCookies?, origin?} and offscreen-brain.js initialises it to null, so a " +
           "third shape is one of those two producers having changed under this view");
  }
  /* THE FINGERPRINT COVERS EVERYTHING THIS PANEL RENDERS, or the cache in front of it makes a rendered fact
     invisible again. It keyed on the KEYS alone, so a service whose scopes arrived after the last key — or a
     document that started sending cookies — short-circuited here and never appeared. */
  const fp = keys.length + ":" + keys.map(k => k[0]).join(",")
           + "|" + scopeRows.map(([s, l]) => s + "=" + l.length).join(",")
           + "|" + (auth ? [!!auth.hasAuthorization, !!auth.hasCookies, auth.origin || ""].join(",") : "-");
  if (fp === _lastKeysFp) return;
  _lastKeysFp = fp;

  const hasData = keys.length > 0 || scopeRows.length > 0 || auth !== null;
  empty.style.display = hasData ? "none" : "block";

  const out = [];
  const meta = (children) => domElement("div", { class: "card-meta" }, children);
  const joined = (list, sep, tag) => list.map((v, i) => [i === 0 ? "" : sep, domElement(tag, {}, [String(v)])]);

  // Keys section
  if (keys.length) {
    out.push(domElement("div", { class: "section-header" }, ["Discovered API Keys"]));
    for (const [key, info] of keys) {
      /* THE WHOLE RECORD, ASSERTED, BECAUSE ONE PROJECTION WRITES ALL OF IT. serializeApiKeyEntry
         (lib/serialize.js) DCHECKs that the four collections are Sets and that requestCount is a number, then
         writes each of them unconditionally — so `info.services || []`, `|| []`, `|| []` and `|| []` here
         could only ever fire for a record that projection did not write, and what they produced was four
         empty lists that read as "this key was never used against any service, host, endpoint or page". */
      for (const _f of ["services", "hosts", "endpoints", "pageUrls"])
        DCHECK(Array.isArray(info[_f]),
               "an API key reached the popup with a `" + _f + "` that is not an array — " +
               "serializeApiKeyEntry spreads the store's Set into one on every entry it writes, so anything " +
               "else is that projection broken and this card would report the key as never used");
      const services = info.services, hosts = info.hosts, eps = info.endpoints, pageUrls = info.pageUrls;

      /* THE KEY'S TYPE, UNDEFAULTED. `info.name || "API Key"` stood in the label and it CONCEALED a real
         defect for as long as it shipped: four components copied an API-key record field-by-field and two of
         them dropped `name`, so every key that crossed a merge or a save/load lost the type lib/keys.js had
         matched ("GitHub Token", "JWT", "Stripe Key") — and the loss rendered as a plausible generic label on
         exactly the keys that matter most, the ones carried across tabs and sessions. One projection writes it
         now (lib/serialize.js serializeApiKeyEntry, called by both writers), so the absence of a name is that
         projection broken and must crash here rather than print a word no producer emitted. */
      DCHECK(typeof info.name === "string" && info.name !== "",
             "an API key reached the popup with no type name — lib/keys.js stamps the matched pattern's name " +
             "on every key it records and serializeApiKeyEntry is the one projection that carries it, so a " +
             "missing one is a copier dropping the field again (or a record stored before it was carried)");
      /* WHERE THE KEY WAS FOUND IS THE PRODUCER'S OWN WORD, and this badge tested it against a name no
         producer has ever written. `info.source === "page_source" ? "page source" : "network"` — nothing in
         this extension passes "page_source" to extractKeysFromText: every one of its twelve call sites passes
         `url`, `header:<name>`, `response_body`, `response_grpc`, `response_protobuf`, `protobuf_body`,
         `send_response_grpc` or `send_response_protobuf` (plus a " > b64" suffix when the key came out of a
         nested base64 blob). So the "page source" arm has never rendered once, every key has been badged
         "network", and the specific provenance the producer computed was thrown away by the consumer. It is
         the `if (kind === N)` on a value the producer never returns, and the fix is to show the value:
         lib/popup-handlers.js already reads this exact vocabulary (`=== "url"`, `startsWith("header:")`) to
         decide where to inject the key, so it is a live contract and not free text. */
      DCHECK(typeof info.source === "string" && info.source !== "",
             "an API key reached the popup with no source context — extractKeysFromText takes it as a required " +
             "argument and lib/popup-handlers.js switches on it to decide the key's injection point, so a " +
             "missing one is that producer broken (key type=" + info.name + ")");
      /* THE BADGE'S UNIT IS A SIGHTING, NOT A REQUEST. What stood here said "grep finds NO writer that ever
         increments an API-key entry's requestCount" and "the badge has never once shown a request count" —
         a claim about THIS TREE that this tree stopped making true: lib/keys.js runs `keyData.requestCount++`
         on every match it records. It is the stale-DFAIL shape with no crash to retire it, accurate about the
         defect it described and wrong about the code, and it read as authoritative enough to send the next
         reader to build an incrementer that is already there.
         What the surviving producer counts is one per PATTERN MATCH per scanned string, and
         extractKeysFromText is called on the request URL, on EACH request header, on response bodies, on
         WebSocket frames, on postMessage bodies, on decoded protobuf/gRPC field values, and again on every
         nested base64 level of any of those — so a key echoed three times in one reply counts three, and a key
         that only ever arrived in a received postMessage counts although no request ever carried it. Across
         documents mergeToGlobal takes the MAX of two running totals rather than a sum, so a cumulative entry
         reads as the most any ONE document's scan saw. "N req" was therefore a wrong unit over a real number,
         which is the same lie as a fabricated one and harder to catch; the badge states the unit it has, and
         the endpoints the key was seen against keep their own line below.
         The FIELD is still spelled `requestCount` where it is produced (lib/keys.js), merged (lib/merge.js)
         and projected (lib/serialize.js) — the same wrong unit, one rename across those three, not this
         consumer's to make. */
      DCHECK(Number.isInteger(info.requestCount) && info.requestCount >= 0,
             "an API key reached the popup with a sighting count that is not a whole non-negative number — " +
             "lib/keys.js initialises it to 0 and only ever `++`s it, mergeToGlobal takes the max of two such " +
             "numbers and serializeApiKeyEntry asserts and writes it on every entry, so anything else is one " +
             "of those producers having changed under this badge (key type=" + info.name + ")");
      const card = domElement("div", { class: "card" }, [
        domElement("div", { class: "card-label" }, [
          info.name + " ",
          domElement("span", { class: "badge badge-source",
                               title: "where this key was found — the request/response context it was matched in" },
                     [info.source]),
          " ",
          domElement("span", { class: "badge badge-status",
                               title: "how many times this key's pattern matched in text this extension scanned — " +
                                      "one per match per scanned string (the request URL, each request header, " +
                                      "each response, WebSocket and postMessage body, each decoded protobuf " +
                                      "field, and every nested base64 level of those). It is not a count of " +
                                      "requests, and across documents it is the most any one document's scan saw." },
                     [String(info.requestCount) + "\u00d7 seen"]),
        ]),
        domElement("div", { class: "card-value" }, [key]),
      ]);
      out.push(card);

      if (hosts.length) {
        card.appendChild(meta([(hosts.length === 1 ? "Host" : "Hosts") + ": ", joined(hosts, ", ", "strong")]));
      }
      if (services.length) {
        card.appendChild(meta([(services.length === 1 ? "Service" : "Services") + ": ", joined(services, " ", "code")]));
      }
      /* THE ENDPOINTS THIS KEY WAS SEEN AGAINST — `<host><path>` per lib/keys.js, and until now the set was
         read only for its `.length`, laundered into the "req" badge above. It is the one measured usage fact
         on the record, so it is stated as itself. */
      if (eps.length) {
        card.appendChild(meta([(eps.length === 1 ? "Endpoint" : "Endpoints") + ": ", joined(eps, " ", "code")]));
      }
      if (pageUrls.length) {
        card.appendChild(meta([(pageUrls.length === 1 ? "Page" : "Pages") + ": ", _urlListNodes(pageUrls)]));
      }
      /* WHEN AND WHERE, the last fields of the record. lib/keys.js stamps `firstSeen`/`lastSeen` on every
         entry and `referer` — the URL the key was matched in — from the text it was scanning; lib/merge.js's
         own comment records that `lastSeen` "was written here and read NOWHERE", and that was true of all of
         them. THE RECORD NO LONGER CARRIES AN `origin`: it was that same URL's origin, minted beside `referer`
         from the same value, carried across the merge and into IndexedDB, and never read on either side —
         and this comment used to say the one line below was "not because `origin` is unread", which was a
         claim about the RENDERING that read as a claim about the FIELD. The address below is the whole of
         what the record states about where; a reader who wants the origin has it as a prefix of that address.
         The two timestamps are asserted; `referer` is guarded because keys.js writes null when the scanned
         text had no source URL, and that null is a positive statement (a body with no address of its own). */
      DCHECK(typeof info.firstSeen === "number" && typeof info.lastSeen === "number",
             "an API key reached the popup without numeric firstSeen/lastSeen — lib/keys.js stamps both when " +
             "it mints the entry and refreshes lastSeen on every later sighting, so a missing one is a " +
             "copier dropping the field");
      card.appendChild(meta([
        `first seen ${new Date(info.firstSeen).toLocaleString()} \u00b7 last seen ${new Date(info.lastSeen).toLocaleString()}`,
        info.referer ? [" \u00b7 matched in ", _urlListNodes([info.referer])] : [],
      ]));
    }
  }

  /* WHAT THE OBSERVED TRAFFIC CARRIED, AND WHAT A REPLAY WOULD NEED. Two facts serializeTabData has put on
     this snapshot from the beginning and no view has ever read. Rendered together because they answer one
     question — is the surface below the anonymous one, and what would it take to reach the other. */
  if (hasData) {
    out.push(domElement("div", { class: "section-header" }, ["Credentials and scopes observed"]));
    const compact = (title, children) => domElement("div", { class: "card card-compact" },
                                                    [domElement("div", { class: "card-label" }, title), meta(children)]);
    if (auth) {
      const carried = [];
      if (auth.hasAuthorization) carried.push(["an ", domElement("code", {}, ["Authorization"]), " header"]);
      if (auth.hasCookies) carried.push("cookies");
      // response-decode.js writes the record ONLY inside `if (authorization || cookie)`, so at least one of
      // the two is true whenever it exists. An empty list here is that producer changed shape, and reporting
      // a credentialed document as anonymous is the wrong half of the tool's headline claim.
      DCHECK(carried.length,
             "a document's authContext claims neither an Authorization header nor cookies — " +
             "lib/response-decode.js writes the record only when one of the two was observed, so an empty " +
             "one is that producer broken and this line would report a credentialed document as anonymous");
      out.push(compact(["Credentialed traffic observed"], [
        "requests from this document carried ", carried.map((c, i) => [i === 0 ? "" : " and ", c]),
        auth.origin ? [" \u00b7 ", domElement("code", {}, ["Origin: " + auth.origin])] : [],
        " — so what this extension learned here includes what those credentials could reach",
      ]));
    } else {
      out.push(compact(["No credentialed traffic observed"], [
        "no request from this document carried an ", domElement("code", {}, ["Authorization"]), " header or a " +
        "cookie, so everything learned here is the ANONYMOUS surface — which is the point: the bundle ships the " +
        "authenticated code path to a logged-out visitor too",
      ]));
    }
    for (const [svc, list] of scopeRows) {
      out.push(compact(["Required OAuth scopes \u2014 ", domElement("code", {}, [svc])], [joined(list, " ", "code")]));
    }
  }

  domReplaceChildren(keysContainer, out);
}



// ─── Send Panel: Endpoint Selection + Schema ─────────────────────────────────

function onSendEndpointSelected() {
  const select = document.getElementById("send-ep-select");
  const epKey = select.value;
  forgetReplayedRequest();

  // Clear previous send result
  const respEl = document.getElementById("send-response");
  if (respEl) respEl.style.display = "none";

  // Handle virtual discovery endpoint
  const selectedOpt = select.options[select.selectedIndex];
  if (selectedOpt?.dataset?.isVirtual === "true") {
    const svc = selectedOpt.dataset.svc;
    const pathTemplate = selectedOpt.dataset.path;
    const validMethod = selectedOpt.dataset.method;
    const discoveryId = selectedOpt.dataset.discoveryId; // Use data-discoveryId
    /* THE SERVICE THE OPTION WAS BUILT FROM, UNDEFAULTED. `tabData?.discoveryDocs?.[svc]` stood here and
       could not fire: lib/popup-send.js's renderMethodDropdown mints this option only while iterating
       `tabData.discoveryDocs`, and only for an entry whose `.doc` is truthy (the published fetch's `status`
       is a different fact and no longer gates it) — so reaching this line means both were there when the
       dropdown was painted. lib/serialize.js
       writes `discoveryDocs` unconditionally beside endpoints/probeResults/requestLog, so an absent one is
       that projection broken, and what the optional chain produced then was `doc === undefined` feeding the
       address ladder below — a send whose target was invented rather than read. */
    DCHECK(tabData && tabData.discoveryDocs && typeof tabData.discoveryDocs === "object",
           "the send panel is offering a discovery method while tabData carries no discoveryDocs — " +
           "serializeTabData (lib/serialize.js) writes that map on every GET_STATE it answers, and the " +
           "option being offered at all means renderMethodDropdown just read it");
    const svcData = tabData.discoveryDocs[svc];
    DCHECK(svcData && svcData.doc,
           "the send panel is offering a method of service \"" + svc + "\" whose discovery entry is gone or " +
           "carries no doc — renderMethodDropdown builds a virtual option only from an entry that had one, " +
           "so this is the store dropping a document between render and selection");
    const doc = svcData.doc;

    /* WHERE THE SERVICE LIVES, PER THE DOCUMENT'S OWN VOCABULARY. Google API Discovery's `RestDescription`
       (discovery:v1, fetched from the live meta-document) describes `baseUrl` as "[DEPRECATED] The base URL
       for REST requests", `rootUrl` as "The root URL under which all API services live" and `servicePath` as
       "The base path for all REST requests" — and on drive/v3, calendar/v3 and sheets/v4 alike
       `rootUrl + servicePath === baseUrl` exactly. So the live spelling is the JOIN, and the deprecated field
       is the shortcut.
       `let baseUrl = doc.baseUrl || doc.rootUrl;` stood on the line above the join and made the join DEAD:
       falling past it required `doc.rootUrl` to be falsy, which is the one condition under which the join
       cannot run either. A document that had dropped the deprecated field — which is what a service does when
       it follows the deprecation — therefore addressed its API at the bare rootUrl with the servicePath
       SILENTLY DISCARDED: `https://www.googleapis.com/` for a method of drive/v3, a URL that is well-formed,
       plausible, and not the endpoint. */
    DCHECK(doc.servicePath === undefined || typeof doc.servicePath === "string",
           "service \"" + svc + "\" published a non-string servicePath — RestDescription types it as a " +
           "string and it is joined onto rootUrl here, so anything else corrupts every method's address");
    // An ABSENT servicePath is a positive statement — this service lives directly under its root (sheets/v4
    // publishes it as ""), not a hole to fill.
    const baseUrl = doc.baseUrl
      ? doc.baseUrl
      : (doc.rootUrl ? doc.rootUrl + (doc.servicePath === undefined ? "" : doc.servicePath) : null);

    /* NO ADDRESS IS A STATEMENT ABOUT THE SERVICE, NEVER AN EMPTY STRING. `baseUrl = ""` stood here behind a
       console.warn, and `"" + pathTemplate` is a PATH-RELATIVE URL: resolved against this popup, every such
       send went to the extension's own chrome-extension:// origin and reported back as though the service had
       answered. Both address fields are the published document's, so their absence is not a broken producer
       of ours to DCHECK — it is a service that did not say where it lives, and the panel says so and offers
       nothing to send. */
    if (baseUrl === null) {
      setRequestTemplate("");
      currentRequestMethod = validMethod;
      currentSchema = null;
      setSendPanelVisible(false);
      document.getElementById("send-form-fields").style.display = "block";
      _sendFormHint(svc + " published no rootUrl or baseUrl, so its methods have no address to send to.");
      renderChainInfo(null);
      return;
    }

    // Fix double slashes just in case
    let base = baseUrl;
    if (base.endsWith("/") && pathTemplate.startsWith("/")) {
      base = base.slice(0, -1);
    }

    setRequestTemplate(base + pathTemplate);
    currentRequestMethod = validMethod;

    select.dataset.svc = svc;
    select.dataset.discoveryId = discoveryId;

    // Load schema via background
    loadVirtualSchema(svc, discoveryId);
    return;
  }

  // Fallback if no matching endpoint found — hide everything below the dropdowns
  setRequestTemplate("");
  currentRequestMethod = "POST";
  currentSchema = null;
  // Exit console mode if active
  if (currentBodyMode === "msgconsole") {
    currentChannelId = null;
    currentChannelType = null;
    currentTargetOrigin = null;
    currentChannelFrameId = null;
    currentChannelDocumentId = null;
    document.getElementById("send-ws-console").style.display = "none";
  }
  currentBodyMode = "form";
  setSendPanelVisible(false);
  document.getElementById("send-frame-row").classList.add("hidden");
  document.getElementById("send-form-fields").style.display = "block";
  _sendFormHint("Select a method to load its schema.");
  renderChainInfo(null);
}

async function loadVirtualSchema(service, methodId, initialData = null) {
  currentSchema = null;
  _sendFormHint("Loading schema...");

  try {
    const schema = await chrome.runtime.sendMessage({
      type: "GET_ENDPOINT_SCHEMA",
      service,
      methodId,
    });

    if (!schema || !schema.method) {
      _sendFormHint("Method definition not found.");
      return;
    }

    currentSchema = schema;

    /* THE CONTENT-TYPE HAS ONE PRODUCER AND IT NEVER ANSWERS NOTHING. resolveEndpointSchema
       (lib/send.js) collects the discovery method's observed types, adds each probe's, and when
       neither named one substitutes application/json + the two protobuf types — so a schema that
       reached this line (it carries a `method`, checked above) always carries a non-empty
       contentTypes. The two branches that stood here guessed past that producer anyway.
       `schema.endpoint.contentType` read a name the endpoint projection DELIBERATELY stopped
       carrying: send.js's own comment lists contentType among five fields it dropped because
       lib/merge.js — the extension's only `endpoints.set` — writes exactly the names
       lib/endpoint-record.js declares, and a content type is not among them. (The names are not
       transcribed here: that file's own header records that this list had been hand-copied into
       seven comments, and a list copied is a list that disagrees with the producer the first time
       the producer moves. `makeEndpointRecord` refuses any name it does not carry, so the question
       "does an endpoint have this field" is answered by reading it and never by reading this.)
       So that branch had been reading `undefined` off a six-field allowlist, and the
       "application/json" tail behind it restated send.js's own first substitute where nothing
       could reach it. A reader whose producer was deleted is dead code that reads as a feature;
       what belongs here is the assertion, not the guess. */
    DCHECK(Array.isArray(schema.contentTypes) && schema.contentTypes.length > 0,
           "GET_ENDPOINT_SCHEMA answered a method with no contentTypes — resolveEndpointSchema " +
           "(lib/send.js) substitutes three media types when neither the discovery method nor a " +
           "probe observed one, so an empty list is that producer broken and the Send panel would " +
           "post this body under a Content-Type nothing learned");
    currentContentType = schema.contentTypes[0];

    // Auto-set body mode: GraphQL if URL matches, otherwise form
    if (isGraphQLUrl(currentRequestUrl)) {
      setBodyMode("graphql");
      if (gqlState.ops.length === 0 || !gqlState.ops[0].query) gqlClear();
    } else {
      setBodyMode("form");
    }

    buildFormFields(schema, initialData);
    renderChainInfo(schema.chains);
    prefillKeyPositions();
    renderServiceOriginHint();
    /* AFTER buildFormFields AND NOT BEFORE. `setBodyMode` above already rendered the row, and it ran while
       the path and query inputs did not exist yet — so that render showed the template with its `{holes}`
       unfilled. This is the only site that builds those inputs, so it is the only site whose defaults the
       row would otherwise never see. */
    renderSendUrl();
  } catch (err) {
    // An invariant abort travels ON through here (check.js RETHROW_FATAL). The GET_ENDPOINT_SCHEMA
    // sendMessage genuinely can reject — the offscreen document is not answering — and that IS the
    // "Error loading schema" state this hint is for; the assertion above is not, and without this
    // line it would render in the same hint and read as a transport hiccup.
    RETHROW_FATAL(err);
    console.error("Error loading virtual schema:", err);
    _sendFormHint("Error loading schema: " + String(err.message));
  }
}

// Replaces the Send form with one hint line.
function _sendFormHint(text) {
  domReplaceChildren(document.getElementById("send-form-fields"), [domElement("div", { class: "hint" }, [text])]);
}

function renderChainInfo(chains) {
  const container = document.getElementById("send-chain-info");
  if (!container) return;
  if (!chains || (!chains.incoming?.length && !chains.outgoing?.length)) {
    container.classList.add("hidden");
    container.replaceChildren();
    return;
  }
  container.classList.remove("hidden");
  const span = (cls, text) => domElement("span", { class: cls }, [text]);
  const count = (link) => (link.observedCount > 1 ? span("chain-count", String(link.observedCount) + "x") : []);
  const box = domElement("div", { class: "chain-info-box" });
  if (chains.incoming?.length) {
    box.appendChild(domElement("div", { class: "chain-section" }, [
      span("chain-section-label", "Receives data from:"),
      chains.incoming.map((link) => domElement("div", { class: "chain-link chain-incoming" }, [
        span("chain-param", String(link.paramName)), span("chain-arrow", "\u2190"),
        span("chain-source", String(link.sourceMethodId)), span("chain-field", "." + String(link.sourceFieldPath)),
        count(link),
      ])),
    ]));
  }
  if (chains.outgoing?.length) {
    box.appendChild(domElement("div", { class: "chain-section" }, [
      span("chain-section-label", "Feeds data to:"),
      chains.outgoing.map((link) => domElement("div", { class: "chain-link chain-outgoing" }, [
        span("chain-field", String(link.sourceFieldPath)), span("chain-arrow", "\u2192"),
        span("chain-source", String(link.targetMethodId)), span("chain-param", "." + String(link.paramName)),
        count(link),
      ])),
    ]));
  }
  domReplaceChildren(container, [box]);
}

function pbTreeToMap(rootNodes) {
  // Iterative tree-to-map: each work item populates an object slot
  // (`target[key] = ...`) from a `nodes` list. Nested messages and JSPB
  // sub-arrays pre-allocate empty objects/arrays whose contents are
  // queued for population. Replaces self-recursion so deeply-nested
  // protobuf trees (or adversarial JSPB arrays) convert without growing
  // the JS call stack.
  if (!rootNodes) return null;
  const root = {};
  const queue = [{ kind: "nodes", nodes: rootNodes, target: root }];
  while (queue.length > 0) {
    const job = queue.shift();
    if (job.kind === "nodes") {
      for (const node of job.nodes) {
        if (node.message) {
          const sub = {};
          job.target[node.field] = sub;
          queue.push({ kind: "nodes", nodes: node.message, target: sub });
        } else if (node.isJspb && Array.isArray(node.value)) {
          const list = new Array(node.value.length);
          job.target[node.field] = list;
          for (let i = 0; i < node.value.length; i++) {
            const item = node.value[i];
            if (Array.isArray(item)) {
              const subMap = {};
              list[i] = subMap;
              queue.push({ kind: "nodes", nodes: jspbToTree(item), target: subMap });
            } else {
              list[i] = item;
            }
          }
        } else {
          job.target[node.field] = node.value ?? node.string ?? node.hex ?? node.asFloat;
        }
      }
    }
  }
  return root;
}






function getStatusBadge(status) {
  if (status === "pending")
    return domElement("span", { class: "badge badge-pending" }, ["pending"]);
  if (status === "error") return domElement("span", { class: "badge badge-error" }, ["error"]);
  const code = parseInt(status);
  if (code >= 200 && code < 300)
    return domElement("span", { class: "badge badge-found" }, [String(code)]);
  if (code >= 400) return domElement("span", { class: "badge badge-notfound" }, [String(code)]);
  return domElement("span", { class: "badge" }, [String(code)]);
}

/* The request-log record the Send panel was loaded from by click-to-replay, or null. Send reads its target
   document and frame and its mode, referrer and referrer policy, so it belongs to the endpoint replayRequest
   selected and to nothing the operator picks after it. */
let currentReplayRequest = null;

/* The operator chose an endpoint of their own: the panel no longer shows the replayed record, so neither its
   facts nor the credentials mode loadSendRequestInit took from it stay, and Send targets the pinned frame. */
function forgetReplayedRequest() {
  currentReplayRequest = null;
  document.getElementById("send-credentials").value = "";
  _refreshSendEnabled();
}

/* THE REPLAY LOOKUP'S KEY IS ASSERTED, NOT CONVERTED — on BOTH sides of the comparison.
   `String(r.id) === String(reqId)` stood here, and the two conversions were the whole defect. Neither
   operand is ever a non-string while the contract holds: offscreen-brain.js's `_pushGlobalLog` asserts
   `entry.id` as a non-empty string before an entry reaches the global log, and lib/popup-reqlog.js renders
   that id into a `data-id` attribute, which reads back as text. So the `String()` pair did no work at all
   on any input the producers can state — EXCEPT the one input it must not survive. ECMAScript §7.1.19
   ToString ( argument ) answers the String value `"undefined"` for `undefined`, so an entry filed without
   an id and a card rendered without one convert to the SAME token and COMPARE EQUAL, and `.find` returns
   the FIRST such entry: the Send panel is then loaded with a request the operator did not click, wearing
   the identity of one they did. That is a wrong answer with the same shape as a right one, which is why
   CLAUDE.md rates the conversion as concealment rather than as a defaulted read — there is no `||` in the
   line for a grep to find, and the absence is not passed through, it is MINTED into a matching value.
   Both values are this codebase's OWN: the engine states neither, and no server states either, so the
   answer to a missing one is a should-never-happen and not a refusal. */
function _replayIdMatches(r, reqId) {
  DCHECK(typeof r.id === "string" && r.id !== "",
         "a request-log entry reached the popup's click-to-replay lookup with no id — " +
         "offscreen-brain.js's _pushGlobalLog DCHECKs id/url/method/service/timestamp/status on every " +
         "entry it files, so an entry without one has reached the global log around that mint, and this " +
         "search would match it for ANY card whose own id is equally absent rather than for the card " +
         "the operator clicked");
  return r.id === reqId;
}

async function replayRequest(reqId, sourceTabId) {
  /* The popup's own bytes, round-tripped through its own DOM: lib/popup-reqlog.js stamps `data-id` on every
     `.request-card` it renders and attaches the click handler to those cards alone, so a card arriving here
     without one is this document disagreeing with itself. Asserted at the ENTRY rather than at the two
     reads below, because that is where the value is born for this function and the two searches share it. */
  DCHECK(typeof reqId === "string" && reqId !== "",
         "click-to-replay was invoked with no request id — lib/popup-reqlog.js writes data-id on every " +
         "request card out of the entry's own id and hangs the handler on those cards, so an absent one is " +
         "a card this popup rendered without the identity its own click handler reads back");

  // Clear previous send result
  lastSendResult = null;
  const sendResp = document.getElementById("send-response");
  if (sendResp) sendResp.style.display = "none";

  // Search the correct log source
  let req;
  if (sourceTabId && allTabsData && allTabsData[sourceTabId]) {
    req = allTabsData[sourceTabId].requestLog.find((r) => _replayIdMatches(r, reqId));
  }
  if (!req && allTabsData) {
    // The request log is GLOBAL — search every loaded tab's slice rather than
    // the per-document tabData.requestLog (always empty post tabId->documentId
    // migration). With the scope-fixed loadRequestLog, allTabsData is populated
    // for every scope including "active", so the entry is here.
    for (const data of Object.values(allTabsData)) {
      req = data.requestLog.find((r) => _replayIdMatches(r, reqId));
      if (req) break;
    }
  }
  if (!req) {
    console.error(`[Replay] Request ${reqId} not found in log`);
    return;
  }
  // Stamp the origin tab so downstream sendRequest routes through it.
  // requestLog entries inside allTabsData are NOT pre-annotated — only the
  // flattened view in renderResponsePanel adds _tabId. Without this stamp,
  // currentReplayRequest._tabId is undefined and Send falls back to the
  // popup's own active tab, silently sending cross-tab replays to the
  // wrong origin.
  if (sourceTabId != null && req._tabId == null) req._tabId = sourceTabId;
  currentReplayRequest = req;

  // Auto-select the frame this request came from (if available)
  if (req.frameId != null && availableFrames.length > 1) {
    var _frameMatch = availableFrames.find(function(f) { return f.frameId === req.frameId; });
    if (_frameMatch) {
      _repinTo(req.frameId);   // pin to the replay's frame — keeps picker + pin consistent
      var _frameSel = document.getElementById("send-frame-select");
      if (_frameSel) _frameSel.value = String(req.frameId);
    }
  }

  // Message console mode: WebSocket or postMessage
  if (req.kind !== "http") {
    // Ensure the captured channel's origin tab is propagated — the click
    // path reaches here with the sourceTabId argument, which is what
    // initMsgConsole needs to route all subsequent SW calls correctly.
    // Without this, the popup's own tab id would be used, and the SW
    // couldn't find the captured entry.
    if (sourceTabId != null && req._tabId == null) req._tabId = sourceTabId;
    await initMsgConsole(req);
    document.querySelector(".tab[data-panel='send']").click();
    return;
  }

  loadSendRequestInit(req);

  // Try to find and select the matching endpoint in the dropdown to load the schema
  const epSelect = document.getElementById("send-ep-select");
  let found = false;

  if (epSelect) {
    // Check if it's a batchexecute request
    const isBatch = req.url.includes("batchexecute");
    let targetRpcId = null;

    if (isBatch && req.rawBodyB64) {
      const bytes = base64ToUint8(req.rawBodyB64);
      const text = new TextDecoder().decode(bytes);
      const calls = parseBatchExecuteRequest(text);
      if (calls && calls.length > 0) {
        targetRpcId = calls[0].rpcId;
      }
    }

    // Set current state from the replayed request
    setRequestTemplate(req.url);
    currentRequestMethod = "POST"; // Default for replayed backend requests or detect from req
    if (req.method) currentRequestMethod = req.method;

    for (const opt of epSelect.options) {
      if (opt.dataset.isVirtual === "true" && opt.dataset.svc === req.service) {
        // 1. For batch, attempt strict RPC ID match first
        if (isBatch && targetRpcId) {
          if (
            opt.dataset.discoveryId &&
            opt.dataset.discoveryId.endsWith("." + targetRpcId)
          ) {
            opt.selected = true;
            found = true;
            break;
          }
          // If it's a batch request but this option isn't the right RPC ID,
          // skip further checks for this option to avoid incorrect path matching.
          continue;
        }

        // 2. Path-based matching (non-batch only), filtered by HTTP verb: one
        //    path can carry both GET and POST probe variants of an endpoint, and
        //    a path-only match would pick the first declared one.
        if (!isBatch) {
          try {
            const reqPath = new URL(req.url).pathname;
            if (opt.dataset.path && reqPath.endsWith(opt.dataset.path) &&
                opt.dataset.method === req.method) {
              opt.selected = true;
              found = true;
              break;
            }
          } catch (e) {
            RETHROW_FATAL(e);
            console.warn("[Replay] URL path resolution failed:", e);
          }
        }
      }
    }

    if (found) {
      const svc = req.service;
      const selectedOpt = epSelect.options[epSelect.selectedIndex];
      const discoveryId = selectedOpt.dataset.discoveryId;

      epSelect.dataset.svc = svc;
      epSelect.dataset.discoveryId = discoveryId;

      // Extract the correct initial data for the selected RPC call
      let initialData = null;
      if (isBatch && req.rawBodyB64) {
        const bytes = base64ToUint8(req.rawBodyB64);
        const text = new TextDecoder().decode(bytes);
        const calls = parseBatchExecuteRequest(text);
        if (calls && calls.length > 0) {
          initialData = jspbToTree(
            Array.isArray(calls[0].data) ? calls[0].data : [calls[0].data],
          );
          initialData = pbTreeToMap(initialData);
        }
      } else if (req.isJson && req.decodedBody) {
        // JSON body — use parsed object directly as named-key initialData
        initialData = req.decodedBody;
      } else if (req.decodedBody) {
        initialData = pbTreeToMap(req.decodedBody) || {};
      } else if (req.rawBodyB64) {
        // Try to extract f.req JSPB from form-urlencoded body
        try {
          const bodyBytes = base64ToUint8(req.rawBodyB64);
          const bodyText = new TextDecoder().decode(bodyBytes);
          const bodyParams = new URLSearchParams(bodyText);
          const fReq = bodyParams.get("f.req");
          if (fReq) {
            const parsed = JSON.parse(fReq);
            if (Array.isArray(parsed)) {
              initialData = pbTreeToMap(jspbToTree(parsed));
            }
          }
        } catch (e) {
          RETHROW_FATAL(e);
          // f.req JSPB extraction failed — expected for non-Google bodies.
          // Falls through to plain-JSON attempt below; debug-log so a real
          // serialization-shape bug is visible without spamming.
          console.debug("[popup:send_load] f.req JSPB parse failed:", e && e.message || e);
        }
        // Also try plain JSON body
        if (!initialData) {
          try {
            const bodyBytes = base64ToUint8(req.rawBodyB64);
            const bodyText = new TextDecoder().decode(bodyBytes);
            const json = JSON.parse(bodyText);
            if (json && typeof json === "object" && !Array.isArray(json)) {
              initialData = json;
            }
          } catch (e) {
            RETHROW_FATAL(e);
            // Plain JSON parse failed — body might be form-encoded or binary.
            // The fallthrough `initialData = {}` below is the correct empty
            // form state for "couldn't parse anything meaningful."
            console.debug("[popup:send_load] plain JSON parse failed:", e && e.message || e);
          }
        }
        if (!initialData) initialData = {};
      } else {
        initialData = {};
      }

      // Merge query parameters from URL for pre-filling
      try {
        const urlObj = new URL(req.url);
        urlObj.searchParams.forEach((val, key) => {
          // $httpHeaders is a gRPC-Web transport param with CRLF separators —
          // putting it through a text input strips \r\n and corrupts the URL.
          if (key === "$httpHeaders" || key === "$ct") return;
          // Prefer body data if it already provides this key, but for GET it's usually just URL params
          if (initialData[key] === undefined || initialData[key] === null) {
            initialData[key] = val;
          }
        });
      } catch (e) {
        RETHROW_FATAL(e);
        console.warn("[Replay] Failed to extract URL parameters:", e);
      }

      await loadVirtualSchema(svc, discoveryId, initialData);
    }
  }

  // Auto-determine Content-Type from the original request (AFTER schema load)
  if (req.requestHeaders) {
    const ctHeader = Object.keys(req.requestHeaders).find(
      (k) => k.toLowerCase() === "content-type",
    );
    if (ctHeader) {
      currentContentType = req.requestHeaders[ctHeader];
    } else {
      currentContentType = "application/json";
    }
  }

  // Auto-determine body mode
  let gqlDetected = false;
  let mpDetected = false;

  // Multipart has precedence when the captured body's content-type is
  // multipart/*. Each sub-part gets its own contextual editor; the flat
  // form builder can only express one part so we must bypass it.
  /* THE REQUEST'S OWN CONTENT TYPE, and the response's only where the request stated none. Both are
     asserted strings on this record, so `""` is a positive statement — the request sent no Content-Type —
     and the choice between them is a real one rather than a hole being filled twice over. */
  const reqCt = req.contentType !== "" ? req.contentType : req.mimeType;
  if (reqCt.toLowerCase().startsWith("multipart/") && req.rawBodyB64) {
    if (mpLoadFromCapturedRequest(req)) {
      mpDetected = true;
      setBodyMode("multipart");
      gqlClear();
    }
  }

  if (!mpDetected && isGraphQLUrl(req.url) && req.rawBodyB64) {
    try {
      const bytes = base64ToUint8(req.rawBodyB64);
      const text = new TextDecoder().decode(bytes);
      const gqlReq = parseGraphQLRequest(text);
      if (gqlReq) {
        gqlDetected = true;
        setBodyMode("graphql");
        gqlLoadOperations(gqlReq.operations, gqlReq.batched);
      }
    } catch (e) {
      RETHROW_FATAL(e);
      // isGraphQLUrl heuristically matched (often false-positive for
      // /graphql-shaped URLs that aren't real GraphQL), so the body might
      // not be a GraphQL envelope. parseGraphQLRequest returning null is the
      // normal "not GraphQL" signal; a THROW here means something unexpected.
      console.debug("[popup:send_load] GraphQL body load threw:", e && e.message || e);
    }
  }
  if (!gqlDetected && !mpDetected) {
    // Form mode if schema was loaded, otherwise raw
    setBodyMode(currentSchema ? "form" : "raw");
    gqlClear();
    mpClear();
  }
  // Add headers (filtering out Content-Type which is auto-determined)
  const headersList = document.getElementById("send-headers-list");
  headersList.replaceChildren();
  if (req.requestHeaders) {
    for (const [k, v] of Object.entries(req.requestHeaders)) {
      if (k.toLowerCase() === "content-type") continue;
      addHeaderRow(k, v);
    }
  }
  // The replayed record's own headers are in; a learned key goes where the record carries none.
  prefillKeyPositions();
  renderSendUrl();

  // Populate raw body textarea with original body as fallback
  document.getElementById("send-raw-body").value = "";
  if (req.rawBodyB64 && !gqlDetected) {
    try {
      const bytes = base64ToUint8(req.rawBodyB64);
      document.getElementById("send-raw-body").value = new TextDecoder().decode(bytes);
    } catch (e) {
      RETHROW_FATAL(e);
      // base64 decode or UTF-8 decode failed — the rawBodyB64 is malformed.
      // The textarea stays empty (correct fallback for "can't display") but
      // surface so a binary/corrupt body capture is diagnosable.
      console.warn("[popup:send] raw body decode failed:", e && e.message || e);
    }
  }
  // Populate historical response if available
  /* "THIS RECORD SAW AN ANSWER", stated against the two fields that carry one. `responseBody` is
     `string | null` and `status` is a number, both written on every record of this kind, so the test is
     about the CAPTURE and not about whether the producer wrote the fields: a form submission whose
     navigation this log never saw the end of has null and 0, and says so. */
  if (req.responseBody !== null || req.status !== 0) {
    /* renderResponse reads statusText on every response record; an HTTP log entry carries it
       (offscreen-brain.js's _checkLogEntry), so it is copied, never defaulted. */
    DCHECK(typeof req.statusText === "string",
           "a replayed request record carries no statusText — offscreen-brain.js's _checkLogEntry asserts one " +
           "on every HTTP entry, so this record is not that kind or its producer stopped writing it");
    const historicalResult = {
      ok: parseInt(req.status) < 400,
      status: req.status,
      statusText: req.statusText,
      headers: req.responseHeaders,
      timing: 0,
      service: req.service,
      /* A captured request was composed by the page, not against a discovery method, and no store was asked
         for its document: both are stated absent, which renderResultBody resolves against the endpoint this
         replay selected above and the popup's cached document for the service. */
      methodId: null,
      discovery: null,
      body: null,
    };

    if (req.responseBody) {
      const mimeType = req.mimeType;
      const isBinaryProtobuf =
        (mimeType.includes("protobuf") && !mimeType.includes("json")) ||
        (req.requestHeaders &&
          Object.entries(req.requestHeaders).some(
            ([k, v]) =>
              k.toLowerCase() === "content-type" &&
              v.toLowerCase().includes("protobuf") &&
              !v.toLowerCase().includes("json"),
          ));
      const isJspb = mimeType.includes("json+protobuf") ||
        mimeType.includes("json; protobuf");

      let bodyText = req.responseBody;
      if (req.responseBase64) {
        try {
          const bytes = base64ToUint8(req.responseBody);
          bodyText = new TextDecoder().decode(bytes);
        } catch (e) {
          RETHROW_FATAL(e);
          // base64 / UTF-8 decode of historical body failed — bodyText stays
          // as the raw base64 string. Downstream format parsing will likely
          // also fail (and log), but surface here for traceability.
          console.warn("[popup:historical] base64 decode failed for %s body: %s", mimeType, e && e.message || e);
        }
      }

      if (isGrpcWeb(mimeType)) {
        // gRPC-Web: pass raw bytes for frame parsing
        try {
          let bytes;
          if (isGrpcWebText(mimeType)) {
            bytes = base64ToUint8(
              req.responseBase64 ? req.responseBody : btoa(req.responseBody),
            );
          } else {
            bytes = req.responseBase64
              ? base64ToUint8(req.responseBody)
              : new TextEncoder().encode(req.responseBody);
          }
          historicalResult.body = {
            format: "grpc_web",
            bytes,
            raw: bodyText,
            size: bytes.length,
          };
        } catch (e) {
          RETHROW_FATAL(e);
          // gRPC-Web frame extraction threw — surface so a corrupt captured
          // body that the MIME type promised but couldn't deliver is visible.
          console.debug("[popup:historical] gRPC-Web parse failed:", e && e.message || e);
        }
      } else if (isJspb) {
        // JSPB (JSON+Protobuf): parse as JSON, convert to protobuf tree
        try {
          const parsed = JSON.parse(bodyText);
          if (Array.isArray(parsed)) {
            historicalResult.body = {
              format: "protobuf_tree",
              parsed: jspbToTree(parsed),
              raw: bodyText,
              size: bodyText.length,
              isJspb: true,
            };
          }
        } catch (e) {
          RETHROW_FATAL(e);
          console.debug("[popup:historical] JSPB parse failed:", e && e.message || e);
        }
      } else if (isBinaryProtobuf) {
        try {
          const bytes = req.responseBase64
            ? base64ToUint8(req.responseBody)
            : new TextEncoder().encode(req.responseBody);
          historicalResult.body = {
            format: "protobuf_tree",
            parsed: pbDecodeTree(bytes),
            raw: req.responseBody,
            size: bytes.length,
          };
        } catch (e) {
          RETHROW_FATAL(e);
          console.debug("[popup:historical] binary protobuf parse failed:", e && e.message || e);
        }
      } else if (mimeType.includes("json") || mimeType.includes("text/plain") || mimeType.includes("javascript")) {
        try {
          // Strip Google XSSI prefix before parsing
          let jsonText = bodyText;
          if (jsonText.trimStart().startsWith(")]}'")) {
            jsonText = jsonText.trimStart().substring(4).trimStart();
          }
          // Strip JSONP wrapper: callbackName({...}) → {...}
          if (mimeType.includes("javascript")) {
            var _jpM = /^[a-zA-Z_$][\w$.]*\s*\(\s*/.exec(jsonText);
            if (_jpM) {
              var _jpInner = jsonText.slice(_jpM[0].length);
              var _jpEnd = _jpInner.lastIndexOf(")");
              if (_jpEnd !== -1) jsonText = _jpInner.slice(0, _jpEnd).trim();
            }
          }
          const parsed = JSON.parse(jsonText);
          // Detect JSPB in text/plain responses (Google returns these)
          if (Array.isArray(parsed) && parsed.length > 0 &&
              parsed.some((item) => item === null || Array.isArray(item) || typeof item !== "object")) {
            historicalResult.body = {
              format: "protobuf_tree",
              parsed: jspbToTree(parsed),
              raw: bodyText,
              size: bodyText.length,
              isJspb: true,
            };
          } else {
            historicalResult.body = {
              format: "json",
              parsed,
              raw: bodyText,
              size: bodyText.length,
            };
          }
        } catch (e) {
          // JSON parse failure on a body the MIME type tagged as JSON/text/JS
          // — common (servers misreport content-type), surface at debug so it's
          // diagnosable without spamming on every misreported body. An invariant abort is not that state and
          // travels ON: the six sibling catches in this same historical-body path all open with this line, and
          // the one that did not was where a broken record would have been logged as a misreported server.
          RETHROW_FATAL(e);
          console.debug("[popup:historical] JSON parse failed for %s body: %s", mimeType, e && e.message || e);
        }
      }

      // SSE, NDJSON, multipart, and async chunked are detected by renderResultBody()
      // via content-type headers and body inspection — just need raw text preserved.
      if (!historicalResult.body) {
        historicalResult.body = {
          format: "text",
          raw: bodyText,
          size: bodyText.length,
        };
      }
    }

    renderResponse(historicalResult);
    domAppendChildren(document.getElementById("send-response-status"),
                      [" ", domElement("span", { class: "badge badge-source ml-8" }, ["Historical"])]);
  } else {
    document.getElementById("send-response").style.display = "none";
  }

  // Switch tab
  document.querySelector(".tab[data-panel='send']").click();
}

document.getElementById("btn-clear-log").addEventListener("click", async () => {
  // The log is GLOBAL: CLEAR_LOG removes entries from the one global array
  // (clearAll, or by tabId). Re-fetch via loadRequestLog so the render reflects
  // the cleared state. (The old `tabData.requestLog = []` was a dead no-op on the
  // empty per-document log, and not re-fetching left "active" showing a stale log.)
  if (logFilter === "all") {
    await chrome.runtime.sendMessage({ type: "CLEAR_LOG", clearAll: true });
  } else {
    const targetTabId = logFilter === "active" ? currentTabId : logFilter;
    await chrome.runtime.sendMessage({ type: "CLEAR_LOG", tabId: targetTabId });
  }
  await loadRequestLog();
  renderResponsePanel();
  populateTabFilter();
});
