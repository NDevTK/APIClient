/* lib/popup-discovery.js — THE ACTIVE-DISCOVERY PANEL: the human surface for the four offscreen commands that
   had handlers and no caller.

   CLAUDE.md §Attacker sources: "Active discovery is REQUIRED (lazy scripts + discovery docs + Google API
   req2proto error-probes) — passive learning is too thin." Three of those probes were reachable only from a
   message no document sent — `PROBE_ENDPOINT` (req2proto field discovery over one endpoint), `DISCOVER_SERVICE`
   (the gapi error-envelope probe: canonical service/method + required OAuth scopes) and `FETCH_DISCOVERY` (pull
   the service's discovery/OpenAPI document with the keys this document learned) — so the REQUIRED half of the
   learning surface shipped unreachable. The fourth, `GET_DISCOVERY_CHANGES`, answers with the whole diff
   history lib/discovery-probe.js records every time a published API's surface changes between two fetches:
   written, persisted across sessions, and shown to nobody.

   `probeResults` was in the same state from the other end — every probe stores its answer in the document view
   and the popup rendered none of it, so even the automatic probes were invisible. This panel is where a probe's
   fields, its canonical service/method and its scopes are read.

   EACH ACTION RUNS AS THE PAGE, so each needs a LIVE pinned document (currentDocumentId()): the offscreen
   resolves `msg.documentId` to the DocData whose tabId routes the page-context relay, and refuses when there is
   none. That refusal is stated in the panel rather than swallowed — a button that silently does nothing is the
   defect this file exists to end. */

// service -> [{ timestamp, fetchUrl, changes: [{type, ...}] }], as recorded by lib/discovery-probe.js's
// _diffDiscoveryDocs. `null` until GET_DISCOVERY_CHANGES has answered — a different statement from `{}`
// (answered, and no API's surface has changed between two fetches yet).
let discoveryChanges = null;
// The one in-flight action ("<action>:<key>"), so the panel says which request is out rather than looking idle.
let _discInFlight = null;
// The last action that came back with no result, and which one it was. An offscreen refusal is REPORTED.
let _discRefused = null;

async function loadDiscoveryChanges() {
  const changes = await chrome.runtime.sendMessage({ type: "GET_DISCOVERY_CHANGES" });
  /* The handler answers `Object.fromEntries(globalStore.discoveryChanges)` unconditionally, so anything but an
     object is that command not reaching the offscreen document at all — which an empty view would report as
     "no API surface has ever changed", a real finding, fabricated. */
  DCHECK(changes && typeof changes === "object",
         "GET_DISCOVERY_CHANGES did not answer with an object — the offscreen builds it straight off " +
         "globalStore.discoveryChanges on every call, so anything else is that command not being handled");
  discoveryChanges = changes;
}

