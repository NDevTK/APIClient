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
// The last few engine RUN records (bridge.js `self._engineLog`), fetched beside tabData. GLOBAL, not
// per-document: one WASM instance per origin-keyed agent cluster runs on the ONE cross-session frontier, so a
// run record belongs to the frontier rather than to the tab whose popup is open. `null` until the first
// GET_ENGINE_RUNS answers — a different statement from an empty array (no engine has run yet).
let engineRuns = null;
/* THE HOST'S LEVEL-1 READING (bridge.js `self._level1`, fetched beside the runs). THREE STATES, AND THE VIEW
   KEEPS THEM APART BECAUSE THE PRODUCER DOES: `undefined` — this popup has not asked yet, or the reply never
   came (asserted at the fetch); `null` — the host answered, and no scheduler round has completed in this
   session; an object — a reading of the order taken at the end of the last round. Declared `undefined` here
   deliberately, so "not asked yet" is not the same value as "asked, and there is no round". */
let hostOrder;
let currentSchema = null;
let currentRequestUrl = "";
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
  const target = (currentReplayRequest && currentReplayRequest.documentId) || currentDocumentId();
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
let currentKeyOverride = null; // null = auto, { key, source, disabled } for override

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
  document.getElementById("send-key-section").style.display =
    isConsole ? "none" : "";
  document.getElementById("send-headers-section").style.display =
    isConsole ? "none" : "";
  document.getElementById("btn-send").style.display =
    isConsole ? "none" : "";
  document.querySelector(".export-row").style.display =
    isConsole ? "none" : "";
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
    .addEventListener("click", addHeaderRow);
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

  // API key selector: delegate to radio buttons and custom input
  document.getElementById("send-key-options").addEventListener("change", onKeySelectionChange);
  document.getElementById("send-key-section").addEventListener("change", (e) => {
    if (e.target.name === "key-inject") onKeySelectionChange();
  });
  document.getElementById("send-key-options").addEventListener("input", (e) => {
    if (e.target.id === "send-key-custom") onKeySelectionChange();
  });

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
            delete lastSendResult.discovery;
            renderResponse(lastSendResult);
          }
        }
      }
    }
  });

  // ─── Export Functions ──────────────────────────────────────────────────────

  async function buildCurrentRequest() {
    const bodyMode = currentBodyMode;
    let url = currentRequestUrl;
    if (!url) return null;

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

    // Local helper: apply form-field params to the URL's query string. canParse
    // guard so an unparseable Send-tab URL stays as-is rather than catching a
    // throw — the URL field's validity is checked upstream when the user types,
    // so reaching here unparseable means a real bug we want surfaced.
    function _applyFormParamsToUrl(rawUrl, params) {
      if (!URL.canParse(rawUrl)) {
        console.warn("[popup:send] URL.canParse(%s) failed — form params not applied", rawUrl);
        return rawUrl;
      }
      const urlObj = new URL(rawUrl);
      for (const [k, v] of Object.entries(params)) urlObj.searchParams.set(k, String(v));
      return urlObj.toString();
    }
    let body;
    if (httpMethod === "GET" || httpMethod === "DELETE") {
      // Collect URL params from form fields even for GET/DELETE
      if (bodyMode === "form") {
        const formValues = collectFormValues();
        if (Object.keys(formValues.params).length > 0) {
          url = _applyFormParamsToUrl(url, formValues.params);
        }
      }
      body = null;
    } else if (bodyMode === "form") {
      const formValues = collectFormValues();
      if (Object.keys(formValues.params).length > 0) {
        url = _applyFormParamsToUrl(url, formValues.params);
      }
      body = {
        mode: "form",
        formData: { fields: formValues.fields },
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
        headers,
        body,
        apiKeyOverride: currentKeyOverride,
      });
    } catch (e) {
      /* An invariant abort travels ON (extension/check.js RETHROW_FATAL): `null` here is the ONE state the
         caller reads as "the background did not answer", and a broken record assertion arriving as that
         would report a contract failure as an unreachable service worker. */
      RETHROW_FATAL(e);
      return null;
    }
  }

  /* THE BUILT REQUEST, ASSERTED WHERE IT ARRIVES — the same shape as lib/schema.js's `_checkPageFetchReply`,
     for the same reason. `buildExportRequest` answers with EITHER an `error` string or the four fields the
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
      } catch (_) {
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
    } catch (_) {
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
        bodySize: r.responseBody ? r.responseBody.length : 0,
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
     rendered exactly that — a clean-looking view of a page the engine had already learned an endpoint from.
     The DCHECK below is the same contract GET_ENGINE_RUNS states one line down; asserting one reply of a pair
     that arrive from the same document is a check on the wrong thing. */
  DCHECK(tabData && typeof tabData === "object",
         "GET_STATE did not answer with a state object — lib/popup-handlers.js answers it unconditionally " +
         "(an unknown document still gets the global moat through _emptyDocView), so a null reply is the " +
         "offscreen brain not reachable, and every panel would render its empty state as a fact about the page");
  // The engine's own run records — see renderEngineRuns for why they had never reached a human surface. No
  // try/catch and no `|| []`: the offscreen answers this command unconditionally off an array bridge.js
  // declares at load, so a failure here is that document not being there, which is the one thing an empty
  // list would have to mean and could not say.
  engineRuns = await chrome.runtime.sendMessage({ type: "GET_ENGINE_RUNS" });
  DCHECK(Array.isArray(engineRuns),
         "GET_ENGINE_RUNS did not answer with an array — the offscreen slices it straight off bridge.js's " +
         "_engineLog, so anything else is that command not reaching the offscreen document at all");
  /* AND THE ORDER THE HOST PUT THOSE RUNS IN. The records above are one document each; this is the one thing
     no document can state — which live instance and which non-resident work item the pool ranked ahead of
     which, at the end of the last scheduler round. `null` is bridge.js saying no round has completed;
     `undefined` is the command not reaching the offscreen at all, which is why it is asserted rather than
     defaulted (a `|| null` here would render "the scheduler has taken no round" for a host that is gone). */
  hostOrder = await chrome.runtime.sendMessage({ type: "GET_HOST_ORDER" });
  DCHECK(hostOrder === null || (hostOrder && typeof hostOrder === "object" && !Array.isArray(hostOrder)),
         "GET_HOST_ORDER did not answer with the host's Level-1 reading or with the null that says it has " +
         "taken no round — the offscreen answers it straight off bridge.js's `_level1`, so anything else is " +
         "that command not reaching the offscreen document, and the ONE observable of the order ACROSS " +
         "documents is invisible again");
  // The recorded API drift (lib/popup-discovery.js). Fetched only when it has not been read yet or an action
  // just re-fetched a document — a re-render never needs it again, and it is the whole history, not a window.
  if (discoveryChanges === null) await loadDiscoveryChanges();
  await loadRequestLog();
  render();
  _refreshSendEnabled();   // readiness may have changed (pin became live / stale / not-ready)
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
         typeof r.stranded === "number" && typeof r.rederived === "number" && typeof r.entries === "number",
         "the frontier-share command answered without the store's own numbers — this row is the only place " +
         "the user can see what the setting is doing, and a partial answer would render a policy that looks " +
         "as if it had never had to act");
  const mb = (n) => (n / (1024 * 1024)).toFixed(1);
  document.getElementById("frontier-share-mb").value = String(Math.round(r.share / (1024 * 1024)));
  document.getElementById("frontier-share-status").textContent =
    mb(r.docBytes) + " MB stored across " + r.entries + " parked page(s)" +
    (r.overShare > 0 ? "; " + mb(r.overShare) + " MB over the share is the only copy there is and was kept" : "") +
    "; " + r.shed + " shed, " + r.rederived + " fetched back, " + r.stranded + " could not be";
}

