// lib/discovery-probe.js — Active API-documentation discovery: diff discovery docs, fetch OpenAPI/Google
// Discovery at well-known paths, error-based schema probing (req2proto), and build/patch virtual discovery
// docs from probe results. Extracted from the offscreen-brain.js monolith (one problem per file); loaded
// before it, functions resolve their callers (makePageGetFn, makePageFetchFn, extractInterfaceName,
// generateSchemaFromJson, safeFetch) at call-time. KEEPS the discovery-document-learning feature, just
// relocated.
//
// THE TWO HALVES USE TWO DIFFERENT RELAY ENTRIES BECAUSE THEY ARE TWO DIFFERENT REQUESTS. Fetching a
// published document is a GET of a public URL, so `fetchDiscoveryForService` reaches the relay through
// `makePageGetFn` and a candidate is `{url, headers}` with no method field — the POST candidate that carried
// `X-Http-Method-Override: GET` is gone, because a GET a service answers 405 to is a GET that failed and not
// a reason to send something else. The error probe is a POST by construction: `performProbeAndPatch` and
// `probeEndpoint` send a DELIBERATELY MALFORMED body to a Google API and read the `google.rpc.Status`
// rejection that describes the request the service wanted, so they reach the relay through `makePageFetchFn`
// and lib/req2proto.js names the method. An endpoint that answers 4xx to a malformed body has not been
// mutated.

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

          // Check if the seedUrl method is actually in the doc.
          // If not, trigger immediate hybrid probe to patch it.
          if (seedUrl) {
            const seedUrlObj = new URL(seedUrl);
            const match = findDiscoveryMethod(doc, seedUrlObj.pathname, "POST");
            if (!match) {
              notifyPopup(tabId);
              await performProbeAndPatch(documentId, service, seedUrl, apiKey);
              return;
            }
          }

          notifyPopup(tabId);
          return;
        }
      } catch (err) {
        // continue to next candidate
      }
    }
  }

  // All keys (including null) failed.
  // FALLBACK: Try req2proto probing if we have a seed URL.
  const currentStatus = tab.discoveryDocs.get(service);
  const finalSeedUrl = seedUrl || currentStatus?.seedUrl;

  if (finalSeedUrl) {
    // Pick a key to try probing with (use the first available one if any)
    const probeKey = keysToTry[0] || null;
    await performProbeAndPatch(documentId, service, finalSeedUrl, probeKey);
  } else {
    /* TRULY NOT FOUND, AND NO TIMESTAMP ON IT. `_failedAt` was recorded here and response-decode.js read it
       as a 300-SECOND COOLDOWN before this service could be asked again — a clock deciding when work may
       happen, which §NO BOUNDS bans by name, and wrong in both directions: it suppressed an attempt that
       would now succeed (an API key learned two seconds later unlocks documents the first sweep could not
       read) and it re-fired an identical sweep the moment the timer expired. What the record keeps is the
       FACT (this key set found nothing) and WHICH KEYS produced it, which is what makes a later attempt with
       a newly-learned key a different question rather than the same one. */
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
}

// Track in-flight probes to prevent concurrent duplicates
const _inflight = new Set();

/**
 * Perform req2proto probing and patch the discovery document.
 */