// Every service this document could ask about: one that already has a discovery document, plus one named by a
// learned endpoint (the case that MATTERS — a service with no document yet is exactly what FETCH_DISCOVERY is
// for, and a list built only from discoveryDocs could never offer it).
//
// ACTIONABLE = LEARNED BY *THIS* DOCUMENT. §Attacker sources: "Each active fetch is made FROM the document that
// learned the endpoint, CORS-bounded both ways." tabData is the GLOBAL cumulative moat overlaid with this
// document's view, so it carries every endpoint every page of every session ever learned — offering those as
// buttons would issue somebody else's endpoint from this page, which is the blind sweep that rule forbids.
// `pageUrl` is the document's own address at the time lib/merge.js recorded the endpoint, so it is what says
// which of them this document is entitled to probe.
function _discPageUrl() {
  const docId = currentDocumentId();
  if (!docId) return null;   // no document is pinned — a real state, and the caller's own empty-panel case
  /* NEITHER HALF OF `(f && f.url) || null` CAN BE ABSENT, AND BOTH WERE DEFAULTED. currentDocumentId() returns
     a documentId only for a frame that IS in availableFrames (that membership is the whole readiness test), so
     the find cannot miss; and lib/popup-handlers.js asserts every frame's `url` as the string webNavigation's
     IDL declares non-optional. What the two defaults produced was the failure named in popup.js's loadState:
     a null page url makes this panel say "no endpoint of this service was learned here" about a document that
     learned one — a lookup miss rendered as a fact about the page. */
  const f = availableFrames.find((x) => x.documentId === docId);
  DCHECK(!!f, "the pinned documentId is not in the frame list it was validated against — currentDocumentId() " +
              "answers non-null only for a frame present in availableFrames, so the two disagreeing means the " +
              "list changed under this render and the discovery panel would disown this document's endpoints");
  DCHECK(typeof f.url === "string" && f.url.length > 0,
         "the pinned document's frame carries no url — GET_FRAMES asserts webNavigation's non-optional url " +
         "string on every frame, so its absence here is that reply rebuilt somewhere in between, and this " +
         "panel would report the document as having learned nothing");
  return f.url;
}
function _discServices(pageUrl) {
  const out = new Map();   // service -> { hostname, endpointKeys: [] }
  if (!tabData) return out;
  /* THE TWO COLLECTIONS THIS PANEL IS MADE OF, ASSERTED WHERE IT READS THEM. `tabData.endpoints || {}` and
     `tabData.discoveryDocs || {}` stood here, and lib/serialize.js's `serializeTabData` — the ONE producer of
     everything on this object — builds both unconditionally as the global store overlaid with the document's
     own view. So the `|| {}` could only ever fire for a reply that projection did not make, and what it
     produced then was this panel's "No service has been learned for this document yet — nothing to probe":
     a claim about the page, assembled out of a missing reply. */
  DCHECK(tabData.endpoints && typeof tabData.endpoints === "object" &&
         tabData.discoveryDocs && typeof tabData.discoveryDocs === "object",
         "the popup's state reply carries no endpoints/discoveryDocs map — lib/serialize.js writes both on " +
         "every GET_STATE answer, so an absent one is that projection broken and this panel would report a " +
         "document that learned a service as having learned nothing");
  for (const [k, ep] of Object.entries(tabData.endpoints)) {
    /* THE WHOLE RECORD, FROM THE ONE PLACE THAT DESCRIBES IT. This was four names checked by hand, which was
       four of the ten and the four this panel happened to read — a transcription of the producer's literal,
       and therefore a second copy of it that could go stale silently. lib/endpoint-record.js is the copy that
       cannot: it is what the producer BUILDS through, so the list here and the list there are one list. */
    checkEndpointRecord(ep, "the discovery panel's GET_STATE reply, key " + JSON.stringify(k));
    if (!pageUrl || ep.pageUrl !== pageUrl) continue;   // another document learned it — not ours to probe
    let e = out.get(ep.service);
    if (!e) { e = { hostname: ep.host, endpointKeys: [] }; out.set(ep.service, e); }
    e.endpointKeys.push(k);
  }
  for (const svc of Object.keys(tabData.discoveryDocs)) {
    if (!out.has(svc)) out.set(svc, { hostname: null, endpointKeys: [] });
  }
  return out;
}

function _discChangeLine(c) {
  switch (c.type) {
    case "method_added":        return "+ method " + c.httpMethod + " " + c.methodId + " (" + (c.path || "") + ")";
    case "method_removed":      return "- method " + c.httpMethod + " " + c.methodId;
    case "param_added":         return "+ param " + c.methodId + "." + c.param;
    case "param_removed":       return "- param " + c.methodId + "." + c.param;
    case "param_type_changed":  return "~ param " + c.methodId + "." + c.param + ": " + c.from + " -> " + c.to;
    case "param_location_changed":
      return "~ param " + c.methodId + "." + c.param + " moved: " +
             String(c.from || "(unstated)") + " -> " + String(c.to || "(unstated)");
    case "param_required":      return "! param " + c.methodId + "." + c.param + " is now required";
    case "schema_added":        return "+ schema " + c.schema;
    case "schema_removed":      return "- schema " + c.schema;
  }
  /* A CHANGE KIND WITH NO SENTENCE IS DRIFT BETWEEN THIS VIEW AND ITS PRODUCER, and the producer is one
     function (lib/discovery-probe.js `_diffDiscoveryDocs`) emitting exactly the nine above. */
  DFAIL("a discovery-change record carries a type this panel has no sentence for (" + JSON.stringify(c.type) +
        ") — _diffDiscoveryDocs emits nine kinds and each one is spelled out here, so a tenth is a producer " +
        "that grew a record nobody reports");
  return String(c.type);
}

