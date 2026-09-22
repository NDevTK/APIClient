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
      /* THE DOC IS THE QUESTION, NOT THE STATUS. `status === "found"` was the published-discovery FETCH's
         outcome standing in for "this service has a method surface", and lib/discovery-probe.js now records a
         failed published fetch without deleting what the bundle taught — so a service whose learned
         RestDescription is right here would have been left out of this list by a fact about a document nobody
         published. What fills the dropdown is `doc`. */
      if (svcData.doc) {
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

  /* THE RULE LINE, READ AS THE PRODUCER'S STATEMENT. `if (g)` was a truthiness test over a field five copiers
     each supplied with their own `|| null`, so this row rendered nothing for three different situations it
     could not tell apart: a bucket no rule named (the OpenAPI import), a bucket whose rule the published-
     document fetch had DROPPED on the way through, and a producer that had stopped stating the field at all.
     `null` is now the one spelling of the first, asserted here, and the row says so out loud rather than
     leaving the reviewer with a blank where a classification they are supposed to judge belongs.
     `matched` and `firstUrl` are stated on every record (lib/discovery-entry.js), so the ternaries that stood
     on them are gone with the same defect one level down. */
  let html = "";
  checkDiscoveryGrouping(svcData, "the Send panel's service-grouping row, service " + JSON.stringify(name));
  const g = svcData.grouping;
  if (g === null) {
    html += '<span class="grouping-label">grouping rule:</span> <code>none recorded</code>'
      + ' <span class="grouping-matched">(this bucket was not named by a URL-structure rule)</span>';
  } else {
    html += '<span class="grouping-label">grouping rule:</span> <code>' + esc(g.rule) + '</code>'
      + ' <span class="grouping-matched">matched: <code>' + esc(g.matched) + '</code></span>'
      + '<div class="grouping-first">first request: <code>' + esc(g.firstUrl) + '</code></div>';
  }

  // Bucket quality: count methods by origin. THE UNUSED BUCKET IS THE PRODUCT — "what the bundle CAN do but
  // didn't" — so it is named that here and in the dropdown tag, from the ONE classifier below, and it is
  // listed first.
  if (svcData.doc) {
    const n = { unused: 0, "unused+fired": 0, fired: 0, declared: 0, asset: 0 };
    let total = 0, forced = 0;
    for (const bucket of Object.values(svcData.doc.resources || {})) {
      for (const m of Object.values(bucket.methods || {})) {
        total++;
        n[_methodOrigin(m)]++;
        /* AND HOW MANY OF THEM EXIST ONLY BECAUSE A GATE WAS FORCED — counted BESIDE the buckets rather than
           as one of them, because it is an orthogonal fact and folding it in would have to pick which of
           "unused" and "forced" a method is. `_methodOrigin` says whether the wire ever carried this request;
           this says whether a real client's code composes it at all. A forced-only method inflating the UNUSED
           headline is the fabrication at the level of the COUNT: "N things the bundle can do but didn't", of
           which some are requests no client makes. */
        if (methodProvenance(m, "lib/popup-send.js service summary") === "forced") forced++;
      }
    }
    if (total > 0) {
      const parts = [];
      if (n.unused) parts.push(n.unused + " UNUSED (in bundle, never fired)");
      if (n["unused+fired"]) parts.push(n["unused+fired"] + " in bundle + fired");
      if (n.fired) parts.push(n.fired + " fired only (no bundle origin)");
      if (n.declared) parts.push(n.declared + " declared by a discovery doc, never fired");
      if (n.asset) parts.push(n.asset + " asset");
      if (forced) parts.push(forced + " of them reached only by FORCING a gate");
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
// WHAT THE ENGINE'S PATH TO THIS METHOD WAS WORTH — the second fact `_methodOrigin` deliberately does not
// carry — IS `methodProvenance` IN lib/endpoint-record.js, AND THE PRIVATE COPY THAT STOOD HERE IS GONE.
// It became one fact asked in TWO zones the moment lib/learn.js needed it: the templated-path reconcile
// grades a concrete record's path segments by the fold over that record's sightings, which is the same
// question this view asks, and `if (!m._astInferred) return null` written twice is two rules free to
// disagree about what an ungraded record means. endpoint-record.js already declares the vocabulary and the
// fold and is already loaded by both zones for exactly that reason, so the reader lives beside them.

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
      if (svcData.doc) {   // the method surface is the doc — see the service selector above
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
            /* …AND THE SECOND FACT, ALWAYS, FOR A METHOD THE BUNDLE NAMED. The tag above says whether the
               WIRE ever carried this request; this says what the ENGINE's own path to it was worth, which is
               CLAUDE.md §A-REQUEST-CARRIES-THE-PROVENANCE's three words verbatim — no synonym, because a
               second vocabulary for one field is two spellings free to drift.
               THE TWO WORDS THAT LOOK LIKE THEY COLLIDE DO NOT. `[fired only]` is network traffic this
               session observed; `[observed]` is the engine's grade and means a real LOAD of the document
               makes exactly this request (its parser fetched it), which a page nobody interacted with can be
               true of. They are different questions and both are on the row.
               IT IS STATED RATHER THAN SHOWN-ONLY-WHEN-FORCED: with `derived` and `observed` silent, a
               reviewer reads the absence of a grade as "fine", which is a consumer defaulting a producer's
               field with a rendering instead of a `||`. */
            const prov = methodProvenance(m, "lib/popup-send.js method dropdown");
            if (prov !== null) tag += " [" + prov + "]";
            /* THE METHOD ID AS THE STORE HOLDS IT. A rewrite stood here that substituted a declared name
               recovered from the page's source map into each `{hole}` of the displayed id (`{e}` → `{owner}`)
               — reading `p._sourceMapName` and `p.location === "path"`. It could not fire, and the reason is
               `_sourceMapName` ALONE: nothing in engine/host has ever emitted a source-map name, so the loop
               read a field the engine's param record does not have. lib/popup-form.js and lib/send.js each
               record the same absence at their own read of it.
               THE SECOND HALF OF THAT REASON IS RETIRED AND IS REWRITTEN RATHER THAN DELETED, BECAUSE IT IS
               AN UNDER-CLAIM AND THOSE ARE THE ONE KIND NOBODY DISCOVERS BY ACTING ON THEM. It said
               `lib/learn.js writes every @H param with location: "query" because endpoint.c mints params
               only out of the query string`, which closes the question — a reader told there are no path
               params does not go looking for one. solver/endpoint.h's own banner says that was true once and
               names the repair, whole rather than trimmed where the sentence turns: "It named one of them for
               the whole life of the file — the query — while every consumer branched on a `location` field
               nothing wrote, so the path-parameter registration and the entire request-body schema had never
               run once and both read as live." Both run now: endpoint.c mints `location` as "path", "query"
               or "body", and lib/learn.js registers a path hole as `location: "path"` with `_astValueClass`
               beside it.
               MEASURED at aa36410c, driving the stamped wasm at a one-page probe carrying
               `fetch("/api/user/" + location.hash.slice(1) + "/profile")`: the @H record carries
               `{"name":"location.hash.slice(1)","location":"path","valueClass":"unknown"}` and the method
               this file reads carries that same parameter with `location: "path"` and `required: true`.
               WHAT DOES NOT CHANGE IS THE DECISION. If the rename is wanted it is the ENGINE's to emit
               beside the param it renames — a view cannot recover a declared name from a minified one — and
               the half that makes the loop dead is the name, which still has no producer.
               RETIREMENT: this record goes when `_sourceMapName` has a writer in engine/host, because the
               sentence it corrects cannot then be re-derived from a dead loop. */
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

  /* AN EMPTY FRAME LIST IS THE BROWSER'S ANSWER, SAID IN WORDS. It is not "no frames reported yet (no content
     script)" \u2014 the comment that stood here named a producer this list has never come from; the frames come
     from webNavigation, which does not need a content script and does not report progressively. Nor is it
     reachable-but-meaningless: lib/popup-handlers.js used to fabricate a one-element frame whenever the
     browser reported none, so this branch was DEAD and the popup instead took the `!currentDocumentId()`
     branch below and displayed "Credentials: waiting for document\u2026" over a tab where nothing was coming.
     Now the two states arrive distinguished and each is stated: `tabResolved === false` is "the browser does
     not resolve this tab id" (it closed under us), and an empty list under `tabResolved === true` is "this tab
     holds no web document" \u2014 a chrome:// page, the New Tab page, an extension page. Neither is a wait. The
     row STAYS VISIBLE saying so, because hiding it is what let the user read the panels below as a page. */
  if (availableFrames.length === 0) {
    DCHECK(tabResolved !== null,
           "the frame picker is rendering before GET_FRAMES has answered once \u2014 render() runs after " +
           "loadState() has assigned both halves of that reply, so a null here is that order broken and this " +
           "panel would have to guess which of the browser's two answers it is looking at");
    row.classList.remove("hidden");
    sel.classList.add("hidden");
    if (originEl) {
      originEl.classList.add("opaque");
      originEl.textContent = tabResolved
        ? "No web document in this tab \u2014 nothing to analyse or send from."
        : "This tab no longer exists \u2014 the browser does not resolve it.";
      originEl.title = tabResolved
        ? "chrome.webNavigation reports no frame for this tab: it is a chrome:// page, the New Tab page or an "
          + "extension page, not a web document. The panels below show the CUMULATIVE cross-site moat, not this tab."
        : "chrome.webNavigation answered null for this tab id (its IDL: \u201cnull if the specified tab ID is "
          + "invalid\u201d) \u2014 the tab closed between the popup asking which tab it is over and this lookup.";
      originEl.style.cursor = "";
      originEl.onclick = null;
    }
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
  // open.
  /* THE availableFrames READ IS NOT ONE OF THEM AND ITS try/catch IS DELETED. Its own comment said a throw
     there "means a real popup state bug", and then caught it and warned — a swallow whose stated reason was
     that it must never fire. `.find` on a value that is not an array is exactly that bug, so it is asserted
     instead: popup.js declares availableFrames as `[]` and loadState assigns it only what GET_FRAMES'
     asserted `frames` array holds, so a non-array here is that chain broken and the hint would silently
     answer "" — which reads as "opaque origin" and shows the cross-origin warning about a page whose origin
     the browser had already reported. */
  DCHECK(Array.isArray(availableFrames),
         "the popup's frame list is not an array — popup.js declares it [] and assigns it only the frames " +
         "GET_FRAMES asserted, so anything else means the origin shown for this document was built out of " +
         "something the browser never said");
  // The current document's origin is the AUTHORITATIVE browser origin GET_FRAMES
  // reports (from _docOrigins) for the MAIN frame — never derived from the tab url:
  // a document url can't be mapped to an origin (about:blank/sandboxed give the
  // generic "null" origin) and a tab has cross-origin documents, so url->origin
  // would both miss real origins and fabricate ones. An opaque main frame
  // ("null:<uuid>") yields "" here -> no match -> the cross-origin hint shows
  // (fail-safe). The pageUrls compared below are the service's RECORDED page urls
  // (historical "seen at"), a display heuristic — not the live principal.
  var tabOrigin = "";
  var _mainF = availableFrames.find(function (f) { return f && f.isMain; }) || availableFrames[0];
  if (_mainF && typeof _mainF.origin === "string" && _mainF.origin.indexOf("://") > 0) tabOrigin = _mainF.origin;
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

// ─── The Address This Panel Will Send To ────────────────────────────

/* THE SURFACE SECURITY.md'S EXEMPTION IS NAMED AFTER, WHICH DID NOT EXIST.
 *
 * `pageContextFetch` is scoped OUT of the credentialed destructive-path deny list, and lib/schema.js states
 * the one ground for it in SECURITY.md's own words: a path answering the operator "is authorized by a human
 * at a surface that shows them the bytes". The Send panel was that surface and it rendered the endpoint
 * dropdown, the verb, the headers and the body — and never the ADDRESS. `currentRequestUrl` had no reader
 * anywhere that wrote it to the DOM, so the strongest authorization grade this project has rested on a
 * sentence that was not true of the panel making the claim.
 *
 * IT STOPPED BEING COSMETIC WHEN A PATH HOLE GAINED THE POWER TO MOVE THE ORIGIN. `applyPathParams`
 * (lib/popup-form.js) substitutes a hole PER SEGMENT because solver/endpoint.c spans a hole over the run of
 * example segments its value occupied, and `url_path_of` cuts at `?` and nowhere else — so a hole can cover
 * the scheme and the authority, and an operator typing into a path field can retarget the request at a host
 * nobody named. Before that, such an endpoint could not be sent at all.
 *
 * WHAT MAKES THIS SAFE TO DISPLAY IS THAT IT IS NOT A DISPLAY COMPOSER. It calls `sendPanelAddress`, which
 * is the function `sendRequest` itself calls for the string it puts in the message — one speller, so the two
 * cannot disagree. A second composer here would be worse than no row: an address that differs from the
 * request is not a wrong number in a report, it is a person deciding on a fact nobody established, and it
 * would FORGE the authorization the relay's exemption rests on rather than supply it.
 *
 * WHAT IT CANNOT KNOW IS SAID RATHER THAN OMITTED. `executeSendRequest` (lib/send.js) re-parses this address
 * and MAY add a `key` query parameter, from `collectKeysForService` over the offscreen's per-tab store and
 * `globalStore.apiKeys` — a cross-session store the popup does not hold, so whether it will is not a small
 * fact this file declined to compute, it is UNKNOWN from here. Replicating that condition would be exactly
 * the second copy the paragraph above refuses. `content.js` then appends a `#_uasr_send` fragment, which the
 * server never sees: RFC 3986 §3.5 "Fragment" — "the fragment identifier is not used in the scheme-specific
 * processing of a URI; instead, the fragment identifier is separated from the rest of the URI prior to a
 * dereference".
 *
 * THIS IS A DISPLAY AND NEVER A GATE. It adds no confirmation, no token list and no veto — CLAUDE.md and
 * SECURITY.md both record a gate built at that relay and REMOVED, because a token list standing there is the
 * tool vetoing its operator on a substring they never saw. The floor under an operator's act is the operator
 * seeing it, and this row is the seeing. */
function renderSendUrl() {
  var row = document.getElementById("send-url-row");
  DCHECK(!!row, "the Send panel carries no #send-url-row — extension/popup.html declares it as the first " +
         "child of .send-actions and this is its only writer, so an absent element is that markup broken " +
         "and the operator would press Send with no address in front of them, which is the state " +
         "SECURITY.md's page-context exemption is written against");
  while (row.firstChild) row.removeChild(row.firstChild);

  /* The message console composes no HTTP request — `setBodyMode` hides `btn-send` for that mode — so there
     is no address for this row to be about and an address left standing would be about the last endpoint. */
  if (currentBodyMode === "msgconsole") {
    row.classList.add("hidden");
    return;
  }
  row.classList.remove("hidden");

  var label = document.createElement("span");
  label.className = "send-url-label";
  label.textContent = "Sending to";
  row.appendChild(label);

  var sent = sendPanelAddress();
  var value = document.createElement("span");
  value.className = "send-url-value";

  /* NO ADDRESS IS SAID, NEVER LEFT BLANK. popup.js writes `""` for a service that published no rootUrl or
     baseUrl and for "no method selected"; both also hide .send-actions, so this row is normally not on
     screen in that state — but a blank row read as an address is the exact defect this whole surface exists
     to prevent, so the empty case states itself rather than relying on being hidden. */
  if (sent === "") {
    value.classList.add("send-url-none");
    value.textContent = "no address — this panel has nothing to send";
    row.appendChild(value);
    return;
  }
  value.textContent = sent;
  row.appendChild(value);

  /* THE PARSER IS PART OF THE SEND PATH, SO ITS ANSWER IS PART OF THE BYTES. `executeSendRequest` does
     `new URL(msg.url)` and sends `parsedUrl.toString()`, and that round trip is not identity: it lowercases
     the host, punycodes an IDN, drops a default port and RESOLVES `..` segments — every one of which can
     move the request to a different origin or resource than the composed string reads as. This is the SAME
     platform primitive on the SAME input, not a second composer: `new URL(x)` with no base in one document
     of this browser answers what it answers in another. Shown only when the two differ, so the common case
     stays one line. */
  if (URL.canParse(sent)) {
    var parsed = new URL(sent).toString();
    if (parsed !== sent) {
      var rewritten = document.createElement("div");
      rewritten.className = "send-url-rewritten";
      rewritten.textContent = "The URL parser rewrites this before it is sent: " + parsed;
      row.appendChild(rewritten);
    }
  } else {
    var unparseable = document.createElement("div");
    unparseable.className = "send-url-rewritten";
    unparseable.textContent = "This does not parse as a URL — the send path will refuse it.";
    row.appendChild(unparseable);
  }

  var note = document.createElement("div");
  note.className = "send-url-note";
  note.textContent =
    "Not decided here: an API key whose learned location is the URL is attached downstream as a `key` " +
    "query parameter, from a key store this panel cannot read — whether that happens to this request is " +
    "UNKNOWN from here. A `#_uasr_send` fragment is also appended; a fragment is not sent to the server.";
  row.appendChild(note);
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

