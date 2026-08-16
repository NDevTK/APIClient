// Main-world script: wraps fetch() and XMLHttpRequest to passively capture
// request headers/bodies and response bodies.  Communicates back to the
// isolated-world content script via CustomEvent on the shared document.
//
// Single capture point — replaces chrome.webRequest for all request data.

(function () {
  "use strict";

  const EVENT_NAME = "__uasr_resp";

  // Decide whether to read a body as bytes (base64 for transport) vs. text.
  // THE CONSUMER IS lib/response-decode.js IN THE OFFSCREEN, not background.js —
  // that name was checked and is wrong: the service worker is STATELESS and never
  // relays page data (SECURITY.md), and the magic-byte classification that needs
  // these bytes is `classifyResponseAsset`, called from response-decode.js.
  // The fact the name was attached to is still true and is the reason this split
  // exists: reading a body via Response.text() decodes it as UTF-8 and destroys
  // every non-ASCII byte, so a body classified by its leading bytes must not be
  // read that way. It is the same reading safe-fetch.js now takes for EVERY body
  // it fetches, where the decode belongs to the engine and the bytes cross as
  // bytes — the difference is that this classifier still decides PER BODY which
  // ones survive, and a content type it has not heard of is the case it gets
  // wrong in the direction that loses data.
  function isBinary(ct) {
    if (!ct) return false;
    const l = ct.toLowerCase().split(";")[0].trim();
    // Explicit text families — decode as text.
    if (l.startsWith("text/")) return false;
    if (l.includes("json")) return false;
    if (l.includes("xml")) return false;
    if (l.includes("javascript") || l.includes("ecmascript")) return false;
    if (l === "application/x-www-form-urlencoded") return false;
    if (l === "application/x-component") return false;
    // Anything not known to be text — capture as bytes. Covers image/video/
    // audio/font/model/*, application/pdf|zip|gzip|wasm|octet-stream|x-protobuf|
    // grpc[-web]*, and unknown types.
    return true;
  }

  // ─── Helpers ────────────────────────────────────────────────────────────────

  function uint8ToBase64(bytes) {
    let bin = "";
    for (let i = 0; i < bytes.length; i += 8192) {
      const chunk = bytes.subarray(i, Math.min(i + 8192, bytes.length));
      bin += String.fromCharCode.apply(null, chunk);
    }
    return btoa(bin);
  }

  function _isInternalUrl(url) {
    return url.includes("#_uasr_send") || url.includes("#_internal_probe");
  }

  // ─── Buffered emit ──────────────────────────────────────────────────────────
  // intercept.js loads at document_start but the content script relay loads at
  // document_idle.  Buffer captured responses until the relay signals ready,
  // then replay and switch to live dispatch.

  let _relayReady = false;
  const _buffer = [];

  /* CustomEvent dispatch sends instrumented data to the isolated-world
     content script. A throw means the document is detached (page
     navigated away mid-event) or the event ctor failed — both are
     real but page-noise-prone conditions. Log at DEBUG so a developer
     debugging the relay can see them in the page console without
     spamming a working page. */
  function emit(data) {
    if (_relayReady) {
      try {
        document.dispatchEvent(new CustomEvent(EVENT_NAME, { detail: data }));
      } catch (e) {
        console.debug("[apiclient:emit]", e && e.message || e);
      }
    } else {
      _buffer.push(data);
    }
  }

  document.addEventListener("__uasr_ready", () => {
    _relayReady = true;
    for (const data of _buffer) {
      try {
        document.dispatchEvent(new CustomEvent(EVENT_NAME, { detail: data }));
      } catch (e) {
        console.debug("[apiclient:emit_buffered]", e && e.message || e);
      }
    }
    _buffer.length = 0;
  });

  // ─── WebSocket send command listener ──────────────────────────────────────
  // Receives commands from content.js to send messages through live WebSocket
  // connections.  Security note: a compromised renderer already has direct
  // access to WebSocket objects, so this relay grants no new capability.

  document.addEventListener("__uasr_ws_send", function (e) {
    if (!e.detail) return;
    var wsId = e.detail.wsId, data = e.detail.data, binary = e.detail.binary;
    var ws = _wsConnections.get(wsId);
    if (!ws || ws.readyState !== 1) return;
    try {
      if (binary && typeof data === "string") {
        var bin = atob(data);
        var bytes = new Uint8Array(bin.length);
        for (var i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
        ws.send(bytes.buffer);
      } else {
        ws.send(data);
      }
    } catch (e) {
      // ws.send throws when the socket is CLOSED/CLOSING — expected when the
      // page navigated away or the server hung up. Debug-log so a "popup Send
      // didn't reach the server" diagnosis can confirm a closed-socket cause.
      console.debug("[apiclient:ws_send]", e && e.message || e);
    }
  });


  // ─── Request header/body capture helpers ──────────────────────────────────

  function _captureHeaders(input, init) {
    var h = {};
    try {
      var src = (init && init.headers) || (input instanceof Request ? input.headers : null);
      if (!src) return h;
      if (src instanceof Headers) {
        src.forEach(function (v, k) { h[k] = v; });
      } else if (Array.isArray(src)) {
        for (var i = 0; i < src.length; i++) h[src[i][0].toLowerCase()] = src[i][1];
      } else if (typeof src === "object") {
        for (var k in src) h[k.toLowerCase()] = src[k];
      }
    } catch (_) {}
    return h;
  }

  function _captureBody(bodySource) {
    var reqBody = null, reqBase64 = false;
    try {
      if (bodySource == null) return { body: null, base64: false };
      if (typeof bodySource === "string") {
        reqBody = bodySource;
      } else if (bodySource instanceof ArrayBuffer) {
        reqBody = uint8ToBase64(new Uint8Array(bodySource));
        reqBase64 = true;
      } else if (bodySource instanceof Uint8Array) {
        reqBody = uint8ToBase64(bodySource);
        reqBase64 = true;
      } else if (typeof URLSearchParams !== "undefined" && bodySource instanceof URLSearchParams) {
        reqBody = bodySource.toString();
      } else if (typeof FormData !== "undefined" && bodySource instanceof FormData) {
        var parts = [];
        bodySource.forEach(function (value, key) {
          if (typeof File !== "undefined" && value instanceof File) {
            parts.push(encodeURIComponent(key) + "=" + encodeURIComponent("[File:" + value.name + "]"));
          } else {
            parts.push(encodeURIComponent(key) + "=" + encodeURIComponent(value));
          }
        });
        reqBody = parts.join("&");
      }
      // Blob, ReadableStream — can't serialize simply, skip
    } catch (_) {}
    return { body: reqBody, base64: reqBase64 };
  }

  // ─── fetch() wrapper ───────────────────────────────────────────────────────

  // Capture the bundle's call-site stack at network-API entry, stripping the
  // intercept.js wrapper frames so the consumer gets the bundle's frames
  // first. Used for per-[live]-endpoint provenance (the network-vs-AST diff: a
  // [live]-only endpoint pinpoints the exact bundle function the forced-exec
  // engine didn't reach). Cheap: `new Error().stack` is just a stack walk.
  function _captureCallStack() {
    try {
      var s = (new Error()).stack || "";
      if (!s) return "";
      var lines = s.split("\n");
      var out = [];
      for (var i = 0; i < lines.length; i++) {
        var ln = lines[i];
        if (i === 0 && /^Error/.test(ln)) continue;       // V8 leading "Error" line
        if (/intercept\.js[:?]/.test(ln)) continue;         // strip our wrapper frames
        out.push(ln);
      }
      return out.join("\n");
    } catch (_) { return ""; }
  }

  const _fetch = window.fetch;

  window.fetch = async function (input, init) {
    var callStack = _captureCallStack();
    // Snapshot request data before calling fetch (body may be consumed)
    var reqHeaders = _captureHeaders(input, init);
    var bodySource = (init && init.body !== undefined) ? init.body : null;
    var captured = _captureBody(bodySource);
    var reqBody = captured.body;
    var reqBase64 = captured.base64;

    const response = await _fetch.apply(this, arguments);

    try {
      const raw =
        typeof input === "string"
          ? input
          : input instanceof Request
            ? input.url
            : String(input);
      const url = new URL(raw, location.href).href;

      if (_isInternalUrl(url)) return response;

      const method =
        (init && init.method) ||
        (input instanceof Request ? input.method : "GET");
      const ct = response.headers.get("content-type") || "";

      const clone = response.clone();
      // Read body asynchronously — never blocks the caller
      (async () => {
        try {
          const headers = {};
          clone.headers.forEach((v, k) => {
            headers[k] = v;
          });

          // If body wasn't captured synchronously (Request with stream body),
          // try reading from a cloned Request
          if (reqBody === null && input instanceof Request && !init) {
            try {
              var rc = input.clone();
              var ct2 = reqHeaders["content-type"] || "";
              if (isBinary(ct2)) {
                var ab = await rc.arrayBuffer();
                reqBody = uint8ToBase64(new Uint8Array(ab));
                reqBase64 = true;
              } else {
                reqBody = await rc.text();
              }
            } catch (_) {}
          }

          let body,
            base64Encoded = false;
          if (isBinary(ct)) {
            const buf = await clone.arrayBuffer();
            body = uint8ToBase64(new Uint8Array(buf));
            base64Encoded = true;
          } else {
            body = await clone.text();
          }

          emit({
            url,
            method: method.toUpperCase(),
            status: clone.status,
            // Response.type is "basic" | "cors" | "opaque" | "opaqueredirect"
            // | "error". Opaque is a fact-level signal that the body was
            // cross-origin-no-cors and therefore unreadable — treat as
            // fire-and-forget, not an API response, in the classifier.
            responseType: clone.type || null,
            contentType: ct,
            responseHeaders: headers,
            body,
            base64Encoded,
            requestHeaders: reqHeaders,
            requestBody: reqBody,
            requestBodyBase64: reqBase64,
            callStack: callStack,
          });
        } catch (_) {}
      })();
    } catch (_) {}

    return response;
  };

  // ─── XMLHttpRequest wrapper ─────────────────────────────────────────────────

  const _xhrOpen = XMLHttpRequest.prototype.open;
  const _xhrSend = XMLHttpRequest.prototype.send;
  const _xhrSetHeader = XMLHttpRequest.prototype.setRequestHeader;

  XMLHttpRequest.prototype.open = function (method, url) {
    this.__uasr_method = method;
    this.__uasr_url = url;
    this.__uasr_reqHeaders = {};
    return _xhrOpen.apply(this, arguments);
  };

  XMLHttpRequest.prototype.setRequestHeader = function (name, value) {
    if (!this.__uasr_reqHeaders) this.__uasr_reqHeaders = {};
    this.__uasr_reqHeaders[name.toLowerCase()] = value;
    return _xhrSetHeader.apply(this, arguments);
  };

  XMLHttpRequest.prototype.send = function (sendBody) {
    if (this.__uasr_hooked) return _xhrSend.apply(this, arguments);
    this.__uasr_hooked = true;

    var callStack = _captureCallStack();
    // Capture request body before sending
    var captured = _captureBody(sendBody);
    var _reqHeaders = this.__uasr_reqHeaders || {};
    var _reqBody = captured.body;
    var _reqBase64 = captured.base64;

    this.addEventListener("load", function () {
      try {
        const url = new URL(
          String(this.__uasr_url || ""),
          location.href,
        ).href;

        if (_isInternalUrl(url)) return;

        const ct = this.getResponseHeader("content-type") || "";
        // Collect response headers
        const rawHeaders = this.getAllResponseHeaders();
        const headers = {};
        for (const line of rawHeaders.trim().split(/\r?\n/)) {
          const idx = line.indexOf(":");
          if (idx > 0)
            headers[line.slice(0, idx).trim().toLowerCase()] =
              line.slice(idx + 1).trim();
        }

        let body,
          base64Encoded = false;
        if (this.responseType === "arraybuffer" && this.response) {
          body = uint8ToBase64(new Uint8Array(this.response));
          base64Encoded = true;
        } else if (this.responseType === "" || this.responseType === "text") {
          body = this.responseText;
          if (!body) return;
        } else if (this.responseType === "json") {
          try {
            body = JSON.stringify(this.response);
            if (!body) return;
          } catch (_) {
            return;
          }
        } else {
          return; // blob, document — skip
        }

        emit({
          url,
          method: (this.__uasr_method || "GET").toUpperCase(),
          status: this.status,
          contentType: ct,
          responseHeaders: headers,
          body,
          base64Encoded,
          requestHeaders: _reqHeaders,
          requestBody: _reqBody,
          requestBodyBase64: _reqBase64,
          callStack: callStack,
        });
      } catch (_) {}
    });

    return _xhrSend.apply(this, arguments);
  };

  // ─── WebSocket wrapper ──────────────────────────────────────────────────────

  var _wsIdCounter = 0;
  var _wsConnections = new Map();

  const _WebSocket = window.WebSocket;

  class WrappedWebSocket extends _WebSocket {
    constructor(url, protocols) {
      super(url, protocols);
      const wsUrl = typeof url === "string" ? url : String(url);
      const wsId = "ws_" + (++_wsIdCounter);
      _wsConnections.set(wsId, this);

      this.addEventListener("open", function () {
        try {
          emit({ url: wsUrl, method: "WS_OPEN", wsId: wsId, status: 0,
            contentType: "websocket", responseHeaders: {}, body: null, base64Encoded: false });
        } catch (_) {}
      });

      this.addEventListener("close", function (ev) {
        try {
          emit({ url: wsUrl, method: "WS_CLOSE", wsId: wsId, status: ev.code || 1000,
            contentType: "websocket", responseHeaders: {}, body: ev.reason || "", base64Encoded: false });
        } catch (_) {}
        _wsConnections.delete(wsId);
      });

      // Capture outbound messages
      const _origSend = this.send.bind(this);
      this.send = function (data) {
        try {
          let body, base64Encoded = false;
          if (typeof data === "string") {
            body = data;
          } else if (data instanceof ArrayBuffer) {
            body = uint8ToBase64(new Uint8Array(data));
            base64Encoded = true;
          } else if (data instanceof Uint8Array) {
            body = uint8ToBase64(data);
            base64Encoded = true;
          } else if (typeof Blob !== "undefined" && data instanceof Blob) {
            data.arrayBuffer().then(function (ab) {
              emit({ url: wsUrl, method: "WS_SEND", wsId: wsId, status: 0,
                contentType: "websocket", responseHeaders: {},
                body: uint8ToBase64(new Uint8Array(ab)), base64Encoded: true });
            }).catch(function () {});
            return _origSend(data);
          }
          emit({ url: wsUrl, method: "WS_SEND", wsId: wsId, status: 0,
            contentType: "websocket", responseHeaders: {}, body, base64Encoded });
        } catch (_) {}
        return _origSend(data);
      };

      // Capture inbound messages
      this.addEventListener("message", function (e) {
        try {
          let body, base64Encoded = false;
          if (typeof e.data === "string") {
            body = e.data;
          } else if (e.data instanceof ArrayBuffer) {
            body = uint8ToBase64(new Uint8Array(e.data));
            base64Encoded = true;
          } else if (typeof Blob !== "undefined" && e.data instanceof Blob) {
            e.data.arrayBuffer().then(function (ab) {
              emit({ url: wsUrl, method: "WS_RECV", wsId: wsId, status: 0,
                contentType: "websocket", responseHeaders: {},
                body: uint8ToBase64(new Uint8Array(ab)), base64Encoded: true });
            }).catch(function () {});
            return;
          }
          emit({ url: wsUrl, method: "WS_RECV", wsId: wsId, status: 0,
            contentType: "websocket", responseHeaders: {}, body, base64Encoded });
        } catch (_) {}
      });
    }
  }

  window.WebSocket = WrappedWebSocket;

  // ─── EventSource wrapper ────────────────────────────────────────────────────

  const _EventSource = window.EventSource;

  if (_EventSource) {
    class WrappedEventSource extends _EventSource {
      constructor(url, opts) {
        super(url, opts);
        const esUrl = typeof url === "string" ? url : String(url);

        this.addEventListener("message", function (e) {
          try {
            emit({
              url: esUrl,
              method: "SSE",
              status: 200,
              contentType: "text/event-stream",
              responseHeaders: {},
              body: e.data,
              base64Encoded: false,
            });
          } catch (_) {}
        });
      }
    }

    window.EventSource = WrappedEventSource;
  }

  // ─── Exploit-probe sink beacon ───────────────────────────────────────────────
  //
  // `apiclientsink(id)` is the proof hook a PoC payload calls when the browser
  // ACTUALLY executes it (HTML parser → event handler, eval, javascript: nav).
  // We install it UNCONDITIONALLY — NEVER gated on a URL token. Correlation data
  // (`id`) rides INSIDE the PoC payload (the finding's crypto.randomUUID), never
  // in the URL: a hash/query carries only the genuine attacker payload, and a
  // hash change is not a value sent to the server, so it must not influence
  // analysis. This is NOT prototype-hooking: a call means the browser truly ran
  // the payload through its pipeline (CSP-correct — if CSP blocks the handler,
  // apiclientsink is never called → honestly NOT REPRODUCED).
  //
  // The hit is relayed to the content script (→ offscreen brain, correlated by
  // `id`) via a CustomEvent AND mirrored onto a documentElement attribute so a
  // content script that loads AFTER the sink fired still drains it — the event
  // alone races content.js's document_idle injection.
  function _uasrRelayHit(id) {
    var hit = { id: String(id == null ? "" : id), at: Date.now(), url: location.href };
    try { document.dispatchEvent(new CustomEvent("__uasr_probe_hit", { detail: hit })); } catch (_) {}
    try {
      var el = document.documentElement;
      var prev = el.getAttribute("data-uasr-hits");
      var arr = prev ? JSON.parse(prev) : [];
      arr.push(hit);
      el.setAttribute("data-uasr-hits", JSON.stringify(arr));
    } catch (_) {}
  }
  if (!window.apiclientsink) {
    try {
      Object.defineProperty(window, "apiclientsink", {
        value: function (id) { try { console.log("[apiclientsink] " + id); } catch (_) {} _uasrRelayHit(id); },
        writable: false, configurable: false,
      });
    } catch (_) { window.apiclientsink = function (id) { _uasrRelayHit(id); }; }
  }

})();