async function performProbeAndPatch(documentId, service, targetUrl, apiKey) {
  // Deduplicate: skip if already probing this service+url combo
  const probeKey = `${service}::${targetUrl}`;
  if (_inflight.has(probeKey)) return;
  _inflight.add(probeKey);

  const tab = _docForLearning(documentId);
  const tabId = (tab && tab.tabId != null) ? tab.tabId : null; // Chrome routing — derived from the doc

  if (typeof probeApiEndpoint === "undefined") {
    console.error("[Debug] CRITICAL: probeApiEndpoint is not defined!");
    _inflight.delete(probeKey);
    return;
  }

  const fetchFn = makePageFetchFn(tabId, documentId);

  const probeHeader = apiKey ? { "x-goog-api-key": apiKey } : {};

  // Add #_internal_probe fragment to avoid interception loop
  const safeTargetUrl = targetUrl + "#_internal_probe";

  try {
    const probeResult = await probeApiEndpoint(safeTargetUrl, probeHeader, {
      fetchFn,
    });

    if (probeResult && probeResult.fields) {
      // Save raw probe result for observability. Previously only the
      // UI-triggered on-demand probe stored into tab.probeResults — the
      // auto-probe kept its output only in the synthesized virtual doc,
      // so consumers auditing "what did probing learn" saw an empty map.
      // Keyed by service::url so repeat probes overwrite cleanly.
      tab.probeResults.set(`auto:${service}::${targetUrl}`, probeResult);

      // Convert probe result to a "Virtual" Discovery Doc
      // Merge with existing if available
      const currentStatus = tab.discoveryDocs.get(service);
      const existingDoc = currentStatus?.doc ? currentStatus.doc : null;

      const virtualDoc = updateOrCreateVirtualDoc(
        service,
        targetUrl,
        probeResult,
        existingDoc,
      );

      var _prevProbed = tab.discoveryDocs.get(service);
      tab.discoveryDocs.set(service, {
        status: "found", // Treat as found so it shows up in UI
        doc: virtualDoc,
        apiKey: apiKey,
        fetchedAt: Date.now(),
        method: existingDoc ? currentStatus.method || "HYBRID" : "PROBE",
        isVirtual: existingDoc ? currentStatus.isVirtual || false : true,
        pageUrls: _prevProbed?.pageUrls || new Set(),
        frameOrigins: _prevProbed?.frameOrigins || new Set(),
      });
      mergeToGlobal(tab);
      notifyPopup(tabId);
    }
  } catch (probeErr) {
    console.error("Probe fallback failed:", probeErr);
  } finally {
    _inflight.delete(probeKey);
  }
}

function updateOrCreateVirtualDoc(service, seedUrl, probeResult, existingDoc) {
  const u = new URL(seedUrl);
  const origin = `${u.protocol}//${u.host}`;
  const fullPath = u.pathname.substring(1); // remove leading /

  const { methodName, methodId } = calculateMethodMetadata(u, service);
  const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Request`;
  const responseSchemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Response`;

  let doc = existingDoc
    ? JSON.parse(JSON.stringify(existingDoc))
    : {
        kind: "discovery#restDescription",
        name: service,
        version: "v1",
        title: `${service} (Probed)`,
        description: "Auto-generated from req2proto probe",
        rootUrl: origin + "/",
        servicePath: "",
        baseUrl: origin + "/", // We use root as base, and full paths for methods
        resources: {},
        schemas: {},
      };

  // Ensure resources structure
  if (!doc.resources) doc.resources = {};
  if (!doc.resources.probed) {
    doc.resources.probed = { methods: {} };
  }

  // Ensure schemas structure
  if (!doc.schemas) doc.schemas = {};

  // Heuristic for response schema: try GetAsyncDataResponse, AsyncDataResponse, etc.
  const responseCandidates = [
    responseSchemaName,
    schemaName.replace("Request", "Response"),
    methodName.charAt(0).toUpperCase() + methodName.slice(1) + "Response",
    methodName.replace(/^Get/, "") + "Response",
    methodName.replace(/^BatchGet/, "") + "Response",
  ];

  let actualResponseRef = responseSchemaName;
  if (doc.schemas) {
    for (const cand of responseCandidates) {
      if (doc.schemas[cand]) {
        actualResponseRef = cand;

        break;
      }
    }
  }

  // Add/Update Method
  doc.resources.probed.methods[methodName] = {
    id: methodId,
    path: fullPath,
    httpMethod: "POST", // Assumed
    description: `Probed endpoint: ${fullPath}`,
    parameters: {},
    request: { $ref: schemaName },
    response: { $ref: actualResponseRef },
  };

  // Create/Merge Schema for Request recursively
  const newProperties = convertProbeFieldsToSchema(
    probeResult.fields,
    doc.schemas,
    schemaName,
  );

  // Merge probe properties into request and response schemas.
  // Probe data has verified field numbers/types — prefer it over learned data,
  // but always preserve user's customName renames.
  function mergeProbeInto(target, probeProps) {
    if (!target.properties) target.properties = {};
    // Build field-number → key index for deduplication
    const numToKey = {};
    for (const [k, p] of Object.entries(target.properties)) {
      const n = p.number ?? p.id;
      if (n != null) numToKey[n] = k;
    }
    for (const [key, probeProp] of Object.entries(probeProps)) {
      const fieldNum = probeProp.number ?? probeProp.id;
      const matchKey = target.properties[key] ? key
        : (fieldNum != null && numToKey[fieldNum]) ? numToKey[fieldNum]
        : null;
      const existing = matchKey ? target.properties[matchKey] : null;
      if (!existing) {
        target.properties[key] = probeProp;
        if (fieldNum != null) numToKey[fieldNum] = key;
      } else {
        // Re-key: probe has the real name, replace generic fieldN key
        if (matchKey !== key && !existing.customName && !/^field\d+$/.test(key)) {
          target.properties[key] = existing;
          delete target.properties[matchKey];
          numToKey[fieldNum] = key;
        }
        // Probe has authoritative field numbers and types
        if (probeProp.id != null) existing.id = probeProp.id;
        if (probeProp.number != null) existing.number = probeProp.number;
        if (probeProp.type && existing.type === "string" && probeProp.type !== "string") {
          existing.type = probeProp.type;
        }
        if (probeProp.$ref && !existing.$ref) existing.$ref = probeProp.$ref;
        if (probeProp.children && !existing.children) existing.children = probeProp.children;
        if (probeProp.description && !existing.description) existing.description = probeProp.description;
        // Preserve user renames
        if (existing.customName) {
          // keep existing.name
        } else if (probeProp.name) {
          existing.name = probeProp.name;
        }
      }
    }
  }

  if (!doc.schemas[schemaName]) {
    doc.schemas[schemaName] = {
      id: schemaName,
      type: "object",
      properties: newProperties,
    };
  } else {
    mergeProbeInto(doc.schemas[schemaName], newProperties);
  }

  if (!doc.schemas[actualResponseRef]) {
    doc.schemas[actualResponseRef] = {
      id: actualResponseRef,
      type: "object",
      properties: newProperties,
    };
  } else {
    mergeProbeInto(doc.schemas[actualResponseRef], newProperties);
  }

  return doc;
}

