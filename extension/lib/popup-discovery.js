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
   `probeEndpoint`, the button below) AND under `auto:<service>::<url>` (`performProbeAndPatch`, the AUTOMATIC
   probe); `discoverServiceInfo` answers {service, method, scopes, contentTypes} and is stored under
   `svc:<endpointKey>` (the button) AND under `svcinfo:POST <path>` (lib/response-decode.js's automatic probe).
   The two the panel skipped are exactly the ones that run with no human watching, which is the state this file
   exists to end — a probe that fires on every page load and reaches no reader is worse off than one nobody
   triggers. */
function _discFieldProbeHtml(label, probe) {
  DCHECK(typeof probe.fieldCount === "number" && probe.fields && typeof probe.fields === "object",
         "a req2proto probe result carries no field map — probeApiEndpoint returns {url, timestamp, " +
         "fieldCount, fields, metadata, scopes, probeDetails} on every path, so one without them is that " +
         "producer broken and the panel would report a probe that found nothing");
  const names = Object.values(probe.fields).map((f) => (f.name || "#" + f.number) + ":" + f.type);
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
  if (svcInfo.contentTypes && svcInfo.contentTypes.length) bits.push("accepts " + svcInfo.contentTypes.join(", "));
  return '<div class="card-meta">' + esc(label) + ": " +
         (bits.length ? "<code>" + esc(bits.join(" | ")) + "</code>"
                      : "the endpoint answered, and its error envelope named no service, method or scope") +
         "</div>";
}

// Per ENDPOINT: the two on-demand answers, plus the automatic service-info probe, which lib/response-decode.js
// keys by the request PATH alone — so the endpoint's own path is what attributes it, and a TEMPLATED endpoint
// path (`/users/{id}`, what the engine learned) never equals the concrete pathname of the live request that
// triggered the probe. Those stay unattributed until that key carries the endpoint it was probed for.
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
  if (autoSvcInfo) html += _discSvcInfoHtml("service info (automatic)", autoSvcInfo);
  return html;
}

// Per SERVICE: the automatic field probes, keyed `auto:<service>::<url>` by lib/discovery-probe.js.
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
    html += '<div class="card-meta">automatic probe of <code>' + esc(k.slice(prefix.length)) + "</code></div>" +
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
    const recs = discoveryChanges[svc];
    DCHECK(Array.isArray(recs),
           "a discoveryChanges entry is not an array — lib/discovery-probe.js pushes {timestamp, fetchUrl, " +
           "changes} records onto one per service, so anything else is that writer or the IDB rehydrate broken");
    html += '<details class="card"><summary><code>' + esc(svc) + "</code> — " + esc(String(recs.length)) +
            " change record(s)</summary>";
    for (const rec of recs) {
      DCHECK(typeof rec.timestamp === "number" && Array.isArray(rec.changes),
             "a discovery-change record carries no timestamp/changes pair — _diffDiscoveryDocs returns a " +
             "non-empty change list and the writer stamps the time it diffed, so a record without both is " +
             "one nobody can date or read");
      html += '<div class="card-meta">' + esc(new Date(rec.timestamp).toLocaleString()) +
              " — <code>" + esc(String(rec.fetchUrl || "")) + "</code></div>";
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