function renderDiscoveryPanel() {
  const el = document.getElementById("discovery-body");
  // popup.html declares this element unconditionally, so its absence is the markup and this view having
  // drifted apart — asserted rather than skipped, which would render the whole panel as "nothing here".
  DCHECK(!!el, "the discovery panel has no body element to render into (#discovery-body)");
  if (!el) return;
  const docId = currentDocumentId();
  const pageUrl = _discPageUrl();
  const services = _discServices(pageUrl);
  let html = "";

  /* WHY A BUTTON MAY BE OFF, SAID OUT LOUD. These requests are issued BY THE PAGE (the page-context relay), so
     they need a live document to be issued from — the same pin the Send panel gates on. */
  if (!docId) {
    html += '<div class="hint">No live document is pinned. Every probe below is issued <em>as the page</em> ' +
            'through its own document, so it needs one that is currently loaded — open the target page (or ' +
            're-confirm the pin in the Send panel) and these become available.</div>';
  }
  if (_discRefused) {
    html += '<div class="hint">The offscreen refused <code>' + esc(_discRefused) + '</code> — it answered with ' +
            'no result. That is the document being gone or the endpoint not being one it holds, never a probe ' +
            'that quietly did nothing.</div>';
  }

  if (services.size === 0) {
    html += '<div class="empty">No service has been learned for this document yet — nothing to probe.</div>';
  } else if (docId && !pageUrl) {
    html += '<div class="hint">The pinned document has not reported an address, so no endpoint can be ' +
            'attributed to it. Only endpoints THIS document learned are probeable from it.</div>';
  }

  for (const [svc, info] of [...services.entries()].sort((a, b) => a[0].localeCompare(b[0]))) {
    const docEntry = tabData.discoveryDocs[svc] || null;   // a service named only by a learned endpoint has no document yet — that ABSENCE is what the panel offers to fetch
    const docState = docEntry ? esc(String(docEntry.status)) : "none";
    html += '<div class="card"><div class="card-label"><code>' + esc(svc) + '</code>' +
            ' <span class="badge badge-status">discovery doc: ' + docState + '</span></div>';
    // FETCH_DISCOVERY — service level. The hostname is one the page actually reached; a service known only by
    // a discovery document carries no endpoint of ours to name a host, and none is invented.
    if (info.hostname) {
      html += '<div class="card-meta"><button class="btn-small btn-disc" data-disc="fetch" data-svc="' +
              esc(svc) + '" data-host="' + esc(info.hostname) + '"' + (docId ? "" : " disabled") + '>' +
              (_discInFlight === "fetch:" + svc ? "fetching…" : "Fetch discovery doc") + '</button>' +
              ' <span class="card-meta">from <code>' + esc(info.hostname) + '</code></span></div>';
    } else {
      html += '<div class="card-meta">no endpoint of this service was learned here, so there is no host of ' +
              'its own to fetch a document from</div>';
    }

    for (const key of info.endpointKeys) {
      const ep = tabData.endpoints[key];
      const isPost = ep.method === "POST";
      html += '<div class="card-meta"><code>' + esc(ep.method + " " + ep.host + ep.path) + '</code>';
      if (isPost) {
        html += ' <button class="btn-small btn-disc" data-disc="probe" data-key="' + esc(key) + '"' +
                (docId ? "" : " disabled") + '>' +
                (_discInFlight === "probe:" + key ? "probing…" : "Probe fields") + '</button>' +
                ' <button class="btn-small btn-disc" data-disc="svcinfo" data-key="' + esc(key) + '"' +
                (docId ? "" : " disabled") + '>' +
                (_discInFlight === "svcinfo:" + key ? "probing…" : "Service info") + '</button>';
      } else {
        // The req2proto probes ARE a POST of a deliberately-malformed body. Offering them on a GET endpoint
        // would introduce a method the page never used against somebody else's server.
        html += ' <span class="badge badge-source">probe is POST-only</span>';
      }
      html += "</div>";
      html += _discHolesHtml(ep, key);
      html += _discResultHtml(key, ep);
    }
    html += _discAutoProbesHtml(svc);
    html += "</div>";
  }

  html += _discChangesHtml();
  el.innerHTML = html;
}

