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
  // these bytes is `classifyResponseAsset` in lib/discovery.js, called from
  // lib/response-decode.js.
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
      } else if (ArrayBuffer.isView(bodySource)) {
        /* Fetch §5.2 BodyInit unions admits a BufferSource, which is ANY ArrayBufferView — every typed array
           and DataView, not just Uint8Array. The `instanceof Uint8Array` test that stood here left
           `fetch(u, {method:"POST", body: new Int32Array(…)})` and every DataView body returning a null
           reqBody, which the log record then reported as a POST that carried none. A view is read through its
           own window on the buffer (byteOffset/byteLength), never the whole buffer, or a subarray would
           capture bytes the request did not send. */
        reqBody = uint8ToBase64(new Uint8Array(bodySource.buffer, bodySource.byteOffset, bodySource.byteLength));
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

  /* A PAGE-SUPPLIED VALUE IS READ THROUGH PAGE CODE, AND THAT IS THE ONLY THING THIS FILE MAY CATCH.
     intercept.js is injected into the page's OWN realm (manifest content_scripts, "world": "MAIN",
     run_at document_start), so a `fetch()` argument, the prototypes it reaches and every getter on it belong
     to the PAGE: `input.url` can be a getter that throws, `String(input)` runs the page's own
     `Symbol.toPrimitive`, and `init` is whatever object the page passed. A throw from one of those is the
     page misbehaving — or deliberately defending against instrumentation — never a bug in this file, and the
     honest consequence is that THIS REQUEST COULD NOT BE OBSERVED: the capture is dropped and the page's own
     fetch is untouched.

     IT IS A NAMED OPERATION RATHER THAN A `try` WRAPPED ROUND A PARAGRAPH. The paragraph it replaces held
     this file's OWN logic too — the internal-URL test, the response clone, the record composition — under one
     silent `catch (_) {}`, so a defect in any of those was indistinguishable from a page that refused to be
     read. This file is the PASSIVE-OBSERVATION half of the tool, the one CLAUDE.md calls a thermometer: when
     it goes quietly wrong the number it reports goes quietly wrong with it. So the page's failures are
     swallowed HERE, by name, at debug volume, and nothing else is swallowed anywhere.

     THERE IS NO DCHECK ON THIS PATH AND THERE MUST NOT BE. extension/check.js is loaded into the ISOLATED
     world only (manifest content_scripts, second entry) — this realm is UNTRUSTED per SECURITY.md, and the
     values it reads are the page's, which CLAUDE.md's own carve-out names as exactly what must NOT be
     asserted ("a forced-exec flow THROWING on opaque/attacker input ... is the exploration surface, not a
     broken invariant"). The sentinel is the statement instead: a caller compares against it and says what
     the absence MEANS, rather than continuing over a value it never got. */
  var _PAGE_THREW = { apiclientPageThrew: true };
  function _pageOwned(read) {
    try {
      return read();
    } catch (e) {
      console.debug("[apiclient:page-owned read threw]", (e && e.message) || e);
      return _PAGE_THREW;
    }
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

    /* THE REQUEST'S ADDRESS AND VERB, AS Fetch §5.4 "Request class" COMPUTES THEM. This is a STATEMENT of
       that constructor's own precedence, not a hole three defaults fill:
         - "If input is a string ... Set request to a new request whose URL is parsedURL", and a new request's
           method is Fetch §2.2.5 "Requests" verbatim — "A request has an associated method (a method). Unless
           stated otherwise it is `GET`." So the trailing "GET" is the SPEC's answer, never an invented one.
         - "Otherwise: Assert: input is a Request object. Set request to input's request." — so both answers
           come off the Request, read through §5.4's own getters ("The url getter steps are to return this's
           request's URL, serialized"; "The method getter steps are to return this's request's method").
         - and last in the constructor steps, "If init["method"] exists, then: ... Set request's method to
           method" — which is why an init method WINS over the other two.
       AN ABSENT `init.method` IS THEREFORE NOT A MISSING DATUM. It is the page declaring that the answer is
       one of the other two, and this expression reads it as that. `init.method === ""` cannot reach here at
       all: §5.4 throws a TypeError for a value that "is not a method", so the await above would have rejected
       and this line would never run. */
    const seen = _pageOwned(function () {
      const raw =
        typeof input === "string"
          ? input
          : input instanceof Request
            ? input.url
            : String(input);
      return {
        url: new URL(raw, location.href).href,
        method:
          (init && init.method) ||
          (input instanceof Request ? input.method : "GET"),
      };
    });
    /* THE PAGE'S OWN CODE REFUSED TO BE READ — a positive answer, and the only one available: this request
       cannot be observed, so NOTHING is emitted for it. A partially-read record would be a plausible datum. */
    if (seen === _PAGE_THREW) return response;

    const url = seen.url;
    const method = seen.method;

    if (_isInternalUrl(url)) return response;

    /* THE RESPONSE IS A REAL Response, BUT ITS PROTOTYPE IS THE PAGE'S — `headers` and `clone` are reachable
       through objects the page may have replaced, so these two reads are page-owned exactly like the two
       above. The clone is taken AFTER the internal-URL bail so a probe of our own never tees a body stream
       that nothing will read. */
    const snap = _pageOwned(function () {
      return {
        ct: response.headers.get("content-type") || "",
        clone: response.clone(),
      };
    });
    if (snap === _PAGE_THREW) return response;

    const ct = snap.ct;
    const clone = snap.clone;

    /* NOTHING BELOW THIS LINE IS PAGE-OWNED AND NOTHING BELOW IT IS WRAPPED. `_isInternalUrl` is a
       `String.prototype.includes` over a string this file built, and an async function's invocation cannot
       throw synchronously — so the paragraph-wide `try` that used to stand here had, once the page-owned
       reads moved into `_pageOwned`, nothing left to catch. It is deleted rather than kept empty. */

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
          } catch (e) {
            /* A REQUEST BODY THAT WAS ALREADY A STREAM. Fetch §5.4 "Request class": "The clone() method
               steps are: If this is unusable, then throw a TypeError", and §5.3 "Body mixin" defines unusable
               as "its body is non-null and its body's stream is disturbed or locked" — the ordinary outcome
               when the page handed `fetch()` a Request it had already begun to consume. That is a request
               whose body this file cannot see, not a defect in it: `reqBody` stays null, which the record
               already states as "no body captured", and saying so at debug volume is what separates that
               from a bug here. */
            console.debug("[apiclient:req body]", (e && e.message) || e);
          }
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
          transport: "fetch",
          method: method.toUpperCase(),
          status: clone.status,
          /* Fetch §2.2.6 Responses' STATUS MESSAGE, read off the same Response the status came from. The
             consumer that needs it is the HAR export, whose `response.statusText` it used to synthesize as
             `"OK"` for every 200 and `""` for everything else — a reason phrase no server ever sent,
             written into a document a reviewer reads as the server's own words. THE EMPTY STRING IS A REAL
             ANSWER AND THE SYNTHESIS WAS WRONG EXACTLY WHERE IT LOOKED RIGHT: §2.2.6 states a response over
             an HTTP/2 connection ALWAYS has the empty byte sequence as its status message, so on every h2
             origin — which is most of them — the export was inventing "OK" for a field the protocol had
             deliberately emptied. An opaque cross-origin response is `""` for the same reason: the browser
             refusing to state one, travelling as itself. */
          statusText: clone.statusText,
          /* Fetch §2.2.6 Responses' TYPE: "basic" | "cors" | "opaque" | "opaqueredirect" | "error".
             Opaque is a fact-level signal that the body was cross-origin-no-cors and therefore unreadable —
             treated as fire-and-forget rather than an API response by the classifier. Every one of the five
             is a non-empty string, so the `|| null` that stood here could not fire; what it DID do was tell
             the reader that absence was possible, and the offscreen wrote a matching `|| null` on the far
             side. One dead default at each end of a field that is always present. */
          responseType: clone.type,
          contentType: ct,
          responseHeaders: headers,
          body,
          base64Encoded,
          requestHeaders: reqHeaders,
          requestBody: reqBody,
          requestBodyBase64: reqBase64,
          callStack: callStack,
        });
      } catch (e) {
        /* READING A BODY IS THE ONE THING HERE THAT LEGITIMATELY FAILS, AND IT NO LONGER FAILS SILENTLY.
           `clone.arrayBuffer()`/`.text()` reject when the underlying stream errors or the document is torn
           down mid-read (Fetch §5.3 "Body mixin"), and `clone.headers`/`.status`/`.type` reach through
           prototypes the page owns. Those are real conditions for one capture to be lost. What must NOT be
           lost with them is a defect in the record composition above — so this states which capture went and
           why, at the same debug volume `emit` and the WebSocket send guard already use. */
        console.debug("[apiclient:fetch capture]", url, (e && e.message) || e);
      }
    })();

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
          transport: "xhr",
          method: (this.__uasr_method || "GET").toUpperCase(),
          status: this.status,
          // XMLHttpRequest §3.6.3 `The statusText getter` — the same field the fetch wrapper reads off its
          // Response, so one HTTP log record shape covers both transports.
          statusText: this.statusText,
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
          emit({ url: wsUrl, transport: "websocket", method: "WS_OPEN", wsId: wsId, status: 0,
            contentType: "websocket", responseHeaders: {}, body: null, base64Encoded: false });
        } catch (_) {}
      });

      this.addEventListener("close", function (ev) {
        try {
          emit({ url: wsUrl, transport: "websocket", method: "WS_CLOSE", wsId: wsId, status: ev.code || 1000,
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
          } else if (ArrayBuffer.isView(data)) {
            /* WebSockets §3.1 Interface definition: `undefined send((BufferSource or Blob or USVString)
               data)`, and BufferSource is any ArrayBufferView. Testing `instanceof Uint8Array` left every
               other typed array and every DataView falling out of this chain with `body` still `undefined`,
               which lib/response-decode.js turned into `""` — a frame the page really sent, logged as empty,
               with the emptiness indistinguishable from a socket that sent nothing. */
            body = uint8ToBase64(new Uint8Array(data.buffer, data.byteOffset, data.byteLength));
            base64Encoded = true;
          } else if (typeof Blob !== "undefined" && data instanceof Blob) {
            data.arrayBuffer().then(function (ab) {
              emit({ url: wsUrl, transport: "websocket", method: "WS_SEND", wsId: wsId, status: 0,
                contentType: "websocket", responseHeaders: {},
                body: uint8ToBase64(new Uint8Array(ab)), base64Encoded: true });
            }).catch(function () {});
            return _origSend(data);
          } else {
            /* THE UNION'S LAST MEMBER IS `USVString`, so WebIDL stringifies anything that is not a
               BufferSource or a Blob before `send` ever sees it — `ws.send({})` transmits "[object Object]".
               Falling out of the chain with `body` undefined logged that real frame as having no body at
               all; the wrapper runs BEFORE the underlying send, so this conversion is ours to perform. */
            body = String(data);
          }
          emit({ url: wsUrl, transport: "websocket", method: "WS_SEND", wsId: wsId, status: 0,
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
              emit({ url: wsUrl, transport: "websocket", method: "WS_RECV", wsId: wsId, status: 0,
                contentType: "websocket", responseHeaders: {},
                body: uint8ToBase64(new Uint8Array(ab)), base64Encoded: true });
            }).catch(function () {});
            return;
          }
          emit({ url: wsUrl, transport: "websocket", method: "WS_RECV", wsId: wsId, status: 0,
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
              transport: "eventsource",
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
  //
  // WHAT THE HOOK CAN AND CANNOT PROVE, AND WHY NO EXTRA FIELD FIXES IT. A call means
  // the payload's CODE RAN in this document; it does NOT mean the SINK produced it,
  // because this hook is installed in the page's own main world and any script here can
  // call it. Nothing added to the payload changes that (the payload IS handed to the
  // page, so it carries no secret), and NEITHER DOES anything reported about the caller:
  // a real `eval(location.hash)` sink fires with the page's own script on the stack and
  // `document.currentScript` set to it, which is byte-identical to a page script calling
  // the hook directly. So the caller's frame is relayed as CONTEXT FOR A HUMAN — the
  // offscreen files it under `pageClaimed`, the verdict never scores it, and a rule over
  // these fields would call a genuine sink a fabrication. Attribution is done where the
  // browser's own facts are (offscreen `_recordProbeHit`), never here.
  function _uasrRelayHit(id) {
    var hit = { id: String(id == null ? "" : id), at: Date.now(), url: location.href };
    try {
      // One block, not five: a page that poisons Error/currentScript refuses the whole
      // answer, and that refusal is RECORDED rather than defaulted into "no evidence".
      var cs = document.currentScript;
      var ev = window.event;
      hit.stack = String(new Error().stack || "").split("\n").slice(1, 6).join("\n").slice(0, 1024);
      hit.currentScript = cs ? String(cs.src || "inline") : null;
      hit.event = ev ? { type: String(ev.type || ""),
                         target: ev.target && ev.target.tagName ? String(ev.target.tagName) : null } : null;
    } catch (_) { hit.evidenceUnavailable = true; }
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
