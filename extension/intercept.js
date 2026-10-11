// Main-world script: wraps fetch() and XMLHttpRequest to capture what chrome.webRequest cannot report, the
// response body and the call site, and wraps WebSocket and EventSource for their frames. Communicates back
// to the isolated-world content script via CustomEvent on the shared document. A fetch()/XHR capture names
// its request by (method, URL, ordinal), and the trusted zone takes every other field of that request from the
// browser's own webRequest record of it (lib/web-request.js); this realm is the page's, so nothing it reports
// about a request is believed.
// Its one write is the person's WebSocket console frame, sent on a socket the page opened (below).

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

  /* lib/discovery.js and lib/discovery-probe.js mark their own probes with an `_internal_probe` fragment
     token. The token is searched in the fragment alone, so a path or query that happens to spell it is
     still page traffic. No URL parse: `url` is a string this file was handed, and `new URL` on it would be
     a throw inside a page-owned wrapper. */
  function _isInternalUrl(url) {
    var h = url.indexOf("#");
    if (h < 0) return false;
    return url.slice(h + 1).includes("_internal_probe");
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
  // Sends the person's typed frame (relayed by content.js) on a live socket the
  // page opened. It is not a safeFetch send because the frame must travel on the
  // page's own connection: the server's reply then runs the page's own message
  // handlers, which a socket the extension opened would never feed. It confers
  // nothing: the page's code can already write to its own sockets.

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


  // ─── Request key helpers ──────────────────────────────────────────────────

  /* Fetch §2.2.1 "Methods"' normalize a method, which XMLHttpRequest §3.5.1 "The open() method" step 4 runs
     over open()'s method: "if it is a byte-case-insensitive match for `DELETE`, `GET`, `HEAD`, `OPTIONS`,
     `POST`, or `PUT`, byte-uppercase it", and every other verb goes out as the page spelled it. The fold is
     ASCII, because Unicode case mapping is wider than a byte comparison (U+017F uppercases to `S`). The
     result is a capture's key, which must equal the method webRequest reports for the request; a fetch()
     capture's key is the Request constructor's own (§5.4 "Request class" step 25 runs the same step). */
  const _NORMALIZED_METHODS = ["DELETE", "GET", "HEAD", "OPTIONS", "POST", "PUT"];
  function _normalizeMethod(m) {
    var up = "";
    for (var i = 0; i < m.length; i++) {
      var c = m.charCodeAt(i);
      up += (c >= 0x61 && c <= 0x7a) ? String.fromCharCode(c - 0x20) : m.charAt(i);
    }
    for (var j = 0; j < _NORMALIZED_METHODS.length; j++)
      if (up === _NORMALIZED_METHODS[j]) return _NORMALIZED_METHODS[j];
    return m;
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
  const _Request = window.Request;

  /* The four request facts a replay must reproduce, as Fetch §5.4 "Request class" sets them: an `init`
     member that exists wins over `input`'s, and a member neither supplies is null, which a replay passes
     as absent so fetch applies its own default. A Request's values are read through getters captured
     here, before any page script runs, so a page-replaced getter cannot restate them. */
  const _apply = Reflect.apply;
  const _INIT_MEMBERS = ["credentials", "mode", "referrer", "referrerPolicy"];
  const _requestGet = {};
  for (var _i = 0; _i < _INIT_MEMBERS.length; _i++)
    _requestGet[_INIT_MEMBERS[_i]] = Object.getOwnPropertyDescriptor(Request.prototype, _INIT_MEMBERS[_i]).get;
  const _requestUrl = Object.getOwnPropertyDescriptor(Request.prototype, "url").get;
  const _requestMethod = Object.getOwnPropertyDescriptor(Request.prototype, "method").get;
  const _requestSignal = Object.getOwnPropertyDescriptor(Request.prototype, "signal").get;
  const _requestBody = Object.getOwnPropertyDescriptor(Request.prototype, "body").get;
  const _requestClone = Request.prototype.clone;
  const _requestArrayBuffer = Request.prototype.arrayBuffer;
  const _Response = window.Response;
  const _responseArrayBuffer = Response.prototype.arrayBuffer;

  /* The request body is the browser's for POST and PUT and this realm's for every other method, because
     webRequest reports `requestBody` for those two methods alone (measured in Chrome 148: PATCH, DELETE,
     OPTIONS and an extension method each sent a body and carried none). So a capture states the body bytes,
     base64, only for a method webRequest leaves silent, and null otherwise; lib/web-request.js takes the
     body from the browser for POST and PUT.
     Named residual. Not covered: a PATCH or DELETE body is the page's statement, and an XHR FormData body is
     serialized here with a boundary of its own rather than the one on the wire. Next diff builds: the body
     of every method from a browser-stated source. Absence shows as: a PATCH or DELETE record whose body
     differs from the bytes a proxy saw. */
  function _bodyIsCaptured(method) {
    return method !== "POST" && method !== "PUT";
  }
  async function _bytesB64(response) {
    return uint8ToBase64(new Uint8Array(await _apply(_responseArrayBuffer, response, [])));
  }
  const _signalAborted = Object.getOwnPropertyDescriptor(AbortSignal.prototype, "aborted").get;

  /* The ordinal a capture names its request by: how many requests with this method and URL this document
     made before it, counted at the call, in the order the browser starts them. lib/web-request.js counts the
     same population off webRequest's onBeforeRequest and joins on (documentId, method, URL, ordinal). */
  const _seqNext = new Map();
  function _seqTake(method, url) {
    var k = method + " " + url;
    var n = _seqNext.has(k) ? _seqNext.get(k) : 0;
    _seqNext.set(k, n + 1);
    return n;
  }
  const _xhrWithCredentials = Object.getOwnPropertyDescriptor(XMLHttpRequest.prototype, "withCredentials").get;

  /* Fetch §5.4's init["referrer"] step: "" is no-referrer; a URL parsed against the document's base that
     is `about:client` or not same origin with the document is "client"; otherwise the parsed URL. The
     answer is spelled as the Request `referrer` getter spells it. A string that does not parse makes
     fetch throw, so no record is emitted for it. */
  function _referrerOfInit(v) {
    if (v === "") return "";
    var parsed = new URL(v, document.baseURI);
    // An opaque origin serializes as "null" and is same origin with nothing but itself.
    if (parsed.href === "about:client" || parsed.origin === "null" || parsed.origin !== location.origin)
      return "about:client";
    return parsed.href;
  }

  function _fetchRequestInit(input, init) {
    var isRequest = input instanceof Request;
    var out = {};
    for (var i = 0; i < _INIT_MEMBERS.length; i++) {
      var k = _INIT_MEMBERS[i];
      var v = init ? init[k] : undefined;
      if (v !== undefined) out[k] = k === "referrer" ? _referrerOfInit(String(v)) : String(v);
      else out[k] = isRequest ? _apply(_requestGet[k], input, []) : null;
    }
    return out;
  }

  window.fetch = async function (input, init) {
    var callStack = _captureCallStack();
    // Read at the call: the page may mutate its own `init` while the request is in flight.
    var requestInit = _pageOwned(function () { return _fetchRequestInit(input, init); });
    /* Fetch §5.6 "Fetch methods" step 2, "Let requestObject be the result of invoking the initial value of
       Request as constructor with input and init as arguments", performed here with that initial value, and
       the Request handed to fetch() in its place. So the URL and method a capture keys on are the
       constructor's own (§5.4 "Request class" parses the URL and normalizes the method), read through getters
       captured before any page script ran, and an argument the constructor refuses rejects this call exactly
       as fetch() would, with no request made and none counted. A request whose signal is already aborted is
       never started (§5.6 step 5) and is not counted either. */
    var request = new _Request(input, init);
    const url = _apply(_requestUrl, request, []);
    const method = _apply(_requestMethod, request, []);
    const seq = _apply(_signalAborted, _apply(_requestSignal, request, []), []) ? null : _seqTake(method, url);
    /* The body bytes for a method webRequest leaves silent, read off a clone taken before fetch() takes the
       request's body (Fetch §5.4 "Request class": clone() clones the body, teeing its stream). */
    const bodyClone = _bodyIsCaptured(method) && _apply(_requestBody, request, []) !== null
      ? _apply(_requestClone, request, []) : null;

    const response = await _fetch.call(this, request);

    /* THE PAGE'S OWN CODE REFUSED TO BE READ — a positive answer, and the only one available: this request
       cannot be observed, so NOTHING is emitted for it. A partially-read record would be a plausible datum. */
    if (requestInit === _PAGE_THREW) return response;

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
      /* The response's status and type, page-owned reads like the two above: Fetch §5.5 "Response class"
         declares both non-nullable, so only a page-replaced getter can fail, and that is the page refusing to
         be observed. Both are facts of the response and not of its body, so reading them before the body
         changes neither. */
      const meta = _pageOwned(function () {
        return { status: clone.status, responseType: clone.type };
      });
      if (meta === _PAGE_THREW) return;
      /* The status this realm observed, which the trusted zone checks against the browser's record of the
         request it joins this capture to; the record's own status is the browser's. */
      const respStatus = meta.status;
      /* Fetch §2.2.6 Responses' TYPE. The spec's list is SIX, not the five that stood here: a response's
         type is "basic", "cors", "default", "error", "opaque", or "opaqueredirect", and "Unless stated
         otherwise, it is 'default'" — which is exactly the one this site can see and the one the old
         comment omitted, because a page that replaced `window.fetch` and hands back a `new Response(...)`
         it built itself gets "default" by that sentence. Opaque is a fact-level signal that the body was
         cross-origin-no-cors and therefore unreadable — treated as fire-and-forget rather than an API
         response by the classifier. Every one of the six is a non-empty string, so the `|| null` that once
         stood here could not fire; what it DID do was tell the reader that absence was possible, and the
         offscreen wrote a matching `|| null` on the far side. */
      const respType = meta.responseType;

      /* THE RESPONSE BODY READ IS THE ONE THING LEFT THAT LEGITIMATELY FAILS, AND THE `try` NOW HOLDS IT
         AND NOTHING ELSE. `clone.arrayBuffer()`/`.text()` reject when the underlying stream errors or the
         document is torn down mid-read (Fetch §5.3 "Body mixin") — one capture lost, honestly, and the
         `return` says so rather than falling through to compose a record around a body that does not exist.
         WHAT USED TO SHARE THIS CATCH AND NO LONGER DOES, in both directions: the four page-owned prototype
         reads went up into `_pageOwned`, where the page's refusal is stated by name; and the `emit` below
         came OUT, because it is this file's own logic and a defect in it must not read as a stream that
         errored. Nothing was lost by lifting it — `emit` already guards its own `CustomEvent` dispatch, for
         the detached-document reason it states at its definition — so the paragraph-wide catch was buying
         one thing only, which was silence about our own record composition.
         `isBinary` and `uint8ToBase64` stay inside it because they stand on the body path: both are pure
         (`btoa` over a `String.fromCharCode` of bytes, every one of which is ≤ 255), so neither throws, and
         moving them out would only split the body derivation across the brace for no gain. */
      let body,
        base64Encoded = false;
      try {
        if (isBinary(ct)) {
          const buf = await clone.arrayBuffer();
          body = uint8ToBase64(new Uint8Array(buf));
          base64Encoded = true;
        } else {
          body = await clone.text();
        }
      } catch (e) {
        console.debug("[apiclient:fetch body]", url, (e && e.message) || e);
        return;
      }

      let requestBodyB64 = null;
      if (bodyClone !== null) {
        try {
          requestBodyB64 = uint8ToBase64(new Uint8Array(await _apply(_requestArrayBuffer, bodyClone, [])));
        } catch (e) {
          console.debug("[apiclient:req body]", url, (e && e.message) || e);
          return;
        }
      }

      emit({
        url,
        transport: "fetch",
        method,
        seq,
        requestBodyB64,
        observedStatus: respStatus,
        responseType: respType,
        body,
        base64Encoded,
        requestInit: requestInit,
        callStack: callStack,
      });
    })();

    return response;
  };

  // ─── XMLHttpRequest wrapper ─────────────────────────────────────────────────

  const _xhrOpen = XMLHttpRequest.prototype.open;
  const _xhrSend = XMLHttpRequest.prototype.send;

  XMLHttpRequest.prototype.open = function (method, url) {
    this.__uasr_method = method;
    this.__uasr_url = url;
    return _xhrOpen.apply(this, arguments);
  };

  XMLHttpRequest.prototype.send = function (sendBody) {
    if (this.__uasr_hooked) return _xhrSend.apply(this, arguments);
    this.__uasr_hooked = true;

    var callStack = _captureCallStack();
    /* The key is read before the send and the ordinal taken only once the send has not thrown: send() throws
       for an object that is not opened or already sent (XMLHttpRequest §3.5.6 steps 1-2), which starts no
       request. A synchronous request fires `load` inside send(), so the ordinal is known before the call.
       The URL is parsed against the document's base URL, which is XMLHttpRequest §3.5.1's "relevant settings
       object" for open(). An open() this wrapper never saw leaves no key, and the request is not captured. */
    /* Named residual. Not covered: `xhr.open(undefined, u)` sends the verb `undefined` (Web IDL converts the
       argument to a ByteString), and the property this wrapper stores reads as an open it never saw, so that
       request is not captured. Next diff stores a value this file mints at open() (`{ raw: method }`). Absence
       shows as: a request the network tab lists and the log does not, for a literal `undefined` method. */
    var self = this;
    var key = this.__uasr_method === undefined ? null : _pageOwned(function () {
      var m = _normalizeMethod(String(self.__uasr_method));
      return { method: m, url: new URL(String(self.__uasr_url), document.baseURI).href };
    });
    var seq = key === null || key === _PAGE_THREW ? null
            : (_seqNext.has(key.method + " " + key.url) ? _seqNext.get(key.method + " " + key.url) : 0);
    /* The body bytes for a method webRequest leaves silent. XMLHttpRequest §3.5.6 step 3 drops a GET or HEAD
       body, and step 4 "safely extracts" any other (Fetch), the extraction the Response constructor performs
       over the same value; a Document body is serialized by §3.5.6 itself and is not captured. */
    var bodyBytesP = seq === null || !_bodyIsCaptured(key.method) || key.method === "GET" ||
                     key.method === "HEAD" || sendBody == null ||
                     (typeof Document !== "undefined" && sendBody instanceof Document)
      ? Promise.resolve(null)
      : _pageOwned(function () { return _bytesB64(new _Response(sendBody)); });

    this.addEventListener("load", function () {
      try {
        if (seq === null) return;
        const method = key.method;
        const url = key.url;

        if (_isInternalUrl(url)) return;

        let body,
          base64Encoded = false;
        if (this.responseType === "arraybuffer" && this.response) {
          body = uint8ToBase64(new Uint8Array(this.response));
          base64Encoded = true;
        } else if (this.responseType === "" || this.responseType === "text") {
          /* AN EMPTY BODY IS A RESPONSE AND NOT AN ABSENCE, AND `if (!body) return;` DROPPED THE WHOLE RECORD
             FOR ONE. This line is reached in the `done` state, where XMLHttpRequest §3.6.10 "The responseText
             getter" ends "Return the result of getting a text response for this" — so `""` is what a 204, a
             `Content-Length: 0` 200 and every empty DELETE/PUT reply MEAN, and a truthiness test read the
             commonest real answer as no observation at all. The fetch wrapper above emits its own `""`
             unconditionally and lib/response-decode.js types this field `string | null`, so the CONSUMER had
             already been hardened to accept exactly what this producer refused to send, and the two transports
             disagreed about the same round trip. This is the half CLAUDE.md calls a thermometer: what it
             declines to log is subtracted from the number that exists to embarrass the solver.
             HOW ITS ABSENCE SHOWS: an endpoint the network panel lists and the request log does not, for any
             XHR whose reply carried no bytes. */
          body = this.responseText;
        } else if (this.responseType === "json") {
          /* AND THE COPY THAT STOOD HERE WAS ASSERTING AN IMPOSSIBLE STATE RATHER THAN LOSING ANYTHING.
             XMLHttpRequest §3.6.9 "The response getter"'s json arm reads "Let jsonObject be the result of
             running parse JSON from bytes on" its received bytes, and returns null both for a null body and
             for a parse that threw — so `this.response` is a JSON value or null and never `undefined`, and
             `JSON.stringify(null)` is the truthy string "null", which no `!body` test can reach. A non-string
             `body` now fails `_relayMessageUnserved` and is dropped BY NAME, with the field and the transport
             on the console, which is where a producer that stopped writing one belongs. */
          try {
            body = JSON.stringify(this.response);
          } catch (_) {
            return;
          }
        } else {
          return; // blob, document — skip
        }

        const observedStatus = this.status;
        // XMLHttpRequest §3.5.6 "The send() method": mode "cors", credentials "include" when
        // withCredentials is true and "same-origin" otherwise; XHR has no referrer or policy to state.
        const requestInit = { credentials: _apply(_xhrWithCredentials, this, []) ? "include" : "same-origin",
                              mode: "cors", referrer: null, referrerPolicy: null };
        if (bodyBytesP === _PAGE_THREW) return;
        bodyBytesP.then(function (requestBodyB64) {
          emit({ url, transport: "xhr", method, seq, requestBodyB64, observedStatus, body, base64Encoded,
                 requestInit, callStack });
        }, function (e) { console.debug("[apiclient:req body]", url, (e && e.message) || e); });
      } catch (_) {}
    });

    var ret = _xhrSend.apply(this, arguments);
    if (seq !== null) _seqTake(key.method, key.url);
    return ret;
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
    const _esWithCredentials = Object.getOwnPropertyDescriptor(_EventSource.prototype, "withCredentials").get;
    class WrappedEventSource extends _EventSource {
      constructor(url, opts) {
        super(url, opts);
        const esUrl = typeof url === "string" ? url : String(url);
        /* HTML §9.2.2 "The EventSource interface": a potential-CORS request, so mode "cors", whose
           credentials are "include" when withCredentials is true and "same-origin" otherwise. */
        const requestInit = { credentials: _apply(_esWithCredentials, this, []) ? "include" : "same-origin",
                              mode: "cors", referrer: null, referrerPolicy: null };

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
              requestInit: requestInit,
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
