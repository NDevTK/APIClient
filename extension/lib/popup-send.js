/* Popup send-panel controls — extracted from popup.js (classic script, shares the popup global scope +
   DOM). renderSendPanel + its selectors: service grouping, method dropdown, frame selector, service-origin
   hint, and the API keys offered at the positions they were seen sent. */
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
  domReplaceChildren(svcSelect, [domElement("option", { value: "" }, ["All Services"])]);
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
  const rows = [];
  checkDiscoveryGrouping(svcData, "the Send panel's service-grouping row, service " + JSON.stringify(name));
  const g = svcData.grouping;
  const ruleLabel = domElement("span", { class: "grouping-label" }, ["grouping rule:"]);
  if (g === null) {
    rows.push(ruleLabel, " ", domElement("code", {}, ["none recorded"]), " ",
              domElement("span", { class: "grouping-matched" }, ["(this bucket was not named by a URL-structure rule)"]));
  } else {
    rows.push(ruleLabel, " ", domElement("code", {}, [g.rule]), " ",
              domElement("span", { class: "grouping-matched" }, ["matched: ", domElement("code", {}, [g.matched])]),
              domElement("div", { class: "grouping-first" }, ["first request: ", domElement("code", {}, [g.firstUrl])]));
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
      rows.push(domElement("div", { class: "grouping-first" },
                           ["methods: " + parts.join(", ") + "  (" + total + " total)"]));
    }
  }

  domReplaceChildren(el, rows);
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

// The Send dropdown's option value for method `m` of service `svcName`; lib/popup-endpoints.js selects by it.
function sendMethodOptionKey(svcName, m) {
  return `DISCOVERY ${m.httpMethod} ${svcName} ${m.id}`;
}

function renderMethodDropdown() {
  const svcFilter = document.getElementById("spec-service-select").value;
  const select = document.getElementById("send-ep-select");
  const prev = select.value;

  domReplaceChildren(select, [domElement("option", { value: "" }, ["-- select method --"])]);

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
            opt.value = sendMethodOptionKey(svcName, m);
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
            /* …and whose act the engine's sightings were: `[driven]` when only the engine's drive of a
               function the page never called made the request (lib/endpoint-record.js's `methodDriven`). */
            if (methodDriven(m, "lib/popup-send.js method dropdown") === true) tag += " [driven]";
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
    sel.replaceChildren();
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
  hint.replaceChildren();

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
    frameNote = " (iframe: " + String(frameOrigins[frameOrigins.length - 1]) + ")";
  }

  hint.classList.remove("hidden");
  domReplaceChildren(hint, [domElement("span", { class: "service-origin-hint" }, [
    "Last used from: " + lastHostname + frameNote + " ",
    domElement("a", { class: "service-origin-open", "data-url": String(lastUrl || "") }, ["Open \u2197"]),
  ])]);
}

// ─── The Address This Panel Will Send To ────────────────────────────

/* THE SURFACE SECURITY.md'S EXEMPTION IS NAMED AFTER, WHICH DID NOT EXIST.
 *
 * The operator tier is scoped OUT of the credentialed destructive-path deny list, and lib/schema.js states
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
    renderSendIdentity(null);
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
    renderSendIdentity(null);
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

  var init = sendPanelRequestInit();
  var typed = sendPanelStatedHeaders();
  var stated = function (v, unstated) { return v === null ? unstated : v; };
  row.appendChild(domElement("div", { class: "send-url-note" }, [
    "Sent from this extension as the frame the request was observed in — " +
    (typed.origin === null ? "its Origin" : typed.origin.header === null ? "no Origin, as you stated"
      : "the Origin you stated, " + typed.origin.header + ", in place of its own") +
    ", " +
    (typed.referer === null ? "its Referer" : "the Referer you stated, " + typed.referer + ", in place of its own") +
    " and " + (typed.jar === null ? "its cookies" : "the cookies of the jar you picked") + " — with mode " + stated(init.mode, "not stated") + ", referrer " +
    stated(init.referrer === "about:client" ? "the page's URL" : init.referrer === "" ? "none" : init.referrer,
           "not stated") + ", referrer policy " +
    stated(init.referrerPolicy === "" ? "the document's" : init.referrerPolicy, "not stated") +
    ". What is not stated is fetch's default (an unstated policy is strict-origin-when-cross-origin). " +
    "Cookie, Origin and Referer cannot be typed as headers: the browser owns them, and Origin and Referer " +
    "are stated only in their own fields below."]));
  if (URL.canParse(sent)) renderSendPolicyVerdict(row, new URL(sent).href);
  renderSendIdentity(URL.canParse(sent) ? new URL(sent).href : null);
  renderKeyExamples();
}

/* The frame's Content Security Policy verdict on a request to `url` (SEND_PANEL_POLICY, answered by the engine
   instance holding the frame's document), as a line in `row`. It is shown and never enforced: the send is the
   person's explicit act. The line is filled when the answer arrives, unless the row was re-rendered since. */
