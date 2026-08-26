// Content script: ships RAW materials to the analyser — ONE CONTENT_HTML per
// document (the page's server-rendered HTML), or ONE CONTENT_HTML_UNAVAILABLE
// naming why that HTML could not be obtained, so a document that cannot be
// analysed is a stated fact in the trusted zone rather than a silence there —
// plus raw network response bodies relayed from intercept.js (RESPONSE_BODY).
// Does NOT ship script sources or run any regex / heuristic pre-scan: HTML parsing is Lexbor's job in the
// worker, and the document's scripts are run by QuickJS (qjs_run_doc_scripts)
// under forced execution — endpoint/API-key discovery is real fetch/XHR
// observed at the host edge, with method/args/call site. Also acts as a fetch
// relay for the SW and a form-metadata collector for the form-submission pipeline.

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

  /* THE FIELDS EVERY TRANSPORT WRITES, and then the ones only SOME of them do. intercept.js emits this event
     from three wrappers with three different shapes — fetch, XMLHttpRequest and WebSocket — so a field's
     absence here is a fact about WHICH TRANSPORT ran, and a relay that fills it in destroys the one thing
     this side knows and the offscreen cannot re-derive.
       responseType  Fetch §2.2.6 response's TYPE, read off `clone.type`. Only the fetch wrapper can produce
                     one: XHR's own spec fails a cross-origin response CORS did not allow rather than handing
                     back an opaque one, and a WebSocket frame is not a Response at all. It is also the one
                     fact about an opaque no-cors response its bytes cannot carry — body null, header list
                     empty, byte-identical to an empty readable response — so "absent" is NOT APPLICABLE and
                     `null` was that statement rendered as the value the classifier reads for "not opaque".
       wsId          written by the WebSocket wrapper alone; its absence is "this is not a socket frame", and
                     lib/response-decode.js reads exactly that (`msg.channelId || msg.wsId`).
       requestHeaders / requestBody / requestBodyBase64 / callStack
                     written by the fetch and XHR wrappers, which have a request to describe. A WS_OPEN /
                     WS_SEND / WS_RECV frame has none, and `null`/`false` said "the request carried no
                     headers and no body", which is a claim about a request that does not exist.
     AND THREE NAMES ARE GONE BECAUSE NOTHING ON THIS EVENT EVER WROTE THEM. `channelId`, `sourceOrigin` and
     `targetOrigin` belong to the postMessage/MessageChannel records THIS FILE builds further down — a
     different producer on a different path — and reading them off intercept.js's detail was a read with no
     writer, with `|| null` as the reason it never crashed. */
  const PER_TRANSPORT = ["responseType", "wsId", "requestHeaders", "requestBody", "requestBodyBase64", "callStack"];
  document.addEventListener("__uasr_resp", (e) => {
    if (!e.detail) return;
    const d = e.detail;
    const m = {
      type: "RESPONSE_BODY",
      url: d.url,
      method: d.method,
      status: d.status,
      contentType: d.contentType,
      responseHeaders: d.responseHeaders,
      body: d.body,
      base64Encoded: d.base64Encoded,
    };
    for (const k of PER_TRANSPORT) if (k in d) m[k] = d[k];
    _sendChunked(m, "body");
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
  /* EVERY INVOCATION SENDS EXACTLY ONE MESSAGE, and that is the whole contract this function has with the
     trusted zone. It used to have two exits that sent NOTHING — a non-OK status and an empty body each threw
     into this realm's console, and a rejected fetch was an unhandled rejection — and the reasoning written
     here was that a throw is "loud" and therefore not a silent no-analysis. It is loud in the UNTRUSTED
     zone, whose console the trusted zone deliberately does not read (SECURITY.md's trust table; and a hot
     engine's traffic would bury it). So from every extension surface the throw was indistinguishable from
     "no content script ever ran here" and from "the engine ran and found nothing" — the three-states-behind-
     one-answer defect at the outermost layer of the product.
     MEASURED, on real Chrome: reddit's JS bot challenge navigates to the SAME path carrying a single-use
     `token`/`solution` in the query, and a second GET of that address answers 403 three times out of three.
     The re-fetch below CANNOT succeed on such a document by construction, so this is not an exotic edge —
     it is a class of target, and the product's answer to it must be a REPORT rather than a console line.
     THE THROWS ARE DELETED, NOT KEPT BESIDE THE REPORT. A superseded mechanism kept "as well" is what makes
     the new one's gaps invisible: the report carries the same fact to a reader that exists, so the throw is
     the strictly weaker half of a pair and goes. What is NOT relaxed is the refusal itself — a 404/500 error
     page is still never smuggled in as "the bundle", and there is still no `r.ok` fallback. */
  function _sendPageHtml() {
    // REAL server response, not the rendered DOM: fetch(location.href) returns the UNMODIFIED HTML the
    // server shipped PLUS the real response headers — and because location.href is same-origin, EVERY
    // header is readable (Content-Security-Policy, Content-Type), unlike a cross-origin offscreen fetch.
    // outerHTML was the post-execution DOM: document.write output serialized as real <script> elements
    // (the engine then re-ran them + fatally crashed on their undefined refs), a JS-mutated tree, and NO
    // headers — so the engine analysed a rendered page, not the shipped bundle, and judged XSS against
    // meta-CSP only (missing the primary header CSP). This is the moat's ground truth: the bundle + its
    // real policy. (Same-origin credentialed by default = the actual document the browser received.)
    // AND THERE IS NO SECOND SOURCE FOR EITHER HALF: Chrome exposes the navigation's response body to no
    // content-script API at all (chrome.debugger's Network.getResponseBody is the only route and it takes a
    // permission this extension does not hold and puts a banner on the user's browser), and this extension
    // holds no `webRequest` permission, so the navigation's real header CSP is reachable only by asking for
    // the document again. "The content script is in the document, it already has the bytes" is FALSE — what
    // it has is the parsed, script-mutated DOM, which is a different document.
    // The DOCUMENT's navigation URL — NOT location.href. An SPA (the tool's PRIMARY target) calls
    // history.pushState/replaceState, which MUTATES location.href to a client-route that the server does not
    // serve — so fetch(location.href) would GET a 404 for /dashboard and report the bundle as unavailable,
    // losing the ENTIRE analysis of the real bundle. PerformanceNavigationTiming.name is the actual URL the
    // browser navigated to (the doc that shipped the bundle), immutable by pushState. Fallback to location.href
    // only if the timing entry is unavailable.
    var docUrl = location.href;
    try { var _nav = performance.getEntriesByType("navigation")[0]; if (_nav && _nav.name) docUrl = _nav.name; } catch (_e) {}
    /* THE TRY HOLDS THE FETCH AND NOTHING ELSE. A network error is a Fetch §2.2.6 Responses outcome — a
       real answer from a hostile network, not our own logic being wrong — so turning it into a report is not
       swallowing an invariant. Widening this catch over the message-building would be: a failure to REACH the
       trusted zone (an invalidated extension context) is a different fact and must not be re-reported as the
       page's own response having failed. */
    var r = null, html = null, netErr = null;
    (async function () {
      try {
        r = await fetch(docUrl, { credentials: "same-origin" });
        if (r.ok) html = await r.text();
      } catch (e) { netErr = String((e && e.message) || e); }
      /* THE CHAIN ASSIGNS, IT DOES NOT RETURN, AND THERE IS EXACTLY ONE SEND BELOW IT. That is the whole
         enforcement of "one message per invocation", and it is STRUCTURAL rather than asserted on purpose:
         an assertion in this file would fire in the untrusted realm's console, which is the very place the
         old throws were loud in and nobody read. A chain of early returns is one edit away from a silent
         exit again; a chain that must produce a value cannot fall out of this function without one. */
      var msg;
      if (netErr !== null) {
        msg = { type: "CONTENT_HTML_UNAVAILABLE", kind: "network", detail: netErr };
      } else if (!r.ok) {
        msg = { type: "CONTENT_HTML_UNAVAILABLE", kind: "status", status: r.status };
      } else if (!html) {
        msg = { type: "CONTENT_HTML_UNAVAILABLE", kind: "empty" };
      } else {
        var headers = {};
        r.headers.forEach(function (v, k) { headers[k.toLowerCase()] = v; });
        msg = { type: "CONTENT_HTML", html: html, responseHeaders: headers };
      }
      _shipPageSource(msg);
    })();
  }

  /* THE ONE SEND, AND WHAT BOTH ITS MESSAGES MAY CARRY.
     CONTENT_HTML is the bundle as shipped. CONTENT_HTML_UNAVAILABLE is this document stating that the bundle
     could not be obtained, WITH the reason — a report that cannot say which failure it was re-collapses the
     states it exists to separate, so `kind` is one of a closed set that this file and the offscreen's arm
     both spell out: `status` (the server answered, not OK — carries `status` and nothing else), `empty` (OK
     with a zero-length body — carries neither), `network` (Fetch §2.2.6 Responses' network error — carries `detail`).
     An absent `status` is a POSITIVE statement, "this kind has no status", which the offscreen reads as one
     rather than defaulting it to a plausible number; it REFUSES a status on a kind that has none.
     MATERIAL ONLY, on both: no origin, no address. `origin: location.origin` and `pageUrl: docUrl` stood on
     CONTENT_HTML and NOTHING ever read either — the offscreen mints every browser fact from the MessageSender
     (`_browserFacts`) and asserts the brand on it (`_statedFacts`). They were worse than dead: they are the
     two fields SECURITY.md's removed hole was spelled in, sitting on the wire under exactly the names a
     future consumer would reach for, and offscreen-brain.js carries a standing warning that the one value in
     the trusted zone ever spelled `pageUrl` is this one. An UNTRUSTED zone does not get to state an origin or
     an address, so it does not get to send a field shaped like one — the trap is deleted rather than
     documented, and the new message inherits the rule rather than being an exception to it.
     `_sendPageHtml` USED TO TAKE A `why` ("init" / "reship") and it is deleted with the throws that were its
     only consumers: it never reached the wire, so no zone that could act on it ever saw which ship point ran.
     A parameter whose every reader is gone is the same broken half-contract as a field nobody writes. */
  function _shipPageSource(msg) {
    if (msg.type === "CONTENT_HTML") { _sendChunked(msg, "html"); return; }
    chrome.runtime.sendMessage(msg);
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
    /* WHAT THE TRUSTED ZONE PROMISED TO SEND, ASSERTED WHERE IT ARRIVES. lib/schema.js's `_sendPageFetch`
       composes this message in ONE object literal and writes url/method/headers/body/bodyEncoding on every
       one it sends — `pageContextFetch` above it DCHECKs the method is a non-empty string before that literal
       is even built — so `msg.headers || {}` and `msg.method || "GET"` were a SECOND layer of the producer's
       own defaults, and what a second layer buys is that a producer which stops writing a field is answered
       here with a plausible request instead of a crash. The verb is the half that matters: a relay that
       silently sends GET for a message whose method went missing performs a DIFFERENT REQUEST than the
       caller asked for, against a page's real credentials, and reports it as that caller's answer. */
    DCHECK(!!msg && typeof msg.url === "string" && msg.url !== "",
           "a PAGE_FETCH reached the page-context relay with no url — lib/schema.js writes one on every " +
           "message it sends, so an absent one is that composition broken and this relay is about to fetch " +
           "the string \"undefined\" against the page's own credentials");
    DCHECK(typeof msg.method === "string" && msg.method !== "",
           "a PAGE_FETCH reached the page-context relay with no method — pageContextFetch asserts one before " +
           "the message is built, and defaulting to GET here would run a different request than the caller " +
           "named and answer them with its result");
    DCHECK(msg.headers && typeof msg.headers === "object",
           "a PAGE_FETCH reached the page-context relay with no headers object — lib/schema.js writes one " +
           "(possibly empty) on every message, and an absent one is a header list the caller composed and " +
           "this relay silently dropped");
    // Strip browser-managed headers
    const headers = { ...msg.headers };
    delete headers["Cookie"];
    delete headers["cookie"];
    delete headers["Origin"];
    delete headers["origin"];
    delete headers["Referer"];
    delete headers["referer"];

    const opts = {
      method: msg.method,
      credentials: "same-origin",
      headers,
    };

    if (msg.body != null) {
      opts.body =
        msg.bodyEncoding === "base64" ? base64ToUint8(msg.body) : msg.body;
    }

    /* THE ADDRESS IS COMPOSED OUTSIDE THE CATCH, because the catch below has a real job and this is not it: a
       fetch that fails is a Fetch §5.6 network-error OUTCOME the caller is entitled to be told about, while a
       message with no url is a broken contract with the trusted zone. Read inside the try, the second was
       reported to the caller as the first. */
    const fetchUrl = msg.url + (msg.url.includes("#") ? "&" : "#") + "_uasr_send";
    try {
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
      // so we re-send the document HTML (the cold-start delivery race — the
      // offscreen wasn't alive when content.js shipped at document_idle, so the
      // brain missed our CONTENT_HTML). Idempotent: the brain dedups by document.
      _sendPageHtml();
      sendResponse({ ok: true });
      return;
    }
    if (msg.type === "PAGE_FETCH") {
      handlePageFetch(msg).then(sendResponse);
      return true;
    }
    if (msg.type === "WS_SEND_MSG") {
      /* `binary` DECIDES HOW THE MAIN WORLD READS `data` — intercept.js base64-decodes it when the flag is
         set and sends the string as-is when it is not — so `|| false` was not a default, it was a DECISION
         made on the producer's behalf: a flag that went missing became "this is text", and a binary frame
         the operator composed would be delivered as its own base64 spelling. lib/popup-handlers.js writes
         the flag on every WS_SEND_MSG it relays. */
      DCHECK(typeof msg.binary === "boolean",
             "a WS_SEND_MSG reached the socket relay with no `binary` flag — it is what decides whether the " +
             "main world base64-decodes the payload, so an absent one would send a binary frame's base64 " +
             "text as the frame itself");
      DCHECK(typeof msg.data === "string",
             "a WS_SEND_MSG reached the socket relay with no `data` — the payload is the whole of what this " +
             "relay exists to deliver");
      document.dispatchEvent(new CustomEvent("__uasr_ws_send", {
        detail: { wsId: msg.wsId, data: msg.data, binary: msg.binary }
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
      /* THE PAYLOAD IS READ OUTSIDE BOTH CATCHES, and the two catches are then about the two things they are
         actually for. The inner one is real and stays: a console payload is human-typed text, so "it is not
         JSON" is a DATUM and the string is posted as itself. The outer one turns a postMessage throw into an
         answer the operator sees. Neither of them is about whether the trusted zone sent a payload at all,
         and with `msg.data` read inside them a message that carried none was delivered as the literal
         `undefined` or reported as a failed post. */
      DCHECK(typeof msg.data === "string",
             "a PM_SEND_MSG reached the postMessage relay with no `data` string — lib/popup-handlers.js " +
             "relays the operator's typed payload on every one, so an absent one is a message this window " +
             "would receive carrying nothing the operator wrote");
      let data = msg.data;
      try {
        try { data = JSON.parse(data); } catch (_) { /* not JSON: the typed text IS the payload */ }
        source.postMessage(data, origin);
        sendResponse({ ok: true });
      } catch (err) {
        RETHROW_FATAL(err);
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
      /* Same split as PM_SEND_MSG one branch up: the payload's PRESENCE is a contract with the trusted zone
         and is asserted here, its JSON-ness is a datum the inner catch answers, and a port that refuses the
         post is the outer catch's business. */
      DCHECK(typeof msg.data === "string",
             "an MC_SEND_MSG reached the MessagePort relay with no `data` string — lib/popup-handlers.js " +
             "relays the operator's typed payload on every one, so an absent one is a port message carrying " +
             "nothing the operator wrote");
      let data = msg.data;
      try {
        try { data = JSON.parse(data); } catch (_) { /* not JSON: the typed text IS the payload */ }
        port.postMessage(data);
        sendResponse({ ok: true });
      } catch (err) {
        RETHROW_FATAL(err);
        sendResponse({ error: err.message });
      }
      return;
    }
  });

  // ─── Scan Dedup ────────────────────────────────────────────────────────────
  // Track hashes of last sent scan results to avoid re-sending identical data
  // on SPA re-renders or MutationObserver re-triggers without actual DOM changes.

  // ─── Init ──────────────────────────────────────────────────────────────────

  // Initial raw-HTML ship — Lexbor in the worker parses this as the
  // analysed document, so subsequent forced execution sees a real
  // server-rendered DOM (custom elements with their attributes, data
  // islands, the structure the bundle's connectedCallback / React
  // effects depend on).
  _sendPageHtml();

  // Forms are NOT scanned here — Lexbor in the engine worker parses
  // the same HTML and __hostDrive calls form.submit() on each form,
  // routing through the same fetch hook the bundle's own JS reaches.
  // One execution flow.

  /* No content-side DOM walker. Lexbor inside the QuickJS worker
     re-parses the CONTENT_HTML message into the same spec DOM the
     bundle reads via document.querySelector / dataset / etc. Any
     "DOM context" the analyser needs is read FROM the engine,
     never shipped as a separate snapshot from the page context. */

  // SCRIPTS_LOADED + the MutationObserver removed. One message per document: the
  // single CONTENT_HTML (_sendPageHtml() above) is the whole input. The
  // engine (Lexbor + qjs_run_doc_scripts) parses it and runs EVERY script in
  // document order in one realm — inline + external <script src>; scripts a page
  // inserts dynamically are discovered by forced exec's createElement host edge,
  // not a content-side observer (which only ever saw what actually fired). No
  // per-script SCRIPT_SOURCE shipping, no load signal, no HTML re-ship churn.

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

      /* Same rule as CONTENT_HTML above: `origin` and `pageUrl` are deleted. `_handleFormSubmit` never read
         either, and a form submission is an OBSERVATION this document makes about its own traffic — `url` is
         the address the page is posting TO (data, and legitimately this side's to state), while an origin or
         a page address would be this zone stating its own identity, which is the MessageSender's job. */
      chrome.runtime.sendMessage({
        type: "CONTENT_FORM_SUBMIT",
        url: url,
        method: method,
        enctype: enctype,
        fields: fields,
      });
    } catch (_) {}
  }, true);
})();
