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
  if (!docId) return null;
  const f = availableFrames.find((x) => x.documentId === docId);
  return (f && f.url) || null;
}
function _discServices(pageUrl) {
  const out = new Map();   // service -> { hostname, endpointKeys: [] }
  if (!tabData) return out;
  for (const [k, ep] of Object.entries(tabData.endpoints || {})) {
    DCHECK(typeof ep.service === "string" && typeof ep.method === "string" && typeof ep.host === "string",
           "an endpoint record reached the discovery panel without service/method/host — lib/merge.js is the " +
           "extension's only endpoints.set and writes all three on every record, so one missing them is that " +
           "producer broken and this panel would probe an address it assembled itself");
    if (!pageUrl || ep.pageUrl !== pageUrl) continue;   // another document learned it — not ours to probe
    let e = out.get(ep.service);
    if (!e) { e = { hostname: ep.host, endpointKeys: [] }; out.set(ep.service, e); }
    e.endpointKeys.push(k);
  }
  for (const svc of Object.keys(tabData.discoveryDocs || {})) {
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
    case "param_required":      return "! param " + c.methodId + "." + c.param + " is now required";
    case "schema_added":        return "+ schema " + c.schema;
    case "schema_removed":      return "- schema " + c.schema;
  }
  /* A CHANGE KIND WITH NO SENTENCE IS DRIFT BETWEEN THIS VIEW AND ITS PRODUCER, and the producer is one
     function (lib/discovery-probe.js `_diffDiscoveryDocs`) emitting exactly the eight above. */
  DFAIL("a discovery-change record carries a type this panel has no sentence for (" + JSON.stringify(c.type) +
        ") — _diffDiscoveryDocs emits eight kinds and each one is spelled out here, so a ninth is a producer " +
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
    const docEntry = (tabData.discoveryDocs || {})[svc] || null;
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
      html += _discResultHtml(key);
    }
    html += "</div>";
  }

  html += _discChangesHtml();
  el.innerHTML = html;
}

// What a probe LEARNED, from the store it was merged into. Nothing rendered these before, so a probe's whole
// answer — the field map, the canonical service/method, the required scopes — reached no reader.
function _discResultHtml(endpointKey) {
  const results = (tabData && tabData.probeResults) || {};
  let html = "";
  const probe = results[endpointKey];
  if (probe) {
    DCHECK(typeof probe.fieldCount === "number" && probe.fields && typeof probe.fields === "object",
           "a req2proto probe result carries no field map — probeApiEndpoint returns {url, timestamp, " +
           "fieldCount, fields, metadata, scopes, probeDetails} on every path, so one without them is that " +
           "producer broken and the panel would report a probe that found nothing");
    const names = Object.values(probe.fields).map((f) => (f.name || "#" + f.number) + ":" + f.type);
    html += '<div class="card-meta">probe: <strong>' + esc(String(probe.fieldCount)) + '</strong> field(s)' +
            (names.length ? " — <code>" + esc(names.join(", ")) + "</code>" : "") + "</div>";
    if (probe.metadata && (probe.metadata.service || probe.metadata.method)) {
      html += '<div class="card-meta">canonical: <code>' +
              esc(String(probe.metadata.service || "?") + "." + String(probe.metadata.method || "?")) + "</code></div>";
    }
    if (probe.scopes && probe.scopes.length) {
      html += '<div class="card-meta">scopes: <code>' + esc(probe.scopes.join(" ")) + "</code></div>";
    }
  }
  const svcInfo = results["svc:" + endpointKey];
  if (svcInfo) {
    const bits = [];
    if (svcInfo.service) bits.push("service " + svcInfo.service);
    if (svcInfo.method) bits.push("method " + svcInfo.method);
    if (svcInfo.scopes && svcInfo.scopes.length) bits.push("scopes " + svcInfo.scopes.join(" "));
    if (svcInfo.contentTypes && svcInfo.contentTypes.length) bits.push("accepts " + svcInfo.contentTypes.join(", "));
    html += '<div class="card-meta">service info: ' +
            (bits.length ? "<code>" + esc(bits.join(" | ")) + "</code>"
                         : "the endpoint answered, and its error envelope named no service, method or scope") +
            "</div>";
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