var _sendPolicyAsked = 0;
function renderSendPolicyVerdict(row, url) {
  var asked = ++_sendPolicyAsked;
  var documentId = sendPanelDocumentId();
  var line = domElement("div", { class: "send-url-note" }, ["Content Security Policy: asking the frame's document…"]);
  row.appendChild(line);
  var say = function (text) { if (asked === _sendPolicyAsked) line.textContent = "Content Security Policy: " + text; };
  if (!documentId) { say("unknown — no document is targeted."); return; }
  chrome.runtime.sendMessage({ type: "SEND_PANEL_POLICY", documentId: documentId, url: url }).then(function (r) {
    DCHECK(!!r && typeof r.verdict === "string" && typeof r.directive === "string",
           "SEND_PANEL_POLICY answered with no {verdict, directive} — lib/popup-handlers.js relays the host's");
    if (r.verdict === "allowed") say("the frame's policy allows this request.");
    else if (r.verdict === "blocked")
      say("the frame's `" + r.directive + "` would block this request in the page. It is sent anyway, as your act.");
    else if (r.verdict === "report-only")
      say("only a report-only `" + r.directive + "` refuses it, so the page's own request would be sent and reported.");
    else {
      DCHECK(r.verdict === "unknown", "SEND_PANEL_POLICY answered the verdict " + JSON.stringify(r.verdict) +
             ", which is none of allowed, blocked, report-only and unknown");
      say("unknown — no running engine instance holds this frame's document.");
    }
  });
}

/* The document a Send speaks as: the replayed record's, else the pinned frame's while it is live; null when
   neither is. Send, its readiness gate and every row that describes the send ask this one function. */
function sendPanelDocumentId() {
  return currentReplayRequest !== null && currentReplayRequest.documentId
    ? currentReplayRequest.documentId : currentDocumentId();
}

/* The Send panel's identity rows: the Origin and Referer the frame's own send carries, as an example chip
   beside each field, and the cookie jar the send speaks with. Every fact is the offscreen's
   SEND_IDENTITY_PREVIEW answer (lib/send.js operatorIdentityPreview, which calls what the send calls), so
   nothing here derives one. A blank field is the frame's own, derived at send, and its example follows the
   URL, method, mode, frame and credentials mode; a typed one is the operator's and follows nothing. The
   Origin's domain and the jar's are the open frames (loadSendJarDomain), with "null" and "none" for Origin.
   `url` is the parsed address, or null when there is none. Answers arriving after a newer render are dropped. */