function convertProbeFieldsToSchema(rootFieldsObj, schemas, rootPrefix = "") {
  // Iterative: build a properties{} map for each fields list. Nested
  // fields (message with children) get a placeholder schemas[name] entry
  // up-front so later refs can attach .children pointers without waiting
  // for recursion to finish; the queue then populates each placeholder's
  // properties in BFS order. Visited-set on nestedName prevents infinite
  // expansion for cyclic schemas.
  const rootProperties = {};
  const visited = new Set();
  const queue = [{ fieldsObj: rootFieldsObj, prefix: rootPrefix, dst: rootProperties }];
  // Track deferred .children attachments — populated once nested
  // properties land in schemas[nestedName].
  const pendingAttach = [];
  while (queue.length > 0) {
    const job = queue.shift();
    const { fieldsObj, prefix, dst } = job;
    const fields = Array.isArray(fieldsObj)
      ? fieldsObj
      : fieldsObj instanceof Map
        ? [...fieldsObj.values()]
        : Object.values(fieldsObj || {});
    for (const field of fields) {
      const prop = {
        id: field.number,
        number: field.number,
        name: field.name,
        type: field.type || "string",
        description: `Field ${field.number} (${field.type || "unknown"})`,
      };
      if (field.label === "repeated") {
        prop.type = "array";
        prop.items = { type: field.type || "string" };
        if (field.type === "message" && field.children) {
          const nestedName = field.messageType ||
            `${prefix}${field.name.charAt(0).toUpperCase() + field.name.slice(1)}Entry`;
          if (!schemas[nestedName] && !visited.has(nestedName)) {
            visited.add(nestedName);
            const nestedProperties = {};
            schemas[nestedName] = { id: nestedName, type: "object", properties: nestedProperties };
            queue.push({ fieldsObj: field.children, prefix: nestedName, dst: nestedProperties });
          }
          prop.items.$ref = nestedName;
          delete prop.items.type;
          pendingAttach.push({ target: prop.items, key: "children", schemaName: nestedName });
        }
      } else if (field.type === "message" && field.children) {
        const nestedName = field.messageType ||
          `${prefix}${field.name.charAt(0).toUpperCase() + field.name.slice(1)}`;
        if (!schemas[nestedName] && !visited.has(nestedName)) {
          visited.add(nestedName);
          const nestedProperties = {};
          schemas[nestedName] = { id: nestedName, type: "object", properties: nestedProperties };
          queue.push({ fieldsObj: field.children, prefix: nestedName, dst: nestedProperties });
        }
        prop.$ref = nestedName;
        delete prop.type;
        pendingAttach.push({ target: prop, key: "children", schemaName: nestedName });
      }
      const fieldKey = field.name || `field_${field.number}`;
      dst[fieldKey] = prop;
    }
  }
  // All nested schemas are now populated; resolve deferred .children refs.
  for (const a of pendingAttach) {
    if (schemas[a.schemaName] && schemas[a.schemaName].properties) {
      a.target[a.key] = schemas[a.schemaName].properties;
    }
  }
  return rootProperties;
}

