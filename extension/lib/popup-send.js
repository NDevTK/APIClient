/* Popup send-panel controls — extracted from popup.js (classic script, shares the popup global scope +
   DOM). renderSendPanel + its selectors: service grouping, method dropdown, frame selector, service-origin
   hint, API-key selector (+ onKeySelectionChange/truncateKey). */
// ─── Send Panel ──────────────────────────────────────────────────────────────

function renderSendPanel() {
  // Fingerprint: skip rebuild if discovery docs haven't changed. Must include
  // PER-DOC method count — when the AST analysis learns new methods on an
  // existing service (e.g. github bundle drops more @H records on the same
  // `github.com` service), docKeys.length stays the same but the dropdown
  // is stale. Without the method-count sum, `renderMethodDropdown()` only
  // fires on a new SERVICE, not new METHODS, so users see "-- select method --"
  // after a fresh nav until manually re-rendering.
  const docKeys = tabData?.discoveryDocs ? Object.keys(tabData.discoveryDocs) : [];
  let methodSum = 0;
  if (tabData?.discoveryDocs) {
    for (const k of docKeys) {
      const svc = tabData.discoveryDocs[k];
      if (svc && svc.doc) methodSum += getDocMethods(svc.doc).length;
    }
  }
  const sendFp = docKeys.length + "/" + methodSum + ":" + (tabData?.requestLog?.length || 0);
  if (sendFp === _lastSendFp) return;
  _lastSendFp = sendFp;

  // Populate service selector
  const svcSelect = document.getElementById("spec-service-select");
  const prevSvc = svcSelect.value;
  svcSelect.innerHTML = '<option value="">All Services</option>';
  if (tabData?.discoveryDocs) {
    for (const [svcName, svcData] of Object.entries(tabData.discoveryDocs).sort((a, b) => a[0].localeCompare(b[0]))) {
      if (svcData.status === "found" && svcData.doc) {
        const methodCount = getDocMethods(svcData.doc).length;
        const opt = document.createElement("option");
        opt.value = svcName;
        opt.textContent = `${svcName} (${methodCount})`;
        svcSelect.appendChild(opt);
      }
    }
  }
  if (prevSvc) svcSelect.value = prevSvc;
  _renderServiceGrouping();

  renderMethodDropdown();
}

// Show which URL-structure rule produced the currently-selected
// service name. Service grouping is heuristic (URL-parsing — no
// server-side fact tells us "this is service X"), so every decision
// must be traceable to the rule that produced it; a reviewer reads
// this row and judges whether the grouping is right for the site.
function _renderServiceGrouping() {
  const el = document.getElementById("spec-service-grouping");
  if (!el) return;
  const svcSelect = document.getElementById("spec-service-select");
  const name = svcSelect.value;
  if (!name || !tabData?.discoveryDocs) { el.textContent = ""; el.className = "service-grouping"; return; }
  const svcData = tabData.discoveryDocs[name];
  if (!svcData) { el.textContent = ""; el.className = "service-grouping"; return; }
  el.className = "service-grouping";

  // Grouping rule line.
  let html = "";
  const g = svcData.grouping;
  if (g) {
    html += '<span class="grouping-label">grouping rule:</span> <code>' + esc(g.rule) + '</code>'
      + (g.matched ? ' <span class="grouping-matched">matched: <code>' + esc(g.matched) + '</code></span>' : '')
      + (g.firstUrl ? '<div class="grouping-first">first request: <code>' + esc(g.firstUrl) + '</code></div>' : '');
  }

  // Bucket quality: count methods by origin. THE UNUSED BUCKET IS THE PRODUCT — "what the bundle CAN do but
  // didn't" — so it is named that here and in the dropdown tag, from the ONE classifier below, and it is
  // listed first.
  if (svcData.doc) {
    const n = { unused: 0, "unused+fired": 0, fired: 0, declared: 0, asset: 0 };
    let total = 0;
    for (const bucket of Object.values(svcData.doc.resources || {})) {
      for (const m of Object.values(bucket.methods || {})) {
        total++;
        n[_methodOrigin(m)]++;
      }
    }
    if (total > 0) {
      const parts = [];
      if (n.unused) parts.push(n.unused + " UNUSED (in bundle, never fired)");
      if (n["unused+fired"]) parts.push(n["unused+fired"] + " in bundle + fired");
      if (n.fired) parts.push(n.fired + " fired only (no bundle origin)");
      if (n.declared) parts.push(n.declared + " declared by a discovery doc, never fired");
      if (n.asset) parts.push(n.asset + " asset");
      html += '<div class="grouping-first">methods: ' + esc(parts.join(", ")) + '  (' + total + ' total)</div>';
    }
  }

  el.innerHTML = html;
}

