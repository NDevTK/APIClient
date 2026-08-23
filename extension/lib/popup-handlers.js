// lib/popup-handlers.js — Popup command handlers: the dispatch for every popup-triggered action (get state,
// send/replay, export, rename, clear, exploit-probe, message-console, etc.). The brain wires the onMessage
// listener to handlePopupMessage; this file holds the command logic. Extracted from the offscreen-brain.js
// monolith (one problem per file); loaded before it, resolves the per-command backends (lib/send.js,
// lib/encode.js, buildExportRequest, startExploitProbe, serializers) at call-time.

async function handlePopupMessage(msg, _sender, sendResponse) {
  await _globalStoreReady;
  const tabId = msg.tabId;            // aggregate/UI filter + Chrome routing (NEVER a storage key)
  const documentId = msg.documentId;  // per-document RPC target (resolved via _docFromMsg)

  switch (msg.type) {
    case "GET_STATE": {
      // A matched document, else a transient empty view so the popup still shows
      // the GLOBAL cumulative moat (serializeTabData overlays globalStore).
      const tab = _docFromMsg(msg) || _emptyDocView();
      const data = serializeTabData(tab);
      sendResponse(data);
      return;
    }

    case "GET_FRAMES": {
      /* THE BROWSER ANSWERS THREE DIFFERENT THINGS AND THIS HANDLER USED TO ANSWER ONE FABRICATED THING.
         chrome/common/extensions/api/web_navigation.json declares getAllFrames' result `"optional": true`,
         "A list of frames in the given tab, null if the specified tab ID is invalid", and every item's
         `documentId` as a NON-optional string (only `parentDocumentId` is optional). So the producer speaks:
           • null            — THE TAB ID IS INVALID (it went away between the popup's tabs.query and this call);
           • []              — the tab resolves and holds NO web document webNavigation reports (a chrome://
                               page, the New Tab page, an extension page — MEASURED: an extension page
                               answers exactly [], not an error);
           • [frame, …]      — the frames, each carrying a real documentId.
         What stood here collapsed all three into a MANUFACTURED frame:
           `catch { console.debug }` → `_wnFrames || []` → `out.length ? out : [{documentId:null,url:"",…}]`,
         plus `f.documentId || null` defaulting a field the IDL marks non-optional. The fabricated frame is
         CLAUDE.md's named defect exactly — a plausible datum indistinguishable from a measurement — and its
         whole observable effect was measured in the rendered popup: with no web document in the tab the
         Request Context stayed visible and read "Credentials: waiting for document…" and the Send button
         "Waiting for the page's document to be ready…", FOREVER, because nothing was coming. The browser had
         already answered definitively; this handler turned that answer into a pending state. It also defeated
         popup.js's own `frames.length > 0` assert, which the fabrication satisfied — the assert asserted that
         the fabricator had run.
         frameType stays the only trustworthy "is this the main frame" signal — a fenced frame reports
         "fenced_frame", so it CANNOT impersonate the outermost frame (a self-reported isTop could). Each
         frame's AUTHORITATIVE origin comes from the DURABLE _docOrigins map by documentId (it survives buffer
         reclamation after review); frameId is Chrome's routing id for tabs.sendMessage, never an identity.
         NO try/catch: the SW is this extension's own service worker and a sendMessage to it STARTS it, so a
         rejection is that relay broken, not a state — it must be loud here, at its origin, rather than become
         a frame list. (Checked before blaming it: the reported "swRpc failed — the MV3 service worker was not
         running" never happened; getAllFrames answered, with the real documentId, on every measurement.) */
      DCHECK(typeof tabId === "number",
             "GET_FRAMES was asked for a frame tree without a numeric tabId — the popup sends the tab its " +
             "own tabs.query answered with, so anything else is that query broken and webNavigation would " +
             "be asked about a tab nobody named");
      const _wnFrames = await swRpc("webNavigation.getAllFrames", { tabId });
      if (_wnFrames === null || _wnFrames === undefined) {
        // The IDL's own words for this: "null if the specified tab ID is invalid". A POSITIVE fact about the
        // TAB — never flattened into a statement about its frames. (`undefined` cannot survive the structured
        // clone in swRpc's {ok,result} envelope, so it arrives as null; both spellings mean the one thing.)
        sendResponse({ tabResolved: false, frames: [] });
        return;
      }
      DCHECK(Array.isArray(_wnFrames),
             "webNavigation.getAllFrames answered with neither an array nor null — its IDL declares exactly " +
             "those two (`optional` array, null for an invalid tab id), so a third shape is the swRpc relay " +
             "corrupting the reply and every document identity below would be built out of it");
      sendResponse({
        tabResolved: true,
        frames: _wnFrames.map((f) => {
          DCHECK(typeof f.documentId === "string" && f.documentId.length > 0,
                 "a frame the browser reported carries no documentId — web_navigation.json declares it a " +
                 "non-optional string, and it is the ONLY stable per-document identity this zone keys on, so " +
                 "without it the popup cannot name the document it is looking at and would show the global " +
                 "moat as if it were this page's");
          DCHECK(typeof f.frameId === "number",
                 "a frame the browser reported carries no numeric frameId — it is Chrome's routing id for " +
                 "tabs.sendMessage, so a send would be routed at nothing");
          DCHECK(typeof f.url === "string",
                 "a frame the browser reported carries no url string — the frame picker labels the frame " +
                 "with it and lib/popup-discovery.js reads it as the pinned document's ADDRESS");
          DCHECK(f.frameType === "outermost_frame" || f.frameType === "fenced_frame" || f.frameType === "sub_frame",
                 "a frame the browser reported carries a frameType outside extensionTypes.FrameType (`" +
                 f.frameType + "`) — isMain is decided by it precisely because a fenced frame must not be " +
                 "able to impersonate the outermost one, and an unknown value would silently answer 'not main'");
          return {
            frameId: f.frameId,
            documentId: f.documentId,
            url: f.url,
            origin: _originForDoc(f.documentId),
            isMain: f.frameType === "outermost_frame",
          };
        }),
      });
      return;
    }

    /* THE THREE ON-DEMAND LEARNING COMMANDS. Each is a USER ACTION over one endpoint or service — the popup
       asks, this zone performs the request as the page, and the answer merges into the store. They were
       deleted on the ground that nothing in the popup posts them, which is true and is a missing BUTTON: a
       backend with no caller is a UI gap, and deleting the backend closes the gap by removing the capability.
       The backends are lib/req2proto.js (`probeEndpoint`, `discoverServiceInfo`) and lib/discovery-probe.js
       (`fetchDiscoveryForService`). */

    case "PROBE_ENDPOINT": {
      const _pdoc = _docFromMsg(msg);
      if (!_pdoc) { sendResponse(null); return; }
      probeEndpoint(_pdoc.documentId, msg.endpointKey).then((result) => {
        sendResponse(result);
      });
      return true;
    }

    case "DISCOVER_SERVICE": {
      const tab = _docFromMsg(msg);
      if (!tab) { sendResponse(null); return; }
      const ep = tab.endpoints.get(msg.endpointKey);
      if (!ep) {
        sendResponse(null);
        return;
      }

      const headers = {};
      const discoverUrl = new URL(ep.url);
      discoverUrl.searchParams.delete("key");
      const svc = ep.service || extractInterfaceName(new URL(ep.url));
      /* THE KEY COMES FROM THE KEY STORE, AND ITS INJECTION POINT IS THE ONE IT WAS OBSERVED IN. `if (ep.apiKey)
         { ep.apiKeySource === "url" ? … : X-Goog-Api-Key }` stood here and NEITHER name exists on an endpoint
         record: lib/merge.js is the extension's only `endpoints.set` and writes {url, method, host, path,
         service, source, pageUrl, requiredHeaders, pathParams, firstSeen}. Both reads were undefined on every
         endpoint, so the probe went out with no key while looking like it was choosing where to put one.
         collectKeysForService is the real producer (the search lib/send.js uses), and lib/keys.js records WHERE
         each key was seen — "url" or "header:<name>". A key seen only in a response body has no observed
         injection point and none is invented: a guessed `X-Goog-Api-Key` on a third-party host is a Google
         header the server does not recognise, which is a fabricated request, not a probe. */
      const _svcKeys = collectKeysForService(tab, svc, discoverUrl.hostname);
      if (_svcKeys.length) {
        const _k = _svcKeys[0];
        const _kEntry = tab.apiKeys.get(_k);
        DCHECK(!!_kEntry, "a key collectKeysForService returned is not in this document's key map — it reads " +
                          "exactly that map, so a miss is the two disagreeing about what was learned");
        DCHECK(typeof _kEntry.source === "string",
               "an API-key entry carries no source — lib/keys.js stamps the context every key was matched in " +
               "('url', 'header:<name>', 'response_body'), and without it there is no observed place to put " +
               "this key back and the probe would have to invent one");
        if (_kEntry.source === "url") discoverUrl.searchParams.set("key", _k);
        else if (_kEntry.source.startsWith("header:")) headers[_kEntry.source.slice("header:".length)] = _k;
      }
      const fetchFn = makePageFetchFn(tab.tabId, tab.documentId);
      discoverServiceInfo(discoverUrl.toString(), headers, { fetchFn }).then(
        (result) => {
          tab.probeResults.set(`svc:${msg.endpointKey}`, result);
          if (result.scopes?.length) {
            tab.scopes.set(svc, result.scopes);
          }
          mergeToGlobal(tab);
          notifyPopup(tab.tabId);
          sendResponse(result);
        },
      );
      return true;
    }

    case "FETCH_DISCOVERY": {
      const tab = _docFromMsg(msg);
      if (!tab) { sendResponse(null); return; }
      /* THE HOSTNAME IS ONE THE PAGE ACTUALLY REACHED, NEVER `${service}.googleapis.com`. That fallback was
         here and it invented an address no code computed — §RUN, DON'T MATCH — so a service with no learned
         endpoint would have sent this zone probing a host the bundle never named. A caller that names neither
         a hostname nor an endpoint has nothing to discover.
         AND IT IS A HOST OF *THIS* SERVICE. `tab.endpoints.values().next().value` took whichever endpoint the
         map happened to hold first, of any service, so a caller naming a service the document knows would be
         answered with an unrelated service's host — one arbitrary record standing in for the named one. */
      let ep = null;
      for (const _e of tab.endpoints.values()) { if (_e.service === msg.service) { ep = _e; break; } }
      const hostname = msg.hostname || (ep && ep.host) || null;
      if (!hostname) { sendResponse(null); return; }
      const apiKeys = collectKeysForService(tab, msg.service, hostname);
      /* NO `ep.apiKey` LIMB. An endpoint record has no such field (lib/merge.js, its only producer, writes
         none), so it added undefined to the candidate list on every call. */
      if (msg.apiKey && !apiKeys.includes(msg.apiKey)) apiKeys.push(msg.apiKey);
      fetchDiscoveryForService(tab.documentId, msg.service, hostname, apiKeys).then(
        () => {
          sendResponse(serializeTabData(tab));
        },
      );
      return true;
    }

    case "CLEAR_TAB": {
      // The main Clear button: delete ALL extension data and stop ALL work.
      (async function () {
        /* 1. STOP ALL WORK FIRST — drop every live engine out of the host pool, reject the documents still
              waiting for a slot, and empty the cross-session frontier's IndexedDB, so nothing in flight and
              nothing parked can repopulate what step 2 wipes. The `try {} catch (e) {}` around this is gone
              with the reason it existed: the bridge answered AST_CLEAR "unknown type" and this swallowed the
              refusal, so for as long as it shipped the Clear button stopped nothing at all and the engines
              merged their findings straight back into the store the user had just emptied. */
        const _cleared = await sendToOffscreen({ type: "AST_CLEAR" });
        DCHECK(!!(_cleared && _cleared.success),
               "AST_CLEAR did not succeed — the Clear button's whole contract is that no work survives it, " +
               "and continuing to the wipe with engines still running repopulates the store as it is emptied");
        // 2. Global findings + the persisted gapiStore (cleared inside clearGlobalStore).
        await clearGlobalStore();
        /* 3. Every LEARNED fact in the per-document views + the in-memory logs, so the next navigation starts
              from a genuinely empty slate. `state.docs.clear()` stood here: it also deleted the BROWSER-STATED
              IDENTITY of documents that are still open, and identity is what every learner resolves its writes
              through — the first request after a Clear would arrive for an unregistered document, which is a
              DCHECK. `_clearDocLearning` empties the learned half and keeps the fact half (offscreen-brain.js). */
        _clearDocLearning();
        globalRequestLog = [];
        _scriptBuffers.clear();
        _wsConnState.clear();
        sendResponse({ ok: true });
      })();
      return true;   // async sendResponse
    }

    /* NO GET_ANALYSIS_OPTS / SET_ANALYSIS_OPTS. They read and wrote an `analysisOpts` IDB record for two
       popup controls whose value was then dispatched to a bridge that answered "unknown type" — refused
       inside a catch, so the user's setting reached nothing and said nothing. The controls are deleted (see
       popup.html): the hot working set is bounded by resident WASM memory rather than a user-set instance
       count, and a wall-clock throttle over the cooperative quantum is a step cap. */

    case "CLEAR_LOG": {
      // Request logs live in-memory only now (session storage layer removed
      // — the offscreen document's stable lifetime makes the mirror moot).
      // Clearing the in-memory array IS the operation.
      if (msg.clearAll) {
        globalRequestLog = [];
      } else {
        if (tabId == null) return;
        globalRequestLog = globalRequestLog.filter(function (r) { return r.tabId !== tabId; });
      }
      sendResponse({ ok: true });
      return;
    }

    case "GET_TAB_LIST": {
      // Closed tabs' documents stay in state.docs (d.closed=true) so one pass
      // covers live AND closed entries.
      // Roll up the documentId-keyed docs into one row per tab (the network tab
      // filters by tab purely in the UI). A tab is "closed" only once ALL its
      // documents are; title/url come from the main-frame document.
      // Count from the GLOBAL log (grouped by tabId); enrich title/url/closed
      // from the tab's documents (a tab whose documents predate this offscreen
      // leaves its log entries but no identity, so it degrades to "Tab N").
      const _byTab = new Map();
      for (const r of globalRequestLog) {
        if (r.tabId == null) continue;
        let row = _byTab.get(r.tabId);
        if (!row) { row = { tabId: r.tabId, title: "", url: "", count: 0, closed: false }; _byTab.set(r.tabId, row); }
        row.count++;
      }
      _byTab.forEach((row, tid) => {
        const docs = docsForTab(tid);
        if (docs.length) {
          const main = docs.find((dd) => dd.frameId === 0) || docs[0];
          row.url = main.url || row.url; row.title = main.title || row.title;
          row.closed = docs.every((dd) => !!dd.closed);
        }
      });
      const tabs = [];
      _byTab.forEach((r) => tabs.push({ tabId: r.tabId, title: r.title || ("Tab " + r.tabId), url: r.url, count: r.count, closed: r.closed }));
      sendResponse(tabs);
      return;
    }

    case "GET_ALL_LOGS": {
      const result = {};
      const filter = msg.filter; // "all" | tabId (number)
      // Aggregate per tab across its documents; each entry keeps its own
      // documentId/frameId for the popup's per-frame sub-views.
      for (const ent of globalRequestLog) {
        if (ent.tabId == null) continue;
        if (filter !== "all" && filter !== ent.tabId) continue;
        let r = result[ent.tabId];
        if (!r) { r = { meta: { title: "", url: "", closed: false }, requestLog: [] }; result[ent.tabId] = r; }
        r.requestLog.push(ent);
      }
      for (const tid in result) {
        const docs = docsForTab(Number(tid));
        if (docs.length) {
          const main = docs.find((dd) => dd.frameId === 0) || docs[0];
          result[tid].meta.url = main.url || result[tid].meta.url;
          result[tid].meta.title = main.title || result[tid].meta.title;
          result[tid].meta.closed = docs.every((dd) => !!dd.closed);
        }
        if (!result[tid].meta.title) result[tid].meta.title = "Tab " + tid;
      }
      sendResponse(result);
      return;
    }

    case "GET_DISCOVERY_CHANGES": {
      sendResponse(Object.fromEntries(globalStore.discoveryChanges));
      return;
    }

    /* THE ENGINE'S OWN RUN RECORD, WHICH NO HUMAN SURFACE HAS EVER SHOWN. solver/result.c emits eight cost
       counters in ONE snprintf, bridge.js `assertResultDocument` checks every one of them field-for-field,
       and `linesToAnalysis` writes one record per run onto `self._engineLog` — and grep found exactly one
       reader for that array, `self.rendererPoolProbe`, which has no caller anywhere in this extension or in
       testing/. So the ONLY observable that the single BFS context-switches, forks and pumps jobs rather
       than running its flows FIFO — plus what each run LEARNED and what it PARKED for the next session —
       was computed, asserted, stored, and then read by nothing. That is the mirror of a defaulted field:
       not a value nobody notices is wrong, a value nobody sees at all.

       IT CROSSES VERBATIM, BECAUSE THE RECORD HAS TWO SHAPES AND THEY MUST NOT BE NORMALISED. A run that
       produced an @RESULT document carries the eight counters plus endpoints/sinks/park/resumed; a run that
       crashed carries `run`, `resumed` and `url` and NO counters at all — bridge.js's own comment
       says why, and it is this rule: nothing may compute a rate, a delta or a total out of a run that never
       reported one, because seven zeroes read as "the engine ran and did nothing", which is a finding. So
       this hands the reader the records as they are and lets the view say which shape each one is.
       AND EVERY ROW IS ONE RUN, WHICH IT DID NOT USED TO BE. A partial snapshot every 750 ms appended its own
       row, so this slice of eight was routinely eight snapshots of ONE analysis; `run` on the record is what
       tells a snapshot from a finished run, and bridge.js gives each run a single row that its partials
       rewrite. Eight rows is now eight runs. */
    case "GET_ENGINE_RUNS": {
      DCHECK(Array.isArray(self._engineLog),
             "there is no _engineLog array in this document — bridge.js declares it at load (never on first " +
             "use, deliberately), so its absence is that file not having run here, and answering [] would " +
             "report 'no engine has run yet' for a bridge that is not present at all");
      sendResponse(self._engineLog.slice(-8));
      return;
    }

    case "GET_ENDPOINT_SCHEMA": {
      // GLOBAL — keyed by endpointKey/service against the cumulative store,
      // never per-tab/document (only the network-stream log is per-tab).
      const result = resolveEndpointSchema(
        msg.endpointKey,
        msg.service,
        msg.methodId,
      );
      sendResponse(result);
      return;
    }

    case "SEND_REQUEST": {
      const _srdoc = _docFromMsg(msg);
      if (!_srdoc) { sendResponse({ error: "no document for request" }); return; }
      executeSendRequest(_srdoc.documentId, msg).then((result) => {
        sendResponse(result);
      });
      return true;
    }

    case "WS_SEND_MSG": {
      if (tabId == null) return;
      // documentId-ONLY routing: a frameId is reused across navigations and
      // could resolve to a DIFFERENT origin; with no target option,
      // tabs.sendMessage would broadcast to every frame. No documentId → refuse.
      if (!msg.documentId) { sendResponse({ error: "blocked: no documentId" }); return true; }
      var _wsOpts = { documentId: msg.documentId };
      swRpc("tabs.sendMessage", tabId, {
        type: "WS_SEND_MSG",
        wsId: msg.channelId,
        data: msg.data,
        binary: msg.binary || false,
      }, _wsOpts).then(() => sendResponse({ ok: true }))
        .catch((err) => sendResponse({ error: err.message }));
      return true;
    }

    case "WS_GET_STATUS": {
      if (tabId == null) return;
      const conn = _findWsConn(tabId, msg.channelId);
      // Also return the messages array for the WS console
      let messages = [];
      if (conn) {
        const entry = _findLogEntry(tabId, msg.channelId, "WEBSOCKET");
        if (entry) messages = entry.messages || [];
      }
      sendResponse({
        readyState: conn ? conn.readyState : 3,
        url: conn?.url || null,
        messages: messages,
      });
      return;
    }

    case "PM_SEND_MSG": {
      if (tabId == null) return;
      // documentId-ONLY routing (no frameId fallback — reused across navs / origins).
      if (!msg.documentId) { sendResponse({ error: "blocked: no documentId" }); return true; }
      /* THE WILDCARD IS NOT A TARGET THIS SURFACE SENDS. The send panel refuses a channel whose page-claimed
         source origin is not a real one (lib/popup-console.js) rather than addressing the reply to `*`, so a
         `*` or an empty target arriving here is that refusal having been bypassed — and the payload is a
         human-typed one going into whatever document holds that window. */
      DCHECK(typeof msg.targetOrigin === "string" && msg.targetOrigin !== "" && msg.targetOrigin !== "*",
             "PM_SEND_MSG was asked to reply to `" + msg.targetOrigin + "` — a postMessage the operator typed " +
             "is addressed to ONE origin, and the wildcard delivers it to whichever document the target " +
             "window holds by the time it lands");
      var _pmOpts = { documentId: msg.documentId };
      swRpc("tabs.sendMessage", tabId, {
        type: "PM_SEND_MSG",
        data: msg.data,
        targetOrigin: msg.targetOrigin,
      }, _pmOpts).then(() => {
        // Record sent message in the log entry (intercept.js can't capture outgoing postMessage)
        const entry = _findLogEntry(tabId, msg.channelId, "POSTMESSAGE");
        if (entry) {
          entry.messages.push({ dir: "sent", time: Date.now(), body: msg.data || "", base64: false });
          if (entry.messages.length > 200) entry.messages.splice(0, entry.messages.length - 200);
              notifyPopup(tabId);
        }
        sendResponse({ ok: true });
      }).catch((err) => sendResponse({ error: err.message }));
      return true;
    }

    case "PM_GET_STATUS": {
      if (tabId == null) return;
      const entry = _findLogEntry(tabId, msg.channelId, "POSTMESSAGE");
      sendResponse({
        readyState: 1, // postMessage is always "active"
        messages: entry ? (entry.messages || []) : [],
      });
      return;
    }

    case "MC_SEND_MSG": {
      if (tabId == null) return;
      // documentId-ONLY routing (no frameId fallback — reused across navs / origins).
      if (!msg.documentId) { sendResponse({ error: "blocked: no documentId" }); return true; }
      var _mcOpts = { documentId: msg.documentId };
      swRpc("tabs.sendMessage", tabId, {
        type: "MC_SEND_MSG",
        channelId: msg.channelId,
        data: msg.data,
      }, _mcOpts).then(() => {
        const entry = _findLogEntry(tabId, msg.channelId, "MSGCHANNEL");
        if (entry) {
          entry.messages.push({ dir: "sent", time: Date.now(), body: msg.data || "", base64: false });
          if (entry.messages.length > 200) entry.messages.splice(0, entry.messages.length - 200);
              notifyPopup(tabId);
        }
        sendResponse({ ok: true });
      }).catch((err) => sendResponse({ error: err.message }));
      return true;
    }

    case "MC_GET_STATUS": {
      if (tabId == null) return;
      const entry = _findLogEntry(tabId, msg.channelId, "MSGCHANNEL");
      sendResponse({
        readyState: 1, // port is active once transferred
        messages: entry ? (entry.messages || []) : [],
      });
      return;
    }

    case "BUILD_REQUEST": {
      // GLOBAL — build/export reads the cumulative store by endpointKey/service,
      // never per-tab/document.
      buildExportRequest(msg).then((result) => {
        sendResponse(result);
      });
      return true;
    }

    // EXPLOIT_PROBE_START: kick off an exploitability probe and return
    // the session id immediately. Caller polls EXPLOIT_PROBE_STATUS to
    // observe progress + results. Split from the "run to completion"
    // shape so a popup button can start a probe, let the popup close,
    // and retrieve results later without losing them.
    case "EXPLOIT_PROBE_START": {
      try {
        const session = startExploitProbe(msg);
        // Return the EXACT PoC JS so the popup displays AND the sandbox runs the
        // one artifact.
        // `pocWhy` is the OTHER answer and it was being dropped here: when the source's engine-declared
        // delivery is one this zone cannot PERFORM (a planted cookie, an attacker-served referrer, a
        // user-supplied file), there is no pocJs and the reason is the whole content of the reply. Without
        // it the popup could only say "no pocJs", which reads as a broken build rather than as the
        // mechanism it actually is.
        // EXACTLY ONE OF THE TWO IS A STRING — startExploitProbe asserts that pairing where it builds the
        // session, so the panel never has to ask which half is missing. `error` is NOT on this reply: it
        // named a session field nothing has ever written, and the catch below owns that name for the one
        // thing it does mean here (a probe that could not be started at all).
        DCHECK(typeof session.pocJs === "string" ? session.pocWhy === null : typeof session.pocWhy === "string",
               "EXPLOIT_PROBE_START has neither a PoC nor a reason to answer with — buildLiveDelivery states " +
               "one of the two on every arm, so this reply would render as a broken build for a finding whose " +
               "breakout is fire-verified");
        sendResponse({ success: true, sessionId: session.marker, pocJs: session.pocJs,
                       pocWhy: session.pocWhy });
      } catch (e) {
        RETHROW_FATAL(e);   // an invariant abort is never reported as a probe that failed to build
        sendResponse({ error: (e && e.message) || String(e) });
      }
      return true;
    }

    // EXPLOIT_PROBE_STATUS: report whether the engine's poc, run against the real page, fired the sink.
    // Correlation is the relayed apiclientsink(<marker>) hit (intercept.js → content.js → PROBE_HIT). A hit
    // = REAL EXPLOIT (Chrome agrees with the engine); no hit = divergence / CSP-blocked.
    case "EXPLOIT_PROBE_STATUS": {
      const ses = msg.sessionId ? _probeSessions.get(msg.sessionId) : null;
      if (!ses) { sendResponse({ error: "session not found or expired" }); return; }
      // NO `executed` FIELD. It was `executed: ses.executed || null` — a default over a name NOTHING has ever
      // written: startExploitProbe builds the session with {marker, status, pageUrl, findingId, sourceUrl,
      // sinkName, waitMs, hits, createdAt, expect, deliveredDocumentId, pocJs, pocWhy} and the only writer
      // after that is PROBE_HIT, which appends to `hits`. So the field crossed as null on every reply, and the popup read it as a
      // SECOND source of "did the payload fire" — a whole alternate evidence vocabulary that could never
      // answer. Deleted here and at its reader (lib/popup-security.js), per CLAUDE.md §Architecture: a name
      // read somewhere and written nowhere is a broken contract, and the default is what stops it crashing.
      // `hits` is the one evidence channel and it is a real array on every session.
      /* `pocJs` IS ASSERTED, NOT DEFAULTED, and `null` is its POSITIVE value: startExploitProbe writes it on
         every session from buildLiveDelivery's record, and null means the engine's declared delivery is one
         this zone cannot PERFORM (a planted cookie, an attacker-served referrer, a user-supplied file) — the
         reason travels as `pocWhy` on the START reply. `|| null` could not tell that from a producer that
         stopped writing the field.
         `error` AND `finishedAt` ARE GONE FROM THIS REPLY. Both were `|| null` over session fields written
         exactly once, to null, and never again by anything: this session has no terminal transition at all,
         so neither could ever carry a value. `error` was the worse of the two — the reply already uses that
         name for "session not found or expired" (above), which the poller reads as a reason to keep polling,
         so a session-level error crossing under it would have been read as an expired session. */
      DCHECK("pocJs" in ses && (ses.pocJs === null || (typeof ses.pocJs === "string" && ses.pocJs.length > 0)),
             "a probe session carries no usable pocJs — startExploitProbe writes buildLiveDelivery's answer " +
             "on every session it stores, so an absent field is that producer having stopped and an empty " +
             "string is a delivery that was built out of nothing");
      DCHECK(Array.isArray(ses.hits),
             "a probe session carries no hits array — it is created with hits:[] and PROBE_HIT only ever " +
             "appends to it, so its absence is the one evidence channel this reply has being unreadable");
      sendResponse({
        success: true, status: ses.status, marker: ses.marker, pageUrl: ses.pageUrl,
        hits: ses.hits.slice(),
        pocJs: ses.pocJs,
        startedAt: ses.createdAt,
      });
      return;
    }

    case "RENAME_FIELD": {
      // GLOBAL per-service edit — never takes a documentId/tabId. The discovery
      // store is global; a rename sets customName=true, which every merge path
      // preserves, so editing the global doc directly persists across later page
      // merges (no mergeToGlobal needed).
      const { service, schemaName, fieldKey, newName } = msg;
      const docEntry = globalStore.discoveryDocs.get(service);
      if (!docEntry || !docEntry.doc) {
        sendResponse({ error: "No discovery document for " + service });
        return;
      }
      const doc = docEntry.doc;

      if (schemaName === "params") {
        // Find method and rename its parameter
        let m = null;
        if (msg.methodId) {
          const match = findMethodById(doc, msg.methodId);
          if (match) m = match.method;
        }

        if (!m) {
          // Fallback: Calculate from URL (less reliable)
          const { methodName } = calculateMethodMetadata(
            new URL(msg.url || ""),
            service,
          );
          m =
            doc.resources.learned?.methods[methodName] ||
            doc.resources.probed?.methods[methodName];
        }

        if (m && m.parameters?.[fieldKey]) {
          m.parameters[fieldKey].name = newName;
          m.parameters[fieldKey].customName = true;
          sendResponse({ ok: true });
        } else {
          sendResponse({ error: "Parameter not found for rename" });
        }
      } else {
        // Handle schema properties or create virtual schema for raw fields
        if (!doc.schemas) doc.schemas = {};
        if (!doc.schemas[schemaName]) {
          doc.schemas[schemaName] = { id: schemaName, type: "object", properties: {} };
        }

        const schema = doc.schemas[schemaName];
        if (!schema.properties) schema.properties = {};

        if (schema.properties[fieldKey]) {
          const prop = schema.properties[fieldKey];
          prop.name = newName;
          prop.customName = true;
        } else {
          // Create a virtual property for a raw field number
          schema.properties[fieldKey] = {
            id: fieldKey,
            number: parseInt(fieldKey) || null,
            name: newName,
            customName: true,
            type: "any"
          };
        }
        sendResponse({ ok: true });
      }
      return;
    }

    case "EXPORT_OPENAPI": {
      // Per-SERVICE and fully GLOBAL — never takes a documentId/tabId. The
      // discovery store is global; nothing in the popup is per-tab except the
      // network-stream log filter (where tabId/documentId is just log metadata).
      const svc = msg.service;
      const docEntry = globalStore.discoveryDocs.get(svc);
      if (!docEntry?.doc) {
        sendResponse({ error: "No discovery document found for " + svc });
        return;
      }
      const openapi = convertDiscoveryToOpenApi(docEntry.doc, svc);
      sendResponse({ ok: true, spec: openapi });
      return;
    }

    case "IMPORT_OPENAPI": {
      // GLOBAL — an imported spec is a user-provided service definition, not a
      // page fetch; it goes straight into the global discovery store (no doc).
      try {
        const spec = msg.spec;
        if (!spec || typeof spec !== "object") {
          sendResponse({ error: "Invalid OpenAPI spec: not an object" });
          return;
        }
        if (!spec.paths || typeof spec.paths !== "object") {
          sendResponse({ error: "Invalid OpenAPI spec: missing or invalid paths" });
          return;
        }
        // Validate OpenAPI version — only 3.0.x and 3.1.x supported
        if (spec.openapi) {
          if (!/^3\.\d+\.\d+/.test(spec.openapi)) {
            sendResponse({ error: "Unsupported OpenAPI version: " + spec.openapi + ". Only 3.x is supported." });
            return;
          }
        } else if (spec.swagger) {
          // Swagger 2.0 — not supported by convertOpenApiToDiscovery
          sendResponse({ error: "Swagger 2.0 is not supported. Please convert to OpenAPI 3.x first." });
          return;
        }
        // Determine service name. Prefer the original internal key when
        // it was preserved via the `x-service-key` vendor extension on
        // export — otherwise a UASR-exported spec for a path-prefixed
        // service like "www.google.com/MapsWizUi" would import back
        // under the hostname-only "www.google.com", silently merging
        // unrelated services. Fall back to hostname (for specs from
        // other tools) and finally to info.title.
        let svcName;
        if (spec.info && typeof spec.info["x-service-key"] === "string" &&
            spec.info["x-service-key"].length > 0) {
          svcName = spec.info["x-service-key"];
        }
        if (!svcName && spec.servers?.[0]?.url) {
          try {
            svcName = new URL(spec.servers[0].url).hostname;
          } catch (e) {
            /* OpenAPI spec's `servers[0].url` isn't a valid absolute URL
               (relative or templated like `{protocol}://api/v1`). Fall
               back to spec.info.title below. Surface so a malformed
               spec doesn't silently lose its hostname-based service
               key. */
            console.debug("[brain] OpenAPI servers[0].url parse failed:", e && e.message || e, "url=" + spec.servers[0].url);
          }
        }
        if (!svcName) {
          svcName = (spec.info?.title || "imported")
            .toLowerCase().replace(/[^a-z0-9.]/g, "_");
        }

        // Convert to internal Discovery format
        const sourceUrl = spec.servers?.[0]?.url || "https://" + svcName;
        const doc = convertOpenApiToDiscovery(spec, sourceUrl);

        // Merge with existing doc if present
        const existing = globalStore.discoveryDocs.get(svcName);
        if (existing?.doc) {
          // Merge imported methods into existing doc
          for (const [rName, resource] of Object.entries(doc.resources)) {
            if (!existing.doc.resources[rName]) {
              existing.doc.resources[rName] = resource;
            } else {
              for (const [mName, method] of Object.entries(resource.methods || {})) {
                if (!existing.doc.resources[rName].methods[mName]) {
                  existing.doc.resources[rName].methods[mName] = method;
                }
              }
            }
          }
          // Merge schemas (imported fills gaps, doesn't overwrite)
          for (const [sName, schema] of Object.entries(doc.schemas)) {
            if (!existing.doc.schemas[sName]) {
              existing.doc.schemas[sName] = schema;
            }
          }
        } else {
          // Store as new discovery doc
          var _prevGlobalEntry = globalStore.discoveryDocs.get(svcName);
          const entry = {
            status: "found",
            url: sourceUrl,
            method: "IMPORT",
            apiKey: null,
            fetchedAt: Date.now(),
            doc,
            isVirtual: false,
            pageUrls: _prevGlobalEntry?.pageUrls || new Set(),
            frameOrigins: _prevGlobalEntry?.frameOrigins || new Set(),
          };
          globalStore.discoveryDocs.set(svcName, entry);
        }
        scheduleSave();
        sendResponse({ ok: true, service: svcName });
      } catch (err) {
        sendResponse({ error: "Import failed: " + err.message });
      }
      return;
    }
  }
}

// ─── Export Request Builder ──────────────────────────────────────────────────

/**
 * Build a fully-encoded request (URL, headers, body) for export.
 * Reuses the same encoding logic as executeSendRequest but returns the
 * request instead of sending it.
 */
// ─── Exploit probe (interactive per-finding verification) ─────────────────


// The real cross-origin attacker origin the PoC runs on. A minimal, stable,
// CSP-free page (IANA's reserved example domain) so the injected PoC can frame
// the target and run without the attacker page's own policy interfering. This
// is what a researcher would paste the PoC onto.
const PROBE_ATTACKER_ORIGIN = "https://example.com/";


// Sessions persist past completion so the popup can reopen after the
// probe finishes and still render the result. Capped via TTL + LRU.
const PROBE_SESSION_TTL_MS = 10 * 60 * 1000;
const PROBE_SESSION_MAX = 50;