var _sendIdentityAsked = 0;
function renderSendIdentity(url) {
  var asked = ++_sendIdentityAsked;
  var el = function (id) {
    var e = document.getElementById(id);
    DCHECK(!!e, "the Send panel carries no #" + id + " — extension/popup.html declares the identity rows");
    return e;
  };
  var originEx = el("send-origin-example"), refererEx = el("send-referer-example"), jar = el("send-jar-row");
  var typed = sendPanelStatedHeaders();
  var mark = function (id, stated) {
    el(id).textContent = stated ? "(operator)" : "(derived)";
    el(id).title = stated ? "typed by you: it follows neither the frame, the URL nor the method"
                          : "blank: the send derives it from the frame, as shown below";
  };
  mark("send-stated-origin-mark", typed.origin !== null);
  mark("send-stated-referer-mark", typed.referer !== null);
  var label = function (text) { return domElement("span", { class: "ast-values-label" }, [text]); };
  var say = function (text) {
    [originEx, refererEx, jar].forEach(function (e) { domReplaceChildren(e, [text]); });
  };
  var documentId = sendPanelDocumentId();
  if (url === null) { say(""); return; }
  if (!documentId) { say("no document is targeted, so no identity is derived"); return; }
  say("asking the frame's document…");
  chrome.runtime.sendMessage({ type: "SEND_IDENTITY_PREVIEW", documentId: documentId, url: url,
                               httpMethod: currentRequestMethod, requestInit: sendPanelRequestInit(),
                               stated: typed }).then(function (p) {
    checkIdentityPreview(p, "renderSendIdentity");
    if (asked !== _sendIdentityAsked) return;
    if (typeof p.error === "string") { say(p.error); return; }
    var derived = function (title) { return { cls: "send-provenance-mark", text: "(derived)", title: title }; };
    var how = p.method + " in mode " + p.mode;
    domReplaceChildren(originEx, [label("Frame's own:"), " ", p.origin === null
      ? "none: Fetch §3.2 Origin header appends no Origin to this " + how
      : offeredValueChip(p.origin, derived("Fetch §3.2 Origin header, for the frame " + p.frame.origin +
                                           "'s own " + how))]);
    var policy = p.policy + (p.policySource === "request" ? ", the request's own" : ", the default, as the " +
                 "frame's own policy is not read");
    var source = p.referrerSource === "client" ? "the frame's URL " + p.frame.docUrl
      : p.referrerSource === "request" ? "the request's referrer" : "no referrer, which the request states";
    domReplaceChildren(refererEx, [label("Frame's own:"), " ", p.referer === null
      ? "none: " + source + " under " + policy + " (Referrer Policy §8.3)"
      : offeredValueChip(p.referer, derived("Referrer Policy §8.3 over " + source + ", under " + policy))]);
    var lines = [domElement("div", {}, [label("Cookie jar:"), " ",
      domElement("span", { class: "send-jar-key" }, [sendJarKeyText(p.jar.partitionKey)]), " ",
      p.jar.source === "frame"
        ? domElement("span", { class: "send-provenance-mark",
                               title: "chrome.cookies.getPartitionKey for the frame's document" }, ["(observed)"])
        : domElement("span", { class: "send-provenance-mark",
                               title: "picked by you: the partition chrome.cookies.getPartitionKey states for " +
                                      p.jar.holder.docUrl }, ["(operator)"]),
      " credentials " + p.credentials])];
    if (p.producibleBy !== null && !p.producibleBy.jarHeld)
      lines.push(domElement("div", {}, ["The frame you picked does not hold this jar: " + p.jar.holder.origin +
                                        " does, and the frame's own is " + sendJarKeyText(p.frame.partitionKey)]));
    lines.push(domElement("div", {}, [
      "Sends: ",
      p.cookieNames.length === 0 ? "no cookies" : p.cookieNames.join(", ")]));
    p.excluded.forEach(function (x) { lines.push(domElement("div", {}, ["Not sent: " + x.name + ", " + x.rule])); });
    if (p.producibleBy === null) lines.push(domElement("div", {}, ["Refused at send: " + p.statedRefusal]));
    else if (p.producibleBy.producible) lines.push(domElement("div", {}, ["A real frame sends this: " + p.producibleBy.by]));
    else lines.push(domElement("div", {}, ["No real frame sends this: " + p.producibleBy.reasons.join("; ") +
                                           ". It is sent as your act."]));
    domReplaceChildren(jar, lines);
  });
}

/* A click on a frame's-own chip copies it into its field, which makes it the operator's. */
function wireSendIdentityExamples() {
  [["send-origin-example", "send-stated-origin"], ["send-referer-example", "send-stated-referer"]].forEach(function (pair) {
    document.getElementById(pair[0]).addEventListener("click", function (e) {
      if (!e.target.classList.contains("ast-value-chip")) return;
      var input = document.getElementById(pair[1]);
      input.value = e.target.textContent;
      input.dispatchEvent(new Event("input", { bubbles: true }));
    });
  });
}

/* The request facts the Send panel sends besides its bytes. Mode, referrer and referrer policy are the
   replayed record's (null where it stated none, or where the panel holds no observed request); the
   credentials mode is the operator's choice in #send-credentials, which loadSendRequestInit initialises
   from the record. One function, so the note above and the request cannot differ. */
function sendPanelRequestInit() {
  var sel = document.getElementById("send-credentials");
  DCHECK(!!sel, "the Send panel carries no #send-credentials — extension/popup.html declares it");
  var observed = currentReplayRequest !== null && currentReplayRequest.kind === "http"
    ? currentReplayRequest.requestInit : null;
  return {
    credentials: sel.value === "" ? null : sel.value,
    mode: observed === null ? null : observed.mode,
    referrer: observed === null ? null : observed.referrer,
    referrerPolicy: observed === null ? null : observed.referrerPolicy,
  };
}

