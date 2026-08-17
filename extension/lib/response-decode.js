// lib/response-decode.js — Response-body protocol decoding: unwrap the protocol chain (JSON/JSONP, JSPB/
// protobuf, gRPC-Web, batchexecute, multipart, async-chunked, SSE/NDJSON), index response values, and feed
// the decoded shapes to learnFromResponse + key extraction. Extracted from the offscreen-brain.js monolith
// (one problem per file); loaded before it, resolves callers (learnFromResponse, extractKeysFromText,
// protobuf/discovery libs) at call-time. The Protocol Reverse-Engineering feature, just relocated.

async function handleResponseBody(tabId, msg, frameId, documentId) {
  if (!msg.url) return;
  await _globalStoreReady;

  // Normalize channel ID from relay messages
  const channelId = msg.channelId || msg.wsId;

  // WebSocket lifecycle: one log entry per connection, messages[] array
  const isWs = msg.method === "WS_OPEN" || msg.method === "WS_CLOSE" ||
    msg.method === "WS_SEND" || msg.method === "WS_RECV";
  if (isWs) {
    if (!_wsConnState.has(documentId)) _wsConnState.set(documentId, new Map());
    const conns = _wsConnState.get(documentId);

    if (msg.method === "WS_OPEN") {
      // Create one combined log entry for this connection
      const entry = {
        id: "ws_" + Date.now() + "_" + Math.random().toString(36).slice(2, 8),
        url: msg.url,
        method: "WEBSOCKET",
        service: extractInterfaceName(new URL(msg.url)),
        timestamp: Date.now(),
        status: 0,
        channelId: channelId,
        wsOpen: true,
        messages: [],
      };
      _pushGlobalLog(entry, tabId, documentId, frameId);
      conns.set(channelId, { url: msg.url, readyState: 1, entryId: entry.id });
      notifyPopup(tabId);
      return;
    }

    if (msg.method === "WS_CLOSE") {
      const conn = conns.get(channelId);
      if (conn) conn.readyState = 3;
      // Mark the log entry as closed
      const entry = globalRequestLog.find((r) => r.channelId === channelId && r.method === "WEBSOCKET" && r.documentId === documentId);
      if (entry) {
        entry.wsOpen = false;
        entry.messages.push({
          dir: "close",
          time: Date.now(),
          body: msg.body || "",
          base64: false,
          status: msg.status || 1000,
        });
        // Cap messages to prevent storage bloat
        if (entry.messages.length > 200) entry.messages.splice(0, entry.messages.length - 200);
      }
      notifyPopup(tabId);
      return;
    }

    // WS_SEND or WS_RECV — append message to existing connection entry
    let entry = globalRequestLog.find((r) => r.channelId === channelId && r.method === "WEBSOCKET" && r.documentId === documentId);
    if (!entry) {
      // WS was opened before extension injected, or after SW restart — create entry now
      entry = {
        id: "ws_" + Date.now() + "_" + Math.random().toString(36).slice(2, 8),
        url: msg.url,
        method: "WEBSOCKET",
        service: extractInterfaceName(new URL(msg.url)),
        timestamp: Date.now(),
        status: 0,
        channelId: channelId,
        wsOpen: true,
        messages: [],
      };
      _pushGlobalLog(entry, tabId, documentId, frameId);
      conns.set(channelId, { url: msg.url, readyState: 1, entryId: entry.id });
    }

    entry.messages.push({
      dir: msg.method === "WS_SEND" ? "sent" : "recv",
      time: Date.now(),
      body: msg.body || "",
      base64: msg.base64Encoded || false,
    });
    entry.timestamp = Date.now(); // Bump to keep it near top in sorted views
    // Cap messages to prevent storage bloat
    if (entry.messages.length > 200) entry.messages.splice(0, entry.messages.length - 200);

    // Key scanning on message body
    if (msg.body) {
      let textBody = msg.body;
      if (msg.base64Encoded) {
        try { textBody = new TextDecoder().decode(base64ToUint8(msg.body)); }
        catch (e) {
          /* Base64-decode or UTF-8 decode of a captured WS message failed
             — likely a binary frame (Protobuf/MessagePack/Flatbuffers)
             rather than text. Surface so key-extraction skip is observable
             and a future diff can route binary WS frames through the same
             protocol classifier the brain runs on HTTP response bodies. */
          console.debug("[brain] WS body decode failed:", e && e.message || e, "url=" + msg.url);
          textBody = null;
        }
      }
      if (textBody) extractKeysFromText(documentId, textBody, msg.url, "response_body");
    }

    notifyPopup(tabId);
    return;
  }

  // postMessage: one log entry per source origin, messages[] array
  // Only PM_RECV — can't wrap window.postMessage (see intercept.js comments)
  if (msg.method === "PM_RECV") {
    let entry = globalRequestLog.find((r) => r.channelId === channelId && r.method === "POSTMESSAGE" && r.documentId === documentId);
    if (!entry) {
      entry = {
        id: "pm_" + Date.now() + "_" + Math.random().toString(36).slice(2, 8),
        url: msg.url,
        method: "POSTMESSAGE",
        service: extractInterfaceName(new URL(msg.url)),
        timestamp: Date.now(),
        status: 0,
        channelId: channelId,
        sourceOrigin: msg.sourceOrigin || "",
        targetOrigin: msg.targetOrigin || "",
        messages: [],
      };
      _pushGlobalLog(entry, tabId, documentId, frameId);
    }

    entry.messages.push({
      dir: "recv",
      time: Date.now(),
      body: msg.body || "",
      base64: false,
    });
    entry.timestamp = Date.now();
    if (entry.messages.length > 200) entry.messages.splice(0, entry.messages.length - 200);

    // Key scanning on message body
    if (msg.body) {
      extractKeysFromText(documentId, msg.body, msg.url, "response_body");
    }

    notifyPopup(tabId);
    return;
  }

  // MessageChannel: MC_OPEN creates entry, MC_RECV appends messages
  if (msg.method === "MC_OPEN") {
    let entry = globalRequestLog.find((r) => r.channelId === channelId && r.method === "MSGCHANNEL" && r.documentId === documentId);
    if (!entry) {
      entry = {
        id: "mc_" + Date.now() + "_" + Math.random().toString(36).slice(2, 8),
        url: msg.url,
        method: "MSGCHANNEL",
        service: extractInterfaceName(new URL(msg.url)),
        timestamp: Date.now(),
        status: 0,
        channelId: channelId,
        sourceOrigin: msg.sourceOrigin || "",
        targetOrigin: msg.targetOrigin || "",
        messages: [],
      };
      _pushGlobalLog(entry, tabId, documentId, frameId);
    }
    notifyPopup(tabId);
    return;
  }

  if (msg.method === "MC_RECV") {
    let entry = globalRequestLog.find((r) => r.channelId === channelId && r.method === "MSGCHANNEL" && r.documentId === documentId);
    if (!entry) {
      // Port message arrived before MC_OPEN (race) — create entry
      entry = {
        id: "mc_" + Date.now() + "_" + Math.random().toString(36).slice(2, 8),
        url: msg.url,
        method: "MSGCHANNEL",
        service: extractInterfaceName(new URL(msg.url)),
        timestamp: Date.now(),
        status: 0,
        channelId: channelId,
        sourceOrigin: "",
        targetOrigin: "",
        messages: [],
      };
      _pushGlobalLog(entry, tabId, documentId, frameId);
    }
    entry.messages.push({
      dir: "recv",
      time: Date.now(),
      body: msg.body || "",
      base64: false,
    });
    entry.timestamp = Date.now();
    if (entry.messages.length > 200) entry.messages.splice(0, entry.messages.length - 200);

    if (msg.body) {
      extractKeysFromText(documentId, msg.body, msg.url, "response_body");
    }

    notifyPopup(tabId);
    return;
  }

  // ─── SSE: streaming events, no request data ─────────────────────────────
  if (msg.method === "SSE") {
    if (!msg.body) return;
    const entry = {
      id: "alt_" + Date.now() + "_" + Math.random().toString(36).slice(2, 8),
      url: msg.url,
      method: "SSE",
      service: extractInterfaceName(new URL(msg.url)),
      timestamp: Date.now(),
      status: msg.status || 200,
      responseBody: msg.body,
      responseBase64: msg.base64Encoded || false,
      mimeType: msg.contentType || "",
      responseHeaders: msg.responseHeaders || {},
    };
    _pushGlobalLog(entry, tabId, documentId, frameId);
    if (msg.body) {
      extractKeysFromText(documentId, msg.body, msg.url, "response_body");
    }
    learnFromResponse(documentId, entry.service, entry);
    notifyPopup(tabId);
    return;
  }

  // ─── HTTP (fetch / XHR): unified request + response ─────────────────────

  // Non-network URL schemes. Page-local blobs/data URIs go through fetch()
  // but produce an empty hostname, which breaks service grouping ("" bucket)
  // and isn't reverse-engineerable API traffic.
  if (/^(blob|data|file|chrome(-extension)?|about):/i.test(msg.url)) return;

  const url = new URL(msg.url);

  // Filter internal extension requests
  if (url.hash.includes("_uasr_send")) return;
  if (url.hash.includes("_internal_probe")) return;

  // Static-asset filtering is CONTENT-based (see classifyResponseAsset
  // below). URL extensions do not decide API-vs-asset here — an API endpoint
  // at /users/42/avatar.png that returns JSON metadata is still an API, and
  // a dynamic endpoint like /models/duck.glb returning a binary 3D model is
  // still an asset. The body's magic bytes are the source of truth.

  // Filter telemetry/tracking noise
  const noisePaths = ["/gen_204", "/client_204", "/jserror", "/ulog", "/log", "/error", "/collect"];
  if (noisePaths.some((p) => url.pathname.includes(p))) return;

  // Skip internal probe requests
  if (url.searchParams.has("_probe")) return;

  const tab = _docForLearning(documentId);
  let service = extractInterfaceName(url);

  // Build request header map from intercept.js capture
  const headerMap = msg.requestHeaders || {};

  // Key scanning: URL + request headers. Record the SPECIFIC header name
  // (lowercased) so on replay we can re-emit the key in the same location
  // instead of defaulting to X-Goog-Api-Key on every non-Google host.
  extractKeysFromText(documentId, msg.url, msg.url, "url");
  for (const [k, v] of Object.entries(headerMap)) {
    extractKeysFromText(documentId, `${k}: ${v}`, msg.url, "header:" + k.toLowerCase());
  }

  // Extract key header values
  let authorization = null, cookie = null, contentType = null;
  let origin = null, referer = null, apiKey = null;
  for (const [name, value] of Object.entries(headerMap)) {
    const lname = name.toLowerCase();
    if (lname === "cookie") { cookie = "[PRESENT]"; headerMap[lname] = "[REDACTED]"; }
    if (lname === "authorization") authorization = value;
    if (lname === "origin") origin = value;
    if (lname === "referer") referer = value;
    if (lname === "content-type") contentType = value;
    if (lname === "x-goog-api-key" || lname === "x-api-key" || lname === "apikey") apiKey = value;
  }

  // Compute rawBodyB64 from request body
  let rawBodyB64 = null;
  if (msg.requestBody) {
    if (msg.requestBodyBase64) {
      rawBodyB64 = msg.requestBody;
    } else {
      rawBodyB64 = uint8ToBase64(new TextEncoder().encode(msg.requestBody));
    }
  }

  // Create entry atomically — request + response together
  const entry = {
    id: "http_" + Date.now() + "_" + Math.random().toString(36).slice(2, 8),
    url: msg.url,
    method: msg.method,
    service: service,
    timestamp: Date.now(),
    status: msg.status,
    completedAt: Date.now(),
    requestHeaders: headerMap,
    contentType: contentType || "",
    rawBodyB64: rawBodyB64,
    responseBody: msg.body || null,
    responseBase64: msg.base64Encoded || false,
    mimeType: msg.contentType || "",
    responseHeaders: msg.responseHeaders || {},
    /* `frameId` IS NOT WRITTEN HERE, AND IT USED TO BE WRITTEN TWICE. This built it as `frameId ?? 0` and then
       handed the same `frameId` to `_pushGlobalLog`, whose first act is `if (entry.frameId == null)
       entry.frameId = frameId != null ? frameId : 0` — so the field had two writers with the same fallback,
       the second one dead for this entry and live for every other entry in this file (the WebSocket,
       postMessage and MessageChannel ones never set it). CLAUDE.md §Offensive-programming names the `??`
       exactly: it turns "the sender carried no frameId" into a plausible 0, which is the top-level traversable
       — a real value, indistinguishable from a measurement, and the popup's tab/frame filter reads it. One
       field, one writer: the log pusher's, which is where the other five entry shapes already leave it. */
    // Bundle call-site stack captured at the network-API hook in intercept.js
    // (`new Error().stack`, wrapper frames stripped). For a [live]-only
    // endpoint (one the forced-execution engine didn't reach), this names the
    // exact bundle function that fired the request — diagnostic provenance
    // for the network-vs-AST diff.
    callStack: msg.callStack || null,
  };

  /* NO `lastSeen` STAMP ON A MATCHING ENDPOINT, BECAUSE THIS LOOKUP COULD NEVER MATCH ONE. It built the key
     as `method + " " + hostname + pathname` and lib/merge.js — the extension's ONLY `endpoints.set` — mints
     `"AST " + method + " " + hostname + path`. Every get() therefore missed, and the miss is invisible by
     construction: `if (_ep)` reads exactly like "this live request hit an endpoint we had not learned",
     which is a real and common outcome. The field it wanted to write had one reader, in mergeToGlobal, and
     that reader did not propagate it either — it set the tab entry's lastSeen to Date.now() because the
     GLOBAL entry had any, which times the merge rather than the sighting. Writer that cannot fire, reader
     that fabricates: both are gone. "This learned endpoint was observed firing live" is worth having, and
     when something needs it, it is keyed on the one key format merge.js mints and it records the time of
     THIS request. */

  // Update auth context
  if (authorization || cookie) {
    tab.authContext = tab.authContext || {};
    if (authorization) tab.authContext.hasAuthorization = true;
    if (cookie) tab.authContext.hasCookies = true;
    if (origin) tab.authContext.origin = origin;
  }

  // Key scanning on response body
  if (msg.body) {
    let textBody = msg.body;
    if (msg.base64Encoded) {
      try { textBody = new TextDecoder().decode(base64ToUint8(msg.body)); }
      catch (e) {
        /* Base64 / UTF-8 decode failure on a captured response body — most
           often a binary frame (Protobuf / gRPC-Web / image / gzipped) that
           the text-decoder rejects. Surface so the skipped key-extraction
           is observable instead of silent (was `catch (_) { textBody = null }`,
           which dropped the diagnostic). The classifier below still runs on
           the raw bytes via magic-byte sniff. */
        console.debug("[brain] response body text-decode failed:", e && e.message || e, "url=" + msg.url);
        textBody = null;
      }
    }
    if (textBody) extractKeysFromText(documentId, textBody, msg.url, "response_body");
  }

  // Classify the captured response by CONTENT — magic bytes first, the server's
  // declared type only as a weaker cross-check. It decides how much schema to
  // synthesize around the capture. Three buckets result:
  //   structured API → full learning (schema, methods, discovery probe)
  //   image API     → learn URL + request body + auth; skip response schema;
  //                    create method entry (user can inspect/replay)
  //   boring asset  → log + key-extract + endpoint tracking ONLY; no method
  //                    entry, no discovery probe, no response schema
  //
  // "Boring" = static asset with zero dynamic signals. Any one of (query
  // string, auth header, request body, non-GET) promotes it to image API.
  // Keeps a signed-URL photo or avatar-generation endpoint visible as an
  // API while preventing 124 synthetic "methods" for HLS video segments
  // or a hash-busted CDN .glb file polluting the discovery doc.
  //
  // Nothing is ever hidden from the log — every request, including boring
  // CDN fetches, is captured and surfaced to the user. The bucket only
  // decides how much schema to synthesize around it.
  /* `entry._assetKind`, `entry._assetLabel` and `entry._boring` USED TO BE WRITTEN HERE AND WERE READ BY
     NOTHING — not by this file, not by learn.js, not by serialize.js, and not by the popup, which receives
     every log entry whole and rendered neither of them. Three fields written on every captured response for
     the whole life of this file, and the comment above claimed the first two were how a signed-URL photo
     endpoint stayed "visible in the log". CLAUDE.md §Offensive-programming states the mirror rule — "a name
     that is READ somewhere and WRITTEN nowhere is a broken contract" — and this is the same contract broken
     from the other end: a producer nobody consumes reads as a capability the surface has. The two locals below
     are the live half; they gate every learning call in this function. */
  /* WHAT THIS RESPONSE IS, ANSWERED WHERE IT IS READ. This was hoisted to the top of the function to be an
     `await` on the browser process, and the hoist was load-bearing FOR THAT SHAPE: from `_docForLearning` to
     the last `notifyPopup` this function runs synchronously — it holds a DocData by reference while
     learnFromRequest looks the same document up again by id — so a suspension in the middle could write one
     response into two records. The call is SYNCHRONOUS again, so there is no suspension to place and the
     classification belongs beside its two readers.
     THE CLASSIFIER IS lib/discovery.js's, AND IT IS NOT A SECOND SNIFFER. safeFetch is the only source of
     sniffing for the bytes SAFEFETCH FETCHED, and it stamps its answer on the reply record it hands the
     renderer. These bytes are not those: intercept.js captured them off the live page and relayed them here,
     and safeFetch has never seen the response they came from. */
  const _assetClass = classifyResponseAsset(msg.body, msg.base64Encoded, {
    /* Fetch §2.2.6's FILTERED RESPONSE KIND. An OPAQUE no-cors response has a null body and an empty header
       list, so an unreadable response and an empty one are byte-identical here and only the zone that held the
       Response can tell them apart. intercept.js reads it off `clone.type`; content.js relays it (it did not,
       for the whole life of the field, which is why this branch had never once run). The XHR path emits none
       because XHR cannot produce one — its own spec fails a cross-origin response CORS did not allow rather
       than handing back an opaque one — so absence is "not applicable", not "not sent". */
    responseType: msg.responseType || null,
    responseContentType: (msg.responseHeaders &&
      (msg.responseHeaders["content-type"] || msg.responseHeaders["Content-Type"])) || null,
  });
  const _isAsset = _assetClass.kind === "asset";
  /* AND THE LOG ENTRY CARRIES THE RULE, because it HAS a reader and the comment above got that half wrong.
     `_assetKind`/`_assetLabel`/`_boring` were deleted here as written-for-nobody, and two of them were —
     but `testing/harness.js`'s `netdiff --unused` diagnostic tested `r._assetKind === "asset"` to tell a
     static asset apart from an endpoint the analyzer missed, so the delete left a reader with no writer and
     the diagnostic silently fell through to a hand-rolled content-type table of its own. ONE field replaces
     all three and carries both halves: non-null IS the verdict, and the string is the classifier's own LABEL
     — the sniffed type, with the server's disagreeing claim in parentheses where there was one, which is what
     tells a reader that the body was mislabelled rather than merely typed.
     There is no `_boring` beside it — that one really was read by nothing, and it is derived below from
     facts the entry already carries. */
  entry._assetReason = _isAsset ? _assetClass.label : null;
  const _isBoringFetch = _isAsset &&
    msg.method === "GET" &&
    !url.search &&
    !rawBodyB64 &&
    !authorization &&
    !cookie &&
    !apiKey;

  // Snapshot discovery status before learnFromRequest (which creates a virtual doc)
  const preLearnDiscovery = tab.discoveryDocs.get(service);

  // Decode request body (protobuf/JSPB/JSON) — must happen BEFORE
  // learnFromRequest so entry.isJson / entry.decodedBody are set when
  // schema learning records body-field stats. Downstream code further
  // in this function (probing trigger, chain analysis) also reads
  // these fields, so the single decode here covers both.
  const logContentType = contentType || "";
  const isProtobuf = logContentType.includes("protobuf") || url.pathname.includes("$rpc");
  if (rawBodyB64) {
    try {
      const bytes = base64ToUint8(rawBodyB64);
      if (isProtobuf) {
        if (logContentType.includes("json") || logContentType.includes("text")) {
          try {
            const text = new TextDecoder().decode(bytes);
            if (text.trim().startsWith("[")) {
              const json = JSON.parse(text);
              if (Array.isArray(json)) {
                entry.decodedBody = jspbToTree(json);
                entry.isJspb = true;
              }
            }
          } catch (e) {
            console.debug("[brain] JSPB-in-text body parse failed:", e && e.message || e, "url=" + msg.url);
          }
        } else {
          entry.decodedBody = pbDecodeTree(bytes, 8, (val) => {
            if (typeof val === "string") {
              extractKeysFromText(documentId, val, msg.url, "protobuf_body");
            }
          });
        }
      } else if (logContentType.includes("x-www-form-urlencoded")) {
        try {
          const text = new TextDecoder().decode(bytes);
          const params = new URLSearchParams(text);
          const fReq = params.get("f.req");
          if (fReq) {
            const json = JSON.parse(fReq);
            if (Array.isArray(json)) {
              entry.decodedBody = jspbToTree(json);
              entry.isJspb = true;
            }
          }
        } catch (e) {
          console.debug("[brain] form-urlencoded f.req decode failed:", e && e.message || e, "url=" + msg.url);
        }
      } else {
        // Try JSON parsing for any non-protobuf, non-form-encoded body.
        // Many analytics SDKs (reddit's /svc/shreddit/events, GA, sentry,
        // segment, ...) send JSON bodies with `Content-Type: text/plain`
        // to bypass CORS preflight. Gating on the content-type alone
        // misses every one. Body STRUCTURE is authoritative: if it parses
        // as a JSON object/array, treat as JSON. A failed parse is the
        // expected outcome for text/binary bodies that aren't JSON; we
        // surface the diagnostic so a NEW class of mis-detected body is
        // visible rather than silently absent from field-extraction.
        try {
          const text = new TextDecoder().decode(bytes);
          const trimmed = text.trimStart();
          if (trimmed.startsWith("{") || trimmed.startsWith("[")) {
            const json = JSON.parse(text);
            if (json && typeof json === "object") {
              entry.decodedBody = json;
              entry.isJson = true;
            }
          }
        } catch (e) {
          console.debug("[brain] structural-JSON request-body parse failed:", e && e.message || e, "url=" + msg.url);
        }
      }
    } catch (e) {
      console.debug("[brain] request-body decode outer failed:", e && e.message || e, "url=" + msg.url);
    }
  }

  // Learn from request — skipped only for "boring" fetches. Image APIs
  // (any dynamic signal present) still learn URL + request body + auth so
  // they can be replayed and inspected.
  if (!_isBoringFetch) {
    learnFromRequest(documentId, service, entry, headerMap);

    // learnFromRequest may migrate the service (e.g. hostname-fallback →
    // path-common-prefix) when observed-prefix clustering promotes the
    // bucket. Use the post-migration name for all downstream lookups —
    // our pre-migration `service` may now point at a bucket that was
    // emptied and deleted during the migration.
    if (entry.interfaceName && entry.interfaceName !== service) {
      service = entry.interfaceName;
    }

    // If the response was binary media (and it's a real endpoint), annotate
    // the method entry with the detected media type so the consumer knows
    // not to expect a JSON/protobuf response schema.
    if (_isAsset && entry.methodId) {
      const _mid = entry.methodId;
      const _methodName = _mid.slice(_mid.lastIndexOf(".") + 1);
      const _methods = tab.discoveryDocs.get(service)?.doc?.resources?.learned?.methods;
      if (_methods && _methods[_methodName]) {
        _methods[_methodName]._responseKind = "asset";
        /* THE RULE THAT DECIDED is the label, and it is the whole of what the browser process answers besides
           the verdict. It used to be a MIME-ish string the JS classifier assembled — a sniffed type with an
           occasional " (declared …)" note glued on — and the popup's reader still cuts it at a semicolon that
           a rule name never has. A rule name is what a human wants there anyway: `font` and `script` say what
           the body is, and `sniffed-image/png` says the server was wrong about it. */
        _methods[_methodName]._responseLabel = _assetClass.label;
      }
    }
  }
  mergeToGlobal(tab);

  // Track which pages/origins this service has been used from.
  var _svcDocEntry = tab.discoveryDocs.get(service);
  if (_svcDocEntry) {
    if (!_svcDocEntry.pageUrls) _svcDocEntry.pageUrls = new Set();
    // The requesting frame's AUTHORITATIVE origin, by the response's documentId,
    // from the DURABLE _docOrigins map — never the transient script buffer (gone
    // after review) and never URL-parsed (about:blank parses to a bogus "null").
    var _svcFrameOrigin = _originForDoc(documentId);
    // pageUrls: the tab's last-seen top url (UI display of "used from"). This is a
    // URL for the human, NOT an origin decision.
    if (tab.url) _svcDocEntry.pageUrls.add(tab.url);
    // frameOrigins: the SET of authoritative origins that used this service. We do
    // NOT derive a "top page origin" from the tab url to flag iframe-ness — mapping
    // a tab to an origin assumes the main frame and races navigation (banned). A
    // cross-origin caller stands out in the set on its own.
    if (_svcFrameOrigin) {
      if (!_svcDocEntry.frameOrigins) _svcDocEntry.frameOrigins = new Set();
      _svcDocEntry.frameOrigins.add(_svcFrameOrigin);
    }
  }

  // Protobuf probing trigger — skip for boring asset fetches.
  if (!_isBoringFetch && isProtobuf && msg.method === "POST") {
    const discoveryStatus = tab.discoveryDocs.get(service);
    const doc = discoveryStatus?.doc;
    const match = doc ? findDiscoveryMethod(doc, url.pathname, msg.method) : null;
    const isLearnedOnly = match &&
      discoveryStatus.doc.resources?.learned?.methods[match.method.id.split(".").pop()];
    if (!match || isLearnedOnly) {
      const keysForService = collectKeysForService(tab, service, url.hostname);
      if (apiKey && !keysForService.includes(apiKey)) keysForService.push(apiKey);
      performProbeAndPatch(documentId, service, msg.url, apiKey || keysForService[0] || null);
    }
  }

  /* AUTOMATIC BACKGROUND DISCOVERY — skip for boring fetches (probing /.well-known/openapi.json on a CDN is
     wasted traffic).

     THE 300-SECOND COOLDOWN IS DELETED. `preLearnDiscovery._failedAt && Date.now() - _failedAt < 300000`
     stood in this condition and suppressed every discovery attempt for five minutes after one came up empty.
     §NO BOUNDS names the shape: "Never a depth/step/TIME/run/memory/recursion cap … Only EMITTED OUTPUT —
     never identity — proves a flow is done." A clock deciding when a service may be asked again is a bound in
     both directions — it suppresses an attempt that would now succeed (an API key learned two seconds later
     unlocks documents the first sweep could not read) and it re-fires an identical sweep once the timer
     expires. `_failedAt` went with it; nothing else read it.

     WHAT REMAINS IS NOT A BOUND. `!preLearnDiscovery || status === "not_found"` is §Attacker sources'
     "one-per-endpoint (no method is universally safe …), never a blind sweep": a service whose document is
     already `found` has nothing to re-ask, and one that is `pending` has the same GETs in flight this instant.
     A `not_found` service is asked AGAIN precisely because the second ask is a DIFFERENT request — the key set
     below has grown since the first, and a candidate carrying a key the page had not yet used is a URL that
     has never been fetched. */
  if (!_isBoringFetch && (!preLearnDiscovery || preLearnDiscovery.status === "not_found")) {
    const discoveryStatus = tab.discoveryDocs.get(service);
    if (discoveryStatus) {
      discoveryStatus.status = "pending";
    } else {
      tab.discoveryDocs.set(service, { status: "pending", seedUrl: msg.url });
    }
    const keysForService = collectKeysForService(tab, service, url.hostname);
    if (apiKey && !keysForService.includes(apiKey)) keysForService.push(apiKey);
    fetchDiscoveryForService(documentId, service, url.hostname, keysForService, msg.url);
  }

  // Error-based service-info probe (lib/req2proto.js; README "Error-Based Schema
  // Probing"). An intentionally-malformed POST to the endpoint provokes a gapi
  // error envelope, learning its CANONICAL service/method + OAuth scopes, merged
  // into the discovery doc — EVEN when a real doc exists, since the probe can
  // surface a HIDDEN service/method/scope the doc omits. This is the automatic
  // form of the DISCOVER_SERVICE message (which is a user action over one
  // endpoint, not a background step). POST endpoints only — the probe's own
  // method, so it introduces no method the page did not already use.
  if (!_isBoringFetch && msg.method === "POST") {
    // Probe the POST request URL DIRECTLY. handleResponseBody sees EVERY captured
    // POST; the request need NOT be a "learned" endpoint in globalStore.endpoints
    // (Google batchexecute / $rpc calls are logged + classified into a service but
    // are not endpoint-keyed).
    const _siUrl = new URL(url.href);
    _siUrl.searchParams.delete("key");
    const _siKey = _siUrl.hostname + _siUrl.pathname;
    if (!_svcInfoProbedUrls.has(_siKey)) {
      _svcInfoProbedUrls.add(_siKey);
      const _siHeaders = {};
      if (apiKey) _siHeaders["X-Goog-Api-Key"] = apiKey;
      const _siPath = _siUrl.pathname;
      discoverServiceInfo(_siUrl.toString(), _siHeaders, { fetchFn: makePageFetchFn(tab.tabId, documentId) }).then((result) => {
        if (!result) return;
        let _merged = false;
        const _scopes = Array.isArray(result.scopes) ? result.scopes.filter(Boolean) : [];
        if (_scopes.length) { tab.scopes.set(service, _scopes); _merged = true; }
        const _doc = globalStore.discoveryDocs.get(service)?.doc;
        if (_doc) {
          const _m = findDiscoveryMethod(_doc, _siPath, "POST")?.method;
          if (_m) {
            if (_scopes.length && (!Array.isArray(_m.scopes) || !_m.scopes.length)) { _m.scopes = _scopes; _merged = true; }
            if (result.service && _m._probedService !== result.service) { _m._probedService = result.service; _merged = true; }
            if (result.method && _m._probedMethod !== result.method) { _m._probedMethod = result.method; _merged = true; }
          }
        }
        if (_merged) { tab.probeResults.set(`svcinfo:POST ${_siPath}`, result); mergeToGlobal(tab); notifyPopup(tab.tabId); }
      }).catch((e) => { if (typeof console !== "undefined") console.debug("[brain] @WHY {phase:'svcinfo-probe',reason:'" + (e && e.message || e) + "'}"); });
    }
  }

  // Extract OAuth scopes from 403 www-authenticate response header
  if (msg.status === 403 && msg.responseHeaders) {
    const wwwAuth = msg.responseHeaders["www-authenticate"];
    if (wwwAuth) {
      const scopeMatch = wwwAuth.match(/scope="([^"]*)"/);
      if (scopeMatch) {
        const scopeList = scopeMatch[1].split(/\s+/).filter(Boolean);
        if (scopeList.length > 0) {
          tab.scopes.set(service, scopeList);
          const endpointKey = `${msg.method} ${url.hostname}${url.pathname}`;
          const ep = tab.endpoints.get(endpointKey);
          if (ep) ep.requiredScopes = scopeList;
        }
      }
    }
  }

  // Add to request log
  _pushGlobalLog(entry, tabId, documentId, frameId);

  // Learn from response — skip for static assets.
  if (entry.responseBody && !_isAsset) {
    learnFromResponse(documentId, service, entry);
  }

  mergeToGlobal(tab);
  notifyPopup(tabId);
}

// ─── Cross-Script AST Buffering ──────────────────────────────────────────────

// _bufferScript / _scriptBufferDecrementPending / _fetchAndBufferScript removed:
// the per-script SCRIPT_SOURCE buffering machinery is gone. content.js ships one
// CONTENT_HTML per document; the engine (Lexbor + qjs_run_doc_scripts) sources the
// document's scripts. The per-document buffer now holds only {tabId, origin, url,
// pageHtml, chunk-state}; the offscreen no longer fetches/combines scripts.
