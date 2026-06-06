// Content script: ships RAW materials to the analyser — raw
// server-rendered HTML (CONTENT_HTML), raw script source per
// <script> (SCRIPT_SOURCE), raw network response bodies relayed from
// intercept.js (RESPONSE_BODY). Does NOT do any regex / heuristic
// pre-scan: HTML parsing is Lexbor's job in the worker; endpoint and
// API-key discovery is the forced-execution pipeline's job (real
// fetch/XHR observed at the host edge, with method/args/call site).
// Also acts as a fetch relay for the SW and a form-metadata
// collector for the form-submission pipeline.

(function () {
  // No URL-token probe mode: correlation rides in the PoC payload (the finding's
  // crypto.randomUUID passed to apiclientsink), never in the URL. A PoC target is
  // just the page with an attacker payload in the hash; the hash is not a value
  // sent to the server, so it must not change the analysis decision. Re-analysis
  // is deduped by the brain's code fingerprint (identical scripts ⇒ cache hit ⇒
  // no rescan, no duplicate finding), so there is nothing to skip here.

  // ─── Response Body Relay (must be first — drains intercept.js buffer) ────
  // Threat model: intercept.js (main world) is untrusted — same origin as the page.
  // This relay only forwards to background.js via sendMessage; background validates
  // the RESPONSE_BODY type against the content-script allowlist. See SECURITY.md.

  document.addEventListener("__uasr_resp", (e) => {
    if (!e.detail) return;
    const d = e.detail;
    _sendChunked({
      type: "RESPONSE_BODY",
      url: d.url,
      method: d.method,
      status: d.status,
      contentType: d.contentType,
      responseHeaders: d.responseHeaders,
      body: d.body,
      base64Encoded: d.base64Encoded,
      wsId: d.wsId || null,
      channelId: d.channelId || null,
      sourceOrigin: d.sourceOrigin || null,
      targetOrigin: d.targetOrigin || null,
      requestHeaders: d.requestHeaders || null,
      requestBody: d.requestBody || null,
      requestBodyBase64: d.requestBodyBase64 || false,
      callStack: d.callStack || null,
    }, "body");
  });
  // Signal intercept.js that the relay is listening — replays buffered events
  document.dispatchEvent(new CustomEvent("__uasr_ready"));

  // Exploit-probe hit relay: when a PoC payload executes, intercept.js's
  // apiclientsink(id) fires with the finding's crypto.randomUUID (carried INSIDE
  // the payload, never in the URL). Forward to the offscreen brain, which
  // correlates the hit to the finding/probe session by that id. Two channels,
  // because the CustomEvent races content.js's document_idle injection:
  //   (1) live CustomEvent for hits fired after we're listening, and
  //   (2) a drain of the durable `data-uasr-hits` documentElement attribute for
  //       hits intercept.js mirrored there BEFORE content.js loaded.
  var _uasrSeenHits = new Set();
  function _forwardProbeHit(hit) {
    if (!hit) return;
    var k = (hit.id || "") + "|" + (hit.at || "");
    if (_uasrSeenHits.has(k)) return;
    _uasrSeenHits.add(k);
    try { chrome.runtime.sendMessage({ type: "PROBE_HIT", hit: hit }); }
    catch (err) { console.warn("[content:probe_hit] sendMessage failed:", err && err.message || err); }
  }
  document.addEventListener("__uasr_probe_hit", (e) => { if (e.detail) _forwardProbeHit(e.detail); });
  function _drainHitMirror() {
    try {
      var raw = document.documentElement.getAttribute("data-uasr-hits");
      if (!raw) return;
      JSON.parse(raw).forEach(_forwardProbeHit);
    } catch (_) {}
  }
  _drainHitMirror();
  try {
    new MutationObserver(_drainHitMirror).observe(document.documentElement, {
      attributes: true, attributeFilter: ["data-uasr-hits"],
    });
  } catch (_) {}

  // ─── postMessage Listener ─────────────────────────────────────────────────
  // Runs in isolated world — no main-world wrapper needed. message events are
  // visible here. Stores event.source per origin for reply from console.

  let _pmIdCounter = 0;
  const _pmChannels = new Map(); // origin → pmId
  const _pmSources = new Map(); // origin → event.source (for reply)

  // ─── MessageChannel Port Tracking ───────────────────────────────────────────
  // Ports arrive via event.ports in postMessage transfers. We store them for
  // bidirectional communication and listen for incoming messages.

  let _mcIdCounter = 0;
  const _mcPorts = new Map(); // mcId → MessagePort (for sending from console)

  function _instrumentPort(port, mcId) {
    _mcPorts.set(mcId, port);
    port.addEventListener("message", (e) => {
      try {
        let body;
        try { body = JSON.stringify(e.data); }
        catch (jsonErr) {
          // structured-cloneable data may not round-trip JSON (BigInt, cycles,
          // typed-array refs). Fall back to String() — still a learning signal
          // about the message shape — but log the JSON failure so the request
          // log can be checked for malformed bodies.
          console.debug("[content:mc] JSON.stringify failed for mcId=%s: %s", mcId, jsonErr && jsonErr.message || jsonErr);
          body = String(e.data);
        }
        chrome.runtime.sendMessage({
          type: "RESPONSE_BODY",
          url: location.href,
          method: "MC_RECV",
          channelId: mcId,
          status: 0,
          contentType: "messagechannel",
          responseHeaders: {},
          body: body,
          base64Encoded: false,
        });
      } catch (err) {
        // Extension context invalidated or sendMessage failure — a MessageChannel
        // RESPONSE_BODY is lost. Surface so missing channel traffic in the request
        // log is diagnosable.
        console.warn("[content:mc] sendMessage failed for mcId=%s: %s", mcId, err && err.message || err);
      }
    });
    port.start();
  }

  window.addEventListener("message", (event) => {
    try {
      // Instrument any transferred MessageChannel ports
      if (event.ports && event.ports.length > 0) {
        for (const port of event.ports) {
          const mcId = "mc_" + (++_mcIdCounter);
          _instrumentPort(port, mcId);
          // Notify background that a channel was established
          chrome.runtime.sendMessage({
            type: "RESPONSE_BODY",
            url: location.href,
            method: "MC_OPEN",
            channelId: mcId,
            status: 0,
            contentType: "messagechannel",
            responseHeaders: {},
            body: "",
            base64Encoded: false,
            sourceOrigin: event.origin || "null",
            targetOrigin: location.origin,
          });
        }
      }

      // Filter same-window self-messages (framework noise)
      if (event.source === window) return;
      const from = event.origin || "null";
      if (!_pmChannels.has(from)) _pmChannels.set(from, "pm_" + (++_pmIdCounter));
      const pmId = _pmChannels.get(from);
      if (event.source) _pmSources.set(from, event.source);
      let body;
      try { body = JSON.stringify(event.data); }
      catch (jsonErr) {
        // Same diagnostic as _instrumentPort above — non-JSON-cloneable
        // data falls back to String() but the JSON failure is logged.
        console.debug("[content:pm] JSON.stringify failed for pmId=%s: %s", pmId, jsonErr && jsonErr.message || jsonErr);
        body = String(event.data);
      }
      chrome.runtime.sendMessage({
        type: "RESPONSE_BODY",
        url: location.href,
        method: "PM_RECV",
        channelId: pmId,
        status: 0,
        contentType: "postmessage",
        responseHeaders: {},
        body: body,
        base64Encoded: false,
        sourceOrigin: from,
        targetOrigin: location.origin,
      });
    } catch (err) {
      // postMessage listener catch — covers extension-context-invalidated
      // sendMessage failures + any throw in the brain RPC. A lost PM_RECV
      // means the brain's request log misses cross-origin postMessage
      // traffic, which can hide an exploit's data channel.
      console.warn("[content:pm] PM_RECV dispatch failed:", err && err.message || err);
    }
  }, true);

  // ─── Chunked-message transport ─────────────────────────────────────────
  // chrome.runtime.sendMessage's structured-clone limit is 64 MiB per
  // call. Large raw materials (full server-rendered HTML on a heavy
  // SPA, multi-MB script bodies, large response bodies) need split-
  // and-reassemble — callers still hand the WHOLE payload as one
  // logical unit; this transport slices it into ≤16 MiB chunks under
  // a stream id, background.js's chunk-reassembly layer merges them
  // back before dispatching to the type-specific handler. No truncation,
  // no caps, no heuristics.
  var _streamSeq = 0;
  function _sendChunked(msg, payloadKey) {
    var payload = msg[payloadKey];
    if (typeof payload !== "string") { chrome.runtime.sendMessage(msg); return; }
    // 16 MiB per chunk leaves head-room for envelope fields + UTF-16
    // string-length-vs-byte-size overhead (the 64 MiB cap is bytes
    // post-clone, JS strings are length-counted in code units).
    var CHUNK = 16 * 1024 * 1024;
    if (payload.length <= CHUNK) { chrome.runtime.sendMessage(msg); return; }
    var streamId = "s_" + (++_streamSeq) + "_" + Date.now() + "_" + Math.random().toString(36).slice(2, 8);
    var total = Math.ceil(payload.length / CHUNK);
    for (var i = 0; i < total; i++) {
      var part = Object.assign({}, msg);
      part[payloadKey] = payload.slice(i * CHUNK, (i + 1) * CHUNK);
      part.__chunk = { streamId: streamId, index: i, total: total, payloadKey: payloadKey };
      chrome.runtime.sendMessage(part);
    }
  }

  // Raw-HTML relay (replaces the heuristic key/endpoint regex scan
  // that used to live here): content.js ships the raw server-rendered
  // HTML to the analyser; Lexbor parses it spec-correctly in the
  // worker. Regex string-matching for "/api/" / "/rpc/" / "graphql"
  // / API-key shapes against HTML text was a heuristic shortcut that
  // bypassed the forced-execution pipeline — it produced
  // `source: page_source` endpoints with no method, no params, no
  // real taint info, AND missed every key/endpoint that wasn't a
  // literal in the HTML (anything constructed in JS, ie the entire
  // API surface of a real SPA). Removing it forces the answer to
  // come through the proper path: Lexbor parses the HTML → bundle
  // executes under forced exec → @H records observe real fetch/XHR
  // with method, args, call site.
  function _sendPageHtml(why) {
    var html;
    try { html = document.documentElement.outerHTML; } catch (_) { return; }
    if (!html) return;
    _sendChunked({
      type: "CONTENT_HTML",
      html: html,
      origin: location.origin,
      pageUrl: location.href,
      reason: why,
    }, "html");
  }

  // ─── Helpers ──────────────────────────────────────────────────────────────

  function isBinaryContentType(ct) {
    if (!ct) return false;
    const lower = ct.toLowerCase();
    // JSPB (json+protobuf) is NOT binary wire format
    if (lower.includes("json")) return false;
    return (
      lower.includes("protobuf") ||
      lower.includes("proto") ||
      lower.includes("grpc") ||
      lower.includes("octet-stream") ||
      lower.startsWith("image/") ||
      lower.startsWith("video/") ||
      lower.startsWith("audio/") ||
      lower.includes("application/pdf") ||
      lower.includes("application/zip")
    );
  }

  function uint8ToBase64(bytes) {
    let bin = "";
    for (let i = 0; i < bytes.length; i += 8192) {
      const chunk = bytes.subarray(i, Math.min(i + 8192, bytes.length));
      bin += String.fromCharCode.apply(null, chunk);
    }
    return btoa(bin);
  }

  function base64ToUint8(b64) {
    const bin = atob(b64);
    const out = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
  }

  // ─── Fetch relay: background → content script → response ──────────────────

  async function handlePageFetch(msg) {
    // Strip browser-managed headers
    const headers = { ...(msg.headers || {}) };
    delete headers["Cookie"];
    delete headers["cookie"];
    delete headers["Origin"];
    delete headers["origin"];
    delete headers["Referer"];
    delete headers["referer"];

    const opts = {
      method: msg.method || "GET",
      credentials: "same-origin",
      headers,
    };

    if (msg.body != null) {
      opts.body =
        msg.bodyEncoding === "base64" ? base64ToUint8(msg.body) : msg.body;
    }

    try {
      const fetchUrl = msg.url + (msg.url.includes("#") ? "&" : "#") + "_uasr_send";
      const resp = await fetch(fetchUrl, opts);
      const respHeaders = {};
      resp.headers.forEach((v, k) => {
        respHeaders[k] = v;
      });
      const ct = resp.headers.get("content-type") || "";

      if (isBinaryContentType(ct)) {
        const buf = await resp.arrayBuffer();
        return {
          ok: resp.ok,
          status: resp.status,
          statusText: resp.statusText,
          headers: respHeaders,
          body: uint8ToBase64(new Uint8Array(buf)),
          bodyEncoding: "base64",
        };
      }

      const body = await resp.text();
      return {
        ok: resp.ok,
        status: resp.status,
        statusText: resp.statusText,
        headers: respHeaders,
        body,
      };
    } catch (err) {
      return { error: err.message };
    }
  }

  // Listen for messages from the background service worker.
  // Threat model: this content script runs in the web page's renderer process.
  // It only accepts PING and PAGE_FETCH from background — never reads storage
  // or handles data-returning message types. See SECURITY.md.
  chrome.runtime.onMessage.addListener((msg, _sender, sendResponse) => {
    if (msg.type === "PING") {
      sendResponse({ ok: true });
      return;
    }
    if (msg.type === "RESHIP") {
      // The offscreen brain came up AFTER our initial ship and broadcasts RESHIP
      // so we re-send everything (the cold-start delivery race — the offscreen
      // wasn't alive when content.js shipped at document_idle, so the brain
      // missed our scripts/HTML). _bufferScript/_buffer dedup by url/hash, so
      // re-ship is idempotent. Clear our local sent-set so we walk fresh.
      _sentScripts.clear();
      _scriptOrder = 0;
      _sendPageHtml("reship");
      try { document.querySelectorAll("script").forEach(extractScriptSource); } catch (_) {}
      if (document.readyState === "complete") _signalScriptsLoaded();
      sendResponse({ ok: true });
      return;
    }
    if (msg.type === "PAGE_FETCH") {
      handlePageFetch(msg).then(sendResponse);
      return true;
    }
    if (msg.type === "WS_SEND_MSG") {
      document.dispatchEvent(new CustomEvent("__uasr_ws_send", {
        detail: { wsId: msg.wsId, data: msg.data, binary: msg.binary || false }
      }));
      sendResponse({ ok: true });
      return;
    }
    if (msg.type === "PM_SEND_MSG") {
      if (!msg.targetOrigin) {
        sendResponse({ error: "targetOrigin is required" });
        return;
      }
      // Allow "*" only for sandboxed iframes (null origin) — otherwise require explicit origin
      const origin = msg.targetOrigin;
      const lookupKey = origin === "*" ? "null" : origin;
      const source = _pmSources.get(lookupKey);
      if (!source) {
        sendResponse({ error: "No source window for origin " + lookupKey });
        return;
      }
      if (origin === "*" && lookupKey !== "null") {
        sendResponse({ error: "Wildcard targetOrigin only allowed for sandboxed iframes" });
        return;
      }
      try {
        let data = msg.data;
        try { data = JSON.parse(data); } catch (_) {}
        source.postMessage(data, origin);
        sendResponse({ ok: true });
      } catch (err) {
        sendResponse({ error: err.message });
      }
      return;
    }
    if (msg.type === "MC_SEND_MSG") {
      const port = _mcPorts.get(msg.channelId);
      if (!port) {
        sendResponse({ error: "No port for channel " + msg.channelId });
        return;
      }
      try {
        let data = msg.data;
        try { data = JSON.parse(data); } catch (_) {}
        port.postMessage(data);
        sendResponse({ ok: true });
      } catch (err) {
        sendResponse({ error: err.message });
      }
      return;
    }
  });

  // ─── Scan Dedup ────────────────────────────────────────────────────────────
  // Track hashes of last sent scan results to avoid re-sending identical data
  // on SPA re-renders or MutationObserver re-triggers without actual DOM changes.

  var _lastScanHash = null;

  function _simpleHash(str) {
    var h = 0;
    for (var i = 0; i < str.length; i++) {
      h = ((h << 5) - h + str.charCodeAt(i)) | 0;
    }
    return h;
  }

  // ─── Init ──────────────────────────────────────────────────────────────────

  // Initial raw-HTML ship — Lexbor in the worker parses this as the
  // analysed document, so subsequent forced execution sees a real
  // server-rendered DOM (custom elements with their attributes, data
  // islands, the structure the bundle's connectedCallback / React
  // effects depend on).
  var _lastHtmlHash = null;
  _sendPageHtml("init");

  // Forms are NOT scanned here — Lexbor in the engine worker parses
  // the same HTML and __hostDrive calls form.submit() on each form,
  // routing through the same fetch hook the bundle's own JS reaches.
  // One execution flow.

  /* No content-side DOM walker. Lexbor inside the QuickJS worker
     re-parses the CONTENT_HTML message into the same spec DOM the
     bundle reads via document.querySelector / dataset / etc. Any
     "DOM context" the analyser needs is read FROM the engine,
     never shipped as a separate snapshot from the page context. */

  // ─── Script Source Extraction (for AST analysis) ────────────────────────────

  const _sentScripts = new Set(); // track URLs/hashes already sent
  // Monotonic DOM-execution order. querySelectorAll("script") yields
  // document order; mutation-added scripts get later indices. Combined
  // analysis MUST concatenate in this order or a bundle whose later
  // chunk reads state a former chunk set up (e.g. GitHub's app chunk
  // reading client-env loaded by environment-*.js) throws
  // "requested before it was loaded" when the order is fetch-arrival.
  var _scriptOrder = 0;

  function sendScriptSource(url, code, order) {
    if (!code) return;
    // No size threshold — CLAUDE.md "no magic numbers, no depth/size
    // caps". A 30-char `document.body.innerHTML=location.hash` is a
    // real reflective XSS and must reach the analyzer like any other.
    // Large bundle scripts (multi-MB) route through _sendChunked so
    // the 64 MiB per-message structured-clone cap doesn't truncate.
    var key = url || hashCode(code);
    if (_sentScripts.has(key)) return;
    _sentScripts.add(key);
    _sendChunked({
      type: "SCRIPT_SOURCE",
      url: url || null,
      code: code,
      order: order == null ? _scriptOrder++ : order,
    }, "code");
  }

  function hashCode(str) {
    var h = 0;
    for (var i = 0; i < Math.min(str.length, 200); i++) {
      h = ((h << 5) - h + str.charCodeAt(i)) | 0;
    }
    return "inline:" + h;
  }

  function extractScriptSource(scriptEl) {
    var sType = scriptEl.type;
    var order = _scriptOrder++;
    // Skip non-JavaScript script types entirely. Data islands
    // (<script type="application/json">, ld+json, importmaps,
    // speculationrules, text/template, text/html) are part of the
    // CONTENT_HTML payload — Lexbor parses them into the spec DOM,
    // and the bundle reads them via getElementById(id).textContent
    // → JSON.parse through the engine's normal DOM hooks. No
    // parallel SCRIPT_SOURCE shipping of island bodies.
    if (sType && sType !== "text/javascript" && sType !== "module" &&
        !/^(application\/javascript|text\/ecmascript)$/i.test(sType)) {
      return;
    }
    // One message per document: content.js ships ONLY CONTENT_HTML. The engine
    // (Lexbor) parses it and runs EVERY script in document order in one realm —
    // inline via the SSR phase, external <script src> via __feLoadScript, dynamic
    // via the createElement host edge. No per-script SCRIPT_SOURCE shipping (it
    // merged a tab's documents and ambiguated the per-document credentialed-read
    // principal).
    return;
  }

  // Extract all existing scripts on page load
  document.querySelectorAll("script").forEach(extractScriptSource);

  // Page-lifecycle signal: when the load event fires, all initial scripts
  // (including <script defer> and <script async>) have finished loading.
  // Tell background to fire analysis NOW even if its 1500ms idle-debounce
  // timer hasn't expired (continuous script-load streams reset the timer
  // forever on busy pages, so it never fires on its own).
  function _signalScriptsLoaded() {
    chrome.runtime.sendMessage({ type: "SCRIPTS_LOADED" });
  }
  if (document.readyState === "complete") _signalScriptsLoaded();
  else window.addEventListener("load", _signalScriptsLoaded, { once: true });

  // ─── Mutation Observer (Dynamic Loading) ───────────────────────────────────

  let debounceTimer = null;
  let _pendingHtmlResend = false;

  const observer = new MutationObserver((mutations) => {
    let changed = false;
    let domGrew = false;

    for (const mutation of mutations) {
      for (const node of mutation.addedNodes) {
        if (node.nodeType === Node.ELEMENT_NODE) {
          if (node.tagName === "SCRIPT") {
            // Raw script source — ships through SCRIPT_SOURCE; no
            // regex pre-scan of textContent.
            extractScriptSource(node);
          }
          // No content-side form metadata extraction — Lexbor in the
          // engine worker re-parses CONTENT_HTML and __hostDrive drives
          // form.submit() through the same fetch hook the bundle reaches.
          domGrew = true;
        }
      }
    }

    // Any DOM growth (SPA route, lazy-loaded fragment, framework
    // re-render) re-ships the raw HTML so the worker's Lexbor document
    // tracks the live page. Debounced so a mutation storm collapses to
    // one CONTENT_HTML message.
    if (domGrew) _pendingHtmlResend = true;

    if (changed || domGrew) {
      if (debounceTimer) clearTimeout(debounceTimer);
      debounceTimer = setTimeout(flushPending, 1000);
    }
  });

  observer.observe(document.documentElement, {
    childList: true,
    subtree: true,
  });

  function flushPending() {
    debounceTimer = null;
    if (_pendingHtmlResend) {
      _pendingHtmlResend = false;
      // De-dup against the prior HTML so identical re-renders don't
      // churn the worker.
      var html;
      try { html = document.documentElement.outerHTML; } catch (_) { return; }
      if (html) {
        var hh = _simpleHash(html);
        if (hh !== _lastHtmlHash) {
          _lastHtmlHash = hh;
          chrome.runtime.sendMessage({
            type: "CONTENT_HTML",
            html: html,
            origin: location.origin,
            pageUrl: location.href,
            reason: "mutation",
          });
        }
      }
    }
  }

  // ─── Form Submission Capture ──────────────────────────────────────────────

  document.addEventListener("submit", function (e) {
    if (!e.target || e.target.tagName !== "FORM") return;
    try {
      var form = e.target;
      // canParse guard so we don't use throw as a parse-validity test. Action
      // resolution defaults to location.href when the form's action attribute
      // is malformed — same as the previous catch fallback, surfaced explicitly.
      var rawAction = form.action || location.href;
      var action = URL.canParse(rawAction, location.href)
        ? new URL(rawAction, location.href).href
        : location.href;

      var method = (form.method || "GET").toUpperCase();
      var enctype = form.enctype || "application/x-www-form-urlencoded";

      var fd;
      try { fd = new FormData(form); } catch (_) { return; }

      // Serialize form fields as key=value pairs
      var fields = [];
      fd.forEach(function (value, key) {
        if (typeof File !== "undefined" && value instanceof File) {
          fields.push({ name: key, value: "[File:" + value.name + "]" });
        } else {
          fields.push({ name: key, value: value });
        }
      });

      if (fields.length === 0) return;

      // For GET forms, build the full URL with query params
      var url = action;
      if (method === "GET") {
        var getUrl = new URL(action);
        for (var i = 0; i < fields.length; i++) {
          if (fields[i].value.indexOf("[File:") !== 0) {
            getUrl.searchParams.set(fields[i].name, fields[i].value);
          }
        }
        url = getUrl.href;
      }

      chrome.runtime.sendMessage({
        type: "CONTENT_FORM_SUBMIT",
        url: url,
        method: method,
        enctype: enctype,
        fields: fields,
        origin: location.origin,
        pageUrl: location.href,
      });
    } catch (_) {}
  }, true);
})();