/* The Send panel's stated record (lib/safe-fetch.js safeFetchStatedShape), each member null for the frame's
   own: the Origin typed in #send-stated-origin, where "none" states that no Origin is sent; the Referer typed
   in #send-stated-referer; and the frame picked in #send-jar, whose partition the send speaks with. The
   offscreen's operator tier refuses a typed value that is not what its header carries. */
function sendPanelStatedHeaders() {
  var o = document.getElementById("send-stated-origin"), r = document.getElementById("send-stated-referer");
  var j = document.getElementById("send-jar");
  DCHECK(!!o && !!r && !!j, "the Send panel carries no stated Origin, Referer or jar control — " +
         "extension/popup.html declares all three");
  var origin = o.value.trim();
  return { origin: origin === "" ? null : { header: origin === "none" ? null : origin },
           referer: r.value.trim() === "" ? null : r.value.trim(),
           jar: j.value === "" ? null : _sendJarHolders[j.value] };
}

/* A partition key as the jar rows write it. */
function sendJarKeyText(pk) {
  return pk.topLevelSite + (pk.hasCrossSiteAncestor ? ", cross-site ancestor" : "");
}

/* The open frames, from the offscreen's SEND_JAR_DOMAIN (lib/send.js operatorJarDomain), as #send-jar's
   options and #send-origin-domain's: one jar per distinct partition key, held by the frame picked before
   when it still holds that key, and every frame origin with "null" and "none". `_sendJarHolders` maps an
   option's documentId to its {tabId, documentId}; a picked frame that has closed keeps its option, marked
   gone, so the panel never changes the person's pick for them and the send refuses it by name. */
var _sendJarHolders = {};
async function loadSendJarDomain() {
  var list = await chrome.runtime.sendMessage({ type: "SEND_JAR_DOMAIN" });
  checkJarDomain(list, "loadSendJarDomain");
  var sel = document.getElementById("send-jar");
  var picked = sel.value;
  var keyOf = function (pk) { return pk.topLevelSite + "|" + pk.hasCrossSiteAncestor; };
  var byKey = {}, order = [];
  list.forEach(function (f) {
    var k = keyOf(f.partitionKey);
    if (!(k in byKey)) { byKey[k] = f; order.push(k); }
    else if (f.documentId === picked) byKey[k] = f;
  });
  var options = [domElement("option", { value: "" }, ["the frame's own"])];
  order.forEach(function (k) {
    var f = byKey[k];
    _sendJarHolders[f.documentId] = { tabId: f.tabId, documentId: f.documentId };
    options.push(domElement("option", { value: f.documentId },
                            [sendJarKeyText(f.partitionKey) + ", held by " + f.origin + " (tab " + f.tabId + ")"]));
  });
  if (picked !== "" && !order.some(function (k) { return byKey[k].documentId === picked; }))
    options.push(domElement("option", { value: picked }, ["the frame picked before, now gone"]));
  domReplaceChildren(sel, options);
  sel.value = picked;
  var origins = [];
  list.forEach(function (f) { if (origins.indexOf(f.origin) < 0) origins.push(f.origin); });
  domReplaceChildren(document.getElementById("send-origin-domain"), origins.concat(["null", "none"]).map(function (o) {
    return domElement("option", { value: o }, []);
  }));
}

function loadSendRequestInit(req) {
  DCHECK(req.requestInit === null || (!!req.requestInit && typeof req.requestInit === "object"),
         "a replayed HTTP record carries no requestInit — offscreen-brain.js's _checkLogEntry asserts one " +
         "(null or the four members) on every entry it files");
  document.getElementById("send-credentials").value =
    req.requestInit === null || req.requestInit.credentials === null ? "" : req.requestInit.credentials;
}

// ─── API keys, offered where they were seen sent ─────────────────────────────

/* Where a learned key was seen sent, from its record (lib/keys.js): the request header its context names
   (`header:<name>`), or each query parameter of the request URL it was seen in (`referer`) whose value is the
   key. A key found only in a response body has no position.
   Named residual. Not covered: a key's later sightings, as lib/keys.js records the first one's context only.
   Next diff records every position a key is seen sent at on its entry. Its absence shows as a key offered
   at one position while requests the page sent carried it at another. */