async function clearState() {
  await chrome.runtime.sendMessage({ type: "CLEAR_TAB", tabId: currentTabId });
  tabData = null;
  allTabsData = null;
  // The run records name the pages that were analysed, so the view drops them with everything else — and so
  // does the offscreen: hostClear truncates `self._engineLog` in place, which is what makes dropping the copy
  // here a view following the store rather than a view hiding what the store still holds.
  engineRuns = null;
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
  let html = "";
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
       REFUSED to make (`blocked-scheme:about:`, `blocked-private-from-public`), and the two need opposite
       things from the person reading this row. It was a content script's own exception text before the load
       moved to the chokepoint; it is a named rule now. */
    const reason = ps.kind === "status" ? `the server answered ${esc(String(ps.status))} to it`
                 : ps.kind === "empty"  ? "the server answered with an empty body"
                 : `the load did not happen: ${esc(String(ps.detail).slice(0, 160))}`;
    html += `<div class="deep-row"><span class="deep-label"><strong>This page was NOT analysed.</strong> `
          + `The analyser loaded this page's address itself — the only way to get the bundle as SHIPPED, with `
          + `its real response headers, rather than this tab's already-executed DOM — and ${reason}. Nothing `
          + `below was learned by running this page's code. A single-use URL (a bot challenge, a one-time `
          + `token) cannot be fetched twice, and neither can a document that was reached by anything other `
          + `than a plain GET.</span></div>`;
  }
  if (tabData.analysisRun === "crashed") {
    html += `<div class="deep-row"><span class="deep-label"><strong>The engine crashed while analysing this `
          + `page.</strong> What is shown below is what it had already learned when it died — a PARTIAL `
          + `result, not a clean bill. See the engine run record for the abort.</span></div>`;
  }
  if (rerrs.length) {
    const rows = rerrs.map((r) => {
      DCHECK(typeof r.context === "string" && typeof r.message === "string",
             "a page-error row reached the popup without a context/message pair — bridge.js writes both as " +
             "strings on every entry it builds, so a row missing one is that relay broken");
      return `<div class="deep-row" title="${esc(r.message)}"><span class="deep-label">${esc(r.context + ": " + r.message.slice(0, 140))}</span></div>`;
    }).join("");
    /* APPENDED, NOT ASSIGNED. `html =` here silently dropped whatever section had been built above it — which
       was nothing while this was the first section, and was the crashed-run notice the moment one existed.
       A section that overwrites the panel instead of adding to it is a defect waiting for the next section. */
    html += `<details class="deep-cross-tab"><summary>Analyzer gaps — reached-but-unresolved / host-model (${rerrs.length})</summary>${rows}</details>`;
  }
  // The two sections are INDEPENDENT. A document with no page errors is the ordinary case and still has a run
  // record worth showing; gating the run record on `rerrs.length` would hide it for exactly the pages that
  // went well, which is most of them.
  html += renderEngineRuns();
  // AND THE ORDER THOSE RUNS WERE PUT IN — independent of them for the same reason the two sections above are
  // independent of each other: a pool with documents WAITING and no run yet is exactly the state whose order
  // matters, and gating this on `engineRuns.length` would hide it there.
  html += renderHostOrder();
  if (!html) { el.style.display = "none"; return; }
  el.innerHTML = html;
  el.style.display = "block";
}

