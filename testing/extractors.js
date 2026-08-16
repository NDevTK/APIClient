// State extractors — run inside the extension service worker via puppeteer
// worker.evaluate(). Each exported string is a function body; the driver calls
// `worker.evaluate(new Function("tabId", body))` to execute it.
//
// We reach privileged state (state.tabs, globalStore, serializeTabData) that
// already exists in background.js. No test code is added to the extension —
// we just read what's already there.

"use strict";

// Pull everything the SW knows about one tab, plus global store.
const DUMP_TAB = `
  const tabId = arg;
  const tab = state.tabs.get(tabId);
  if (!tab) return null;
  const meta = (typeof _tabMeta !== "undefined") ? _tabMeta.get(tabId) : null;

  const serialized = serializeTabData(tab);

  // Raw request log — serializeTabData may filter, so pull it explicitly.
  // Each log entry covers regular HTTP + WebSocket + postMessage +
  // MessageChannel + EventSource since they all push into tab.requestLog.
  const requestLog = (tab.requestLog || []).map(function(r) {
    return {
      id: r.id,
      url: r.url,
      method: r.method,
      service: r.service,
      timestamp: r.timestamp,
      status: r.status,
      contentType: r.contentType || (r.responseHeaders && (r.responseHeaders["content-type"] || r.responseHeaders["Content-Type"])) || "",
      requestHeaders: r.requestHeaders || {},
      responseHeaders: r.responseHeaders || {},
      rawBodyB64: r.rawBodyB64 || null,
      responseBody: r.responseBody || null,
      responseBase64: !!r.responseBase64,
      mimeType: r.mimeType || "",
      _source: r._source || null,
      methodId: r.methodId || null,
      format: r.format || (r.body && r.body.format) || null,
      // Content-based asset classification — the RULE that decided, stamped by
      // response-decode.js from the browser process's answer
      // (engine/host/browser_process/network/resource_kind.c). Non-null means the
      // body is a static asset: it still appears in the log, but no response-body
      // schema is extracted from it. _assetKind / _assetLabel / _boring stood here
      // and named classifyResponseAsset in lib/discovery.js, which is deleted.
      _assetReason: r._assetReason || null,
      // WS / postMessage / MessageChannel frames live here:
      messages: r.messages || null,
      channelId: r.channelId || null,
    };
  });

  // AST per-script results: fetch call sites discovered before any network
  // hit, value constraints, proto field maps, proto enums, source map URL,
  // and (if fetched) the parsed source map contents.
  const astResults = (tab._astResults || []).map(function(a) {
    return {
      sourceUrl: a.sourceUrl || null,
      fetchCallSites: a.fetchCallSites || [],
      valueConstraints: a.valueConstraints || [],
      protoFieldMaps: a.protoFieldMaps || [],
      protoEnums: a.protoEnums || [],
      securitySinks: a.securitySinks || [],
      dangerousPatterns: a.dangerousPatterns || [],
      sourceMapUrl: a.sourceMapUrl || null,
      sourceMap: a.sourceMap
        ? {
            sources: (a.sourceMap.sources || []).length,
            names: (a.sourceMap.names || []).length,
            protoFileNames: a.sourceMap.protoFileNames || [],
            apiClientFiles: a.sourceMap.apiClientFiles || [],
            typesExtracted: a.sourceMap.types ? Object.keys(a.sourceMap.types).length : 0,
          }
        : null,
    };
  });

  // Cross-script buffer — what the AST engine actually saw per tab.
  // Don't dump full code bodies (can be many MB); just URLs + lengths.
  const scriptBuf = (typeof _scriptBuffers !== "undefined") ? _scriptBuffers.get(tabId) : null;
  const scripts = scriptBuf
    ? (scriptBuf.scripts || []).map(function(s) {
        return { url: s.url || null, length: (s.code || "").length };
      })
    : [];

  // Frame tree learned via chrome.webNavigation.
  const framesMap = (typeof _tabFrames !== "undefined") ? _tabFrames.get(tabId) : null;
  const frames = framesMap
    ? [...framesMap.entries()].map(function([fid, f]) {
        return { frameId: fid, url: f.url, origin: f.origin, isTop: !!f.isTop, lastSeen: f.lastSeen };
      })
    : [];

  // Auth context (tab-local).
  const authContext = tab.authContext
    ? {
        hasAuthorization: !!tab.authContext.hasAuthorization,
        hasCookies: !!tab.authContext.hasCookies,
        origin: tab.authContext.origin || null,
      }
    : null;

  return {
    tabId: tabId,
    meta: meta ? { title: meta.title, url: meta.url, closed: !!meta.closed } : null,
    serialized: serialized,
    requestLog: requestLog,
    astResults: astResults,
    scripts: scripts,
    frames: frames,
    authContext: authContext,
  };
`;

