/* Popup request-log Response Panel (virtual scroll) — extracted from popup.js (classic script, shares the
   popup global scope + DOM). Renders the captured-traffic log as virtualized cards (_renderLogCard /
   _renderVisibleSlice) so a long log stays responsive; renderResponsePanel is the entry. */
// ─── Response Panel (Request Log) — Virtual Scroll ──────────────────────────

/* AN ORIGIN PAIR ON A postMessage / MessageChannel RECORD IS THE PAGE'S CLAIM, AND IT IS RENDERED AS ONE.
   Both halves are read off a real MessageEvent — but in `content.js`, inside the RENDERER, which SECURITY.md
   classes UNTRUSTED ("a compromised renderer controls them"). They do NOT reach the engine (no `sourceOrigin`
   consumer in bridge.js), so no bundle's `event.origin` check is decided by one and the rule that the engine
   never gets a forgeable origin is intact. What was wrong is only here: `A → B` printed an untrusted string
   in the same visual register as a browser-stated fact, which is SECURITY.md's "the sender's origin is
   stamped by the trusted zone and never taken from the message" broken at the presentation layer.
   AN EMPTY HALF IS A STATEMENT, NOT A HOLE — response-decode.js stores `msg.sourceOrigin || ""`, so "" is the
   renderer having sent no origin at all, and `|| "?"` erased the difference between that and an origin the
   renderer named. */
const PAGE_CLAIMED_ORIGINS_TITLE =
  "Page-CLAIMED: read off a MessageEvent inside the page's own renderer, which this extension does not " +
  "trust. The browser never stated these. Context, not evidence.";
function _claimedOriginPair(req) {
  DCHECK(typeof req.sourceOrigin === "string" && typeof req.targetOrigin === "string",
         "a channel record reached the request log without both origin halves — response-decode.js writes " +
         "each as a string on every POSTMESSAGE/MSGCHANNEL entry it creates, and the empty string is the " +
         "renderer having stated none");
  const half = (o) => (o === "" ? "(none stated)" : o);
  return half(req.sourceOrigin) + " → " + half(req.targetOrigin);
}