// WHICH BUCKET A METHOD IS IN — asked in ONE place. The grouping summary and the dropdown tag each derived
// this from `_astInferred` / `_stats.requestCount` on their own, which is one fact answered from two places
// for the SAME method, and the two spellings ("AST" / "[ast]") did not even agree on what to call it.
//
// "unused" is the whole claim of the tool: learned from the bundle, never observed on the wire — the
// login/click/route/flag-gated, dead-but-shipped, lazy-chunk endpoint a sniffer cannot see. It is the word
// used, not "ast", because "ast" names the mechanism and hides the finding.
//
// ENGINE/STORE GAP: this class is a FACT about the record and should be stamped ON it where the record is
// built (the offscreen store, from the engine's @H surface plus the request log), not re-derived by the view
// every render. The view then paints a field instead of classifying.
function _methodOrigin(m) {
  if (m._responseKind === "asset") return "asset";
  const inBundle = !!m._astInferred;
  const fired = !!(m._stats && m._stats.requestCount);
  if (inBundle && fired) return "unused+fired";
  if (inBundle) return "unused";
  // Neither: a method that came from a PROBED discovery document, so it is declared by the service and never
  // observed here. Declared-not-fired is not the same claim as in-the-bundle-not-fired, and collapsing the two
  // would credit the solver with a surface it did not learn.
  return fired ? "fired" : "declared";
}

function renderMethodDropdown() {
  const svcFilter = document.getElementById("spec-service-select").value;
  const select = document.getElementById("send-ep-select");
  const prev = select.value;

  select.innerHTML = '<option value="">-- select method --</option>';

  if (tabData?.discoveryDocs) {
    const services = Object.entries(tabData.discoveryDocs).sort((a, b) =>
      a[0].localeCompare(b[0]),
    );

    for (const [svcName, svcData] of services) {
      if (svcFilter && svcName !== svcFilter) continue;
      if (svcData.status === "found" && svcData.doc) {
        const methods = getDocMethods(svcData.doc);
        methods.sort((a, b) => a.id.localeCompare(b.id));

        if (methods.length > 0) {
          const group = document.createElement("optgroup");
          group.label = svcData.doc.title || svcName;

          for (const m of methods) {
            const opt = document.createElement("option");
            const key = `DISCOVERY ${m.httpMethod} ${svcName} ${m.id}`;
            opt.value = key;
            // The tag IS the product's differentiator, so it says the finding, not the mechanism:
            //   [UNUSED]     — learned from the bundle, never observed on the wire. The headline: what the
            //                  bundle CAN do but didn't (login/click/route/flag-gated, dead-but-shipped,
            //                  lazy-chunk). A sniffer cannot produce this row.
            //   [in bundle + fired]      — learned AND observed; a sniffer sees it too.
            //   [fired only]             — observed in traffic with no bundle origin.
            //   [declared, never fired]  — from a probed discovery document, not from this bundle.
            //   [asset:X]                — response magic-bytes classified as static (usually noise).
            // Class from _methodOrigin — the same one the service summary counts, never a second derivation.
            const origin = _methodOrigin(m);
            let tag = "";
            if (origin === "asset") {
              tag = " [asset" + (m._responseLabel ? ":" + String(m._responseLabel).split(";")[0].trim() : "") + "]";
            } else if (origin === "unused") tag = " [UNUSED]";
            else if (origin === "unused+fired") tag = " [in bundle + fired]";
            else if (origin === "fired") tag = " [fired only]";
            else tag = " [declared, never fired]";
            /* THE METHOD ID AS THE STORE HOLDS IT. A rewrite stood here that substituted a declared name
               recovered from the page's source map into each `{hole}` of the displayed id (`{e}` → `{owner}`)
               — reading `p._sourceMapName` and `p.location === "path"`. Neither name has a producer: nothing
               in engine/host has ever emitted a source-map name, and lib/learn.js writes every @H param with
               `location: "query"` because endpoint.c mints params only out of the query string. So the loop
               could not fire on any parameter of any method, and it read as a working feature.
               If the rename is wanted it is the ENGINE's to emit beside the param it renames — a view cannot
               recover a declared name from a minified one. */
            opt.textContent = `[${m.httpMethod}] ${m.id}${tag}`;
            opt.dataset.method = m.httpMethod;
            opt.dataset.isVirtual = "true";
            opt.dataset.svc = svcName;
            opt.dataset.path = m.path;
            opt.dataset.discoveryId = m.id;
            group.appendChild(opt);
          }
          select.appendChild(group);
        }
      }
    }
  }

  if (prev) select.value = prev;
}

