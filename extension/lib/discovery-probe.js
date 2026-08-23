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
      /* WHERE A PARAM LIVES IS PART OF THE SURFACE, and every producer of a `parameters` entry in this
         extension now states it: lib/openapi-import.js writes `p.in`, lib/learn.js writes "path"/"query" off
         endpoint.c's per-param `location`, and a published Google Discovery document carries it natively. A
         param moving between the query string and the path is a REST restructure — the request built for the
         old shape stops resolving — so it is a change record like the other two, not a silent one. */
      if (op.location !== np.location) {
        changes.push({ type: "param_location_changed", methodId: id, param: p, from: op.location, to: np.location });
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

/* MAY THIS ZONE ERROR-PROBE THAT SEED? The probe is a POST of a deliberately-malformed body, so the answer is
   yes only for a seed the page itself POSTed. A seed whose verb nobody stated is REFUSED and the refusal
   ABORTS rather than being logged: the caller that named the address knows the method it saw
   (`handleResponseBody` holds `msg.method` beside `msg.url`), and passing the address without it is what let a
   GET endpoint be POSTed. Same rule as the API key two functions down — a request assembled out of something
   nobody observed is a fabricated request, not a probe. */
function _seedIsProbeable(seedUrl, seedMethod) {
  DCHECK(typeof seedMethod === "string" && seedMethod.length > 0,
         "a discovery seed (" + seedUrl + ") reached the error probe with no METHOD — the probe is a POST of a " +
         "malformed body, so a seed whose verb was never stated would be sent a method the page never used " +
         "against somebody else's server; the caller that captured the request must pass msg.method beside " +
         "msg.url");
  return seedMethod === "POST";
}

/**
 * Fetch discovery document for a service, trying multiple API keys.
 * Some discovery documents only load with the correct API key.
 *
 * @param {string} documentId  the document this fetch is issued AS
 * @param {string} service
 * @param {string} hostname
 * @param {string[]} apiKeys - All API keys to try for this service
 * @param {string} [seedUrl]  the request that named this service
 * @param {string} [seedMethod] THE VERB THE PAGE USED ON `seedUrl`. The probe fallback below is a POST of a
 *   deliberately-malformed body, so a seed whose method this zone was never told is a seed it may not probe.
 */
async function fetchDiscoveryForService(
  documentId,
  service,
  hostname,
  apiKeys,
  seedUrl,
  seedMethod,
) {
  const tab = _docForLearning(documentId);
  /* NO TERNARY. `_docForLearning` asserts the document is registered AND that it carries a tabId (the browser
     states one on every content message and the page-context relay routes by it), so `(tab && tab.tabId != null)
     ? … : null` re-answered a question that has already been answered by an abort. */
  const tabId = tab.tabId;

  /* A GET FUNCTION, AND THERE IS NO PARAMETER HERE IN WHICH TO ASK FOR ANYTHING ELSE — the candidate carries a
     URL and headers, and the relay's learning entry (lib/schema.js `pageContextGet`) takes exactly those. This
     loop used to hand the relay a `method` off each candidate, one of which was a POST. */
  const getFn = makePageGetFn(tabId, documentId);
  const triedKeys = new Set();

  // Build a deduplicated candidate list across all keys
  // Try each key separately to track which one works
  DCHECK(Array.isArray(apiKeys),
         "fetchDiscoveryForService was handed no key ARRAY — every caller passes collectKeysForService's " +
         "result, and `apiKeys || []` turned a caller that passed nothing into a keyless sweep that looks " +
         "like a service with no keys learned");
  const keysToTry = [...new Set(apiKeys)];

  // Always try without key as a fallback (some public APIs don't need one)
  if (!keysToTry.includes(null)) keysToTry.push(null);

  for (const apiKey of keysToTry) {
    if (apiKey) triedKeys.add(apiKey);
    const candidates = buildDiscoveryUrls(hostname, apiKey);

    for (const { url, headers } of candidates) {
      try {
        const resp = await getFn(url, headers);
        /* A RELAY REPLY IS A RECORD, and a non-record here is the relay edge broken rather than a candidate
           that 404'd — which `resp.error || !resp.ok` would have read as "this address does not publish a
           document" for every candidate of every service. */
        DCHECK(resp && typeof resp === "object",
               "the page-context GET relay answered with no reply record — pageContextGet returns " +
               "{ok, status, headers, body} or {error}, so anything else is that edge broken and every " +
               "discovery candidate would read as an address that published nothing");

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

          /* FETCH AGAINST FETCH, NEVER FETCH AGAINST OUR OWN MERGE. This diffed `existingEntry.doc` — which
             `mergeVirtualParts` has already loaded with the `learned` bucket lib/learn.js mints and the
             `probed` bucket the function below mints, plus every schema either invented — against the raw
             document the service just published, which has none of them. So EVERY fetch after the first
             reported every method this extension had learned as `method_removed` and every schema it had
             synthesized as `schema_removed`: a service accused of withdrawing methods it never published,
             recorded into the permanent history, and this panel is its first reader. What the record is about
             is what the SERVICE says, so both sides must be a document the service sent — kept as the
             serialized bytes of the last one, which is also what makes it a fact rather than a live graph
             aliased into the merge that is about to mutate it. */
          if (existingEntry?.publishedJson) {
            var diff = _diffDiscoveryDocs(JSON.parse(existingEntry.publishedJson), unifiedDoc);
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

          // The published bytes, captured BEFORE mergeVirtualParts — which mutates `unifiedDoc` in place and
          // returns it, so after the next line the two names are one already-merged object.
          const publishedJson = JSON.stringify(unifiedDoc);
          const mergedDoc = mergeVirtualParts(unifiedDoc, existingEntry?.doc);

          var _prevDiscovery = tab.discoveryDocs.get(service);
          tab.discoveryDocs.set(service, {
            status: "found",
            doc: mergedDoc,
            publishedJson,
            url,
            apiKey: apiKey || null,
            fetchedAt: Date.now(),
            // The seed survives a re-fetch: it is the request that NAMED this service, and it is what makes a
            // later probe of this service possible at all.
            seedUrl: seedUrl || _prevDiscovery?.seedUrl || null,
            seedMethod: seedMethod || _prevDiscovery?.seedMethod || null,
            pageUrls: _prevDiscovery?.pageUrls || new Set(),
            frameOrigins: _prevDiscovery?.frameOrigins || new Set(),
          });
          mergeToGlobal(tab);

          /* Is the seed's own method in the document we just stored? If not, the error probe patches it in.
             `findDiscoveryMethod(doc, …, "POST")` stood here and got BOTH arguments wrong. It read `doc` — the
             RAW body — so an OpenAPI/Swagger service (which has no `resources` until
             `convertOpenApiToDiscovery` runs) never matched anything and probed on every single fetch; and it
             asserted the seed was a POST, when `handleResponseBody` hands this function the URL of whatever it
             just captured. The probe is a POST of a malformed body, so a GET seed was answered by sending a
             method the page never used to somebody else's server — the one thing §Attacker-sources forbids
             outright. The method now travels with the seed. */
          if (seedUrl && _seedIsProbeable(seedUrl, seedMethod)) {
            const seedUrlObj = new URL(seedUrl);
            const match = findDiscoveryMethod(mergedDoc, seedUrlObj.pathname, seedMethod);
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
        /* AN INVARIANT ABORT IS NOT A CANDIDATE THAT DID NOT ANSWER. Everything inside this try —
           convertOpenApiToDiscovery, mergeVirtualParts, mergeToGlobal, performProbeAndPatch and the DCHECKs
           above — asserts its own contract, and on this side an assertion is a THROW (extension/check.js). So
           this `catch` was the local off-switch for every one of them: a broken merge read as "that address
           does not publish a document" and the sweep moved on to the next URL. */
        RETHROW_FATAL(err);
        // A candidate that did not answer with a document — try the next address.
      }
    }
  }

  // All keys (including null) failed.
  // FALLBACK: Try req2proto probing if we have a seed URL.
  const currentStatus = tab.discoveryDocs.get(service);
  const finalSeedUrl = seedUrl || currentStatus?.seedUrl;
  const finalSeedMethod = (seedUrl ? seedMethod : currentStatus?.seedMethod) || null;

  if (finalSeedUrl && _seedIsProbeable(finalSeedUrl, finalSeedMethod)) {
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
      /* THE SEED SURVIVES THE FAILURE. This record dropped it, and it is the one field that makes the next
         attempt possible: `finalSeedUrl = seedUrl || currentStatus?.seedUrl` above, and the popup's
         FETCH_DISCOVERY passes no seed at all — so a service that came up not_found could never be probed
         from the panel, because the address that named it had been erased by the record of its own failure. */
      seedUrl: seedUrl || _prevDiscoveryNF?.seedUrl || null,
      seedMethod: (seedUrl ? seedMethod : _prevDiscoveryNF?.seedMethod) || null,
      publishedJson: _prevDiscoveryNF?.publishedJson || null,
      pageUrls: _prevDiscoveryNF?.pageUrls || new Set(),
      frameOrigins: _prevDiscoveryNF?.frameOrigins || new Set(),
      /* AND SO DOES THE LEARNED DOCUMENT, WHICH IS THE WHOLE METHOD SURFACE. The comment above worked this
         out for ONE field and stopped there: this `set` REPLACES the entry, so a service whose published
         discovery document could not be found lost `doc` — the virtual RestDescription lib/learn.js fills
         with every endpoint the forced execution learned — along with `isVirtual`, `grouping` and the
         published fetch's own `url`/`apiKey`/`fetchedAt`. The two facts are orthogonal: "this service
         publishes no discovery document" says nothing whatever about what the BUNDLE taught us, and deleting
         the second because the first came back empty is the tool erasing its own product.
         MEASURED in Chrome: on the one-fetch probe page, a not_found landing between two renders emptied the
         Send panel's method list for a service whose endpoint the Discovery panel was listing on the same
         screen. `status` is the PUBLISHED FETCH's outcome and the presence of `doc` is the method surface —
         two facts, two fields, which is why every consumer now asks `.doc` for the second. */
      doc: _prevDiscoveryNF?.doc || null,
      isVirtual: _prevDiscoveryNF ? !!_prevDiscoveryNF.isVirtual : false,
      grouping: _prevDiscoveryNF?.grouping || null,
      url: _prevDiscoveryNF?.url || null,
      apiKey: _prevDiscoveryNF?.apiKey || null,
      fetchedAt: _prevDiscoveryNF?.fetchedAt || null,
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
  const tabId = tab.tabId; // Chrome routing — asserted non-null by _docForLearning

  /* THE BACKEND IS LOADED OR THIS FILE IS NOT RUNNING. ast-worker.html loads lib/req2proto.js before this
     file, so `probeApiEndpoint` being absent is that loader having changed — which the `console.error` +
     `return` reported as a probe that found nothing, for every probe, forever. */
  DCHECK(typeof probeApiEndpoint === "function",
         "probeApiEndpoint is not loaded — lib/req2proto.js is loaded before this file in ast-worker.html, so " +
         "its absence is the loader having dropped it and every error probe in the extension silently doing " +
         "nothing");

  const fetchFn = makePageFetchFn(tabId, documentId);

  /* THE SIXTH COPY, AND THE ONE THAT WAS FACING THE OTHER WAY. `apiKey ? {"x-goog-api-key": apiKey} : {}`
     stood here: where `probeEndpoint` below refuses to invent an injection point, this INVENTED one for every
     probe — a Google header stapled onto whatever host the seed named, which is the fabricated request that
     comment forbids by name. lib/keys.js recorded WHERE each key was seen ("url" / "header:<name>"), so the
     key goes back exactly there and nowhere else. */
  const probeUrlObj = new URL(targetUrl);
  const probeHeader = {};
  if (apiKey) {
    const _kEntry = tab.apiKeys.get(apiKey);
    DCHECK(!!_kEntry,
           "the key handed to the error probe is not in this document's key map — every caller picks it out of " +
           "collectKeysForService over exactly that map, so a miss is the two disagreeing about what was learned");
    DCHECK(_kEntry && typeof _kEntry.source === "string",
           "an API-key entry carries no source — lib/keys.js stamps the context every key was matched in, and " +
           "without it there is no observed place to put this key back");
    if (_kEntry.source === "url") probeUrlObj.searchParams.set("key", apiKey);
    else if (_kEntry.source.startsWith("header:")) probeHeader[_kEntry.source.slice("header:".length)] = apiKey;
  }

  // Add #_internal_probe fragment to avoid interception loop
  const safeTargetUrl = probeUrlObj.toString() + "#_internal_probe";

  try {
    const probeResult = await probeApiEndpoint(safeTargetUrl, probeHeader, {
      fetchFn,
    });

    /* WHAT A PROBE ANSWERS, AND WHAT IT MEANS WHEN IT ANSWERED NOTHING. `if (probeResult && probeResult.fields)`
       was true on EVERY path — probeApiEndpoint returns `fields: Object.fromEntries(...)`, and `{}` is truthy —
       so a probe that learned no field still wrote a virtual document and stamped the service `found`, which
       lib/response-decode.js reads as "stop asking": the real document a later API key would have unlocked was
       then never fetched again. A rejection that described no field is a real outcome of the probe and it
       patches nothing. */
    DCHECK(probeResult && typeof probeResult.fieldCount === "number" &&
           probeResult.fields && typeof probeResult.fields === "object",
           "probeApiEndpoint answered without {fieldCount, fields} — it returns them on every path, so " +
           "anything else is that producer broken and this would file a virtual document over nothing");
    if (probeResult.fieldCount > 0) {
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

      /* `method: existingDoc ? currentStatus.method || "HYBRID" : "PROBE"` IS DELETED, AND ITS ONLY READER WAS
         ITSELF. Nothing projects it to the popup (lib/serialize.js names nine fields and not this one), nothing
         carries it to globalStore (lib/merge.js the same), nothing rehydrates it (lib/persistence.js the same) —
         so "was this document PROBED, FETCHED or both" was computed on every probe, `||`-defaulted off its own
         previous value, and read by no surface there has ever been. `isVirtual` beside it is the field that
         does have readers (popup.js's method-picker dataset), and it stays. */
      var _prevProbed = tab.discoveryDocs.get(service);
      tab.discoveryDocs.set(service, {
        status: "found", // Treat as found so it shows up in UI
        doc: virtualDoc,
        apiKey: apiKey,
        fetchedAt: Date.now(),
        isVirtual: existingDoc ? currentStatus.isVirtual || false : true,
        // A probe does not un-publish what a fetch read, nor un-name the request that named the service.
        publishedJson: _prevProbed?.publishedJson || null,
        url: _prevProbed?.url || null,
        seedUrl: _prevProbed?.seedUrl || targetUrl,
        seedMethod: "POST",   // the probe IS a POST, and both call sites gate on the page having made one
        pageUrls: _prevProbed?.pageUrls || new Set(),
        frameOrigins: _prevProbed?.frameOrigins || new Set(),
      });
      mergeToGlobal(tab);
      notifyPopup(tabId);
    }
  } catch (probeErr) {
    /* An assertion inside the probe (this file's, lib/req2proto.js's, lib/learn.js's or the merge's) is an
       invariant abort and travels on through — `console.error` here reported one as "the probe failed". */
    RETHROW_FATAL(probeErr);
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

  /* NO `location` ON A PROBED FIELD, AND THAT IS THE ENGINE'S OWN VOCABULARY AGREEING RATHER THAN A GAP.
     endpoint.c stamps each @H param path|query|body and lib/learn.js dispatches on it — a body param never
     enters `parameters`, it becomes the request SCHEMA. Everything a req2proto probe learns IS a body field of
     the request message, so it lands in `doc.schemas[…Request].properties` below, which is where the discovery
     vocabulary puts a body and where `m.request.$ref` points. `parameters` stays empty because this probe
     observed no path or query param: the query/path half of the SAME address is lib/learn.js's record under
     `resources.learned`, with its own `location`. One field, one producer each. */
  doc.resources.probed.methods[methodName] = {
    id: methodId,
    path: fullPath,
    httpMethod: "POST", // the probe IS a POST, and both call sites gate on the page having made one
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
  const tabId = tab.tabId; // Chrome routing — asserted non-null by _docForLearning

  /* THE RECORD THE PANEL OFFERED IS THE ONE THIS PROBES, AND THE PANEL READS THE OVERLAY. `tab.endpoints.get`
     alone stood here, and lib/popup-discovery.js renders its buttons off `serializeTabData`'s endpoint map —
     which is globalStore's cumulative moat with THIS document's map laid over it (lib/serialize.js). So every
     endpoint learned by an EARLIER document of the same page (the ordinary case: a reload mints a new
     documentId while the records stay in globalStore under the same pageUrl) rendered a live button whose click
     found nothing here and came back as a refusal. Same two tiers, same order as the serializer's.
     THE ENTITLEMENT (§Attacker sources: "each active fetch is made FROM the document that learned the
     endpoint") IS STILL ONLY THE PANEL'S `ep.pageUrl` FILTER, and it cannot move here as an equality on that
     field: `pageUrl` is the document's address AT MERGE TIME, and a fragment write or a pushState route change
     moves `tab.url` without ending the document — so an equality check here would refuse exactly the JS-heavy
     app pages this tool targets. What states it soundly is the documentId, and lib/merge.js's `endpoints.set`
     does not record one; until it does, this reads the same moat the panel read. */
  const ep = tab.endpoints.get(endpointKey) || globalStore.endpoints.get(endpointKey) || null;
  if (!ep) return null;   // no such endpoint in this document or the moat — the panel reports the refusal
  DCHECK(ep.method === "POST",
         "the probe was asked for a non-POST endpoint (" + endpointKey + ") — the endpoint key encodes its " +
         "method and lib/popup-discovery.js offers the button for POST records only, so a GET arriving here is " +
         "that view and this backend disagreeing about the record they share");
  if (ep.method !== "POST") return null;

  // Cookie, Origin, Referer are handled by the browser via the content script relay.
  const headers = {};
  const probeUrl = new URL(ep.url);
  probeUrl.searchParams.delete("key");

  /* THE FIFTH COPY OF A DEAD PAIR. `if (ep.apiKey) { ep.apiKeySource === "url" ? … : X-Goog-Api-Key }` stood
     here and neither name exists on an endpoint record — lib/merge.js is the only `endpoints.set` and writes
     {url, method, host, path, service, source, pageUrl, requiredHeaders, pathParams, firstSeen}. Both reads
     were undefined on every endpoint, so this probe went out with no key while looking like it chose where to
     put one. Same producer and same rule as DISCOVER_SERVICE: collectKeysForService finds the key, lib/keys.js
     recorded WHERE it was seen, and a key with no observed injection point gets none invented — a guessed
     X-Goog-Api-Key on a third-party host is a fabricated request, not a probe. */
  DCHECK(typeof ep.service === "string",
         "an endpoint record reached the probe with no service — lib/merge.js writes `service: interfaceName` " +
         "on every record it mints, so `ep.service || extractInterfaceName(...)` was a second classifier " +
         "standing by for a field that is always there");
  const _svc = ep.service;
  const _keys = collectKeysForService(tab, _svc, probeUrl.hostname);
  if (_keys.length) {
    const _k = _keys[0];
    const _e = tab.apiKeys.get(_k);
    DCHECK(!!_e, "a key collectKeysForService returned is not in this document's key map — it reads exactly " +
                 "that map, so a miss is the two disagreeing about what was learned");
    DCHECK(typeof _e.source === "string",
           "an API-key entry carries no source — lib/keys.js stamps the context every key was matched in, and " +
           "without it there is no observed place to put this key back");
    if (_e.source === "url") probeUrl.searchParams.set("key", _k);
    else if (_e.source.startsWith("header:")) headers[_e.source.slice("header:".length)] = _k;
  }

  const fetchFn = makePageFetchFn(tabId, documentId);
  const result = await probeApiEndpoint(probeUrl.toString(), headers, {
    fetchFn,
  });
  /* The key lib/popup-discovery.js reads the answer back under, and the shape it asserts there. */
  DCHECK(result && typeof result.fieldCount === "number" && result.fields && typeof result.fields === "object",
         "probeApiEndpoint answered without {fieldCount, fields} — the panel reads exactly those two off this " +
         "record, so anything else renders as a probe that found nothing");
  tab.probeResults.set(endpointKey, result);

  // Store scopes if the probe discovered them
  if (result.scopes?.length) {
    tab.scopes.set(_svc, result.scopes);
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