/* WHAT A PROBE LEARNED — one renderer per ANSWER SHAPE, because `tab.probeResults` holds two of them under four
   key spellings and only two of those spellings had a reader. lib/req2proto.js `probeApiEndpoint` answers
   {fieldCount, fields, metadata, scopes} and is stored under the bare endpoint key (lib/discovery-probe.js
   `probeEndpoint`, the button below) AND under `auto:<service>::<url>` (`performProbeAndPatch`);
   `discoverServiceInfo` answers {service, method, scopes, contentTypes} and is stored under
   `svc:<endpointKey>` (the button) AND under `svcinfo:POST <path>`.
   TWO OF THE FOUR SPELLINGS WERE MINTED BY PROBES THAT RAN WITH NO HUMAN WATCHING, AND THAT IS NO LONGER A
   THING THIS EXTENSION DOES. The paragraph here used to say the panel skipping those two was the defect — "a
   probe that fires on every page load and reaches no reader is worse off than one nobody triggers" — and the
   renderers were added. The deeper answer arrived later: lib/schema.js's page-context relay is exempt from the
   credentialed destructive-path deny list because a HUMAN composed what it carries, and a probe firing on
   every page load was that exemption covering an act nobody initiated. So `svcinfo:` has no writer in this
   tree at all (its producer is deleted, and the read below serves records already in a store), and `auto:` is
   now minted only from the operator's FETCH_DISCOVERY. The prefix names WHAT THE KEY IS — a probe keyed by
   service+URL rather than by endpoint key — and never who asked for it; it is not renamed because renaming a
   persisted dispatch key orphans every record already written under it, which is a silent loss of collected
   data traded for a better name. */
/* NEITHER RENDERER ASKS WHAT ITS RECORD IS ANY MORE, AND THE ASYMMETRY BETWEEN THEM IS WHY THAT MATTERS. A
   DCHECK stood in the field-probe renderer for `fieldCount`/`fields` and there was NONE AT ALL in the
   service-info one below it — one reader's opinion of half of one of the two shapes, and silence about the
   other. Both are lib/store-record.js's (`_SR_PROBE_FIELDS`, `_SR_PROBE_SVCINFO`), dispatched on the same key
   spelling this file dispatches its renderers on, and lib/serialize.js asks that question of every entry as
   it projects `probeResults` onto the GET_STATE reply — so what arrives here is the shape the store states,
   for both renderers, without either of them having to say what it believes.
   THE TRUTHINESS READS BELOW ARE NOT DEFAULTS AND MUST NOT BECOME ASSERTS. `metadata` and `scopes` on a field
   probe, and `service`/`method`/`scopes` on a service-info answer, are `null` when no rejection named one —
   lib/req2proto.js initialises each to `null` and assigns only on a named one, and the shape declares them as
   stated absences. Reading that `null` as "the envelope named none" is reading the producer's statement; the
   service-info renderer prints exactly that sentence when every one of them is absent. */
/* WHAT TO CALL A PROBED FIELD — the NAME the server's own rejection spelled, else the WIRE NUMBER it stated,
   else a statement that it named neither. `f.name || "#" + f.number` read those three as two, and the third
   is reachable from bytes a server chose: lib/req2proto.js's Google arm composes `name` out of the last
   segment of `violation.field`, so an envelope whose field path ends in a dot (`"a."`) splits to `""` — a
   name that is PRESENT and EMPTY, which is not the same fact as a name the envelope never spelled. Under the
   `||` that empty name fell through to `"#" + f.number`, and `number` is `null` whenever the rejection stated
   no wire tag (lib/req2proto.js's `_statedFieldNumber` yields a positive integer or nothing), so the panel
   rendered the literal label `#null:string` — a wire tag no document ever named, in a list a reviewer reads
   as the server's own field inventory.
   THE NAME IS NEVER ABSENT AND THAT IS WHY THIS IS A REFUSAL AND NOT A DCHECK: both of lib/req2proto.js's
   mints state `name` on every push, so the field is always there — what varies is whether the DOCUMENT said
   anything in it, and asserting on a server's empty string would be the trusted zone aborting on bytes a
   target chose. So each of the three is said as itself.
   THE SENTENCE THAT USED TO CARRY THAT ARGUMENT SAID "the merge keys on `#number` or on the name itself",
   AND IT IS RETIRED RATHER THAN DELETED, because a reader who re-derives it will re-introduce the defect it
   described: `probeApiEndpoint` keys on the field's PATH now, which is `parentPath` plus the cleaned name, so
   two fields of two different messages that happen to share a wire tag no longer collapse onto one record.
   The key is therefore never a NAME either, which is why lib/send.js's `_probeFieldsToDefs` reads the values
   and never the keys. */
