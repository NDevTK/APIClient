/* Popup message console (WebSocket + postMessage) — extracted from popup.js (classic script, shares the
   popup global scope + DOM). Live send/receive console for a selected WS/postMessage channel:
   initMsgConsole/refreshMsgConsole render the message log, sendConsoleMessage dispatches. */
// ─── Message Console (WebSocket + postMessage) ─────────────────────────────

async function initMsgConsole(req) {
  currentChannelId = req.channelId;
  currentChannelType = req.kind; // the record's producer-stated kind, never the page-chosen verb
  /* For a PM reply the target is the page-claimed sourceOrigin — who the renderer says sent to us. It is
     carried VERBATIM, including the empty string the renderer sends when it stated no origin: what an absent
     one may NOT become is `"*"`, which is what sendConsoleMessage used to substitute. See the refusal there. */
  /* ONLY A postMessage/MessageChannel RECORD HAS ONE. A WebSocket connection has no origin pair at all —
     it is not a MessageEvent — and reading `sourceOrigin` off one produced `undefined`, which only ever
     stayed harmless because the reply path consults this value in its postMessage arm alone. Asking the
     record for a field its kind does not carry is how that stops being true silently. */
  currentTargetOrigin = req.kind === "websocket" ? null : req.sourceOrigin;
  /* BOTH ARE WRITTEN BY `_pushGlobalLog` ON EVERY RECORD IT FILES — the documentId under a DCHECK that it
     is a non-empty string, the frameId defaulted to the top-level traversable exactly once, there. The `??
     null` copies here were a SECOND answer to a question already settled: null is what these routing fields
     mean when there is no document to send to, and reading it out of a record that always carries one would
     have addressed a channel reply at no frame in no document with nothing to say so. */
  currentChannelFrameId = req.frameId;
  currentChannelDocumentId = req.documentId;
  // Bind the channel to the tab that captured it. When logFilter=="all"
  // or we're viewing a closed-tab log, `req._tabId` is set to the origin
  // tab during log flattening. Default back to currentTabId when missing
  // (same-tab log view).
  currentChannelTabId = (req._tabId != null) ? req._tabId : currentTabId;
  setBodyMode("msgconsole");

  // Dynamic label based on channel type
  const labelEl = document.querySelector("#send-ws-console .ws-label");
  const urlEl = document.getElementById("ws-console-url");
  // THE ORIGIN PAIR IS THE PAGE'S CLAIM AND SAYS SO \u2014 one statement of that, in popup-reqlog.js, which
  // carries why (the halves are read in the untrusted renderer, and the browser never stated them).
  if (currentChannelType === "postmessage") {
    labelEl.textContent = "postMessage";
    urlEl.textContent = "page-claimed: " + _claimedOriginPair(req);
    urlEl.title = PAGE_CLAIMED_ORIGINS_TITLE;
  } else if (currentChannelType === "msgchannel") {
    labelEl.textContent = "MessageChannel";
    urlEl.textContent = "page-claimed: " + _claimedOriginPair(req);
    urlEl.title = PAGE_CLAIMED_ORIGINS_TITLE;
  } else {
    labelEl.textContent = "WebSocket";
    urlEl.textContent = req.url;
  }

  // Render all messages from the combined entry
  const historyEl = document.getElementById("ws-console-history");
  historyEl.replaceChildren();
  const messages = req.messages;
  _renderConsoleMessages(historyEl, messages);
  historyEl.scrollTop = historyEl.scrollHeight;

  // Pre-fill input with last sent message for easy re-send
  const input = document.getElementById("ws-console-input");
  const lastSent = messages.filter((m) => m.dir === "sent").pop();
  if (lastSent) {
    let bodyText = lastSent.body;
    if (lastSent.base64) {
      try { bodyText = new TextDecoder().decode(base64ToUint8(lastSent.body)); }
      catch (_) {}
    }
    input.value = bodyText;
  } else {
    input.value = "";
  }

  await refreshMsgConsole();
}

function _renderConsoleMessages(container, messages) {
  const rows = [];
  for (const msg of messages) {
    if (msg.dir === "close") {
      const time = new Date(msg.time).toLocaleTimeString();
      rows.push(domElement("div", { class: "ws-msg ws-msg-close" }, [
        domElement("span", { class: "ws-msg-dir" }, ["closed"]), " ",
        domElement("span", { class: "ws-msg-time" }, [time]),
        msg.body ? domElement("pre", { class: "ws-msg-body" }, [String(msg.body)]) : [],
      ]));
      continue;
    }
    let bodyText = msg.body;
    if (msg.base64) {
      try { bodyText = new TextDecoder().decode(base64ToUint8(msg.body)); }
      catch (_) {}
    }
    const dirLabel = msg.dir === "sent" ? "sent" : "received";
    const dirClass = msg.dir === "sent" ? "ws-msg-sent" : "ws-msg-recv";
    const time = new Date(msg.time).toLocaleTimeString();
    rows.push(domElement("div", { class: "ws-msg " + dirClass }, [
      domElement("span", { class: "ws-msg-dir" }, [dirLabel]), " ",
      domElement("span", { class: "ws-msg-time" }, [time]),
      // The body is the renderer's frame payload; an absent one renders as no text.
      domElement("pre", { class: "ws-msg-body" }, [bodyText == null ? "" : String(bodyText)]),
    ]));
  }
  domReplaceChildren(container, rows);
}

// The console's status badge: one span carrying the channel state's class and word.
function _consoleStatus(statusEl, cls, word) {
  domReplaceChildren(statusEl, [domElement("span", { class: cls }, [word])]);
}