// WHAT EACH ENGINE RUN COST AND WHAT IT LEARNED — the eight counters solver/result.c emits in one snprintf,
// which bridge.js asserts field-for-field and writes onto `self._engineLog`, and which until recently reached
// no human at all: the array's only reader was `self.rendererPoolProbe`, a function nothing in this extension
// or in testing/ calls. They are the ONLY observable that the single BFS context-switches, forks and pumps jobs
// rather than running its flows FIFO, and beside them ride what the run LEARNED (endpoints, sinks) and what
// it PARKED for the next session — the cross-session frontier, otherwise invisible.
//
// A CRASH RECORD IS RENDERED AS AN ABSENCE, NEVER AS ZEROES. bridge.js builds two shapes on purpose: a run
// with an @RESULT document carries all thirteen fields, and a run without one carries `run`, `resumed` and
// `url` and nothing else, because "seven zeroes read as 'the engine ran and did nothing' —
// indistinguishable in the log from a real run that explored nothing, which is a finding". A `|| 0` here would
// undo exactly that distinction, one field at a time, which is why every field below is asserted instead.
//
// AND A SNAPSHOT OF A RUN STILL GOING IS NOT A RUN. The engine emits a partial every 750 ms and every one of
// them used to become its own row here, so ONE page being analysed rendered as EIGHT engine runs of one URL,
// each having "learned" a slightly different number of endpoints — eight measurements of a thing that had
// happened once. bridge.js now gives a run ONE row and a partial WRITES it, and the row says which of the
// three states it is reporting. The rendering follows that and does not merely relabel it: an in-progress row
// is a SNAPSHOT, its numbers are "so far" rather than totals, and the section header counts runs and says how
// many of them have not ended — because the reader's question about a list of runs is how many pages were
// analysed, and the wrong answer there is a false impression rather than a crash.
const RUN_STATES = ["partial", "complete", "crashed"];
function renderEngineRuns() {
  if (!engineRuns || !engineRuns.length) return "";
  const FULL = [
    ["endpoints", "endpoints learned"], ["sinks", "@S sinks"], ["park", "flows parked for next session"],
    ["switches", "flow context-switches"], ["flows", "flows created"], ["candidates", "@S candidates run"],
    ["jobsQueued", "jobs queued"], ["jobsRun", "jobs run"],
    /* WHAT MAKES "jobs run 0" READABLE: a queued job may only run with the flow's execution context stack
       empty (HTML §8.1.4.4 step 3 of clean up after running script), so this says whether that boundary was
       ever reached. Zero here and thousands queued is a document no program of which has finished. */
    ["unitsDone", "units of work completed"],
    ["worldSegmentsHeld", "cross-instance world segments held"],
    ["worldSegmentsMade", "…made (cumulative)"], ["worldSegmentsForked", "…forked"],
    /* THE FOUR THAT MAKE AN EMPTY @S SURFACE READABLE, and they were written by bridge.js and read by nobody.
       solver/result.c emits them for one reason: `securitySinks: []` has four readings that take opposite
       actions — no attacker source was ever read, none reached a sink, sinks ran and only the page's own
       strings arrived, or taint arrived and the search was DECLINED because the check on it was unforgeable —
       and the last of those is the engine's strongest negative result. bridge.js asserts all four onto the
       record; this list is where a reader meets them, and without them the popup showed a run's COST and
       never what its work MET. A computed value with a writer and no reader is not a mechanism. */
    ["sourceReads", "attacker-source reads"], ["sinkReached", "sinks reached"],
    ["sinkTainted", "…with tainted input"], ["sinkSuppressed", "…search declined (unforgeable check)"],
  ];
  const rows = engineRuns.slice().reverse().map((m) => {
    DCHECK(typeof m.url === "string",
           "an engine run record reached the popup without a url — bridge.js writes it on BOTH record shapes " +
           "(the crash arm included), so a record missing it is that relay broken");
    /* `resumed` IS A COUNT OR THE STATED ABSENCE OF ONE, AND THIS VIEW SPEAKS BOTH. It was asserted here as a
       number, which is why nothing caught that it was a number that had been MANUFACTURED: bridge.js
       re-derived it at each reader out of whichever slice of the engine's output that reader held, and the
       incremental snapshot holds one line, so every still-running row rendered "0 resumed" no matter how
       large the residue that had come back. A type check cannot see that, because the wrong answer is
       perfectly well-typed — which is the whole shape of a defaulted field, and the reason the fix is a third
       state rather than a stricter number. `null` means no instance ever reached the seeding of its frontier,
       and it is rendered as the sentence it is; a `?? 0` here would put the defect straight back. */
    DCHECK(m.resumed === null ||
           (typeof m.resumed === "number" && Number.isInteger(m.resumed) && m.resumed >= 0),
           "an engine run record reached the popup with a resume count that is neither a count nor the stated " +
           "'not known' (`" + String(m.resumed) + "`) — bridge.js writes one or the other on BOTH record " +
           "shapes, so anything else is that relay broken and the user is shown a number of parked flows " +
           "nothing observed");
    /* THE ONE SPELLING OF BOTH STATES, so the three rows below cannot disagree about which sentence an
       absent count gets. "0 resumed" is a real and useful thing to say — this session was seeded with no
       residue — and it is exactly what must not be said when nobody looked. */
    const resumedTxt = m.resumed === null
      ? "resume state not known (this run never seeded a frontier)"
      : `${esc(String(m.resumed))} resumed`;
    /* WHICH OF THE THREE THIS ROW IS, ASSERTED AND NEVER INFERRED. It was inferred — from a `crashed` boolean,
       which could say one of three things and so said the other two identically — and a mid-run snapshot
       therefore rendered as a completed analysis with a full set of totals. A record whose state this view
       does not know is bridge.js's log seam having changed, not a row to render as whichever arm falls out. */
    DCHECK(RUN_STATES.indexOf(m.run) >= 0,
           "an engine run record reached the popup in a state this view does not speak (`" + m.run + "`) — " +
           "bridge.js writes `run` on every record it logs, so a missing or unknown one is that seam broken " +
           "and a snapshot of a page still being analysed would be shown as a finished run of it");
    const where = esc(m.url ? _shortUrl(m.url) : "(no source url)");
    if (m.run === "crashed") {
      /* AND IT SAYS WHY, because the reader of this row is the one person who can act on the answer. The row
         used to state the absence of counters and stop there, so the cause — which bridge.js has always had,
         and which carries the engine's ROOT @WHY naming the capability that is missing — lived only in an
         offscreen console the renderer does not tee. A crash that announces itself without pointing at its
         cause is the half of the crash contract this view was not keeping. */
      DCHECK(typeof m.err === "string" && m.err !== "",
             "a crashed engine run reached the popup with no `err` — bridge.js reads the `engine-crash` line " +
             "off the run's own output and asserts it there, so an absent one here is that relay broken and " +
             "the user is shown a crash with no cause");
      /* AND THE RESUME STATE IS STATED ON THE CRASH ROW UNCONDITIONALLY, WHICH IS WHERE IT IS WORTH MOST.
         It was suppressed whenever it was falsy, so a crash of a session seeded from a rebuilt residue and a
         crash of a session seeded from a fresh boot flow produced the identical sentence — and the cold-tier
         rebuild is precisely the suspect a reader of this row is trying to rule in or out. All three states
         are now said: a count (the residue came back and then the run died), zero (no residue was handed to
         this session, so the cold tier is not implicated) and not-known (the boot died before the frontier
         was seeded at all, which is itself the answer to when it died). */
      return `<div class="deep-row"><span class="deep-label">${where} — <strong>the engine crashed</strong>`
           + `; this run reported no result document, so it has no counters at all (not zeroes)`
           + `; ${resumedTxt}`
           + ` — ${esc(m.err)}</span></div>`;
    }
    const live = m.run === "partial";
    const parts = FULL.map(([k, label]) => {
      DCHECK(typeof m[k] === "number",
             "an engine run record reached the popup with no numeric " + k + " — solver/result.c emits the " +
             "eight cost counters in one snprintf and bridge.js asserts each of them, so a record that has " +
             "an @RESULT document and is missing one is that seam having changed underneath both");
      return esc(label) + " " + esc(String(m[k]));
    });
    /* AND WHAT THE ONE BFS WAS ORDERING ITS FLOWS BY — solver/result.c's `_wfq`, relayed whole by bridge.js.
       Until it rode the result document the scheduler's ordering was written ONLY by the smoke driver's own
       loop, which the extension's ABI never enters, so every ordering number this project had ever quoted was
       a reading of one fixture and no person running this had ever seen one.
       IT IS NOT IN `FULL`, AND THAT IS THE POINT OF BOTH LISTS. Every row of `FULL` is a TOTAL over the run
       and is asserted BY NAME because this view names it; these are readings of ONE INSTANT and are rendered
       GENERICALLY, from whatever rows the census carries. A hand list here would be a third copy of
       solver/flow.h's field list — and that list's own history is a row computed and printed by nobody
       (`svc_min`) beside a row printed and read by nobody (`families`), which is precisely what a hand-kept
       copy at each end produces. Rendered this way, a row added to the census reaches this reader unedited.
       AND `members: 0` IS RENDERED AS THE SENTENCE IT IS, NEVER AS AN ORDER OF ZEROES. A finalize composes its
       document after the frontier has drained or been parked, so its census has no term rows at all — the
       absence IS the statement, and printing "reward 0..0, spread 0" there would report "nothing orders this
       frontier" about a run that finished. The snapshots taken while the run is live are where the order is. */
    DCHECK(m.wfq && typeof m.wfq === "object" && Number.isInteger(m.wfq.members),
           "an engine run record reached the popup with no WFQ census — solver/result.c composes it into " +
           "every document it builds and bridge.js asserts its shape before relaying it whole, so a record " +
           "that has an @RESULT document and no `wfq` is that relay broken, and the ONE observable of what " +
           "the scheduler is ordering its flows by is invisible again");
    const order = m.wfq.members === 0
      ? `frontier order: no members standing when this was taken${live ? "" : " — the run drained or parked, so there was no order left to report"}`
      : `frontier order over ${esc(String(m.wfq.members))} flows: `
        + Object.keys(m.wfq).filter((k) => k !== "members")
                .map((k) => esc(k) + " " + esc(String(m.wfq[k]))).join(" · ");
    /* AND WHAT THAT ORDER WAS DENOMINATED IN — solver/quantum.h's `_quantum`, relayed whole by bridge.js and
       rendered HERE, immediately under the order, because it is the fact that decides whether the order above
       may be compared with another run's at all. A person reading this panel is doing exactly one thing with
       two rows of it: comparing them. On the host this extension runs — the engine's realm is an opaque origin
       and so is never crossOriginIsolated, which is what denies it the watchdog thread a CPU-clocked slice
       would need — solver/engine.c's `flow_age_running` charge is billed in WALL TIME, and that charge is a
       comparison BETWEEN flows, so a descheduling the OS chose lands on whichever flow was running and moves
       its rank alone. Two runs of ONE build over ONE page then take different frontier orders. Without this
       sentence the only available reading of that is "the engine changed", which is the one thing it is not.
       IT IS NOT A CENSUS ROW AND IS NOT RENDERED AS ONE. The three below are readings of an instant and are
       rendered generically from whatever rows they carry; this is three NAMED facts about the host, one of
       which is a string and one a boolean, so it is read by name — and it is written as a SENTENCE rather than
       `isCpu false`, because the consequence is what a reader needs and the field name is not it.
       ONLY THE CAVEAT IS LOUD. `isCpu` true is the positive statement that none of this applies, said in one
       clause; a warning printed on every run is a warning nobody reads, which is the same reason
       engine/build.mjs marks its verdict `WALL-SLICED` only where it is false. */
    DCHECK(m.quantum && typeof m.quantum === "object" && typeof m.quantum.isCpu === "boolean"
           && typeof m.quantum.measure === "string" && typeof m.quantum.sliceMs === "number",
           "an engine run record reached the popup with no `quantum` denomination — solver/quantum.c composes " +
           "it into every document result.c builds and bridge.js asserts its three fields before relaying it " +
           "whole, so a record that has an @RESULT document and no `quantum` is that relay broken, and every " +
           "frontier order on this panel becomes a number nobody can tell from another run's");
    const denom = `<span class="deep-label">`
      + `scheduler slice ${esc(String(m.quantum.sliceMs))} ms, billed in ${esc(m.quantum.measure)}`
      + (m.quantum.isCpu
          ? ` — real thread CPU, so this run's order is invariant to what else the machine was doing`
          : ` — NOT CPU: the aging charge that produced the order above bills wall time to whichever flow the `
            + `OS left running, so two runs of THIS SAME build over this same page take different orders. `
            + `Compare two runs of one build before reading a difference between two builds.`)
      + `</span>`;
    /* AND THE THREE SUBSYSTEMS UNDER THE ORDER — solver/result.c's `_cold`, `_heap`, `_swap` and decide.c's
       `_forkAt`, relayed whole by bridge.js. Same defect as `_wfq` and four times the size: each was printed
       ONLY by the smoke driver's loop, which the extension's ABI never enters, so what the pager is holding,
       what the heap is made of, what a context switch costs and which predicate is growing the frontier had
       never once been visible to a person running this — on the one host where a real page actually reaches
       RAM pressure, the realm ceiling and a long-lived delta chain.
       RENDERED GENERICALLY FOR THE REASON THE ORDER IS: a hand list here would be a third copy of
       solver/result.c's format string, and that surface's own history is a row computed and printed by nobody
       beside a row printed and read by nobody. A row added to any census reaches this reader unedited.
       AND THE FORK TABLE'S EMPTY SHAPE IS RENDERED IN WORDS, NEVER AS A BLANK. `{}` is the positive statement
       that this document never forked — loud on a bundle that should have branched on opaque input — and a
       row that simply printed nothing there would say it in the same way as a census that failed to arrive.
       The other three cannot be empty; bridge.js asserts that rather than this view assuming it. */
    const CENSUS = [["cold", "frontier + cold tier"], ["heap", "heap"], ["swap", "COW switch"]];
    const censusRows = CENSUS.map(([k, label]) => {
      DCHECK(m[k] && typeof m[k] === "object",
             "an engine run record reached the popup with no `" + k + "` census — solver/result.c composes " +
             "it into every document it builds and bridge.js asserts its shape before relaying it whole, so " +
             "a record that has an @RESULT document and no `" + k + "` is that relay broken");
      return `<span class="deep-label">${esc(label)}: `
           + Object.keys(m[k]).map((f) => esc(f) + " " + esc(String(m[k][f]))).join(" · ") + `</span>`;
    });
    DCHECK(m.forkAt && typeof m.forkAt === "object",
           "an engine run record reached the popup with no `forkAt` table — solver/decide.c composes it into " +
           "every document result.c builds, so its absence is that relay broken and not a document that " +
           "never forked, which is `{}` and is rendered as the sentence it is");
    censusRows.push(`<span class="deep-label">` + (Object.keys(m.forkAt).length === 0
      ? `forked at: nothing — no predicate of this document ever split a flow`
      : `forked at: ` + Object.keys(m.forkAt).map((f) => esc(f) + " ×" + esc(String(m.forkAt[f]))).join(" · "))
      + `</span>`);
    // The numbers on a snapshot are real observations of a page that is STILL being analysed, so they are
    // shown — and they are labelled as a running total, which is the one thing a completed run's row is not.
    const head = live
      ? `<strong>still running</strong> — snapshot, not a total; ${resumedTxt} · so far: `
      : `run complete · ${resumedTxt} · `;
    return `<div class="deep-row"><span class="deep-label">${where} — ${head}` + parts.join(" · ")
         + `</span><span class="deep-label">${order}</span>` + denom + censusRows.join("") + `</div>`;
  }).join("");
  const live = engineRuns.filter((m) => m.run === "partial").length;
  return `<details class="deep-cross-tab"><summary>Engine runs — cost, what was learned, what was parked `
       + `(${engineRuns.length}${live ? `, ${live} still running` : ""})</summary>${rows}</details>`;
}