function _discProbeFieldLabel(f) {
  DCHECK(typeof f.name === "string",
         "a probed field carries no `name` string — lib/req2proto.js's two mints both state it on every " +
         "field they push and lib/popup-handlers.js keys the merge on it, so an absent one is that producer " +
         "gone silent and this list would label the field with whatever the fallback reached for");
  if (f.name !== "") return f.name;
  if (f.number !== null && f.number !== undefined) return "#" + f.number;
  return "(the rejection named neither a name nor a number)";
}

function _discFieldProbeHtml(label, probe) {
  const names = Object.values(probe.fields).map((f) => _discProbeFieldLabel(f) + ":" + f.type);
  let html = '<div class="card-meta">' + esc(label) + ": <strong>" + esc(String(probe.fieldCount)) +
             "</strong> field(s)" + (names.length ? " — <code>" + esc(names.join(", ")) + "</code>" : "") + "</div>";
  if (probe.metadata && (probe.metadata.service || probe.metadata.method)) {
    html += '<div class="card-meta">canonical: <code>' +
            esc(String(probe.metadata.service || "?") + "." + String(probe.metadata.method || "?")) + "</code></div>";
  }
  if (probe.scopes && probe.scopes.length) {
    html += '<div class="card-meta">scopes: <code>' + esc(probe.scopes.join(" ")) + "</code></div>";
  }
  return html;
}

function _discSvcInfoHtml(label, svcInfo) {
  const bits = [];
  if (svcInfo.service) bits.push("service " + svcInfo.service);
  if (svcInfo.method) bits.push("method " + svcInfo.method);
  if (svcInfo.scopes && svcInfo.scopes.length) bits.push("scopes " + svcInfo.scopes.join(" "));
  /* NO `svcInfo.contentTypes &&`. `scopes` above keeps its null test because `null` is that field's STATED
     absence; `contentTypes` is a STATED NAME — `discoverServiceInfo` builds the list unconditionally and
     states what it tried — so the guard could not fire, and what it stood in front of was the one line that
     says which content types the endpoint accepted at all. An empty list is still a real answer (nothing the
     probe sent was accepted) and reads as such below. */
  if (svcInfo.contentTypes.length) bits.push("accepts " + svcInfo.contentTypes.join(", "));
  return '<div class="card-meta">' + esc(label) + ": " +
         (bits.length ? "<code>" + esc(bits.join(" | ")) + "</code>"
                      : "the endpoint answered, and its error envelope named no service, method or scope") +
         "</div>";
}

/* WHAT THE RUN LEARNED TO PUT IN THIS ADDRESS'S TEMPLATED SEGMENTS — BOTH POOLS, ON TWO ROWS, IN THE ONE
   SURFACE THAT LISTS ADDRESSES.

   THE ENDPOINT RECORD HAS CARRIED THIS AND NO LIST HAS EVER SHOWN IT. `pathParams` and `pathParamsForced` had
   exactly two readers — lib/merge.js's moat fold and lib/send.js's Send-panel schema — so a hole this run
   could only fill by FORCING a gate was stated on the record, carried across documents and sessions, and
   reached a human only if that human opened the Send panel for that one method. The row here printed the
   address and nothing else, which made a run whose whole logged-in surface was forced look exactly like a run
   that learned nothing to put in it. CLAUDE.md §What-the-tool-produces makes that the headline rather than a
   nicety: the surface the bundle ships to a logged-out visitor and never fires is what forcing a gate reaches,
   so the endpoints whose segments were filled ONLY on a forced arm are disproportionately the ones this tool
   exists to find, and the list was reporting them as the ones it learned least about.

   TWO ROWS, NOT ONE LIST, AND THE SPLIT IS THE SAME ONE lib/popup-form.js DRAWS ONE RECORD DOWN.
   lib/endpoint-record.js's `provenanceOffersExample` states the line: a value every sighting of which stood on
   a forced arm may never be OFFERED as an example of what this app computes, and the way that is made
   impossible rather than discouraged is that the two pools are separate lists with separate readers. A single
   row of segment values here would be one bag a reviewer copies out of, which is §@H's fabrication with a
   server behind it — plausible, and rejected only after it is sent. So the forced row says what it is, in the
   same words and the same colour the field panel uses (`.ast-forced-label`), and it says it BESIDE the values
   rather than instead of them: §@H's shape states PROVENANCE and DOMAIN, and a row carrying one of the two is
   a wrong report and not a thin one.

   NOTHING IS PREFILLED AND NOTHING IS CLICKABLE. `.ast-value-chip` carries a cursor and a click handler in the
   Send form because a value there lands in an input; there is no input on this panel, so these are plain
   `<code>` and the reader is told what was learned without being handed it.

   A NAME IN BOTH ROWS IS CORRECT AND IS THE RECORD SPEAKING. lib/endpoint-record.js: one `{orgId}` can have a
   real example from one path and a forced one from another; what may NOT repeat is a VALUE, which
   `checkEndpointRecord` asserts on every record `_discServices` passed before this runs.

   NAMED RESIDUAL — an address whose templated segments were ALL unfilled renders exactly like an address that
   has none, because this function speaks only from the record's two lists and neither of them mentions a hole
   nothing filled.
     WHAT IS NOT COVERED: "this address has a segment and the run learned nothing for it" is a fact a reader
       gets only by looking at the braces in the address printed above, never as a sentence.
     WHAT THE NEXT DIFF BUILDS: the hole GRAMMAR minted once — what `{name}` means inside a learned path — so
       the segments of the address can be enumerated and each one stated as filled, forced, or empty. It is
       not done here because that grammar already has a spelling (lib/popup-form.js's `applyPathParams`
       substitution regex) and a second copy of it is exactly the defect lib/endpoint-record.js's key section
       records happening twice in one file; minting it means that substitution consuming the same walk, which
       is a change to what the Send panel actually sends.
     HOW ITS ABSENCE SHOWS: an endpoint carrying `{orgId}` with both pools empty prints one bare address line,
       the same line a hole-free address prints. */