// ─── req2proto Fallback Probing ──────────────────────────────────────────────

async function probeEndpoint(documentId, endpointKey) {
  const tab = _docForLearning(documentId);
  const tabId = (tab && tab.tabId != null) ? tab.tabId : null; // Chrome routing — derived from the doc
  const ep = tab.endpoints.get(endpointKey);
  if (!ep || ep.method !== "POST") return null;

  // Pass the API key the same way it was originally sent (URL param vs header).
  // Cookie, Origin, Referer are handled by the browser via the content script relay.
  const headers = {};
  const probeUrl = new URL(ep.url);
  probeUrl.searchParams.delete("key");

  if (ep.apiKey) {
    if (ep.apiKeySource === "url") {
      probeUrl.searchParams.set("key", ep.apiKey);
    } else {
      headers["X-Goog-Api-Key"] = ep.apiKey;
    }
  }

  const fetchFn = makePageFetchFn(tabId, documentId);
  const result = await probeApiEndpoint(probeUrl.toString(), headers, {
    fetchFn,
  });
  tab.probeResults.set(endpointKey, result);

  // Store scopes if the probe discovered them
  if (result.scopes?.length) {
    const svc = ep.service || extractInterfaceName(new URL(ep.url));
    tab.scopes.set(svc, result.scopes);
  }

  mergeToGlobal(tab);
  notifyPopup(tabId);
  return result;
}

// ─── Request/Response Handling (from intercept.js via content.js relay) ──────

// Host+path of POST URLs already error-probed by discoverServiceInfo (the
// automatic service-info probe below). Module-global so it dedupes across
// documents/tabs; resets on offscreen reload (a re-probe is harmless).
//
// THIS IS THE `one-per-endpoint` RULE, NOT A SOLVER BOUND, AND THE DISTINCTION IS WHY IT SURVIVES A
// SECTION THAT BANS SEEN-SETS BY NAME. §NO BOUNDS governs the FRONTIER — flows, exploration, and the
// principle that only EMITTED OUTPUT proves a flow is done, because shared state means byte-identical
// args can still progress. Nothing here is a flow. This gates whether the trusted zone puts a
// deliberately-malformed POST ON THE WIRE to a third-party API, and §Attacker sources states the rule
// for exactly that and states it the other way: "Each active fetch is made FROM the document that
// learned the endpoint, CORS-bounded both ways, one-per-endpoint (no method is universally safe — GET
// can mutate via /logout, /delete?id=), never a blind sweep." Without this set, `handleResponseBody`
// probes on EVERY captured POST — the blind sweep that sentence forbids, aimed at somebody else's
// server. Deleting it does not widen the search; the probe's answer is a property of the SERVER, not
// of any path through the page, so a second identical probe cannot learn what the first did not.
//
// It has been read the other way once already and that is why this comment is here: the commit that
// moved this file into C cited the set as one of three defects it was deleting, under §NO BOUNDS. The
// C it produced had no entry that could issue a credentialed POST at all, so what actually shipped was
// not an unbounded probe but no probe.
const _svcInfoProbedUrls = new Set();
