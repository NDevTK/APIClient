// Content script: ships ONE CONTENT_SEED per document — the ADDRESS the browser actually navigated to, and
// nothing else. A seed is an address, never bytes: this zone ships no document body, no response headers and
// no identity, because the custom browser LOADS the suggested address itself through the one network
// chokepoint (lib/safe-fetch.js), where the scheme allowlist, the origin-relative SSRF/PNA guard, CORB and the
// destructive-path deny list live. Also relays raw network response bodies from intercept.js (RESPONSE_BODY),
// acts as a fetch relay for the trusted zone, and collects form metadata for the form-submission pipeline.
// Does NOT ship script sources or run any regex / heuristic pre-scan: HTML parsing is Lexbor's job in the
// worker, and the document's scripts are run by QuickJS (qjs_run_doc_scripts)
// under forced execution — endpoint/API-key discovery is real fetch/XHR
// observed at the host edge, with method/args/call site.

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
       statusText    Fetch §2.2.6 Responses' status message / XMLHttpRequest §3.6.3 `The statusText getter` —
                     written by the fetch and XHR wrappers, which have a status LINE to have read one from. A
                     WebSocket frame and an EventSource message have none, so its absence is "no status line
                     was observed" and NOT "the reason phrase was empty", which is a different and legal
                     answer the h2 wrappers do report.
       requestHeaders / requestBody / requestBodyBase64 / callStack
                     written by the fetch and XHR wrappers, which have a request to describe. A WS_OPEN /
                     WS_SEND / WS_RECV frame has none, and `null`/`false` said "the request carried no
                     headers and no body", which is a claim about a request that does not exist.
     AND THREE NAMES ARE GONE BECAUSE NOTHING ON THIS EVENT EVER WROTE THEM. `channelId`, `sourceOrigin` and
     `targetOrigin` belong to the postMessage/MessageChannel records THIS FILE builds further down — a
     different producer on a different path — and reading them off intercept.js's detail was a read with no
     writer, with `|| null` as the reason it never crashed. */
  const PER_TRANSPORT = ["responseType", "wsId", "statusText",
                         "requestHeaders", "requestBody", "requestBodyBase64", "callStack"];
  document.addEventListener("__uasr_resp", (e) => {
    if (!e.detail) return;
    const d = e.detail;
    const m = {
      type: "RESPONSE_BODY",
      /* THE TRANSPORT IS THE WRAPPER'S OWN NAME FOR ITSELF, and it is a BASE field precisely because every
         wrapper writes one. It is what lib/response-decode.js routes on, and `method` is what it used to
         route on — but on the fetch/XHR arm `method` is the verb the PAGE chose, so `fetch(u, {method:
         "WS_OPEN"})` steered its own HTTP round trip into the WebSocket record builder. A page may still
         spell any verb it likes; it cannot spell which of intercept.js's wrappers ran. */
      transport: d.transport,
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
          transport: "messagechannel",
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
            transport: "messagechannel",
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
        transport: "postmessage",
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

  /* THE ONE FACT AN AMBIENT OBSERVER HOLDS THAT NO SOLVER CAN DERIVE, AND IT IS AN ADDRESS.
     A solver explores the bundle it was handed; WHICH DOCUMENTS EXIST — which origins someone reaches, in
     which app, behind which auth state — is not a fact inside any one document, so it is not a finding the
     forced execution failed to make. It is the SEED, and it enters the ONE frontier as a document to explore.
     THIS FUNCTION USED TO FETCH THE DOCUMENT AND SHIP ITS BYTES, AND THAT WAS A SECOND DOCUMENT-LOAD
     TRANSPORT — unpoliced BY CONSTRUCTION rather than by oversight. Bytes fetched here are fetched in the
     PAGE'S OWN REALM with the person's cookies and reach `lib/safe-fetch.js` never, so the scheme allowlist,
     the origin-relative SSRF/PNA guard on the initial AND post-redirect URL, CORB by expected type and the
     credentialed destructive-path deny list applied to the engine's own navigations and to NOTHING that
     arrived this way. It was also a SECOND CREDENTIALED GET of a document the person had already loaded,
     which a server may legitimately answer differently (cache, nonce, personalised SSR, rate limit) — so it
     did not even buy the observed bytes it appeared to.
     WHAT IS SHIPPED INSTEAD IS THE ADDRESS THE BROWSER ACTUALLY NAVIGATED TO, and the custom browser loads
     it as an HTML §7.4 "Navigation" load through the one chokepoint, reading the policy off the reply as
     §7.4.5 "Populating a session history entry"'s attempt-to-populate-the-history-entry's-document requires.
     Everything the old message carried but the address is DERIVED there and better: the response's own header
     list (not one map built in a page realm), Fetch §2.2.6 "Responses"' URL list (which only the fetching zone
     can see), and a refusal that names WHICH rule refused. There is no `CONTENT_SEED_UNAVAILABLE` beside this
     message, and its absence is the design: a report about a load is the LOADER's to make, and this zone no
     longer loads anything.
     WHY NOT `location.href`. An SPA — this tool's PRIMARY target — calls `history.pushState`, whose HTML
     §7.2.5 "The History interface" shared history push/replace state steps run the URL and history update
     steps, MUTATING the document's URL to a client route the server does not serve. Fetching that address
     404s and loses the entire bundle. `PerformanceNavigationTiming`'s `name` is set from the DOCUMENT'S URL
     at the moment the entry is created (Navigation Timing Level 2 §5 "Creating a navigation timing entry":
     "Setup the resource timing entry ... given \"navigation\", document's URL, ..."), i.e. the post-redirect
     address the navigation landed on, frozen there — pushState cannot rewrite it because it is a copy.
     An entry is ABSENT rather than wrong where there was no navigation to time (`about:blank`, a srcdoc
     frame), which is why the fallback is `location.href` and not a catch: the two arms are two facts.
     AND THE SAME §7.2.5 ALGORITHM IS WHY THIS ADDRESS IS SAFE TO SUGGEST FROM AN UNTRUSTED ZONE. "A Document
     document can have its URL rewritten to a URL targetURL" returns false "if targetURL and documentURL
     differ in their scheme, username, password, host, or port components" — so pushState can move the PATH
     and never the ORIGIN. The trusted zone therefore accepts this address only where its origin is the one
     the browser itself reported for this document, which admits every SPA route and no other address at all. */
  function _sendPageSeed() {
    var _nav = performance.getEntriesByType("navigation")[0];
    var seedUrl = (_nav && _nav.name) ? _nav.name : location.href;
    /* MATERIAL ONLY, exactly as the message this replaces was. No origin and no `pageUrl`: an UNTRUSTED zone
       does not get to state an identity, so it does not get to send a field shaped like one — the trusted
       zone mints every browser fact from the MessageSender. `seedUrl` is not that field and is not an
       exception to that rule: it is a SUGGESTION about which document to load, checked against the browser's
       own answer before anything is fetched, and never read as a principal by anything. */
    chrome.runtime.sendMessage({ type: "CONTENT_SEED", seedUrl: seedUrl });
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
      // so we re-send the seed (the cold-start delivery race — the offscreen wasn't alive when content.js
      // shipped at document_idle, so the brain missed our CONTENT_SEED). Idempotent: the brain keys the
      // document by the browser-provided documentId, and a re-seeded document re-loads through the same
      // chokepoint — which is the same one GET the first seed made, not a second transport's second GET.
      _sendPageSeed();
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

  // The initial seed — the address, which the custom browser then loads and parses with Lexbor, so forced
  // execution sees the SERVER'S document (custom elements with their attributes, data islands, the structure
  // the bundle's connectedCallback / React effects depend on) rather than this realm's post-execution DOM.
  _sendPageSeed();

  // Forms are NOT scanned here — Lexbor in the engine worker parses
  // the loaded document and __hostDrive calls form.submit() on each form,
  // routing through the same fetch hook the bundle's own JS reaches.
  // One execution flow.

  /* No content-side DOM walker. Lexbor inside the QuickJS worker
     parses the document the chokepoint loaded into the same spec DOM the
     bundle reads via document.querySelector / dataset / etc. Any
     "DOM context" the analyser needs is read FROM the engine,
     never shipped as a separate snapshot from the page context. */

  // SCRIPTS_LOADED + the MutationObserver removed. One message per document: the
  // single CONTENT_SEED (_sendPageSeed() above) is the whole input. The
  // engine (Lexbor + qjs_run_doc_scripts) parses the loaded document and runs EVERY script in
  // document order in one realm — inline + external <script src>; scripts a page
  // inserts dynamically are discovered by forced exec's createElement host edge,
  // not a content-side observer (which only ever saw what actually fired). No
  // per-script SCRIPT_SOURCE shipping, no load signal, no re-ship churn.

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

      /* Same rule as CONTENT_SEED above: `origin` and `pageUrl` are deleted. `_handleFormSubmit` never read
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