// ─── Frame Selector ──────────────────────────────────────────────────────────

function renderFrameSelector() {
  const row = document.getElementById("send-frame-row");
  const sel = document.getElementById("send-frame-select");
  const originEl = document.getElementById("send-principal-origin");
  if (!row || !sel) return;

  // No frames reported yet (no content script) \u2014 hide the whole context.
  if (availableFrames.length === 0) {
    row.classList.add("hidden");
    renderServiceOriginHint();
    _refreshSendEnabled();
    return;
  }

  row.classList.remove("hidden");

  // The picker only matters with >1 frame; single-frame pages just show the
  // principal origin below (no dropdown).
  if (availableFrames.length <= 1) {
    sel.classList.add("hidden");
  } else {
    sel.classList.remove("hidden");
    sel.innerHTML = "";
    for (var i = 0; i < availableFrames.length; i++) {
      var f = availableFrames[i];
      var opt = document.createElement("option");
      opt.value = f.frameId;
      // Authoritative origin (documentId\u2192origin) is the label; the url host is
      // only a fallback when the origin is opaque/unknown.
      var shown = _origOf(f);
      if (!shown) { try { shown = new URL(f.url).host; } catch (_) { shown = f.origin || "opaque"; } }
      opt.textContent = (f.isMain ? "Top frame" : "iframe (" + f.frameId + ")") + " \u2014 " + shown;
      sel.appendChild(opt);
    }
    // Restore the selection to the PINNED document's frame (not a reused frameId);
    // fall back to the last selected frame while a replacement is settling.
    var pf = availableFrames.find(function (f) { return f.documentId === _pinnedDocId; });
    sel.value = String(pf ? pf.frameId : currentFrameId);
    currentFrameId = parseInt(sel.value, 10) || 0;
  }

  // Principal-origin / state line. The Send button itself is governed by
  // _refreshSendEnabled (readiness), never here.
  if (originEl) {
    if (_pinStale) {
      // Confirmed cross-origin swap \u2014 warn + offer a one-click re-target.
      originEl.classList.add("opaque");
      originEl.textContent = "\u26a0 Document changed \u2192 " + (_staleOrigin || "opaque origin") + " \u2014 click to re-target";
      originEl.title = "The pinned document navigated to a different origin ("
        + (_pinnedOrigin || "?") + " \u2192 " + (_staleOrigin || "opaque")
        + "). Send is gated off; click to pin the new document and send to it.";
      originEl.style.cursor = "pointer";
      originEl.onclick = function () { _repinTo(currentFrameId); loadState(); };
    } else if (!currentDocumentId()) {
      // Pinned doc gone, replacement not ready yet (origin not reported).
      originEl.classList.add("opaque");
      originEl.textContent = "Credentials: waiting for document\u2026";
      originEl.title = "The document changed; Send stays off until the new document reports.";
      originEl.style.cursor = "";
      originEl.onclick = null;
    } else {
      var po = currentPrincipalOrigin();
      originEl.classList.toggle("opaque", !po);
      originEl.textContent = po ? ("Credentials: " + po) : "Credentials: opaque / unknown origin";
      originEl.title = po
        ? "A page-context send uses " + po + "'s cookies (authoritative documentId\u2192origin mapping)."
        : "No resolvable origin (sandboxed / about:blank); page-context sends fail closed.";
      originEl.style.cursor = "";
      originEl.onclick = null;
    }
  }

  renderServiceOriginHint();
  _refreshSendEnabled();
}