/* THE ORDER ACROSS DOCUMENTS — bridge.js's `_level1`, the reading its scheduler takes at the end of every
   round. The census one function up is LEVEL-2 and rides the engine's own result document, which is the only
   place it could ride: it is the order WITHIN one document's frontier. Level-1 is the other one — which live
   instance, which waiting tab and which parked residue the host ranked ahead of which — and it can ride
   nothing the engine emits, because it is composed in the trusted zone out of one weight per instance and one
   per non-resident work item, and no engine can see another engine. Until this row existed nothing recorded
   it at all, which is how BOTH Level-1 ranking defects found this session came to be found by reading the
   code: `frontierWeight(FRONTIER_UNSERVED)` frozen at the constant 1.0 for every waiting document, and the
   cold walk deleting the weight of every row whose address a live document held. Neither is visible in the
   winner — the first is a SPREAD that has collapsed and the second is an ABSENCE — which is why the row that
   matters most here is `candWMax` beside `candWMin`, and why this view says so in words when they meet.

   THE ROWS ARE RENDERED GENERICALLY AND THE STATES ARE RENDERED IN WORDS, which is the same split
   renderEngineRuns makes for `_wfq` and for the same two reasons. Generic, because a row added to the census
   must reach a human with no edit here — a hand list at this end is the second copy that goes stale. In
   words, because the census distinguishes its states by PRESENCE and a reader must never have to infer that
   from a missing key: a round that never asked the non-resident order, a round that asked it and ranked
   nothing, and a real ordering are three different facts, and all three would otherwise render as the same
   short row of numbers. */