function _renderLogCard(req, showTabLabel) {
  const hasProto = !!req.decodedBody;

  // Combined WebSocket entry — show message counts and connection status
  if (req.method === "WEBSOCKET") {
    const msgs = req.messages || [];
    const sentCount = msgs.filter((m) => m.dir === "sent").length;
    const recvCount = msgs.filter((m) => m.dir === "recv").length;
    const statusClass = req.wsOpen ? "ws-status-open" : "ws-status-closed";
    const statusText = req.wsOpen ? "OPEN" : "CLOSED";
    return `<div class="card request-card clickable-card mb-8" data-id="${esc(String(req.id))}" data-tab-id="${esc(String(req._tabId))}">
    <div class="card-label flex-between">
      <span>
        <span class="badge badge-ws">WEBSOCKET</span>
        <span class="text-timestamp">${new Date(req.timestamp).toLocaleTimeString()}</span>
        ${showTabLabel ? `<span class="badge badge-tab">${esc(req._tabTitle || "Tab " + req._tabId)}</span>` : ""}
      </span>
      <span class="badge ${esc(statusClass)}">${esc(statusText)}</span>
    </div>
    <div class="card-value card-value-mono">${esc(req.url)}</div>
    <div class="card-meta">
      ${req.service ? `Service: <strong>${esc(req.service)}</strong>` : ""}
      <span class="ws-msg-counts">${sentCount} sent / ${recvCount} received</span>
    </div>
  </div>`;
  }

  // Combined postMessage entry — show origin pair and message counts
  if (req.method === "POSTMESSAGE") {
    const msgs = req.messages || [];
    const sentCount = msgs.filter((m) => m.dir === "sent").length;
    const recvCount = msgs.filter((m) => m.dir === "recv").length;
    const origins = _claimedOriginPair(req);
    return `<div class="card request-card clickable-card mb-8" data-id="${esc(String(req.id))}" data-tab-id="${esc(String(req._tabId))}">
    <div class="card-label flex-between">
      <span>
        <span class="badge badge-pm">POSTMESSAGE</span>
        <span class="text-timestamp">${new Date(req.timestamp).toLocaleTimeString()}</span>
        ${showTabLabel ? `<span class="badge badge-tab">${esc(req._tabTitle || "Tab " + req._tabId)}</span>` : ""}
      </span>
      <span class="badge ws-status-open">ACTIVE</span>
    </div>
    <div class="card-value card-value-mono" title="${esc(PAGE_CLAIMED_ORIGINS_TITLE)}">page-claimed: ${esc(origins)}</div>
    <div class="card-meta">
      ${req.service ? `Service: <strong>${esc(req.service)}</strong>` : ""}
      <span class="ws-msg-counts">${recvCount} received${sentCount ? ` / ${sentCount} replied` : ""}</span>
    </div>
  </div>`;
  }

  // Combined MessageChannel entry — show origin pair and message counts
  if (req.method === "MSGCHANNEL") {
    const msgs = req.messages || [];
    const sentCount = msgs.filter((m) => m.dir === "sent").length;
    const recvCount = msgs.filter((m) => m.dir === "recv").length;
    const origins = _claimedOriginPair(req);
    return `<div class="card request-card clickable-card mb-8" data-id="${esc(String(req.id))}" data-tab-id="${esc(String(req._tabId))}">
    <div class="card-label flex-between">
      <span>
        <span class="badge badge-mc">MSGCHANNEL</span>
        <span class="text-timestamp">${new Date(req.timestamp).toLocaleTimeString()}</span>
        ${showTabLabel ? `<span class="badge badge-tab">${esc(req._tabTitle || "Tab " + req._tabId)}</span>` : ""}
      </span>
      <span class="badge ws-status-open">ACTIVE</span>
    </div>
    <div class="card-value card-value-mono" title="${esc(PAGE_CLAIMED_ORIGINS_TITLE)}">page-claimed: ${esc(origins)}</div>
    <div class="card-meta">
      ${req.service ? `Service: <strong>${esc(req.service)}</strong>` : ""}
      <span class="ws-msg-counts">${recvCount} received${sentCount ? ` / ${sentCount} sent` : ""}</span>
    </div>
  </div>`;
  }

  return `<div class="card request-card clickable-card mb-8" data-id="${esc(String(req.id))}" data-tab-id="${esc(String(req._tabId))}">
    <div class="card-label flex-between">
      <span>
        <span class="badge ${esc(req.method)}">${esc(req.method)}</span>
        <span class="text-timestamp">${new Date(req.timestamp).toLocaleTimeString()}</span>
        ${showTabLabel ? `<span class="badge badge-tab">${esc(req._tabTitle || "Tab " + req._tabId)}</span>` : ""}
      </span>
      ${getStatusBadge(req.status)}
    </div>
    <div class="card-value card-value-mono">${esc(req.url)}</div>
    <div class="card-meta">
      ${req.service ? `Service: <strong>${esc(req.service)}</strong>` : ""}
      ${hasProto ? ' <span class="badge badge-found">PROTOBUF</span>' : ""}
      ${req.url.includes("batchexecute") ? ' <span class="badge badge-batch">BATCHEXECUTE</span>' : ""}
      ${isGrpcWeb(req.mimeType || req.contentType || "") ? ' <span class="badge badge-grpc">gRPC-WEB</span>' : ""}
      ${isSSE(req.mimeType || "") ? ' <span class="badge badge-sse">SSE</span>' : ""}
      ${isNDJSON(req.mimeType || "") ? ' <span class="badge badge-ndjson">NDJSON</span>' : ""}
      ${isGraphQLUrl(req.url) ? ' <span class="badge badge-graphql">GRAPHQL</span>' : ""}
      ${isMultipartBatch(req.mimeType || "") ? ' <span class="badge badge-multipart">MULTIPART</span>' : ""}
      ${/\/async\//.test(req.url) ? ' <span class="badge badge-batch">ASYNC</span>' : ""}
      ${req.method === "SSE" ? ' <span class="badge badge-sse">SSE</span>' : ""}
    </div>
  </div>`;
}

