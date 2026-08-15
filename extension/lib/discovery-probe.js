// lib/discovery-probe.js — Active API-documentation discovery: diff discovery docs and fetch
// OpenAPI/Google Discovery at well-known paths. Extracted from the offscreen-brain.js monolith (one problem
// per file); loaded before it, functions resolve their callers (makePageGetFn, extractInterfaceName,
// generateSchemaFromJson, safeFetch) at call-time.
//
// ERROR-BASED SCHEMA PROBING IS NO LONGER HERE. It moved to engine/host/solver/req2proto.c, which reads the
// `google.rpc.Status` envelope off replies the engine already holds instead of firing an intentionally
// malformed POST to provoke one — CLAUDE.md §Architecture (the semantics are the engine's) and §Attacker
// sources (a state-mutating request is NEVER fired to learn). What is LEFT in this file is the document
// FETCH, which is a GET of a published URL and stays JS until discovery.c exists — and it is a GET
// STRUCTURALLY now: a candidate is `{url, headers}` with no method field, and the relay entry this file can
// reach (`pageContextGet`) has no method parameter. One candidate used to be a POST carrying
// `X-Http-Method-Override: GET`, issued automatically by passive learning, which is the same rule this header
// already cites being broken by a parameter rather than by a missing check.

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
  const tabId = (tab && tab.tabId != null) ? tab.tabId : null; // Chrome routing (makePageGetFn/notifyPopup) — derived from the doc

  /* A GET FUNCTION, AND THERE IS NO PARAMETER HERE IN WHICH TO ASK FOR ANYTHING ELSE — the candidate carries a
     URL and headers, and the relay's learning entry (lib/schema.js `pageContextGet`) takes exactly those. This
     loop used to hand the relay a `method` off each candidate, one of which was a POST. */
  const getFn = makePageGetFn(tabId, documentId);
  const triedKeys = new Set();

  // Build a deduplicated candidate list across all keys
  // Try each key separately to track which one works
  const keysToTry = [...new Set(apiKeys || [])];

  // Always try without key as a fallback (some public APIs don't need one)
  if (!keysToTry.includes(null)) keysToTry.push(null);

  for (const apiKey of keysToTry) {
    if (apiKey) triedKeys.add(apiKey);
    const candidates = buildDiscoveryUrls(hostname, apiKey);

    for (const { url, headers } of candidates) {
      try {
        const resp = await getFn(url, headers);

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
              /* THE WHOLE HISTORY. `if (dcList.length > 20) dcList = dcList.slice(-20)` stood here and dropped
                 the OLDEST change record once a service had accumulated twenty — §NO BOUNDS: a cap truncates
                 distinct work, and the first time an API's surface changed is exactly the record a reader
                 wants. The natural size is the number of times this service's document actually changed
                 between two fetches, which is small and is not ours to decide. */
              var dcList = globalStore.discoveryChanges.get(service) || [];
              dcList.push({ timestamp: Date.now(), fetchUrl: url, changes: diff });
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
  /* NO TIMESTAMP ON THE FAILURE. `_failedAt` was here and response-decode.js read it as a 300-second COOLDOWN
     before this service could be asked again — a clock deciding when work may happen, which §NO BOUNDS bans by
     name. What the record keeps is the FACT (this key set found nothing) and WHICH KEYS produced it, which is
     what makes a later attempt with a newly-learned key a different question rather than the same one. */
  var _prevDiscoveryNF = tab.discoveryDocs.get(service);
  tab.discoveryDocs.set(service, {
    status: "not_found",
    _triedKeys: triedKeys,
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
