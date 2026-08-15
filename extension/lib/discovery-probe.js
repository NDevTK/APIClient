// lib/discovery-probe.js — Active API-documentation discovery: diff discovery docs and fetch
// OpenAPI/Google Discovery at well-known paths. Extracted from the offscreen-brain.js monolith (one problem
// per file); loaded before it, functions resolve their callers (makePageFetchFn, extractInterfaceName,
// generateSchemaFromJson, safeFetch) at call-time.
//
// ERROR-BASED SCHEMA PROBING IS NO LONGER HERE. It moved to engine/host/solver/req2proto.c, which reads the
// `google.rpc.Status` envelope off replies the engine already holds instead of firing an intentionally
// malformed POST to provoke one — CLAUDE.md §Architecture (the semantics are the engine's) and §Attacker
// sources (a state-mutating request is NEVER fired to learn). What is LEFT in this file is the document
// FETCH, which is a GET of a published URL and stays JS until discovery.c exists.

// ─── Discovery Document Diffing ──────────────────────────────────────────────

function _collectAllMethods(doc) {
  var result = {};
  if (!doc || !doc.resources) return result;
  function walk(resources, prefix) {
    for (var rName in resources) {
      var res = resources[rName];
      if (res.methods) {
        for (var mName in res.methods) {
          var id = (prefix ? prefix + "." : "") + rName + "." + mName;
          result[id] = res.methods[mName];
        }
      }
      // Recurse into sub-resources
      if (res.resources) walk(res.resources, (prefix ? prefix + "." : "") + rName);
    }
  }
  walk(doc.resources, "");
  return result;
}

function _diffDiscoveryDocs(oldDoc, newDoc) {
  if (!oldDoc || !newDoc) return null;
  var changes = [];

  var oldMethods = _collectAllMethods(oldDoc);
  var newMethods = _collectAllMethods(newDoc);

  // New methods
  for (var id in newMethods) {
    if (!oldMethods[id]) {
      changes.push({ type: "method_added", methodId: id, path: newMethods[id].path, httpMethod: newMethods[id].httpMethod });
    }
  }
  // Removed methods
  for (var id in oldMethods) {
    if (!newMethods[id]) {
      changes.push({ type: "method_removed", methodId: id, path: oldMethods[id].path, httpMethod: oldMethods[id].httpMethod });
    }
  }
  // Changed methods — compare parameters
  for (var id in newMethods) {
    if (!oldMethods[id]) continue;
    var oldM = oldMethods[id], newM = newMethods[id];
    var oldParams = Object.keys(oldM.parameters || {});
    var newParams = Object.keys(newM.parameters || {});

    for (var pi = 0; pi < newParams.length; pi++) {
      var p = newParams[pi];
      if (!oldM.parameters || !oldM.parameters[p]) {
        changes.push({ type: "param_added", methodId: id, param: p, details: newM.parameters[p] });
      }
    }
    for (var pi = 0; pi < oldParams.length; pi++) {
      var p = oldParams[pi];
      if (!newM.parameters || !newM.parameters[p]) {
        changes.push({ type: "param_removed", methodId: id, param: p });
      }
    }
    for (var pi = 0; pi < newParams.length; pi++) {
      var p = newParams[pi];
      if (!oldM.parameters || !oldM.parameters[p]) continue;
      var op = oldM.parameters[p], np = newM.parameters[p];
      if (op.type !== np.type) {
        changes.push({ type: "param_type_changed", methodId: id, param: p, from: op.type, to: np.type });
      }
      if (!op.required && np.required) {
        changes.push({ type: "param_required", methodId: id, param: p });
      }
    }
  }

  // Compare schemas — new/removed schema names
  var oldSchemas = Object.keys(oldDoc.schemas || {});
  var newSchemas = Object.keys(newDoc.schemas || {});
  for (var si = 0; si < newSchemas.length; si++) {
    if (oldSchemas.indexOf(newSchemas[si]) === -1) {
      changes.push({ type: "schema_added", schema: newSchemas[si] });
    }
  }
  for (var si = 0; si < oldSchemas.length; si++) {
    if (newSchemas.indexOf(oldSchemas[si]) === -1) {
      changes.push({ type: "schema_removed", schema: oldSchemas[si] });
    }
  }

  return changes.length > 0 ? changes : null;
}

/**
 * Fetch discovery document for a service, trying multiple API keys.
 * Some discovery documents only load with the correct API key.
 *
 * @param {number} tabId
 * @param {string} service
 * @param {string} hostname
 * @param {string[]} apiKeys - All API keys to try for this service
 */