function apiKeyPositions(key, entry) {
  DCHECK(typeof entry.source === "string",
         "an API-key entry carries no source — lib/keys.js records the context it was found in on every entry");
  if (entry.source.startsWith("header:")) return [{ in: "header", name: entry.source.slice("header:".length) }];
  if (entry.source !== "url") return [];
  DCHECK(typeof entry.referer === "string" && URL.canParse(entry.referer),
         "an API-key entry found in a URL carries no URL — lib/keys.js records the one it scanned as `referer`");
  var out = [];
  new URL(entry.referer).searchParams.forEach(function (v, n) {
    if (v === key && !out.some(function (p) { return p.name === n; })) out.push({ in: "query", name: n });
  });
  return out;
}

/* The learned keys for this panel's request, seen with its service or host (collectKeysForService's rule),
   as [{in, name, offers: [{key, seen, context}]}]: one entry per position a key was seen sent at, where
   `seen` is "sent" for those keys and "response" for a key found only in a response, offered at every
   position. With no position at all, those keys are one entry with `in` null. */
function sendPanelKeyPositions() {
  var epSelect = document.getElementById("send-ep-select");
  var selectedOpt = epSelect.options[epSelect.selectedIndex];
  var svc = selectedOpt && typeof selectedOpt.dataset.svc === "string" ? selectedOpt.dataset.svc : "";
  var host = currentRequestUrl !== "" && URL.canParse(currentRequestUrl) ? new URL(currentRequestUrl).hostname : "";
  DCHECK(!!tabData && !!tabData.apiKeys && typeof tabData.apiKeys === "object",
         "the Send panel was asked for its key positions before GET_STATE answered with the key map");
  var positions = [], unplaced = [];
  var at = function (p) {
    var e = positions.find(function (x) { return x.in === p.in && x.name.toLowerCase() === p.name.toLowerCase(); });
    if (!e) { e = { in: p.in, name: p.name, offers: [] }; positions.push(e); }
    return e;
  };
  Object.keys(tabData.apiKeys).forEach(function (k) {
    var kd = tabData.apiKeys[k];
    if (kd.services.indexOf(svc) < 0 && kd.hosts.indexOf(host) < 0) return;
    var ps = apiKeyPositions(k, kd);
    if (ps.length === 0) unplaced.push({ key: k, seen: "response", context: kd.source });
    ps.forEach(function (p) { at(p).offers.push({ key: k, seen: "sent", context: kd.source }); });
  });
  if (positions.length === 0) return unplaced.length ? [{ in: null, name: null, offers: unplaced }] : [];
  positions.forEach(function (p) { p.offers = p.offers.concat(unplaced); });
  return positions;
}

/* The Send panel's own control for a position: a parameter field of that name and location, else a typed
   header row, else (for a query parameter) the address template's own query. */
function _keyPositionField(pos) {
  var fields = document.querySelectorAll("#send-form-fields > .form-section > .form-field");
  for (var i = 0; i < fields.length; i++)
    if (fields[i].dataset.category === "param" && fields[i].dataset.location === pos.in &&
        fields[i].dataset.name === pos.name) {
      var input = fields[i].querySelector(".form-input");
      DCHECK(!!input, "a " + pos.in + " parameter field " + pos.name + " holds no .form-input — _buildFieldStep " +
             "gives every scalar parameter one, and a key goes nowhere else");
      return { kind: "field", input: input };
    }
  if (pos.in === "header") {
    var rows = document.querySelectorAll("#send-headers-list .header-row");
    for (var j = 0; j < rows.length; j++)
      if (rows[j].querySelector(".header-key").value.trim().toLowerCase() === pos.name.toLowerCase())
        return { kind: "row", input: rows[j].querySelector(".header-val"), row: rows[j] };
    return null;
  }
  return { kind: "template" };
}

/* The address template's query, split off before a fragment; holes live in the path, so the query parses. */
function _templateQuery(t) {
  var q = t.indexOf("?"), h = t.indexOf("#");
  var end = h < 0 ? t.length : h;
  return { head: q < 0 || q > end ? t.slice(0, end) : t.slice(0, q),
           params: new URLSearchParams(q < 0 || q > end ? "" : t.slice(q + 1, end)), tail: t.slice(end) };
}

/* The value the panel will send at `pos`, or null when it sends none there. */
function keyPositionValue(pos) {
  var f = _keyPositionField(pos);
  if (f === null) return null;
  if (f.kind !== "template") return f.input.value === "" ? null : f.input.value;
  var q = _templateQuery(currentRequestUrl);
  return q.params.has(pos.name) ? q.params.get(pos.name) : null;
}

