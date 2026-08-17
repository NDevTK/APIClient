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
      // Authoritative frame tree from the BROWSER: chrome.webNavigation.getAllFrames
      // returns each live frame's documentId + url + frameType. frameType is the
      // only trustworthy "is this the main frame" signal — a fenced frame is reported
      // as "fenced_frame", so it CANNOT impersonate the outermost frame (which a
      // self-reported isTop could). Each frame's AUTHORITATIVE origin comes from the
      // DURABLE _docOrigins map by documentId (survives buffer reclamation after
      // review); frameId is Chrome's routing id for tabs.sendMessage, not an identity.
      let _wnFrames = null;
      try { _wnFrames = await swRpc("webNavigation.getAllFrames", { tabId }); }
      catch (e) { console.debug("[GET_FRAMES] getAllFrames failed:", e && e.message || e); }
      const out = (_wnFrames || []).map((f) => ({
        frameId: f.frameId,
        documentId: f.documentId || null,
        url: f.url,
        origin: _originForDoc(f.documentId),
        isMain: f.frameType === "outermost_frame",
      }));
      sendResponse(out.length ? out : [{ frameId: 0, documentId: null, url: "", origin: "", isMain: true }]);
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
      if (ep.apiKey) {
        if (ep.apiKeySource === "url") {
          discoverUrl.searchParams.set("key", ep.apiKey);
        } else {
          headers["X-Goog-Api-Key"] = ep.apiKey;
        }
      }
      const fetchFn = makePageFetchFn(tab.tabId, tab.documentId);
      discoverServiceInfo(discoverUrl.toString(), headers, { fetchFn }).then(
        (result) => {
          tab.probeResults.set(`svc:${msg.endpointKey}`, result);
          if (result.scopes?.length) {
            const svc = ep.service || extractInterfaceName(new URL(ep.url));
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
         a hostname nor an endpoint has nothing to discover. */
      const ep = tab.endpoints.values().next().value;
      const hostname = msg.hostname || ep?.host || null;
      if (!hostname) { sendResponse(null); return; }
      const apiKeys = collectKeysForService(tab, msg.service, hostname);
      if (msg.apiKey && !apiKeys.includes(msg.apiKey)) apiKeys.push(msg.apiKey);
      if (ep?.apiKey && !apiKeys.includes(ep.apiKey)) apiKeys.push(ep.apiKey);
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
        // 3. All in-memory request logs + per-tab working state, so the next
        //    navigation starts from a genuinely empty slate.
        state.docs.clear();
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
      // from the tab's live documents (an evicted doc leaves its log entries
      // but no identity, so that tab degrades to "Tab N").
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
        // one artifact. error surfaces a build failure (e.g. opaque page URL).
        // `pocWhy` is the OTHER answer and it was being dropped here: when the source's engine-declared
        // delivery is one this zone cannot PERFORM (a planted cookie, an attacker-served referrer, a
        // user-supplied file), there is no pocJs and the reason is the whole content of the reply. Without
        // it the popup could only say "no pocJs", which reads as a broken build rather than as the
        // mechanism it actually is.
        sendResponse({ success: true, sessionId: session.marker, pocJs: session.pocJs || null,
                       pocWhy: session.pocWhy || null, error: session.error || null });
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
      // sinkName, waitMs, hits, createdAt, finishedAt, error} and the only writer after that is PROBE_HIT,
      // which appends to `hits`. So the field crossed as null on every reply, and the popup read it as a
      // SECOND source of "did the payload fire" — a whole alternate evidence vocabulary that could never
      // answer. Deleted here and at its reader (lib/popup-security.js), per CLAUDE.md §Architecture: a name
      // read somewhere and written nowhere is a broken contract, and the default is what stops it crashing.
      // `hits` is the one evidence channel and it is a real array on every session.
      sendResponse({
        success: true, status: ses.status, marker: ses.marker, pageUrl: ses.pageUrl,
        hits: ses.hits.slice(),
        pocJs: ses.pocJs || null, error: ses.error || null,
        startedAt: ses.createdAt, finishedAt: ses.finishedAt || null,
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