function _discHolesHtml(ep, endpointKey) {
  const holes = endpointHolePairs(ep, "the discovery panel's endpoint row, key " + JSON.stringify(endpointKey));
  if (!holes.size) return "";   // both lists stated their absence: no segment was filled at either grade
  const offered = [], forced = [];
  for (const [name, pools] of holes) {
    /* A NAME WITH TWO EMPTY POOLS WOULD PRINT A SEGMENT AS LEARNED WITH NOTHING BEHIND IT. `endpointHolePairs`
       mints an entry only from a list entry it has already asserted carries at least one value, so an empty
       pair is that walk having changed under this row — and what it renders is the claim this panel is here
       to stop making: a templated segment reported as filled by a run that filled nothing. */
    DCHECK(pools.valid.length > 0 || pools.forced.length > 0,
           "a templated segment reached the discovery panel with both pools empty (`" + name + "`, key " +
           JSON.stringify(endpointKey) + ") — `endpointHolePairs` builds a pair only out of a {name, values} " +
           "entry it asserted non-empty, so this is that walk broken and the row would report a segment as " +
           "learned while naming no value for it");
    if (pools.valid.length) offered.push(_discHoleValuesHtml(name, pools.valid));
    if (pools.forced.length) forced.push(_discHoleValuesHtml(name, pools.forced));
  }
  let html = "";
  if (offered.length) {
    /* THE LABEL NAMES THE GRADE AND NOT MERELY THE FACT THAT SOMETHING WAS LEARNED. Both rows are things the
       run learned, so "learned" on one of them and "forced" on the other reads as a scale of confidence
       rather than as the two different claims they are — this row says a real path of the app's own code
       computed the value, which is what `provenanceOffersExample` admits and the forced row denies. */
    html += '<div class="card-meta"><span class="ast-values-label" title="values this run computed for the ' +
            'templated segments of this address on a path that forced nothing — what the app itself puts ' +
            'here">path segments the app&#39;s own code computes:</span> ' + offered.join(" &middot; ") +
            "</div>";
  }
  if (forced.length) {
    html += '<div class="card-meta"><span class="ast-forced-label" title="every sighting of these values ' +
            'stood on an arm the forced execution took, so a real client is not known to compose this ' +
            'address — a reply to it is evidence about a request no client makes">path segments reached ' +
            'only by FORCING a gate (a request no client makes):</span> ' + forced.join(" &middot; ") +
            "</div>";
  }
  return html;
}

/* ONE SEGMENT'S NAME AND THE VALUES OF ONE POOL. The name is printed as the record spells it, with no braces
   added around it: writing `{name}` would be this file asserting how a hole appears inside a path, which is
   the grammar the residual above declines to copy. The address in the row above carries the braces. */