function _renderVisibleSlice() {
  if (_vs.rendering) return;
  const container = document.getElementById("response-log");
  const scrollEl = document.getElementById("panel-response");
  const n = _vs.entries.length;
  if (!n) return;

  const rh = _vs.estHeight;
  const buf = _vs.buffer;
  // Subtract the header/search offset so index math is relative to the first card
  const logOffset = container.offsetTop;
  const scrollTop = Math.max(0, scrollEl.scrollTop - logOffset);
  const viewH = scrollEl.clientHeight;

  // Calculate visible range using fixed row height (no feedback loop)
  let startIdx = Math.max(0, Math.floor(scrollTop / rh) - buf);
  let endIdx = Math.min(n - 1, Math.ceil((scrollTop + viewH) / rh) + buf);

  // Skip re-render if visible range unchanged
  if (startIdx === _vs.lastStart && endIdx === _vs.lastEnd) return;

  _vs.lastStart = startIdx;
  _vs.lastEnd = endIdx;

  _vs.rendering = true;

  const topPad = startIdx * rh;
  const totalHeight = n * rh;
  const showTabLabel = logFilter !== "active";

  // Fixed minHeight keeps scroll range stable regardless of actual card heights
  container.style.minHeight = totalHeight + "px";

  let html = '<div id="vs-top-spacer"></div>';
  for (let i = startIdx; i <= endIdx; i++) {
    html += _renderLogCard(_vs.entries[i], showTabLabel);
  }

  container.innerHTML = html;
  document.getElementById("vs-top-spacer").style.height = topPad + "px";

  // Attach click handlers
  container.querySelectorAll(".request-card").forEach((c) => {
    c.onclick = () => {
      const sourceTabId = c.dataset.tabId ? parseInt(c.dataset.tabId, 10) : undefined;
      replayRequest(c.dataset.id, sourceTabId);
    };
  });

  // Self-correct estHeight from measured card heights to prevent scroll oscillation
  const cards = container.querySelectorAll(".request-card");
  if (cards.length > 0) {
    let totalH = 0;
    cards.forEach((c) => { totalH += c.offsetHeight + 8; });
    const avgH = Math.round(totalH / cards.length);
    if (Math.abs(avgH - _vs.estHeight) > 10) {
      _vs.estHeight = avgH;
      container.style.minHeight = (n * avgH) + "px";
    }
  }

  _vs.rendering = false;
}

function renderResponsePanel() {
  const container = document.getElementById("response-log");
  const scrollEl = document.getElementById("panel-response");

  // Build entry list. The request log is GLOBAL (no per-document slice), so ALL
  // scopes render from allTabsData — loadRequestLog populated it via GET_ALL_LOGS
  // with the right filter ("active"->active tab, "all"->every tab, number->that tab).
  let entries = [];
  if (allTabsData) {
    for (const [tidStr, data] of Object.entries(allTabsData)) {
      const tid = parseInt(tidStr, 10);
      const meta = data.meta || {};
      for (const req of data.requestLog) {
        entries.push({ ...req, _tabId: tid, _tabTitle: meta.title || `Tab ${tid}` });
      }
    }
    entries.sort((a, b) => b.timestamp - a.timestamp);
  }

  // Apply text search filter
  if (logSearchQuery) {
    entries = entries.filter((r) => {
      return (r.url && r.url.toLowerCase().includes(logSearchQuery)) ||
        (r.method && r.method.toLowerCase().includes(logSearchQuery)) ||
        (r.service && r.service.toLowerCase().includes(logSearchQuery)) ||
        (r.mimeType && r.mimeType.toLowerCase().includes(logSearchQuery)) ||
        (r._tabTitle && r._tabTitle.toLowerCase().includes(logSearchQuery));
    });
  }

  // Fingerprint: skip full rebuild if entries unchanged
  const lastId = entries.length > 0 ? (entries[entries.length - 1].id || entries[entries.length - 1].timestamp) : "";
  const fp = entries.length + ":" + lastId;
  if (fp === _lastLogFp && _vs.scrollHandler) return;
  _lastLogFp = fp;

  // Detach old scroll handler if entries changed
  if (_vs.scrollHandler) {
    scrollEl.removeEventListener("scroll", _vs.scrollHandler);
    _vs.scrollHandler = null;
  }

  _vs.entries = entries;
  _vs.lastStart = -1;
  _vs.lastEnd = -1;

  if (entries.length === 0) {
    container.style.minHeight = "";
    container.innerHTML = '<div class="empty">No requests captured yet.</div>';
    return;
  }

  // Render initial visible slice
  _renderVisibleSlice();

  // Attach scroll-driven rendering
  let rafPending = false;
  _vs.scrollHandler = () => {
    if (rafPending) return;
    rafPending = true;
    requestAnimationFrame(() => {
      rafPending = false;
      _renderVisibleSlice();
    });
  };
  scrollEl.addEventListener("scroll", _vs.scrollHandler, { passive: true });
}