function renderHostOrder() {
  if (hostOrder === undefined) return "";   // not asked yet — a different fact from the host having no round
  if (hostOrder === null)
    return `<details class="deep-cross-tab"><summary>Level-1 order — across documents</summary>`
         + `<div class="deep-row"><span class="deep-label">the host has completed no scheduler round in this `
         + `session, so there is no order to report — this is the absence of a reading, not an order of `
         + `zeroes</span></div></details>`;
  DCHECK(Number.isInteger(hostOrder.round) && hostOrder.round > 0 && Number.isInteger(hostOrder.hot),
         "the host's Level-1 reading carries no round number or no hot count — bridge.js writes both on every " +
         "record it composes, in the `finally` of the round itself, so a reading missing one is that composer " +
         "having changed underneath this reader and every row below it is a reading of an unknown instant");
  for (const k of Object.keys(hostOrder))
    DCHECK(typeof hostOrder[k] === "number" && Number.isFinite(hostOrder[k]),
           "the host's Level-1 reading carries a non-finite `" + k + "` — every row of it is a population " +
           "count, a 0/1 state or a weight over a non-empty population, and the one value that is legitimately " +
           "not a number (an engine's -Infinity for a drained frontier) is carried as the `drained` COUNT, so " +
           "a non-finite here is that rule broken or a value the message channel could not carry");
  const n = (k) => esc(String(hostOrder[k]));
  const rankable = hostOrder.hot - (hostOrder.drained === undefined ? 0 : hostOrder.drained);
  const resident =
    hostOrder.hot === 0
      ? "resident order: no instance was rankable this round"
      : !("wTop" in hostOrder)
        ? `resident order: all ${n("hot")} hot instance(s) answered a DRAINED frontier — each said it holds no `
          + `runnable flow, so there was nothing to rank`
        : `resident order over ${esc(String(rankable))} rankable of ${n("hot")} hot: top ${n("wTop")}`
          + ("wRunner" in hostOrder ? `, runner-up ${n("wRunner")} (the value yield floor the winner is handed)` : "")
          + `, bottom ${n("wMin")}`
          + (hostOrder.drained ? `; ${n("drained")} drained and therefore unranked` : "");
  const nonres =
    !("cands" in hostOrder)
      ? "non-resident order: NOT ASKED this round — the round spent its one advance on a navigation, or a "
        + "reservation in flight held admission while the working set was under the floor. This is the order "
        + "not being taken, which is a different fact from its being taken and finding nothing"
      : hostOrder.cands === 0
        ? "non-resident order: asked, and it ranked nothing — no document is waiting for an instance and no "
          + "parked residue was admissible"
        /* "WHEN IT WAS ASKED" IS LOAD-BEARING: this half is read at the top of the round and the pool counts
           in the raw row below are read at its end, so a waiting-document count here that is higher than
           `waiting` there is the round having SEATED one — the difference is what an admission looks like,
           not two rows disagreeing. */
        : `non-resident order over ${n("cands")} work item(s) when it was asked — ${n("candDocs")} waiting document(s)`
          + ("candDocWMax" in hostOrder ? ` (best ${n("candDocWMax")})` : "")
          /* AND THE ROUTES THE APPLICATIONS THEMSELVES DECLARED — the third kind of work item, rendered as its
             own column for the reason the other two are: what this view exists to show is WHICH kind the order
             thought was worth the next instance, and a count folded into a neighbour states the total while
             erasing the comparison. This is also the one column that is entirely this tool's own contribution:
             a waiting document is a page the person opened and a parked frontier is one it opened before, while
             a declared route is an address a bundle NAMED and no link exposes. */
          + `, ${n("candSeeds")} declared route(s)`
          + ("candSeedWMax" in hostOrder ? ` (best ${n("candSeedWMax")})` : "")
          + `, ${n("candCold")} parked frontier(s)`
          + ("candColdWMax" in hostOrder ? ` (best ${n("candColdWMax")})` : "")
          + `: best ${n("candWMax")}, lowest ${n("candWMin")}`
          + (hostOrder.cands > 1 && hostOrder.candWMax === hostOrder.candWMin
              ? " — every one of them ranks IDENTICALLY, which is what a rank frozen at a constant looks like "
                + "from outside; the order between them is then whatever the walk happened to visit first"
              : "");
  const excl = "cands" in hostOrder
    ? `left the order: ${n("exclLive")} parked residue(s) whose address a live or waiting document holds `
      + `(that weight is carried by the instance instead), ${n("exclHeld")} whose key an instance already `
      + `holds, ${n("exclStranded")} whose re-derivation stopped answering, ${n("exclSub")} waiting `
      + `document(s) that are sub-frames their embedder names, and ${n("exclSeedLive")} + `
      + `${n("exclSeedParked")} declared route(s) whose address a live document already holds or a parked `
      + `frontier already carries`
    : "";
  const raw = Object.keys(hostOrder).map((k) => esc(k) + " " + n(k)).join(" · ");
  const rows = [resident, nonres, excl, raw].filter((s) => s !== "")
    .map((s) => `<div class="deep-row"><span class="deep-label">${s}</span></div>`).join("");
  return `<details class="deep-cross-tab"><summary>Level-1 order — across documents, as of round `
       + `${n("round")}${hostOrder.atFloor ? " (at the RAM floor)" : ""}</summary>${rows}</details>`;
}

