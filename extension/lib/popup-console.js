/* Popup message console (WebSocket + postMessage) — extracted from popup.js (classic script, shares the
   popup global scope + DOM). Live send/receive console for a selected WS/postMessage channel:
   initMsgConsole/refreshMsgConsole render the message log, sendConsoleMessage dispatches. */
// ─── Message Console (WebSocket + postMessage) ─────────────────────────────

async function initMsgConsole(req) {
  currentChannelId = req.channelId;
  currentChannelType = req.method; // "WEBSOCKET" or "POSTMESSAGE"
  // For PM reply: target is the sourceOrigin (who sent to us, we reply back to them)
  currentTargetOrigin = req.sourceOrigin || null;
  currentChannelFrameId = req.frameId ?? null;
  currentChannelDocumentId = req.documentId ?? null;
  // Bind the channel to the tab that captured it. When logFilter=="all"
  // or we're viewing a closed-tab log, `req._tabId` is set to the origin
  // tab during log flattening. Default back to currentTabId when missing
  // (same-tab log view).
  currentChannelTabId = (req._tabId != null) ? req._tabId : currentTabId;
  setBodyMode("msgconsole");

  // Dynamic label based on channel type
  const labelEl = document.querySelector("#send-ws-console .ws-label");
  const urlEl = document.getElementById("ws-console-url");
  if (currentChannelType === "POSTMESSAGE") {
    labelEl.textContent = "postMessage";
    urlEl.textContent = (req.sourceOrigin || "?") + " \u2192 " + (req.targetOrigin || "?");
  } else if (currentChannelType === "MSGCHANNEL") {
    labelEl.textContent = "MessageChannel";
    urlEl.textContent = (req.sourceOrigin || "?") + " \u2192 " + (req.targetOrigin || "?");
  } else {
    labelEl.textContent = "WebSocket";
    urlEl.textContent = req.url;
  }

  // Render all messages from the combined entry
  const historyEl = document.getElementById("ws-console-history");
  historyEl.innerHTML = "";
  const messages = req.messages || [];
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
  let html = "";
  for (const msg of messages) {
    if (msg.dir === "close") {
      const time = new Date(msg.time).toLocaleTimeString();
      html +=
        `<div class="ws-msg ws-msg-close">` +
        `<span class="ws-msg-dir">closed</span> ` +
        `<span class="ws-msg-time">${esc(time)}</span>` +
        (msg.body ? `<pre class="ws-msg-body">${esc(String(msg.body))}</pre>` : "") +
        `</div>`;
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
    html +=
      `<div class="ws-msg ${dirClass}">` +
      `<span class="ws-msg-dir">${esc(dirLabel)}</span> ` +
      `<span class="ws-msg-time">${esc(time)}</span>` +
      `<pre class="ws-msg-body">${esc(bodyText)}</pre></div>`;
  }
  container.innerHTML = html;
}

async function refreshMsgConsole() {
  const statusEl = document.getElementById("ws-console-status");
  const sendBtn = document.getElementById("ws-console-send");
  if (!currentChannelId) {
    statusEl.innerHTML = '<span class="ws-status-closed">NO CONNECTION</span>';
    sendBtn.disabled = true;
    return;
  }

  // Pick the right status message type based on channel
  const statusType = currentChannelType === "POSTMESSAGE" ? "PM_GET_STATUS"
    : currentChannelType === "MSGCHANNEL" ? "MC_GET_STATUS" : "WS_GET_STATUS";
  try {
    const routedTab = currentChannelTabId != null ? currentChannelTabId : currentTabId;
    const result = await chrome.runtime.sendMessage({
      type: statusType, tabId: routedTab, channelId: currentChannelId,
    });

    if (currentChannelType === "POSTMESSAGE" || currentChannelType === "MSGCHANNEL") {
      // postMessage / MessageChannel — always "active", no connection lifecycle
      statusEl.innerHTML = '<span class="ws-status-open">ACTIVE</span>';
      sendBtn.disabled = false;
    } else {
      const names = { 0: "CONNECTING", 1: "OPEN", 2: "CLOSING", 3: "CLOSED" };
      const classes = { 0: "ws-status-connecting", 1: "ws-status-open",
        2: "ws-status-closing", 3: "ws-status-closed" };
      const rs = result.readyState;
      statusEl.innerHTML = `<span class="${classes[rs] || "ws-status-closed"}">${names[rs] || "CLOSED"}</span>`;
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
    statusEl.innerHTML = '<span class="ws-status-closed">UNKNOWN</span>';
    sendBtn.disabled = true;
  }
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
    if (currentChannelType === "POSTMESSAGE") {
      msgPayload = {
        type: "PM_SEND_MSG", tabId: routedTab, channelId: currentChannelId,
        data: data, targetOrigin: currentTargetOrigin || "*",
        documentId: currentChannelDocumentId, frameId: currentChannelFrameId,
      };
    } else if (currentChannelType === "MSGCHANNEL") {
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
    if (result.error) {
      const historyEl = document.getElementById("ws-console-history");
      historyEl.innerHTML +=
        `<div class="ws-msg ws-msg-error">` +
        `<span class="ws-msg-dir">error</span> ${esc(result.error)}</div>`;
    }
  } catch (err) {
    const historyEl = document.getElementById("ws-console-history");
    historyEl.innerHTML +=
      `<div class="ws-msg ws-msg-error">` +
      `<span class="ws-msg-dir">error</span> ${esc(err.message)}</div>`;
  }
  sendBtn.textContent = "Send Message";
  await refreshMsgConsole();
}