// Appends one error row to the console history.
function _consoleError(text) {
  domAppendChildren(document.getElementById("ws-console-history"), [
    domElement("div", { class: "ws-msg ws-msg-error" },
               [domElement("span", { class: "ws-msg-dir" }, ["error"]), " ", text]),
  ]);
}

async function refreshMsgConsole() {
  const statusEl = document.getElementById("ws-console-status");
  const sendBtn = document.getElementById("ws-console-send");
  if (!currentChannelId) {
    _consoleStatus(statusEl, "ws-status-closed", "NO CONNECTION");
    sendBtn.disabled = true;
    return;
  }

  // Pick the right status message type based on channel
  const statusType = currentChannelType === "postmessage" ? "PM_GET_STATUS"
    : currentChannelType === "msgchannel" ? "MC_GET_STATUS" : "WS_GET_STATUS";
  try {
    const routedTab = currentChannelTabId != null ? currentChannelTabId : currentTabId;
    const result = await chrome.runtime.sendMessage({
      type: statusType, tabId: routedTab, channelId: currentChannelId,
    });

    if (currentChannelType === "postmessage" || currentChannelType === "msgchannel") {
      // postMessage / MessageChannel — always "active", no connection lifecycle
      _consoleStatus(statusEl, "ws-status-open", "ACTIVE");
      sendBtn.disabled = false;
    } else {
      const names = { 0: "CONNECTING", 1: "OPEN", 2: "CLOSING", 3: "CLOSED" };
      const classes = { 0: "ws-status-connecting", 1: "ws-status-open",
        2: "ws-status-closing", 3: "ws-status-closed" };
      const rs = result.readyState;
      _consoleStatus(statusEl, classes[rs] || "ws-status-closed", names[rs] || "CLOSED");
      sendBtn.disabled = rs !== 1;
    }

    // Live-update message history
    if (result.messages) {
      const historyEl = document.getElementById("ws-console-history");
      const wasAtBottom = historyEl.scrollTop + historyEl.clientHeight >= historyEl.scrollHeight - 20;
      _renderConsoleMessages(historyEl, result.messages);
      if (wasAtBottom) historyEl.scrollTop = historyEl.scrollHeight;
    }
  } catch (_) {
    _consoleStatus(statusEl, "ws-status-closed", "UNKNOWN");
    sendBtn.disabled = true;
  }
}

/* A REAL TUPLE ORIGIN, which is the only thing HTML §9.3.3 "Posting messages" lets `targetOrigin` be besides
   `*` and `/` — anything else is parsed as a URL and throws when it does not parse. The opaque origin
   serializes as `"null"`, does not parse, and is same-origin with NOTHING; the empty string is the renderer
   having stated none. Both are refusals, not defaults. */
function _isRealTargetOrigin(o) {
  if (typeof o !== "string" || o === "" || o === "null") return false;
  try { return new URL(o).origin === o; } catch (_) { return false; }
}

async function sendConsoleMessage() {
  const input = document.getElementById("ws-console-input");
  const sendBtn = document.getElementById("ws-console-send");
  const data = input.value;
  if (!data || !currentChannelId) return;

  sendBtn.disabled = true;
  sendBtn.textContent = "Sending...";
  try {
    // Route through the channel's OWN tab (captured) not the popup's tab.
    const routedTab = currentChannelTabId != null ? currentChannelTabId : currentTabId;
    let msgPayload;
    if (currentChannelType === "postmessage") {
      /* NO `|| "*"`. `targetOrigin` is the one thing that keeps a postMessage from being readable by whatever
         document the target window holds by the time it lands, and the value here is a page CLAIM that may be
         the empty string (the renderer stated none) or `"null"` (the sender's origin was opaque, which is
         same-origin with nothing and can never be a target). Substituting the wildcard turns a missing
         recipient into EVERY recipient — a human-typed payload broadcast at an origin nobody named. Refuse
         and say so, which is a state the operator can act on. */
      if (!_isRealTargetOrigin(currentTargetOrigin)) {
        _consoleError(
          "not sent: this channel's page-claimed source origin is " +
          (currentTargetOrigin === "" ? "(none stated)" : currentTargetOrigin) + ", which is not an origin a " +
          "reply can be addressed to. A postMessage needs a real target origin; the wildcard would deliver " +
          "this payload to whatever document holds that window.");
        sendBtn.textContent = "Send Message";
        await refreshMsgConsole();   // the same tail the sent path takes — it owns the button's state
        return;
      }
      msgPayload = {
        type: "PM_SEND_MSG", tabId: routedTab, channelId: currentChannelId,
        data: data, targetOrigin: currentTargetOrigin,
        documentId: currentChannelDocumentId, frameId: currentChannelFrameId,
      };
    } else if (currentChannelType === "msgchannel") {
      msgPayload = {
        type: "MC_SEND_MSG", tabId: routedTab, channelId: currentChannelId,
        data: data, documentId: currentChannelDocumentId, frameId: currentChannelFrameId,
      };
    } else {
      msgPayload = {
        type: "WS_SEND_MSG", tabId: routedTab,
        channelId: currentChannelId, data: data,
        documentId: currentChannelDocumentId, frameId: currentChannelFrameId,
      };
    }
    const result = await chrome.runtime.sendMessage(msgPayload);
    if (result.error) _consoleError(String(result.error));
  } catch (err) {
    _consoleError(String(err.message));
  }
  sendBtn.textContent = "Send Message";
  await refreshMsgConsole();
}