function _discHoleValuesHtml(name, values) {
  return "<code>" + esc(name) + "</code> = " +
         values.map((v) => "<code>" + esc(String(v)) + "</code>").join(", ");
}

// Per ENDPOINT: the two on-demand answers, plus `svcinfo:POST <path>` records this extension NO LONGER MINTS.
//
// THAT THIRD READ IS NOT A READER WITH NO WRITER, AND THE DISTINCTION IS THE STORE. lib/response-decode.js's
// automatic service-info probe — which fired a malformed credentialed POST the instant a response body arrived,
// with nobody at any surface — is deleted, so nothing in THIS TREE writes that spelling. Records under it are
// still real: `probeResults` is persisted to IndexedDB and lib/persistence.js restores it through
// lib/store-record.js's shape table, which still recognises the prefix for exactly this reason. So the producer
// is a store written by an earlier build, and dropping the read here would hide data the panel already holds
// while dropping the prefix from the shape table would abort the RESTORE on any profile that has one.
// It keys by the request PATH alone, so a TEMPLATED endpoint path (`/users/{id}`, what the engine learned)
// never equals the concrete pathname of the live request that triggered the probe; those stayed unattributed
// then and stay unattributed now. The live capability is the "Service info" button, keyed `svc:<endpointKey>`.
function _discResultHtml(endpointKey, ep) {
  /* `(tabData && tabData.probeResults) || {}` stood here twice. Both callers run inside the per-service loop,
     which `_discServices` can only fill from a non-null `tabData`, and `serializeTabData` writes
     `probeResults` on every reply — so the guard could not fire and the default could only hide the map that
     holds every probe's answer arriving as nothing, which renders as a probe that ran and learned no fields. */
  DCHECK(tabData && tabData.probeResults && typeof tabData.probeResults === "object",
         "the popup's state reply carries no probeResults map — lib/serialize.js writes it on every " +
         "GET_STATE answer, so an absent one is that projection broken and every probe this document ran " +
         "would render as one that learned nothing");
  const results = tabData.probeResults;
  let html = "";
  const probe = results[endpointKey];
  if (probe) html += _discFieldProbeHtml("probe", probe);
  const svcInfo = results["svc:" + endpointKey];
  if (svcInfo) html += _discSvcInfoHtml("service info", svcInfo);
  const autoSvcInfo = results["svcinfo:POST " + ep.path];
  if (autoSvcInfo) html += _discSvcInfoHtml("service info (automatic, from an earlier build)", autoSvcInfo);
  return html;
}

// Per SERVICE: the field probes keyed `auto:<service>::<url>` by lib/discovery-probe.js's
// `performProbeAndPatch` — reached from the operator's FETCH_DISCOVERY, never automatically (the prefix names
// the KEY SHAPE, service+URL rather than endpoint key, and is a persisted dispatch spelling; see above).
function _discAutoProbesHtml(svc) {
  /* `(tabData && tabData.probeResults) || {}` stood here twice. Both callers run inside the per-service loop,
     which `_discServices` can only fill from a non-null `tabData`, and `serializeTabData` writes
     `probeResults` on every reply — so the guard could not fire and the default could only hide the map that
     holds every probe's answer arriving as nothing, which renders as a probe that ran and learned no fields. */
  DCHECK(tabData && tabData.probeResults && typeof tabData.probeResults === "object",
         "the popup's state reply carries no probeResults map — lib/serialize.js writes it on every " +
         "GET_STATE answer, so an absent one is that projection broken and every probe this document ran " +
         "would render as one that learned nothing");
  const results = tabData.probeResults;
  const prefix = "auto:" + svc + "::";
  let html = "";
  for (const k of Object.keys(results).sort()) {
    if (!k.startsWith(prefix)) continue;
    html += '<div class="card-meta">error probe of <code>' + esc(k.slice(prefix.length)) + "</code></div>" +
            _discFieldProbeHtml("learned", results[k]);
  }
  return html;
}