function renderServiceOriginHint() {
  var hint = document.getElementById("send-service-origin-hint");
  if (!hint) return;
  hint.classList.add("hidden");
  hint.innerHTML = "";

  // Get selected service
  var epSelect = document.getElementById("send-ep-select");
  var selectedOpt = epSelect?.options?.[epSelect.selectedIndex];
  var svc = selectedOpt?.dataset?.svc;
  if (!svc || !tabData?.discoveryDocs?.[svc]) return;

  var svcData = tabData.discoveryDocs[svc];
  var pageUrls = svcData.pageUrls || [];
  if (pageUrls.length === 0) return;

  // Check if any known page URL matches the current tab. URL parse failures
  // here are EXPECTED — `tabUrl` may be a chrome:// page or empty, and stored
  // `pageUrls` from older brain versions can be malformed; the fallback is to
  // treat them as non-matching, which is the correct hint behavior. The catches
  // stay bare here precisely because surfacing them would spam on every popup
  // open. The OUTER availableFrames read is the only path where a throw means
  // a real popup state bug (availableFrames should always be an array).
  // The current document's origin is the AUTHORITATIVE browser origin GET_FRAMES
  // reports (from _docOrigins) for the MAIN frame — never derived from the tab url:
  // a document url can't be mapped to an origin (about:blank/sandboxed give the
  // generic "null" origin) and a tab has cross-origin documents, so url->origin
  // would both miss real origins and fabricate ones. An opaque main frame
  // ("null:<uuid>") yields "" here -> no match -> the cross-origin hint shows
  // (fail-safe). The pageUrls compared below are the service's RECORDED page urls
  // (historical "seen at"), a display heuristic — not the live principal.
  var tabOrigin = "";
  try {
    var _mainF = availableFrames.find(function (f) { return f && f.isMain; }) || availableFrames[0];
    if (_mainF && typeof _mainF.origin === "string" && _mainF.origin.indexOf("://") > 0) tabOrigin = _mainF.origin;
  } catch (e) {
    console.warn("[popup:renderServiceOriginHint] availableFrames read threw:", e && e.message || e);
  }
  var matchesCurrentTab = false;
  if (tabOrigin) for (var i = 0; i < pageUrls.length; i++) {
    if (pageUrls[i] && URL.canParse(pageUrls[i]) && new URL(pageUrls[i]).origin === tabOrigin) {
      matchesCurrentTab = true; break;
    }
  }

  if (matchesCurrentTab) return;

  // Show hint with the most recent page URL — same canParse root-cause fix.
  // Fallback to the raw lastUrl when it isn't a parseable URL (legacy stored
  // entries can be just a hostname) is the correct display semantic.
  var lastUrl = pageUrls[pageUrls.length - 1];
  var lastHostname = (lastUrl && URL.canParse(lastUrl)) ? new URL(lastUrl).hostname : (lastUrl || "");

  var frameNote = "";
  var frameOrigins = svcData.frameOrigins || [];
  if (frameOrigins.length > 0) {
    frameNote = " (iframe: " + esc(frameOrigins[frameOrigins.length - 1]) + ")";
  }

  hint.classList.remove("hidden");
  hint.innerHTML = '<span class="service-origin-hint">Last used from: ' +
    esc(lastHostname) + frameNote +
    ' <a class="service-origin-open" data-url="' + esc(lastUrl) + '">Open \u2197</a></span>';
}

// ─── API Key Selector ────────────────────────────────────────────────────────

