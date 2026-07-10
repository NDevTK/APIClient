// lib/serialize.js — Serialize the merged doc/global state for the popup + persistence: merge virtual
// discovery parts, serialize API-key entries + security findings, and build the per-tab snapshot the popup
// renders. Extracted from the offscreen-brain.js monolith (one problem per file); loaded before it, resolves
// globalStore + globalRequestLog at call-time.

function mergeVirtualParts(newDoc, oldDoc) {
  if (!oldDoc || !newDoc) return newDoc;

  // Preserve "learned" methods (deep copy to avoid aliasing)
  if (oldDoc.resources?.learned) {
    if (!newDoc.resources) newDoc.resources = {};
    newDoc.resources.learned = JSON.parse(JSON.stringify(oldDoc.resources.learned));
  }

  // Preserve "probed" methods (deep copy to avoid aliasing)
  if (oldDoc.resources?.probed) {
    if (!newDoc.resources) newDoc.resources = {};
    newDoc.resources.probed = JSON.parse(JSON.stringify(oldDoc.resources.probed));
  }

  // Preserve learned schemas + carry over custom renames into new schemas
  if (oldDoc.schemas) {
    for (const [name, schema] of Object.entries(oldDoc.schemas)) {
      if (!newDoc.schemas[name]) {
        newDoc.schemas[name] = schema;
      } else {
        // Schema exists in both — preserve customName fields from old
        const oldProps = schema.properties || {};
        const newProps = newDoc.schemas[name].properties || {};
        for (const [pKey, pVal] of Object.entries(oldProps)) {
          if (pVal.customName && newProps[pKey]) {
            newProps[pKey].name = pVal.name;
            newProps[pKey].customName = true;
          }
        }
      }
    }
  }

  // Carry over custom parameter renames from old methods
  if (oldDoc.resources) {
    function carryRenames(oldRes, newRes) {
      if (!oldRes || !newRes) return;
      for (const [rName, r] of Object.entries(oldRes)) {
        if (!newRes[rName]) continue;
        for (const [mName, oldM] of Object.entries(r.methods || {})) {
          const newM = newRes[rName]?.methods?.[mName];
          if (!newM) continue;
          // Carry parameter renames
          if (oldM.parameters) {
            for (const [pName, pVal] of Object.entries(oldM.parameters)) {
              if (pVal.customName && newM.parameters?.[pName]) {
                newM.parameters[pName].name = pVal.name;
                newM.parameters[pName].customName = true;
              }
            }
          }
          // Carry stats and chains
          if (oldM._stats && !newM._stats) newM._stats = oldM._stats;
          if (oldM._chains && !newM._chains) newM._chains = oldM._chains;
        }
      }
    }
    carryRenames(oldDoc.resources, newDoc.resources);
  }

  return newDoc;
}

function serializeApiKeyEntry(v) {
  return {
    name: v.name,
    origin: v.origin,
    referer: v.referer,
    source: v.source,
    firstSeen: v.firstSeen,
    lastSeen: v.lastSeen,
    requestCount: v.requestCount || 0,
    services: [...(v.services instanceof Set ? v.services : v.services || [])],
    hosts: [...(v.hosts instanceof Set ? v.hosts : v.hosts || [])],
    endpoints: [
      ...(v.endpoints instanceof Set ? v.endpoints : v.endpoints || []),
    ],
    pageUrls: [
      ...(v.pageUrls instanceof Set ? v.pageUrls : v.pageUrls || []),
    ],
  };
}

function mergedSecurityFindings(tab) {
  // Global base (keyed by sourceUrl), tab overwrites
  var merged = new Map();
  for (const [k, v] of globalStore.securityFindings) {
    merged.set(k, v);
  }
  if (tab._securityFindings) {
    for (var i = 0; i < tab._securityFindings.length; i++) {
      var f = tab._securityFindings[i];
      merged.set(f.sourceUrl || ("unknown_" + i), f);
    }
  }
  return [...merged.values()];
}

function serializeTabData(tab) {
  // Merge global store (base) with tab data (tab wins on conflict)

  // API keys: global base, tab overwrites
  const mergedKeys = {};
  for (const [k, v] of globalStore.apiKeys) {
    mergedKeys[k] = serializeApiKeyEntry(v);
  }
  for (const [k, v] of tab.apiKeys) {
    mergedKeys[k] = serializeApiKeyEntry(v);
  }

  // Endpoints: global base, tab overwrites
  const mergedEndpoints = {};
  for (const [k, v] of globalStore.endpoints) {
    mergedEndpoints[k] = v;
  }
  for (const [k, v] of tab.endpoints) {
    mergedEndpoints[k] = v;
  }

  // Scopes: global base, tab overwrites
  const mergedScopes = {};
  for (const [k, v] of globalStore.scopes) {
    mergedScopes[k] = v;
  }
  for (const [k, v] of tab.scopes) {
    mergedScopes[k] = v;
  }

  // Discovery docs: global base, tab overwrites with full doc
  const mergedDiscovery = {};
  for (const [k, v] of globalStore.discoveryDocs) {
    if (v.status === "found") {
      mergedDiscovery[k] = {
        status: v.status,
        url: v.url,
        method: v.method,
        apiKey: v.apiKey || null,
        fetchedAt: v.fetchedAt,
        doc: v.doc || null,
        grouping: v.grouping || null,
        isVirtual: !!v.isVirtual,
        pageUrls: [...(v.pageUrls instanceof Set ? v.pageUrls : v.pageUrls || [])],
        frameOrigins: [...(v.frameOrigins instanceof Set ? v.frameOrigins : v.frameOrigins || [])],
      };
    } else {
      mergedDiscovery[k] = { status: v.status };
    }
  }
  for (const [k, v] of tab.discoveryDocs) {
    if (v.status === "found") {
      // Merge pageUrls/frameOrigins from global base if present
      var _existingMerged = mergedDiscovery[k];
      var _allPageUrls = new Set(_existingMerged?.pageUrls || []);
      if (v.pageUrls) for (var _pu of v.pageUrls) _allPageUrls.add(_pu);
      var _allFrameOrigins = new Set(_existingMerged?.frameOrigins || []);
      if (v.frameOrigins) for (var _fo of v.frameOrigins) _allFrameOrigins.add(_fo);
      mergedDiscovery[k] = {
        status: v.status,
        url: v.url,
        method: v.method,
        apiKey: v.apiKey || null,
        fetchedAt: v.fetchedAt,
        doc: v.doc || null,
        grouping: v.grouping || (_existingMerged && _existingMerged.grouping) || null,
        isVirtual: v.isVirtual || (_existingMerged && _existingMerged.isVirtual) || false,
        pageUrls: [..._allPageUrls],
        frameOrigins: [..._allFrameOrigins],
      };
    } else {
      mergedDiscovery[k] = { status: v.status };
    }
  }
  // Probe results: global base, tab overwrites
  const mergedProbe = {};
  for (const [k, v] of globalStore.probeResults) {
    mergedProbe[k] = v;
  }
  for (const [k, v] of tab.probeResults) {
    mergedProbe[k] = v;
  }

  return {
    apiKeys: mergedKeys,
    endpoints: mergedEndpoints,
    authContext: tab.authContext,
    scopes: mergedScopes,
    discoveryDocs: mergedDiscovery,
    probeResults: mergedProbe,
    requestLog: tab.documentId ? globalRequestLog.filter(function (r) { return r.documentId === tab.documentId; }) : [],
    securityFindings: mergedSecurityFindings(tab),
    resolverErrors: tab._resolverErrors || [],
  };
}
