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
//
// AND THERE IS A THIRD AXIS THAT IS NOT ABOUT THE REQUEST AT ALL — WHOSE ACT IT IS — WHICH DECIDES THE
// TRANSPORT BEFORE THE VERB DECIDES THE ENTRY. The page-context relay issues the request AS THE PAGE with the
// person's own cookies, and lib/schema.js scopes it out of the credentialed destructive-path deny list on the
// ground that a human composed what it carries. `fetchDiscoveryForService` is reached from BOTH grades — the
// popup's FETCH_DISCOVERY button and lib/response-decode.js's automatic sweep — so it takes the grade as an
// argument and picks its transport from it: a human's act goes to the relay, this tool's own act goes to
// `safeFetch`, the chokepoint that asks the provenance, credential and deny-list questions the relay cannot.
// It is not a fallback and neither arm is legacy: they are two POLICIES over one question, and the grade is
// stated by the caller rather than inferred here, so a site that cannot name it aborts instead of defaulting
// into the exempt one.
//
// WHAT THAT COSTS THE AUTOMATIC ARM IS STATED RATHER THAN WORKED AROUND. `safeFetch` sends no cookies on this
// path and hardcodes `method:"GET"` (it reads neither `opts.method` nor `opts.body`, which is how RFC 9110
// §9.2.1 "Safe Methods" is enforced structurally rather than by a test). So an automatic sweep sees only what
// a logged-out client sees, and the POST error probe below has NO automatic form at all — there is no
// chokepoint that can carry it, and giving it one would be adding an autonomous credentialed POST to the one
// file whose safety argument is that it cannot make one. The probe survives at the grade entitled to it: the
// Discovery panel's per-endpoint "probe" and "service info" buttons.

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

/* THE AUTOMATIC ARM'S GET FUNCTION — the same question the relay's answers, asked at the chokepoint instead.
 *
 * WHY IT IS AN ADAPTER AND NOT A SECOND LOOP. The candidate walk below is written against a FUNCTION taking
 * `(url, headers)` and answering `{ok, status, headers, body}` or `{error}`, which is the relay's vocabulary;
 * the chokepoint answers its own record (`{ok, status, statusText, headers, body: Uint8Array, urlList,
 * computedType, refusal}`). One loop over two transports is the whole point — duplicating the walk would be
 * two copies of the candidate list, the diff, the merge and the not_found record, and the day they disagreed
 * a service would publish a document to one grade and not the other.
 *
 * WHAT IT STATES TO THE CHOKEPOINT, AND WHY EACH ANSWER IS THE ONE IT IS.
 *   pageUrl / pageOrigin — the analysed DOCUMENT's own browser-stated facts, off the DocData that
 *     `handleContentMessage` stamps from `_browserFacts` (`doc.url` is `MessageSender.url`, `doc.origin` is
 *     `MessageSender.origin`). NOT the tab's top-level address, which is the SSRF-principal substitution
 *     SECURITY.md records removing, and not a URL-derived origin, which fabricates a tuple origin for a
 *     sandboxed document the browser refused to give one.
 *   provenance `derived` — the app's own code named this HOST (the page reached it) and `buildDiscoveryUrls`
 *     composed a published well-known PATH onto it. No gate was forced to reach it, so it is not `forced`; the
 *     page never fetched `$discovery/rest` itself, so it is not `observed`. Calling it observed would be the
 *     fabrication §A-REQUEST-CARRIES-THE-PROVENANCE names — a reply merged into the observed pool for a
 *     request no client made.
 *   destination `""` — Fetch §2.2.5 "Requests"' destination for a plain data fetch. A discovery document is
 *     JSON this zone parses, never program text it compiles, so it takes no CORB.
 *
 * A REFUSAL COMES BACK AS `{error}` CARRYING ITS OWN GRADE AND REASON, never as a silent miss: the loop reads
 * `resp.error` as "this address published no document", and a request this tool DECLINED to make is a
 * different fact from one a server answered 404 to. `safeFetch`'s `statusText` is the only account anyone gets
 * of a request that did not happen, so it travels rather than being flattened. */