function renderKeySelector() {
  var section = document.getElementById("send-key-section");
  var optionsEl = document.getElementById("send-key-options");
  var badge = document.getElementById("send-key-badge");
  if (!section || !optionsEl) return;

  // Get selected service and hostname
  var epSelect = document.getElementById("send-ep-select");
  var selectedOpt = epSelect?.options?.[epSelect.selectedIndex];
  var svc = selectedOpt?.dataset?.svc || "";
  // Explicit canParse guard — same root-cause fix as renderServiceOriginHint.
  var hostname = (currentRequestUrl && URL.canParse(currentRequestUrl)) ? new URL(currentRequestUrl).hostname : "";

  // Collect matching keys (same logic as collectKeysForService in background)
  var matchingKeys = [];
  if (tabData?.apiKeys) {
    for (var k in tabData.apiKeys) {
      var kd = tabData.apiKeys[k];
      var svcMatch = kd.services && kd.services.indexOf(svc) !== -1;
      var hostMatch = kd.hosts && kd.hosts.indexOf(hostname) !== -1;
      if (svcMatch || hostMatch) {
        matchingKeys.push({ key: k, data: kd });
      }
    }
  }

  if (matchingKeys.length === 0) {
    section.classList.add("hidden");
    currentKeyOverride = null;
    return;
  }

  section.classList.remove("hidden");
  badge.textContent = matchingKeys.length + " available";

  var html = '';

  // Auto option (default)
  var autoTruncated = truncateKey(matchingKeys[0].key);
  html += '<label class="key-option">' +
    '<input type="radio" name="send-key-select" value="auto" checked /> ' +
    '<span class="key-option-label">Auto</span> ' +
    '<span class="key-value-truncated">' + esc(autoTruncated) + '</span>' +
    '</label>';

  // One option per matching key
  for (var i = 0; i < matchingKeys.length; i++) {
    var mk = matchingKeys[i];
    var truncated = truncateKey(mk.key);
    var sourceBadge = mk.data.source ? '<span class="key-source-badge">' + esc(mk.data.source) + '</span>' : '';
    html += '<label class="key-option">' +
      '<input type="radio" name="send-key-select" value="key-' + i + '" data-key="' + esc(mk.key) + '" /> ' +
      '<span class="key-value-truncated">' + esc(truncated) + '</span> ' +
      '<span class="key-option-label">' + esc(mk.data.name || "Key") + '</span> ' +
      sourceBadge +
      '</label>';
  }

  // Custom key option
  html += '<label class="key-option">' +
    '<input type="radio" name="send-key-select" value="custom" /> ' +
    '<span class="key-option-label">Custom</span>' +
    '</label>' +
    '<input type="text" class="key-custom-input hidden" id="send-key-custom" placeholder="Paste API key..." />';

  // None option
  html += '<label class="key-option">' +
    '<input type="radio" name="send-key-select" value="none" /> ' +
    '<span class="key-option-label">None (no key injection)</span>' +
    '</label>';

  optionsEl.innerHTML = html;

  // Reset override to auto
  currentKeyOverride = null;
}

function truncateKey(key) {
  if (!key || key.length <= 16) return key || "";
  return key.slice(0, 8) + "\u2026" + key.slice(-4);
}

function onKeySelectionChange() {
  var selected = document.querySelector('input[name="send-key-select"]:checked');
  var customInput = document.getElementById("send-key-custom");
  if (!selected) { currentKeyOverride = null; return; }

  var injectSource = "header";
  var injectRadio = document.querySelector('input[name="key-inject"]:checked');
  if (injectRadio) injectSource = injectRadio.value;

  if (customInput) customInput.classList.add("hidden");

  if (selected.value === "auto") {
    currentKeyOverride = null;
  } else if (selected.value === "none") {
    currentKeyOverride = { disabled: true };
  } else if (selected.value === "custom") {
    if (customInput) customInput.classList.remove("hidden");
    var val = customInput ? customInput.value.trim() : "";
    currentKeyOverride = val ? { key: val, source: injectSource } : null;
  } else if (selected.dataset.key) {
    currentKeyOverride = { key: selected.dataset.key, source: injectSource };
  }
}