/* Puts `value` at `pos` in the panel's own control, so the address row and the header rows show it. Answers
   the header row it added, or null. */
function setKeyPositionValue(pos, value) {
  var f = _keyPositionField(pos);
  if (f === null) return addHeaderRow(pos.name, value);
  if (f.kind !== "template") { f.input.value = value; return null; }
  var q = _templateQuery(currentRequestUrl);
  q.params.set(pos.name, value);
  setRequestTemplate(q.head + "?" + q.params.toString() + q.tail);
  return null;
}

/* What the last endpoint load put in: [{pos, value, row, template}], `row` the header row it added and
   `template` the address it wrote. A prefilled value is the derived one; the next load removes a header row
   still holding it and fills again, while a value the person changed is theirs. */
var _keyPrefilled = [];

/* On an endpoint load: each position with a key seen sent there, and nothing in the panel's control for it,
   gets the first such key. A key found only in a response is offered and never prefilled. */
function prefillKeyPositions() {
  _keyPrefilled.forEach(function (p) {
    if (p.row !== null && p.row.isConnected && p.row.querySelector(".header-val").value === p.value) p.row.remove();
  });
  // An address template this prefill wrote and nothing has replaced since still holds the derived value.
  _keyPrefilled = _keyPrefilled.filter(function (p) { return p.template !== null && p.template === currentRequestUrl; });
  sendPanelKeyPositions().forEach(function (pos) {
    if (pos.in === null || keyPositionValue(pos) !== null) return;
    var sent = pos.offers.find(function (o) { return o.seen === "sent"; });
    if (!sent) return;
    var row = setKeyPositionValue(pos, sent.key);
    _keyPrefilled.push({ pos: pos, value: sent.key, row: row,
                         template: _keyPositionField(pos).kind === "template" ? currentRequestUrl : null });
  });
}

/* The key lines under the headers: per position, its state (derived, in the request as the replayed record
   or the person put it, or none sent) and each key offered there as a chip; a click puts that key there. */
function renderKeyExamples() {
  var box = document.getElementById("send-key-examples");
  DCHECK(!!box, "the Send panel carries no #send-key-examples — extension/popup.html declares it");
  var positions = currentRequestUrl === "" || currentBodyMode === "msgconsole" ? [] : sendPanelKeyPositions();
  domReplaceChildren(box, positions.map(function (pos) {
    var where = pos.in === null ? "API keys found in responses, never seen sent:"
      : "API key at " + (pos.in === "header" ? "header " : "query parameter ") + pos.name + ":";
    var state = [];
    if (pos.in !== null) {
      var now = keyPositionValue(pos);
      var derived = _keyPrefilled.some(function (p) {
        return p.pos.in === pos.in && p.pos.name === pos.name && p.value === now &&
               (p.row !== null ? p.row.isConnected : p.template === null || p.template === currentRequestUrl);
      });
      state = [" ", domElement("span", { class: "send-provenance-mark" },
                               [now === null ? "(none sent)" : derived ? "(derived)" : "(in the request)"])];
    }
    var chips = pos.offers.map(function (o) {
      return [" ", domElement("span", { class: "send-key-chip", "data-in": pos.in === null ? "" : pos.in,
                                        "data-name": pos.name === null ? "" : pos.name }, [
        offeredValueChip(o.key, { cls: "send-provenance-mark",
          text: o.seen === "sent" ? "(seen sent)" : "(found in a response)",
          title: o.seen === "sent" ? "observed in a request at this position (" + o.context + ")"
                                   : "found in a response body (" + o.context + "), never seen sent" })])];
    });
    return domElement("div", { class: "field-ast-values" }, [domElement("span", { class: "ast-values-label" }, [where]),
                                                              state, chips]);
  }));
}

/* A chip click puts its key at its position; the panel's input listener then re-renders the address. */
function wireKeyExamples() {
  document.getElementById("send-key-examples").addEventListener("click", function (e) {
    if (!e.target.classList.contains("ast-value-chip")) return;
    var holder = e.target.closest(".send-key-chip");
    if (holder.dataset.in === "") return;
    setKeyPositionValue({ in: holder.dataset.in, name: holder.dataset.name }, e.target.textContent);
    document.getElementById("panel-send").dispatchEvent(new Event("input", { bubbles: true }));
  });
}