// ─── Data Panel ──────────────────────────────────────────────────────────────

// A LIST OF URLs AS LINKS, one spelling. The key card wrote this inline for `pageUrls` and the new endpoint
// line needs the same thing, and two copies of it is how one of them ends up escaping a title and the other not.
function _urlListHtml(list) {
  return list.map((u) => {
    if (/^https?:\/\//i.test(u)) return `<a href="${esc(u)}" target="_blank" title="${esc(u)}">${esc(_shortUrl(u))}</a>`;
    return `<span title="${esc(u)}">${esc(_shortUrl(u))}</span>`;
  }).join(", ");
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
    scopeRows = Object.entries(tabData.scopes).sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0));
    for (const [_svc, _list] of scopeRows)
      DCHECK(Array.isArray(_list) && _list.length > 0,
             "a scopes entry is not a non-empty array of scope strings — all three writers (lib/" +
             "response-decode.js, lib/discovery-probe.js, lib/popup-handlers.js) store the split scope list " +
             "ONLY when it has entries, so an empty one is a producer that stopped guarding and this line " +
             "would report a service as requiring no scope (service=" + _svc + ")");
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

  let html = "";

  // Keys section
  if (keys.length) {
    html += '<div class="section-header">Discovered API Keys</div>';
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
      html += `<div class="card">
        <div class="card-label">${esc(info.name)} <span class="badge badge-source" title="where this key was found — the request/response context it was matched in">${esc(info.source)}</span>
          <span class="badge badge-status" title="how many times this key's pattern matched in text this extension scanned — one per match per scanned string (the request URL, each request header, each response, WebSocket and postMessage body, each decoded protobuf field, and every nested base64 level of those). It is not a count of requests, and across documents it is the most any one document's scan saw.">${esc(String(info.requestCount))}&times; seen</span>
        </div>
        <div class="card-value">${esc(key)}</div>`;

      if (hosts.length) {
        html += `<div class="card-meta">${hosts.length === 1 ? "Host" : "Hosts"}: ${hosts.map((h) => `<strong>${esc(h)}</strong>`).join(", ")}</div>`;
      }
      if (services.length) {
        html += `<div class="card-meta">${services.length === 1 ? "Service" : "Services"}: ${services.map((s) => `<code>${esc(s)}</code>`).join(" ")}</div>`;
      }
      /* THE ENDPOINTS THIS KEY WAS SEEN AGAINST — `<host><path>` per lib/keys.js, and until now the set was
         read only for its `.length`, laundered into the "req" badge above. It is the one measured usage fact
         on the record, so it is stated as itself. */
      if (eps.length) {
        html += `<div class="card-meta">${eps.length === 1 ? "Endpoint" : "Endpoints"}: ${eps.map((e2) => `<code>${esc(e2)}</code>`).join(" ")}</div>`;
      }
      if (pageUrls.length) {
        html += `<div class="card-meta">${pageUrls.length === 1 ? "Page" : "Pages"}: ${_urlListHtml(pageUrls)}</div>`;
      }
      /* WHEN AND WHERE, the last fields of the record. lib/keys.js stamps `firstSeen`/`lastSeen` on every
         entry and `origin`/`referer` from the URL the key was matched in; lib/merge.js's own comment records
         that `lastSeen` "was written here and read NOWHERE", and that was true of all four of them.
         `referer` is that URL in full and `origin` is the SAME url's origin, so the address below subsumes it
         — which is why there is one line here and not two, and not because `origin` is unread. The two
         timestamps are asserted; `referer` is guarded because keys.js writes null when the scanned text had no
         source URL, and that null is a positive statement (a body with no address of its own). */
      DCHECK(typeof info.firstSeen === "number" && typeof info.lastSeen === "number",
             "an API key reached the popup without numeric firstSeen/lastSeen — lib/keys.js stamps both when " +
             "it mints the entry and refreshes lastSeen on every later sighting, so a missing one is a " +
             "copier dropping the field");
      html += `<div class="card-meta">first seen ${esc(new Date(info.firstSeen).toLocaleString())}`
            + ` &middot; last seen ${esc(new Date(info.lastSeen).toLocaleString())}`
            + (info.referer ? ` &middot; matched in ${_urlListHtml([info.referer])}` : "")
            + `</div>`;
      html += `</div>`;
    }
  }

  /* WHAT THE OBSERVED TRAFFIC CARRIED, AND WHAT A REPLAY WOULD NEED. Two facts serializeTabData has put on
     this snapshot from the beginning and no view has ever read. Rendered together because they answer one
     question — is the surface below the anonymous one, and what would it take to reach the other. */
  if (hasData) {
    html += '<div class="section-header">Credentials and scopes observed</div>';
    if (auth) {
      const carried = [];
      if (auth.hasAuthorization) carried.push("an <code>Authorization</code> header");
      if (auth.hasCookies) carried.push("cookies");
      // response-decode.js writes the record ONLY inside `if (authorization || cookie)`, so at least one of
      // the two is true whenever it exists. An empty list here is that producer changed shape, and reporting
      // a credentialed document as anonymous is the wrong half of the tool's headline claim.
      DCHECK(carried.length,
             "a document's authContext claims neither an Authorization header nor cookies — " +
             "lib/response-decode.js writes the record only when one of the two was observed, so an empty " +
             "one is that producer broken and this line would report a credentialed document as anonymous");
      html += '<div class="card card-compact"><div class="card-label">Credentialed traffic observed</div>'
           + '<div class="card-meta">requests from this document carried ' + carried.join(" and ")
           + (auth.origin ? ' &middot; <code>Origin: ' + esc(auth.origin) + '</code>' : "")
           + ' — so what this extension learned here includes what those credentials could reach</div></div>';
    } else {
      html += '<div class="card card-compact"><div class="card-label">No credentialed traffic observed</div>'
           + '<div class="card-meta">no request from this document carried an <code>Authorization</code> header '
           + 'or a cookie, so everything learned here is the ANONYMOUS surface — which is the point: the bundle '
           + 'ships the authenticated code path to a logged-out visitor too</div></div>';
    }
    for (const [svc, list] of scopeRows) {
      html += '<div class="card card-compact"><div class="card-label">Required OAuth scopes &mdash; <code>'
           + esc(svc) + '</code></div><div class="card-meta">'
           + list.map((s) => '<code>' + esc(s) + '</code>').join(" ")
           + '</div></div>';
    }
  }

  keysContainer.innerHTML = html;
}



