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

/* THE ONE PLACE AN API-KEY ENTRY BECOMES JSON — for the popup AND for IndexedDB.
   lib/persistence.js held a SECOND copy of this projection, and the two disagreed about exactly one field:
   this one carried `name` (the key TYPE lib/keys.js matched — "GitHub Token", "JWT", "Stripe Key") and that
   one did not, so every key surviving a save/load lost its type. lib/merge.js held a THIRD copy, in
   mergeToGlobal, which dropped the same field on the way from a tab to the cumulative store. Four components
   copying one record field-by-field, two of them omitting the same name, and the consumer reading
   `info.name || "API Key"` — so the loss rendered as a plausible generic label on exactly the keys that
   matter most, the ones carried across tabs and sessions. One projection now, called by both writers.

   The four collections are SETS and the count is a NUMBER, from every producer that exists: lib/keys.js
   builds them, mergeToGlobal rebuilds them, and _deserializeIntoGlobalStore reconstructs them from the
   stored arrays. The `instanceof Set ? … : v.<f> || []` ladder that stood on each one could therefore only
   ever fire for a record none of those three wrote. */
function serializeApiKeyEntry(v) {
  DCHECK(v && typeof v === "object", "serializeApiKeyEntry: an apiKeys value must be an entry object");
  DCHECK(typeof v.requestCount === "number",
         "an API-key entry carries no requestCount — lib/keys.js initialises it to 0 and every merge path " +
         "keeps a number there, so a missing one is a producer that stopped writing it and would serialize " +
         "as a real zero");
  for (const _f of ["services", "hosts", "endpoints", "pageUrls"]) {
    DCHECK(v[_f] instanceof Set,
           "an API-key entry's `" + _f + "` is not a Set — the three producers (lib/keys.js, mergeToGlobal, " +
           "_deserializeIntoGlobalStore) all build one, and anything else here serializes to an empty list " +
           "that reads as 'this key was never used against any'");
  }
  return {
    /* The key TYPE lib/keys.js's pattern named. NOT defaulted: an entry stored before this field was carried
       across the merge has none, and that absence is "recorded before the type travelled", which the reader
       must be able to tell from a key whose type is genuinely unknown. */
    name: v.name,
    /* NO `origin`. This projection carried it to BOTH consumers of an API-key entry — the popup over
       chrome.runtime.sendMessage and IndexedDB over lib/persistence.js — and neither has ever read it. It is
       the origin of the very URL `referer` states in full, computed from it in the same breath by lib/keys.js,
       so it was one fact written twice and looked at zero times. */
    referer: v.referer,
    source: v.source,
    firstSeen: v.firstSeen,
    lastSeen: v.lastSeen,
    requestCount: v.requestCount,
    services: [...v.services],
    hosts: [...v.hosts],
    endpoints: [...v.endpoints],
    pageUrls: [...v.pageUrls],
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
      /* THE SECOND COPY OF `f.sourceUrl || ("unknown_" + i)`, and the one that decided what the PANEL sees.
         lib/merge.js held the other and both are gone: bridge.js DCHECKs `msg.sourceUrl` as a non-empty
         string before it builds the analysis a finding is minted from, so the default could not fire — and
         had it ever fired, the two copies would have disagreed about the made-up name (the tab array's index
         here, the merge loop's index there), so ONE finding would have arrived at the popup under two keys
         and been listed twice. Asserted through the one shape lib/store-record.js declares, which is what the
         IndexedDB door asks of the same record a session later. */
      checkStoreRecord("securityFindings", f.sourceUrl, f,
                       "lib/serialize.js projecting this document's @S findings to the popup");
      merged.set(f.sourceUrl, f);
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
  /* WHAT MAKES AN ENTRY WORTH PROJECTING IS ITS `doc`, NOT ITS `status`. Both loops asked `status === "found"`
     and sent a bare `{status}` otherwise, which was a valid proxy only while nothing produced a record with a
     learned document and a non-"found" status. lib/discovery-probe.js's not_found branch now produces exactly
     that (it stopped deleting the method surface a failed PUBLISHED-document fetch has nothing to say about),
     and the TAB loop is where it bit: a tab entry overwrites the global one, so a not_found on the document
     the popup is standing on replaced the cumulative moat's whole RestDescription with a two-word stub, and
     the Send panel offered no method at all for a service whose endpoint the Discovery panel was listing on
     the same screen. `status` is the published fetch's outcome and rides along as one; the doc is the fact. */
  const mergedDiscovery = {};
  for (const [k, v] of globalStore.discoveryDocs) {
    if (v.doc) {
      /* THE SAME LADDER serializeApiKeyEntry ABOVE DELETED, surviving one loop later on the same two names.
         Every producer of a `status:"found"` GLOBAL entry writes both as Sets — lib/merge.js's mergeToGlobal
         builds them with `new Set(...)`, _deserializeIntoGlobalStore rebuilds them from the stored arrays, and
         lib/popup-handlers.js's re-fetch carries the previous entry's — so `instanceof Set ? … : … || []`
         could only ever fire for a record none of those three wrote, and what it produced then was an EMPTY
         LIST, which reads as "this service was never used from any page" rather than as a broken producer.
         (The TAB loop below is the case that genuinely IS optional and says so there: lib/learn.js mints a
         virtual doc entry carrying neither field, and lib/response-decode.js adds them the first time a
         request names the service.) */
      DCHECK(v.pageUrls instanceof Set && v.frameOrigins instanceof Set,
             "a global discoveryDocs entry carrying a doc has a pageUrls/frameOrigins that is not " +
             "a Set — mergeToGlobal, _deserializeIntoGlobalStore and the popup's re-fetch all build Sets, so " +
             "anything else here serializes as an empty list that reads as 'never used from any page' " +
             "(service=" + k + ")");
      checkDiscoveryGrouping(v, "lib/serialize.js projecting the moat's discovery docs to the popup, service " +
                                JSON.stringify(k));
      mergedDiscovery[k] = {
        status: v.status,
        url: v.url,
        apiKey: v.apiKey || null,
        fetchedAt: v.fetchedAt,
        doc: v.doc || null,
        grouping: v.grouping,
        isVirtual: !!v.isVirtual,
        pageUrls: [...v.pageUrls],
        frameOrigins: [...v.frameOrigins],
      };
    } else {
      mergedDiscovery[k] = { status: v.status, grouping: null };
    }
  }
  for (const [k, v] of tab.discoveryDocs) {
    if (v.doc) {
      // Merge pageUrls/frameOrigins from global base if present
      var _existingMerged = mergedDiscovery[k];
      var _allPageUrls = new Set(_existingMerged?.pageUrls || []);
      if (v.pageUrls) for (var _pu of v.pageUrls) _allPageUrls.add(_pu);
      var _allFrameOrigins = new Set(_existingMerged?.frameOrigins || []);
      if (v.frameOrigins) for (var _fo of v.frameOrigins) _allFrameOrigins.add(_fo);
      /* THE UNION OF TWO STATEMENTS, NOT A LADDER OVER TWO ABSENCES. Both sides state `grouping`, so the
         question this line asks is which of them NAMES a rule — the tab's classification is the fresher one,
         and the moat's is what a bucket named in an earlier session still knows. `v.grouping || (…) || null`
         could not ask that: an unstated field and a stated `null` both fell through it, so a producer that
         stopped writing the field read exactly like a bucket no rule had named (lib/discovery-entry.js). */
      checkDiscoveryGrouping(v, "lib/serialize.js projecting a tab's discovery docs to the popup, service " +
                                JSON.stringify(k));
      if (_existingMerged) checkDiscoveryGrouping(_existingMerged,
        "lib/serialize.js's own moat projection, being unioned with a tab entry for service " + JSON.stringify(k));
      mergedDiscovery[k] = {
        status: v.status,
        url: v.url,
        apiKey: v.apiKey || null,
        fetchedAt: v.fetchedAt,
        doc: v.doc || null,
        grouping: v.grouping !== null ? v.grouping : (_existingMerged ? _existingMerged.grouping : null),
        isVirtual: v.isVirtual || (_existingMerged && _existingMerged.isVirtual) || false,
        pageUrls: [..._allPageUrls],
        frameOrigins: [..._allFrameOrigins],
      };
    } else if (!mergedDiscovery[k]) {
      /* A DOC-LESS TAB ENTRY NO LONGER DELETES THE GLOBAL ONE. This arm ran `mergedDiscovery[k] = {status}`
         unconditionally, so a "pending" or "not_found" record for the document in front of you erased every
         method every OTHER page of every OTHER session had learned for that service. What a doc-less tab
         entry states is the published fetch's outcome, which is only news where the moat has nothing. It also
         states that no rule is recorded for the bucket, because the popup reads `svcData.grouping` off
         whichever record it is handed and an absent field there is the one thing it must not have to guess. */
      mergedDiscovery[k] = { status: v.status, grouping: null };
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
    /* THE ENGINE'S PAGE ERRORS, WHOSE ABSENCE IS A STATEMENT. offscreen-brain.js creates `_resolverErrors`
       only when the engine's result actually carried one, so the field being missing MEANS "the engine ran
       this document and recorded nothing that went wrong" — a real finding, not a hole. It is written that
       way rather than `|| []` so that a malformed one still crashes instead of being flattened into the
       same empty list. */
    /* WHAT THIS DOCUMENT'S ENGINE RUN WAS, WHICH IS THE ONLY THING THAT MAKES THE FINDINGS BELOW READABLE.
       A run that CRASHED has still learned everything it emitted before it died — those endpoints and sinks
       are in the maps above, merged as the engine streamed them — and the one thing that must never happen is
       for that page to read as a completed clean analysis. `null` is the positive statement "no run has
       returned for this document yet" (it is also what an unknown document gets, through _emptyDocView),
       which is a different fact from "a run returned and crashed" and from "a run returned complete", and the
       popup says all three in words. NOT defaulted to a plausible "complete": offscreen-brain.js writes
       `_astRun` off bridge.js's `_run` on every terminal path, so a value outside the set is that chain
       broken, and the shape it would break into is exactly the false clean bill this field exists to stop. */
    analysisRun: (function () {
      if (tab._astRun === undefined) return null;   // no engine run has returned for this document
      DCHECK(tab._astRun === "complete" || tab._astRun === "crashed" || tab._astRun === "nothing-to-run",
             "a DocView's _astRun is present but is not a run outcome this seam speaks (`" + tab._astRun +
             "`) — offscreen-brain.js writes it straight off the analysis's `_run`, so anything else is that " +
             "relay broken and a crashed run would render as a page that was analysed and found clean");
      return tab._astRun;
    })(),
    resolverErrors: (function () {
      if (tab._resolverErrors === undefined) return [];   // the engine recorded no page error for this document
      DCHECK(Array.isArray(tab._resolverErrors),
             "a DocView's _resolverErrors is present but is not an array — offscreen-brain.js pushes " +
             "{context, message} records into it and the popup's diagnostic view iterates them");
      return tab._resolverErrors;
    })(),
    /* WHETHER THIS DOCUMENT'S BUNDLE EVER REACHED THE ANALYSIS, WHICH `analysisRun` ABOVE CANNOT SAY. A page
       whose page source could not be obtained never starts a run, so `analysisRun` is `null` for it — and
       `null` is also what a page still being explored carries, and what a page nobody has reported on
       carries. Three facts, one value, and the one that reads worst is the failure: reddit.com's bot
       challenge makes its own document unfetchable a second time, so the whole site rendered here exactly
       like a page that was analysed and found nothing.
       `null` here is still a real statement and a NARROWER one: nothing has been reported about this
       document's page source at all (an unknown document, through _emptyDocView, is the same). It is not
       defaulted to a plausible "delivered" — offscreen-brain.js writes the record on BOTH content arms, so a
       missing one where a content script has run is that relay broken, and the shape it would break into is
       the false clean bill this field exists to stop. */
    pageSource: (function () {
      if (tab._pageSource === undefined) return null;   // nothing has been reported about this document's source
      const ps = tab._pageSource;
      DCHECK(ps.state === "delivered" || ps.state === "unavailable",
             "a DocView's _pageSource is present but its state is `" + ps.state + "` — offscreen-brain.js's " +
             "_setPageSource writes exactly two, so anything else is that writer bypassed and the popup " +
             "would render a document whose analysability it cannot state");
      if (ps.state === "delivered") return { state: "delivered" };
      DCHECK(ps.kind === "status" || ps.kind === "empty" || ps.kind === "network",
             "an unavailable page source reached the popup seam with kind `" + ps.kind + "` — the arm that " +
             "writes it admits exactly three, and a report that cannot say WHICH failure it was is the " +
             "silence it exists to replace wearing a field name");
      DCHECK((ps.kind === "status") === (ps.status !== undefined),
             "an unavailable page source carries kind `" + ps.kind + "` with status `" + ps.status + "` — a " +
             "status rides the `status` kind and no other, so this pair is the two halves of one report " +
             "disagreeing about which failure it describes");
      DCHECK((ps.kind === "network") === (ps.detail !== undefined),
             "an unavailable page source carries kind `" + ps.kind + "` with detail `" + ps.detail + "` — a " +
             "detail rides the `network` kind and no other");
      const out = { state: "unavailable", kind: ps.kind };
      if (ps.status !== undefined) out.status = ps.status;
      if (ps.detail !== undefined) out.detail = ps.detail;
      return out;
    })(),
  };
}