// Snapshot global (cross-tab) persistent state.
const DUMP_GLOBAL = `
  const findings = {};
  for (const [url, f] of globalStore.securityFindings) {
    findings[url] = {
      sourceUrl: f.sourceUrl,
      pageUrl: f.pageUrl || null,
      securitySinks: (f.securitySinks || []).map(function(s) { return Object.assign({}, s); }),
      dangerousPatterns: (f.dangerousPatterns || []).map(function(p) { return Object.assign({}, p); }),
    };
  }

  const discovery = {};
  for (const [svc, v] of globalStore.discoveryDocs) {
    discovery[svc] = {
      status: v.status,
      url: v.url || null,
      method: v.method || null,
      fetchedAt: v.fetchedAt || null,
      isVirtual: !!v.isVirtual,
      doc: v.doc || null,
      pageUrls: v.pageUrls instanceof Set ? [...v.pageUrls] : (v.pageUrls || []),
    };
  }

  // Per-service delta log: method_added, param_added, param_type_changed, etc.
  const discoveryChanges = {};
  if (globalStore.discoveryChanges) {
    for (const [svc, list] of globalStore.discoveryChanges) {
      discoveryChanges[svc] = list;
    }
  }

  // API keys seen, global view.
  const apiKeys = {};
  if (globalStore.apiKeys) {
    for (const [k, v] of globalStore.apiKeys) {
      apiKeys[k] = {
        name: v.name || null,
        origin: v.origin || null,
        source: v.source || null,
        services: v.services instanceof Set ? [...v.services] : [],
        hosts: v.hosts instanceof Set ? [...v.hosts] : [],
        firstSeen: v.firstSeen || null,
        lastSeen: v.lastSeen || null,
      };
    }
  }

  // Endpoints map (cross-tab).
  const endpoints = {};
  if (globalStore.endpoints) {
    for (const [k, v] of globalStore.endpoints) endpoints[k] = v;
  }

  // Error-based probe results.
  const probeResults = {};
  if (globalStore.probeResults) {
    for (const [k, v] of globalStore.probeResults) probeResults[k] = v;
  }

  // No script cache to report: the replay cache was a document-identity seen-set and is deleted.

  const tabIds = [...state.tabs.keys()];

  return {
    tabIds: tabIds,
    discovery: discovery,
    findings: findings,
    discoveryChanges: discoveryChanges,
    apiKeys: apiKeys,
    endpoints: endpoints,
    probeResults: probeResults,
  };
`;

// Fetch the script source that a security finding refers to. There is no cached
// copy to read: the analysis holds the document's own HTML per document and the
// replay cache is deleted, so reviewer triage refetches the URL.
const FETCH_SCRIPT_SOURCE = `
  const sourceUrl = arg;
  try {
    const resp = await fetch(sourceUrl, { credentials: "omit", cache: "force-cache" });
    const text = await resp.text();
    return { ok: true, sourceUrl: sourceUrl, length: text.length, text: text };
  } catch (e) {
    return { ok: false, sourceUrl: sourceUrl, error: String(e) };
  }
`;

module.exports = { DUMP_TAB, DUMP_GLOBAL, FETCH_SCRIPT_SOURCE };