// API DRIFT — the diff history that was recorded forever and shown to no one.
function _discChangesHtml() {
  if (discoveryChanges === null) return "";   // not answered yet — not "nothing changed"
  const svcs = Object.keys(discoveryChanges).sort();
  if (!svcs.length) {
    return '<div class="section-header">API drift</div><div class="hint">No published API surface has changed ' +
           'between two fetches yet. This is the whole history, never a window — the first change a service ' +
           'ever made is still here.</div>';
  }
  let html = '<div class="section-header">API drift</div>';
  for (const svc of svcs) {
    /* THREE READS, NO OPINION. Two DCHECKs stood here — one that the per-service value is an array, one that
       a record carries a `timestamp`/`changes` pair — and a `rec.fetchUrl || ""` beside them, which is the
       shape of the whole defect: the same file both asserting part of a contract and defaulting past another
       part of it, each in its own words. All three names are stated by lib/store-record.js's
       `_SR_DRIFT_HISTORY` / `_SR_DISCOVERY_CHANGE`, and lib/popup-handlers.js now asks that question of every
       service's history before GET_DISCOVERY_CHANGES answers — so `fetchUrl` is a non-empty string here, and
       printing it raw is reading the producer's statement rather than supplying one for it. The `|| ""` could
       only ever have rendered a drift record as a change to an address nobody can name. */
    const recs = discoveryChanges[svc];
    html += '<details class="card"><summary><code>' + esc(svc) + "</code> — " + esc(String(recs.length)) +
            " change record(s)</summary>";
    for (const rec of recs) {
      html += '<div class="card-meta">' + esc(new Date(rec.timestamp).toLocaleString()) +
              " — <code>" + esc(rec.fetchUrl) + "</code></div>";
      for (const c of rec.changes) html += '<div class="card-meta"><code>' + esc(_discChangeLine(c)) + "</code></div>";
    }
    html += "</details>";
  }
  return html;
}

/* THE ONE CLICK PATH. Each action posts the command the offscreen already handles, then re-reads state: the
   handlers merge into the store and notifyPopup, so the panel's own re-render shows what was learned. A null
   answer is the offscreen REFUSING (no such document / no such endpoint) and is recorded and shown. */
async function _discAction(kind, ds) {
  const documentId = currentDocumentId();
  DCHECK(!!documentId,
         "a discovery action fired with no live pinned document — every button is rendered disabled without " +
         "one, so reaching here is the render and the gate disagreeing");
  _discRefused = null;
  let result;
  if (kind === "fetch") {
    _discInFlight = "fetch:" + ds.svc;
    renderDiscoveryPanel();
    result = await chrome.runtime.sendMessage({
      type: "FETCH_DISCOVERY", tabId: currentTabId, documentId,
      service: ds.svc, hostname: ds.host,
    });
  } else if (kind === "probe") {
    _discInFlight = "probe:" + ds.key;
    renderDiscoveryPanel();
    result = await chrome.runtime.sendMessage({
      type: "PROBE_ENDPOINT", tabId: currentTabId, documentId, endpointKey: ds.key,
    });
  } else if (kind === "svcinfo") {
    _discInFlight = "svcinfo:" + ds.key;
    renderDiscoveryPanel();
    result = await chrome.runtime.sendMessage({
      type: "DISCOVER_SERVICE", tabId: currentTabId, documentId, endpointKey: ds.key,
    });
  } else {
    DFAIL("a discovery button carries an action this panel does not perform (" + JSON.stringify(kind) + ")");
    return;
  }
  _discInFlight = null;
  if (!result) _discRefused = _discActionLabel(kind, ds);
  await loadDiscoveryChanges();   // a fetch that changed the document just recorded a drift entry
  await loadState();              // re-reads tabData (probeResults, discoveryDocs) and re-renders
}

function _discActionLabel(kind, ds) {
  return kind === "fetch" ? "fetch discovery for " + ds.svc
       : kind === "probe" ? "probe " + ds.key
       : "service info for " + ds.key;
}

function initDiscoveryPanel() {
  const el = document.getElementById("discovery-body");
  // DCHECK, not CHECK: this asserts our own markup, not a production invariant — and the next line throws on
  // a null element in release regardless, which is the same loud failure by another route.
  DCHECK(!!el, "the discovery panel has no body element — popup.html declares #discovery-body beside the " +
               "panel this file renders into, so its absence means the three active-discovery probes have no " +
               "way to be triggered at all");
  el.addEventListener("click", (e) => {
    const btn = e.target.closest(".btn-disc");
    if (!btn) return;
    _discAction(btn.dataset.disc, btn.dataset);
  });
}