// ─── Send Panel: Endpoint Selection + Schema ─────────────────────────────────

function onSendEndpointSelected() {
  const select = document.getElementById("send-ep-select");
  const epKey = select.value;

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
      currentRequestUrl = "";
      currentRequestMethod = validMethod;
      currentSchema = null;
      setSendPanelVisible(false);
      document.getElementById("send-form-fields").style.display = "block";
      document.getElementById("send-form-fields").innerHTML =
        '<div class="hint">' + esc(svc) + " published no rootUrl or baseUrl, so its methods have no " +
        "address to send to.</div>";
      renderChainInfo(null);
      return;
    }

    // Fix double slashes just in case
    let base = baseUrl;
    if (base.endsWith("/") && pathTemplate.startsWith("/")) {
      base = base.slice(0, -1);
    }

    currentRequestUrl = base + pathTemplate;
    currentRequestMethod = validMethod;

    select.dataset.svc = svc;
    select.dataset.discoveryId = discoveryId;

    // Load schema via background
    loadVirtualSchema(svc, discoveryId);
    return;
  }

  // Fallback if no matching endpoint found — hide everything below the dropdowns
  currentRequestUrl = "";
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
  document.getElementById("send-key-section").classList.add("hidden");
  document.getElementById("send-form-fields").style.display = "block";
  document.getElementById("send-form-fields").innerHTML =
    '<div class="hint">Select a method to load its schema.</div>';
  renderChainInfo(null);
}