async function fetchDiscoveryForService(
  documentId,
  service,
  hostname,
  apiKeys,
  seedUrl,
) {
  const tab = _docForLearning(documentId);
  const tabId = (tab && tab.tabId != null) ? tab.tabId : null; // Chrome routing (makePageFetchFn/notifyPopup) — derived from the doc

  const fetchFn = makePageFetchFn(tabId, documentId);
  const triedKeys = new Set();

  // Build a deduplicated candidate list across all keys
  // Try each key separately to track which one works
  const keysToTry = [...new Set(apiKeys || [])];

  // Always try without key as a fallback (some public APIs don't need one)
  if (!keysToTry.includes(null)) keysToTry.push(null);

  for (const apiKey of keysToTry) {
    if (apiKey) triedKeys.add(apiKey);
    const candidates = buildDiscoveryUrls(hostname, apiKey);

    for (const { url, headers, method } of candidates) {
      try {
        const resp = await fetchFn(url, { method: method || "GET", headers });

        if (resp.error || !resp.ok) continue;

        let doc;
        try {
          doc = JSON.parse(resp.body);
        } catch (_) {
          continue;
        }

        if (
          doc &&
          (doc.discoveryVersion ||
            doc.kind === "discovery#restDescription" ||
            doc.openapi ||
            doc.swagger)
        ) {
          let unifiedDoc = doc;
          if (doc.openapi || doc.swagger) {
            unifiedDoc = convertOpenApiToDiscovery(doc, url);
          }

          const existingEntry = tab.discoveryDocs.get(service);

          // Diff before merge overwrites — track API surface changes
          if (existingEntry?.doc) {
            var diff = _diffDiscoveryDocs(existingEntry.doc, unifiedDoc);
            if (diff) {
              var dcList = globalStore.discoveryChanges.get(service) || [];
              dcList.push({ timestamp: Date.now(), fetchUrl: url, changes: diff });
              if (dcList.length > 20) dcList = dcList.slice(-20);
              globalStore.discoveryChanges.set(service, dcList);
              console.debug("[Discovery:diff] %d changes detected for %s", diff.length, service);
            }
          }

          const mergedDoc = mergeVirtualParts(unifiedDoc, existingEntry?.doc);

          var _prevDiscovery = tab.discoveryDocs.get(service);
          tab.discoveryDocs.set(service, {
            status: "found",
            doc: mergedDoc,
            url,
            method: method || "GET",
            apiKey: apiKey || null,
            fetchedAt: Date.now(),
            pageUrls: _prevDiscovery?.pageUrls || new Set(),
            frameOrigins: _prevDiscovery?.frameOrigins || new Set(),
          });
          mergeToGlobal(tab);

          notifyPopup(tabId);
          return;
        }
      } catch (err) {
        // continue to next candidate
      }
    }
  }

  // Every candidate URL failed: the service publishes no discovery document at any well-known path.
  // THERE IS NO req2proto FALLBACK HERE ANY MORE. Error-based schema learning is the ENGINE's
  // (engine/host/solver/req2proto.c): it reads the `google.rpc.Status` envelope off replies the engine already
  // holds, so a second implementation in this zone would be the JS orchestration layer CLAUDE.md §Architecture
  // says to delete — and this one additionally FIRED the probe, which §Attacker sources forbids doing to learn.
  var _prevDiscoveryNF = tab.discoveryDocs.get(service);
  tab.discoveryDocs.set(service, {
    status: "not_found",
    _triedKeys: triedKeys,
    _failedAt: Date.now(),
    pageUrls: _prevDiscoveryNF?.pageUrls || new Set(),
    frameOrigins: _prevDiscoveryNF?.frameOrigins || new Set(),
  });
  mergeToGlobal(tab);
  notifyPopup(tabId);
}

/* THE PROBE-RESULT -> VIRTUAL-DISCOVERY-DOC CONVERTER IS GONE WITH ITS PRODUCER.
   `updateOrCreateVirtualDoc` + `convertProbeFieldsToSchema` turned a req2proto probe's field map into a
   synthetic discovery document, and their only caller was `performProbeAndPatch`, which fired the probe from
   this zone. The schema is now the engine's (engine/host/solver/req2proto.c) and arrives in the result document
   as `probeResults`, keyed by the same `<METHOD> <host><path>` identity the Send panel already resolves a body
   schema with — so there is nothing here to convert. Synthesising a discovery doc from it is discovery.c's when
   discovery moves; leaving the converter behind with no caller would read as a live capability that has not run
   since this commit. */