function _chokepointGetFn(tab, who) {
  DCHECK(typeof tab.url === "string" && tab.url !== "",
         "the automatic discovery sweep has no page principal for " + who + " — handleContentMessage stamps " +
         "`doc.url` from _browserFacts on every content message, so its absence is that mint broken and " +
         "safeFetch would classify this document's SSRF target against no principal at all");
  return async function (url, headers) {
    const r = await safeFetch(url, {
      pageUrl: tab.url,
      pageOrigin: tab.origin,
      provenance: "derived",
      destination: "",
      headers: headers || {},
    });
    /* `refusal` IS A POSITIVE STATEMENT ON EVERY RECORD safeFetch RETURNS — `null` means the request reached
       the wire and what follows is the server's answer, an HTTP error status included. So this reads the
       field rather than inferring a refusal from `!ok`, which would report a 404 as something this zone
       declined and a decline as something the service said. */
    if (r.refusal) return { error: r.refusal.kind + ":" + r.refusal.reason };
    return { ok: r.ok, status: r.status, headers: r.headers, body: new TextDecoder().decode(r.body) };
  };
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
 * @param {string} initiator WHOSE ACT THIS IS — lib/schema.js's `PAGE_CONTEXT_USER_INITIATED` (the popup's
 *   FETCH_DISCOVERY button) or `PAGE_CONTEXT_TOOL_INITIATED` (lib/response-decode.js's automatic sweep). It
 *   decides the TRANSPORT the candidate GETs leave by and whether the POST error-probe tail may fire at all.
 */
async function fetchDiscoveryForService(
  documentId,
  service,
  hostname,
  apiKeys,
  seedUrl,
  seedMethod,
  initiator,
) {
  pageContextStatedGrade(initiator, "lib/discovery-probe.js fetchDiscoveryForService, service " +
                                    JSON.stringify(service));
  const tab = _docForLearning(documentId);
  /* NO TERNARY. `_docForLearning` asserts the document is registered AND that it carries a tabId (the browser
     states one on every content message and the page-context relay routes by it), so `(tab && tab.tabId != null)
     ? … : null` re-answered a question that has already been answered by an abort. */
  const tabId = tab.tabId;

  /* A GET FUNCTION, AND THERE IS NO PARAMETER HERE IN WHICH TO ASK FOR ANYTHING ELSE — the candidate carries a
     URL and headers, and the relay's learning entry (lib/schema.js `pageContextGet`) takes exactly those. This
     loop used to hand the relay a `method` off each candidate, one of which was a POST.
     WHICH GET FUNCTION IS THE GRADE'S ANSWER AND NOT THIS LOOP'S. Both arms answer the same question in the
     same vocabulary — `{ok, status, headers, body}` or `{error}` — which is what lets one loop drive either,
     and is also why the DCHECK below can assert that vocabulary for both without knowing which one ran. */
  const getFn = initiator === PAGE_CONTEXT_USER_INITIATED
    ? makePageGetFn(tabId, documentId, initiator)
    : _chokepointGetFn(tab, "lib/discovery-probe.js fetchDiscoveryForService, service " +
                            JSON.stringify(service));
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
        /* A GET-FN REPLY IS A RECORD, and a non-record here is the edge broken rather than a candidate
           that 404'd — which `resp.error || !resp.ok` would have read as "this address does not publish a
           document" for every candidate of every service. It is asked of BOTH transports and names both,
           because the point of one loop over two is that neither gets to answer in its own dialect. */
        DCHECK(resp && typeof resp === "object",
               "the discovery GET function answered with no reply record — lib/schema.js's `pageContextGet` " +
               "and this file's `_chokepointGetFn` each return {ok, status, headers, body} or {error}, so " +
               "anything else is one of those two edges broken and every discovery candidate would read as " +
               "an address that published nothing");

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
            /* AND SO DOES THE RULE THAT NAMED THE BUCKET, which this record used to state NOWHERE — the same
               drop the not_found branch below has a paragraph about, one field over and with no `set` there to
               notice it. A published document answers "what does this service publish"; it says nothing about
               WHY the bucket has the name it has, and replacing the entry deleted lib/learn.js's answer to
               that. The Send panel's "grouping rule:" row therefore went blank for exactly the services whose
               documents were fetched, and the five `|| null`s downstream rendered that identically to a
               service no rule had ever named. lib/discovery-entry.js. */
            grouping: carriedGrouping(_prevDiscovery,
                                      "lib/discovery-probe.js storing a fetched published document, service " +
                                      JSON.stringify(service)),
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
          /* AND THE PROBE IS THE OPERATOR'S TAIL, NOT THE SWEEP'S. `performProbeAndPatch` POSTs a
             deliberately-malformed body with the person's cookies attached; the grade is what says a person
             asked for that. Under the automatic grade the fall-through below records what was fetched and
             stops — the seed and its verb stay on the record, which is what makes the Discovery panel's own
             probe button a request the operator can still choose to make. */
          if (seedUrl && _seedIsProbeable(seedUrl, seedMethod)) {
            const seedUrlObj = new URL(seedUrl);
            const match = findDiscoveryMethod(mergedDoc, seedUrlObj.pathname, seedMethod);
            if (!match && initiator === PAGE_CONTEXT_USER_INITIATED) {
              notifyPopup(tabId);
              await performProbeAndPatch(documentId, service, seedUrl, apiKey, initiator);
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

  /* THE SAME TAIL, THE SAME GRADE TEST, AND THE `else` IS WHERE THE AUTOMATIC ARM NOW LANDS — which is the
     right place rather than a consolation. The not_found record below states the fact this sweep established
     (this key set found no published document), keeps `seedUrl` and `seedMethod`, and keeps everything the
     bundle taught us; §Attacker-sources says a derived-and-unfired request "is not a gap in the report, it IS
     the report", and this record is that report's entry for one. What used to happen instead was a
     credentialed malformed POST at somebody else's server on the strength of a response body arriving. */
  if (finalSeedUrl && _seedIsProbeable(finalSeedUrl, finalSeedMethod) &&
      initiator === PAGE_CONTEXT_USER_INITIATED) {
    // Pick a key to try probing with (use the first available one if any)
    const probeKey = keysToTry[0] || null;
    await performProbeAndPatch(documentId, service, finalSeedUrl, probeKey, initiator);
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
      grouping: carriedGrouping(_prevDiscoveryNF,
                                "lib/discovery-probe.js recording a not_found published fetch, service " +
                                JSON.stringify(service)),
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
 *
 * A HUMAN'S ACT, ASSERTED AT THE DOOR. This function composes a POST of a deliberately-malformed body and
 * sends it with the person's own session attached, at a host the page named. RFC 9110 §9.2.1 "Safe Methods"
 * does not contain POST — "Of the request methods defined by this specification, the GET, HEAD, OPTIONS, and
 * TRACE methods are defined to be safe" — so nothing about the METHOD makes this cheap, and the body is one
 * the app never produced. CLAUDE.md §A-REQUEST-CARRIES-THE-PROVENANCE names credentialed + state-mutating +
 * values-the-app-never-produced as the one combination that is never a setting; what takes this out of that
 * combination is not a property of the request, it is that a person asked for it at a surface naming the
 * endpoint. So the grade is a REQUIRED argument and its absence aborts: this used to be reached from
 * lib/response-decode.js the instant a captured response body was protobuf, with nobody at any surface.
 */
async function performProbeAndPatch(documentId, service, targetUrl, apiKey, initiator) {
  pageContextRequireUserInitiated(initiator,
    "lib/discovery-probe.js performProbeAndPatch, service " + JSON.stringify(service));
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

  const fetchFn = makePageFetchFn(tabId, documentId, initiator);

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
        // A probe does not un-publish what a fetch read, nor un-name the request that named the service, nor
        // un-answer WHICH RULE named the bucket — an error-envelope probe learns a method surface and decides
        // nothing about the bucket's identity, so replacing the entry without carrying the rule forward was
        // the fetched record's drop repeated a second time (lib/discovery-entry.js).
        publishedJson: _prevProbed?.publishedJson || null,
        url: _prevProbed?.url || null,
        grouping: carriedGrouping(_prevProbed,
                                  "lib/discovery-probe.js storing a req2proto-probed document, service " +
                                  JSON.stringify(service)),
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
  /* THE TWO SIDES OF THIS MERGE HAVE DIFFERENT OWNERS, AND THAT DECIDES REFUSE-VERSUS-ASSERT AT EVERY READ.
     `target.properties` is a DISCOVERY DOCUMENT'S schema — bytes the target's server published, a spec file
     the researcher imported, or a store written by an earlier build — so every read of one is refused through
     lib/field-def.js. `probeProps` is `newProperties` from `convertProbeFieldsToSchema` below, at both call
     sites and no other, so it is OURS and a gap in it is this file broken: asserted, per CLAUDE.md
     §Offensive-programming, and asserted at the merge because that is where the record is first read back.
     WHICH TYPE SPELLINGS ARE "THE DOCUMENT SAID NOTHING". `"string"` is what lib/discovery.js's
     `mapJsonSchemaType` answers for a property whose document declared no type, and `"unknown"` is
     lib/req2proto.js's spelling of the same absence in the probe's own vocabulary. This merge knew only the
     first, so an untyped probe field — which arrives as `"unknown"` and is exactly the field most in need of
     a type — could never be upgraded by a later probe that had learned one. lib/req2proto.js's own merge has
     always asked the `"unknown"` half; this is the other half of that same question. */
  const PROBE_TYPE_UNSTATED = ["string", "unknown"];
  function mergeProbeInto(target, probeProps) {
    if (!target.properties) target.properties = {};
    // Build field-number → key index for deduplication
    const numToKey = {};
    for (const [k, p] of Object.entries(target.properties)) {
      /* A DOCUMENT'S FIELD NUMBER OR NOTHING. `?? ` asked whether the KEY was present, which admits
         `{"number": {}}` and files this property under the index `"[object Object]"` — a wire address no
         request can carry, minted out of a server's bytes. `{"widget": null}` reached `.number` off a null
         property, which is the TypeError lib/field-def.js's `fdDocRecord` was written for. */
      const rec = fdDocRecord(p) === null ? {} : p;
      const stated = fdDocKey(rec.number);
      const n = stated === null ? fdDocKey(rec.id) : stated;
      if (n !== null) numToKey[n] = k;
    }
    for (const [key, probeProp] of Object.entries(probeProps)) {
      const _scalarOrNull = (v) => v === null || typeof v === "string" || typeof v === "number";
      DCHECK(!!probeProp && typeof probeProp === "object" &&
             _scalarOrNull(probeProp.number) && _scalarOrNull(probeProp.id) &&
             typeof probeProp.type === "string" && probeProp.type !== "",
             "a probe-derived schema property arrived without the identity convertProbeFieldsToSchema states " +
             "for every one it mints (key `" + key + "`) — `number`/`id` are that function's refusal of the " +
             "server's field number, so `null` MEANS the rejection named none and a MISSING key means this " +
             "producer stopped stating it; a merge reading that absence as \"no match\" would file the " +
             "property under a fresh key and split one field into two");
      /* BOTH NAMES ARE STATED BY OUR OWN MINT, so this is the record's declared absence read as one — not a
         `??` asking which key happens to exist. `id` and `number` are the discovery vocabulary's two
         spellings of one wire address and this reads them in that order. */
      const fieldNum = probeProp.number === null ? probeProp.id : probeProp.number;
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
        /* AN UPGRADE NEEDS A REAL TYPE ON ONE SIDE AND AN UNSTATED ONE ON THE OTHER, and `"unknown"` belongs
           on BOTH lists — as a probe type it is the rejection having named none, so it must not overwrite a
           document's declaration, and as an existing type it is the very property a later probe should be
           allowed to fill in. A type this document does not state AT ALL is left alone deliberately: a
           property carrying a `$ref` has its `type` deleted, and writing a scalar type onto one would replace
           a message reference with a claim about a leaf. */
        if (PROBE_TYPE_UNSTATED.indexOf(probeProp.type) < 0 &&
            PROBE_TYPE_UNSTATED.indexOf(fdDocString(existing.type)) >= 0) {
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
    for (const raw of fields) {
      /* THE RECORD'S SHAPE IS OURS AND ITS VALUES ARE THE SERVER'S, which is lib/field-def.js's TRUST split
         and decides every line below. lib/req2proto.js writes these object literals in the trusted zone, so
         the NAMES are ours — but every value in them is a capture group off the target's own rejection text
         (`cleanName` is the tail of a path the error names; `typeStr || "unknown"` is whatever the message
         spelled), and a probe answer restored from IndexedDB was written by whatever build shipped then:
         lib/store-record.js's `_SR_PROBE_FIELDS` states `fields: _srObj` and says nothing about an element.
         So these are third-party bytes and every read of one REFUSES rather than asserts — a DCHECK here
         would be the trusted zone aborting on a stranger's error string. */
      const field = fdDocRecord(raw) === null ? {} : raw;
      /* THE WIRE KEY, DERIVED ONCE. It was derived twice — at the bottom for the properties map, and again
         inside each nested-message arm as `field.name.charAt(0)…` — and the second spelling had no refusal in
         front of it at all: a probe field the server described with a type and children but no name reached
         `undefined.charAt(0)` and threw a TypeError out of the trusted zone, which `probeEndpoint`'s catch
         then swallowed as "Probe fallback failed". The whole virtual document was lost on one malformed
         field, and the only trace was a console line. */
      const named = fdDocString(field.name);
      const number = fdDocKey(field.number);
      /* A FIELD THE REPLY GAVE NEITHER A NAME NOR A NUMBER HAS NO WIRE ADDRESS, and a discovery `properties`
         map is KEYED BY ONE. This used to key it `field_undefined` — an address no client can send and the
         Send panel renders as a real field name, which is §@H's fabricated value one layer up. Skipping is
         the true statement about it: the rejection said a field exists and said nothing this map can file it
         under. (`field_<n>` where a number IS stated is not the same thing — the number is the wire address,
         and the key is only how this document spells it.) */
      if (named === null && number === null) continue;
      const fieldKey = named === null ? `field_${number}` : named;
      /* `"unknown"` AND NOT `"string"`, BECAUSE THE TWO ARE DIFFERENT FACTS AND ONE OF THEM IS A REAL TYPE.
         `"unknown"` is lib/req2proto.js's own spelling for "the rejection named no type" (its enum/default
         arm mints exactly that, and its merge reads it back as the value a later probe may replace), so this
         is that vocabulary carried rather than a second one coined. `|| "string"` claimed the server had said
         `string` — the same bytes a genuinely string-typed field produces — while the DESCRIPTION built on
         the very next line said "unknown" about the same absence: one field, two spellings, and the
         confident one was the invented one. Downstream is unmoved either way: lib/discovery.js's
         `mapJsonSchemaType` answers "string" for an unrecognised type and for an absent one alike. */
      const type = fdDocString(field.type) === null ? "unknown" : field.type;
      const prop = {
        id: number,
        number,
        name: named,
        type,
        description: number === null
          ? `Probed field \`${fieldKey}\` (${type})`
          : `Field ${number} (${type})`,
      };
      /* `messageType` IS THE SCHEMA NAME THE REJECTION STATED, and its absence means the rejection named
         none — so the name is synthesized from the key this document already files the field under, which is
         the one identity available here. It is derived from `fieldKey` rather than from `field.name` so a
         field with a number and no name gets `…Field_3Entry` instead of reaching through a refusal. */
      const nestedFrom = (suffix) => {
        /* THE EMPTY STRING IS TEXT AND IS NOT A SCHEMA NAME, which is the same distinction the wire key above
           turns on: `fdDocString` admits `""` because a document may legitimately say a field's description
           is empty, and `schemas[""]` is an entry no `$ref` can address. The truthiness test this replaced
           happened to reject it; stating the reason is what keeps that true. */
        const stated = fdDocString(field.messageType);
        if (stated !== null && stated !== "") return stated;
        return `${prefix}${fieldKey.charAt(0).toUpperCase()}${fieldKey.slice(1)}${suffix}`;
      };
      if (field.label === "repeated") {
        prop.type = "array";
        prop.items = { type };
        if (type === "message" && field.children) {
          const nestedName = nestedFrom("Entry");
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
      } else if (type === "message" && field.children) {
        const nestedName = nestedFrom("");
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

/* THE PANEL'S PER-ENDPOINT PROBE. Same malformed credentialed POST as `performProbeAndPatch`, same reason the
   grade is a required argument, and this is the entry the capability now lives at: lib/popup-handlers.js's
   PROBE_ENDPOINT, posted by a click listener on the Discovery panel's own button. */
async function probeEndpoint(documentId, endpointKey, initiator) {
  pageContextRequireUserInitiated(initiator,
    "lib/discovery-probe.js probeEndpoint, endpoint " + JSON.stringify(endpointKey));
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
     exactly the names lib/endpoint-record.js declares. Both reads were undefined on every endpoint, so this
     probe went out with no key while looking like it chose where to put one.
     Same producer and same rule as DISCOVER_SERVICE: collectKeysForService finds the key, lib/keys.js
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

  const fetchFn = makePageFetchFn(tabId, documentId, initiator);
  const result = await probeApiEndpoint(probeUrl.toString(), headers, {
    fetchFn,
  });
  /* THE WHOLE RECORD AT THE MINT, THROUGH THE ONE TABLE. A DCHECK for `fieldCount`/`fields` stood here, in
     this file's own words, justified by "the shape lib/popup-discovery.js asserts there" — and that panel's
     assertion is gone, because a reader's opinion of half a contract is not where the contract lives. Both
     halves are lib/store-record.js's `_SR_PROBE_FIELDS`, which states all seven names lib/req2proto.js
     `probeApiEndpoint` returns and the two whose `null` MEANS the rejection named nothing; asking it here
     puts the abort at the PRODUCER, where a broken probe is, rather than at the next popup open. The bare
     endpoint key selects that shape (`_srProbeShape`), which is the same dispatch the panel renders on. */
  checkStoreRecord("probeResults", endpointKey, result,
                   "lib/discovery-probe.js storing probeApiEndpoint's answer for " + JSON.stringify(endpointKey));
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

// THE `_svcInfoProbedUrls` SET IS GONE WITH THE REQUEST IT GATED, and the reasoning is kept because the
// argument it made is still correct about a request nobody may make automatically any more.
//
// It held the host+path of every POST URL the automatic service-info probe had already error-probed, and it
// was the `one-per-endpoint` rule rather than a §NO BOUNDS seen-set: §NO BOUNDS governs the FRONTIER — flows,
// exploration, and the principle that only EMITTED OUTPUT proves a flow is done, because shared state means
// byte-identical args can still progress — and nothing this set touched was a flow. It gated whether the
// trusted zone put a deliberately-malformed POST ON THE WIRE at a third-party API, which is exactly what
// §Attacker sources states the rule for, the other way round: "Each active fetch is made FROM the document
// that learned the endpoint, CORS-bounded both ways, one-per-endpoint (a method's safety is a PROTOCOL
// CONTRACT and never a URL shape — see the safety rule below), never a blind sweep." Without it,
// `handleResponseBody` probed on EVERY captured POST — the blind sweep that sentence forbids.
//
// WHAT ACTUALLY ENDED THE BLIND SWEEP IS THE GRADE, WHICH IS A STRICTLY LARGER ANSWER TO THE SAME QUESTION.
// A dedup set makes an unauthorised request happen ONCE per address instead of many times; it never asked
// whether it should happen at all. lib/schema.js now requires an act's INITIATOR at the page-context relay,
// the automatic caller in lib/response-decode.js is deleted, and the probe survives at the Discovery panel's
// per-endpoint buttons — where a human names the endpoint, which is also why a per-address dedup would now be
// a tool refusing to repeat an act its operator asked for twice.
//
// AND ONE THING IT WAS READ AS AND MUST NOT BE AGAIN: the commit that moved this file into C cited the set as
// one of three defects it was deleting under §NO BOUNDS. The C it produced had no entry that could issue a
// credentialed POST at all, so what shipped was not an unbounded probe but NO probe. Deleting a gate is not
// the same act as deleting what it gated, and only the second of those is what happened here.