async function loadVirtualSchema(service, methodId, initialData = null) {
  currentSchema = null;
  document.getElementById("send-form-fields").innerHTML =
    '<div class="hint">Loading schema...</div>';

  try {
    const schema = await chrome.runtime.sendMessage({
      type: "GET_ENDPOINT_SCHEMA",
      service,
      methodId,
    });

    if (!schema || !schema.method) {
      document.getElementById("send-form-fields").innerHTML =
        '<div class="hint">Method definition not found.</div>';
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
       lib/merge.js — the extension's only `endpoints.set` — writes {url, method, host, path,
       service, source, pageUrl, requiredHeaders, pathParams, firstSeen} and never a content type.
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
    renderKeySelector();
    renderServiceOriginHint();
  } catch (err) {
    // An invariant abort travels ON through here (check.js RETHROW_FATAL). The GET_ENDPOINT_SCHEMA
    // sendMessage genuinely can reject — the offscreen document is not answering — and that IS the
    // "Error loading schema" state this hint is for; the assertion above is not, and without this
    // line it would render in the same hint and read as a transport hiccup.
    RETHROW_FATAL(err);
    console.error("Error loading virtual schema:", err);
    document.getElementById("send-form-fields").innerHTML =
      `<div class="hint">Error loading schema: ${esc(err.message)}</div>`;
  }
}

function renderChainInfo(chains) {
  const container = document.getElementById("send-chain-info");
  if (!container) return;
  if (!chains || (!chains.incoming?.length && !chains.outgoing?.length)) {
    container.classList.add("hidden");
    container.innerHTML = "";
    return;
  }
  container.classList.remove("hidden");
  let html = '<div class="chain-info-box">';
  if (chains.incoming?.length) {
    html += '<div class="chain-section"><span class="chain-section-label">Receives data from:</span>';
    for (const link of chains.incoming) {
      html += `<div class="chain-link chain-incoming">` +
        `<span class="chain-param">${esc(link.paramName)}</span>` +
        `<span class="chain-arrow">&larr;</span>` +
        `<span class="chain-source">${esc(link.sourceMethodId)}</span>` +
        `<span class="chain-field">.${esc(link.sourceFieldPath)}</span>` +
        (link.observedCount > 1 ? `<span class="chain-count">${esc(String(link.observedCount))}x</span>` : "") +
        `</div>`;
    }
    html += '</div>';
  }
  if (chains.outgoing?.length) {
    html += '<div class="chain-section"><span class="chain-section-label">Feeds data to:</span>';
    for (const link of chains.outgoing) {
      html += `<div class="chain-link chain-outgoing">` +
        `<span class="chain-field">${esc(link.sourceFieldPath)}</span>` +
        `<span class="chain-arrow">&rarr;</span>` +
        `<span class="chain-source">${esc(link.targetMethodId)}</span>` +
        `<span class="chain-param">.${esc(link.paramName)}</span>` +
        (link.observedCount > 1 ? `<span class="chain-count">${esc(String(link.observedCount))}x</span>` : "") +
        `</div>`;
    }
    html += '</div>';
  }
  html += '</div>';
  container.innerHTML = html;
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
    return '<span class="badge badge-pending">pending</span>';
  if (status === "error") return '<span class="badge badge-error">error</span>';
  const code = parseInt(status);
  if (code >= 200 && code < 300)
    return `<span class="badge badge-found">${code}</span>`;
  if (code >= 400) return `<span class="badge badge-notfound">${code}</span>`;
  return `<span class="badge">${code}</span>`;
}

let currentReplayRequest = null;

async function replayRequest(reqId, sourceTabId) {
  // Clear previous send result
  lastSendResult = null;
  const sendResp = document.getElementById("send-response");
  if (sendResp) sendResp.style.display = "none";

  // Search the correct log source
  let req;
  if (sourceTabId && allTabsData && allTabsData[sourceTabId]) {
    req = allTabsData[sourceTabId].requestLog.find((r) => String(r.id) === String(reqId));
  }
  if (!req && allTabsData) {
    // The request log is GLOBAL — search every loaded tab's slice rather than
    // the per-document tabData.requestLog (always empty post tabId->documentId
    // migration). With the scope-fixed loadRequestLog, allTabsData is populated
    // for every scope including "active", so the entry is here.
    for (const data of Object.values(allTabsData)) {
      req = data.requestLog.find((r) => String(r.id) === String(reqId));
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
    currentRequestUrl = req.url;
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

        // 2. Match by methodId AND HTTP verb — the same methodId covers
        //    both GET and POST probe variants of an endpoint, so a methodId-
        //    only match picks the first declared variant (often POST) even
        //    when replaying a captured GET, misattributing the schema.
        if (req.methodId && opt.dataset.discoveryId === req.methodId &&
            opt.dataset.method === req.method) {
          opt.selected = true;
          found = true;
          break;
        }

        // 3. Fallback for path-based matching (Non-batch only) — also
        //    filtered by HTTP verb, same rationale as (2).
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
  headersList.innerHTML = "";
  if (req.requestHeaders) {
    for (const [k, v] of Object.entries(req.requestHeaders)) {
      if (k.toLowerCase() === "content-type") continue;
      addHeaderRow(k, v);
    }
  }

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
    const historicalResult = {
      ok: parseInt(req.status) < 400,
      status: req.status,
      headers: req.responseHeaders,
      timing: 0,
      service: req.service,
      methodId: req.methodId,
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
          // diagnosable without spamming on every misreported body.
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
    document.getElementById("send-response-status").innerHTML +=
      ' <span class="badge badge-source ml-8">Historical</span>';
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
