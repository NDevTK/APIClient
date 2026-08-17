/* Popup security panel — extracted from popup.js (classic script, shares the popup global scope + DOM).
   Renders the @S securityFindings (source -> sink -> poc) and drives live-verify: _handleVerify opens the
   sandboxed attacker popup, _pollVerify + the message listener report REAL EXPLOIT / NOT REPRODUCED. */
// ─── Security Panel ──────────────────────────────────────────────────────────

// §@S(d)'s ENVELOPE VOCABULARY, rendered. The TOKENS are the engine's — solve.h documents both sets, one
// `firesOn` per sink class and one `delivery` per source mechanism — and this file owns only the English for
// them. Keeping the sentences here and the tokens in C is the split that was missing: the old card carried a
// host-side `{hash}|{search}|{pm}|{reply}` taxonomy of its own, which is a second statement of the engine's
// attacker-source model and had already drifted into naming sources no component declares. A token with no
// sentence is a contract drift and DCHECKs at the card rather than rendering the bare token, because a bare
// token in a reproduction envelope is exactly the silence this record exists to end.
var _FIRES_ON = {
  "sink-evaluates": "auto-fires: the sink evaluates the payload where it stands — no interaction, no navigation",
  "parse-insert":   "auto-fires at insertion: the injected markup's onload/onerror handler runs when the page parses it — no click needed",
  "navigation":     "fires on navigation: the payload is a javascript: URL, so it runs when the navigation this sink starts is performed (for a form action, on submission)",
};
var _DELIVERY = {
  "address":           function (p) { return "delivered in the victim's own URL (" + (p === "#" ? "fragment" : p === "?" ? "query string" : String(p)) + ") — one navigation is the whole PoC"; },
  "plant":             function ()  { return "TWO-STAGE (stored): the attacker plants the value on the victim's origin first, and the victim's later load is what fires it"; },
  "referring-address": function ()  { return "the payload rides the address the victim ARRIVES FROM — the attacker's URL, not the victim's"; },
  "user-file":         function ()  { return "the user must hand the document an attacker-supplied file — no navigation delivers it"; },
};

// The delivery clause for either entry shape. An ABSENT `delivery` is a statement (the source declared none);
// a PRESENT one this view has no sentence for is drift, and drift asserts — then still says WHICH token it
// could not render, because a release build strips the assert and a thrown TypeError here would empty the
// whole panel, which reads as "nothing is here".
function _deliverySentence(item) {
  if (!item.delivery)
    return "the engine declares no browser delivery for this source — nothing carries or transforms these bytes "
         + "on the way in, so there is no navigation that reproduces it";
  DCHECK(!!_DELIVERY[item.delivery],
    "an @S record reached the popup with a delivery mechanism this view has no sentence for ("
    + JSON.stringify(item.delivery) + ") — the token vocabulary is solve.h's, declared beside each attacker "
    + "source in C, so either a mechanism was added there without its sentence here or this record did not "
    + "come from solve_json_array (source=" + item.source + ")");
  if (!_DELIVERY[item.delivery])
    return "the engine declares the delivery `" + item.delivery + "`, which this view has no sentence for";
  return _DELIVERY[item.delivery](item.deliveryPrefix);
}

// §@S(a): A FIRING BREAKOUT IN THE MODEL IS NOT YET A WORKING EXPLOIT — it has to run under the page's ACTUAL
// policy, and the engine answers that with TWO INDEPENDENT facts on the record. `cspBlocks` carries the policy
// text that kills this vector; `trustedTypes` carries the sink GROUP the document requires a trusted type for,
// which makes the assignment THROW before the markup is ever parsed. Either one alone means the payload does
// not run on the real page.
//
// The badge asked only the first. So a sink under `require-trusted-types-for 'script'` badged a clean HIGH XSS
// one line above an envelope that said the assignment throws — the card contradicting itself, and what a
// reader takes away from a card is the badge. That is CLAUDE.md's recorded `cspBlocked` defect a second time:
// there, the popup read a name the engine never wrote; here, it reads one of the two names the engine DOES
// write and ignores the other, and the visible consequence is identical — a policy-dead vector reported as a
// clean XSS. So the verdict is computed ONCE, here, and every surface that states one reads this.
function _policyBlockers(item) {
  var out = [];
  if (item.cspBlocks)    out.push({ what: "CSP", detail: item.cspBlocks });
  if (item.trustedTypes) out.push({ what: "Trusted Types", detail: item.trustedTypes });
  return out;
}
function _blockerNames(blockers) {
  var n = [];
  for (var i = 0; i < blockers.length; i++) n.push(blockers[i].what);
  return n.join(" + ");
}

// THE ENCODE SET IS THREE STATES AND THIS READ TWO. `concolic_source_encodes` (solver/concolic.h) returns the
// bytes a source's own component percent-encodes, the EMPTY STRING for a declared source that encodes nothing,
// and NULL only for a source with no delivery declaration at all — and `emit_delivery` emits the empty string
// as an empty string, because `if (enc)` is true for it in C. On this side `if (item.sourceEncodes)` is FALSE
// for it, so a source that carries the attacker's bytes to the page UNTOUCHED — the strongest fact this field
// has, and the reason a raw-fragment JS-context breakout is real — read exactly like a source the engine never
// declared. Returns HTML (it emits <code>), so callers place it WITHOUT esc().
function _encodeSentence(item) {
  if (typeof item.sourceEncodes !== "string") return "";   // undeclared source — _deliverySentence states that
  if (item.sourceEncodes === "")
    return "the browser percent-encodes <strong>nothing</strong> in this source — the bytes reach the page "
         + "exactly as the attacker wrote them";
  return "the browser percent-encodes <code>" + esc(item.sourceEncodes) + "</code> in this source, so a "
       + "candidate needing those bytes arrives escaped";
}

// Stable-ish key for a finding card within a tab. The engine emits one record per
// (sink, source); key by that + the script the sink lives in.
function _findingKey(entry) {
  const it = entry.item || {};
  return (entry.sourceUrl || "(inline)") + "|" + (it.sink || "?") + "|" + (it.source || "") + "|" + (it.poc || "");
}


function renderSecurityPanel() {
  const container = document.getElementById("security-findings");
  const empty = document.getElementById("security-empty");

  // `tabData` ABSENT and `securityFindings` ABSENT ARE TWO DIFFERENT STATEMENTS, and `tabData?.x || []` said
  // the same thing for both. A null tabData is "GET_STATE has not answered yet, or Clear just emptied the
  // view" — a real state this panel renders as its empty text. A tabData that HAS no securityFindings is
  // serializeTabData broken: `mergedSecurityFindings` returns an array on every path, so there is no run in
  // which the field is missing, and defaulting it to [] renders "nothing has been reported" for a document
  // whose findings were dropped in transit — the "safe" verdict §@S forbids, arrived at by a `||`.
  if (!tabData) { container.innerHTML = ""; empty.style.display = "block"; return; }
  DCHECK(Array.isArray(tabData.securityFindings),
         "GET_STATE answered without a securityFindings array — serializeTabData builds one on every path "
         + "(lib/serialize.js -> mergedSecurityFindings), so its absence is that serializer broken and this "
         + "panel is about to report a page with dropped findings as a page with none");
  const findings = tabData.securityFindings;
  // A securitySink is one of TWO records (see the split below): a FIRED PoC — a concrete candidate driven
  // through the real code + branches + filters that BROKE OUT at the sink, self-verifying by replay — or a
  // PARKED search on a sink attacker input demonstrably reaches. Neither carries a verdict to compute: a
  // proven exploit is HIGH unless the page's own policy kills the vector (see _policyBlockers), and the
  // absence of either record is NOT "safe".
  let secCount = 0;
  for (let i = 0; i < findings.length; i++) {
    // lib/merge.js pushes `securitySinks: secSinks` straight from the analysis it has already DCHECKed is an
    // array, so an entry without one is that merge broken — not a source that reported no sinks (merge.js
    // does not create an entry at all in that case).
    DCHECK(Array.isArray(findings[i].securitySinks),
           "a securityFindings entry reached the popup with no securitySinks array — lib/merge.js only "
           + "creates an entry when it HAS sinks, so an entry without the array is that merge broken "
           + "(sourceUrl=" + findings[i].sourceUrl + ")");
    secCount += findings[i].securitySinks.length;
  }
  const fp = findings.length + ":" + secCount;
  if (fp === _lastSecFp) return;
  _lastSecFp = fp;

  // The container is NOT cleared here. A contract DCHECK below throws where the engine's record does not
  // carry what a card claims, and clearing first would leave the panel EMPTY on that throw — an empty Vulns
  // panel reads as "nothing is here", which is the "safe" verdict solve.h forbids, arrived at by a crash
  // instead of by a claim. The clear happens where a replacement is ready (the early return, and the final
  // assignment), so a failed render leaves the LAST honest cards standing beside a loud console @WHY.

  // TWO STATES, NEVER ONE LIST. The engine reports every sink an attacker source REACHES, and a sink whose
  // breakout has not been solved carries "search":"parked" instead of a poc. Rendering both under one fired
  // heading would badge a parked search HIGH with an empty payload — a claim the engine never made. They are
  // separated here for the same reason the engine emits them apart: a fired PoC is proof, a parked search is
  // an open lead, and neither is a statement that the sink is safe.
  var allItems = [], parked = [];
  for (var fi = 0; fi < findings.length; fi++) {
    var f = findings[fi];
    var srcLabel = f.sourceUrl ? _shortUrl(f.sourceUrl) : "(unknown)";
    for (var si = 0; si < f.securitySinks.length; si++) {
      var it = f.securitySinks[si];
      var e = { item: it, sourceUrl: f.sourceUrl, srcLabel: srcLabel, pageUrl: f.pageUrl };
      (it && it.search === "parked" ? parked : allItems).push(e);
    }
  }

  if (!allItems.length && !parked.length) { container.innerHTML = ""; empty.style.display = "block"; return; }
  empty.style.display = "none";

  // THE HEADING IS A CLAIM TOO. "Working XSS PoCs" was written over a list that includes every breakout the
  // page's own CSP or Trusted-Types policy kills on real Chrome — §@S(a)'s "never a bare XSS", made by the
  // section title rather than by a card. What the engine proved about every entry in this list is exactly one
  // thing: the candidate BROKE OUT AND FIRED in the model. Whether it runs on the real page is the per-card
  // policy verdict, so the heading says the first and counts the second.
  var blockedCount = 0;
  for (var bi = 0; bi < allItems.length; bi++) if (_policyBlockers(allItems[bi].item).length) blockedCount++;
  var html = allItems.length
    ? '<div class="section-header">Breakouts the engine FIRED <span class="badge badge-status">' + allItems.length + '</span>'
      + (blockedCount
          ? ' <span class="badge badge-medium" title="the sink is REAL and the breakout fired in the model, but the page\'s own policy kills the vector on real Chrome — each needs a policy-permitted vector">'
            + blockedCount + ' policy-blocked</span>'
          : "")
      + '</div>'
    : "";

  for (var i = 0; i < allItems.length; i++) {
    var entry = allItems[i];
    var item = entry.item;

    var srcLink = entry.sourceUrl && /^https?:\/\//i.test(entry.sourceUrl)
      ? '<a href="' + esc(entry.sourceUrl) + '" target="_blank" title="' + esc(entry.sourceUrl) + '">' + esc(entry.srcLabel) + '</a>'
      : esc(entry.srcLabel);
    if (entry.pageUrl && entry.pageUrl !== entry.sourceUrl)
      srcLink += ' <span class="page-context" title="' + esc(entry.pageUrl) + '">in ' + esc(_shortUrl(entry.pageUrl)) + '</span>';

    // THE CARD READS THE RECORD THE ENGINE ACTUALLY EMITS. `solve_json_array` (engine/host/solver/solve.c)
    // writes {sink, source, poc, firesOn, cspBlocks?, trustedTypes?, sourceEncodes?, delivery?,
    // deliveryPrefix?} for a fired sink and {sink, source, search:"parked", tried, sourceEncodes?, delivery?,
    // deliveryPrefix?} for a parked one — and nothing else. This card used to read `shape`, `evidence`,
    // `cspBlocked`, `cspReason` and `csp`: five names from a contract that no longer exists, so every card
    // silently dropped its source line AND its CSP verdict, and the live-verify button (gated on `shape`)
    // could not appear for any finding the engine has ever emitted. A bridge edge asserts its contract, per
    // CLAUDE.md §Architecture, so drift like that crashes where it is born instead of quietly rendering less.
    DCHECK(item.sink && item.source && item.poc,
      "an @S record reached the popup without sink/source/poc — solve_json_array emits all three for a fired "
      + "sink, so a card is about to claim a working PoC it cannot show (sink=" + item.sink + " source="
      + item.source + ")");
    DCHECK(_FIRES_ON[item.firesOn],
      "an @S record reached the popup with a firesOn this view has no sentence for (" + JSON.stringify(item.firesOn)
      + ") — the token vocabulary is solve.h's, one per sink class, and a PoC whose firing semantics cannot be "
      + "stated is a payload the reader cannot reproduce (sink=" + item.sink + ")");

    // NO `|| "?"` BESIDE AN ASSERTED FIELD. The DCHECK above states that sink/source/poc are all present on a
    // fired record; a placeholder beside it is the release-build behaviour of a contract that has already
    // been declared, and it is what lets a drifted record render a card that looks complete.
    var srcHtml = '<div class="card-value" title="the attacker-controlled source whose bytes reach this sink">'
      + 'source: <code>' + esc(item.source) + '</code></div>';
    var pocHtml = '<div class="card-poc"><span class="poc-lbl">breakout input</span> <code class="poc-payload">'
      + esc(item.poc) + '</code></div>';

    // §@S(d): EVERY PoC CARRIES ITS REPRODUCTION ENVELOPE — what makes it fire, whether it is stored, and the
    // CSP/Trusted-Types state it needs — because a payload without those is not reproducible by the person
    // reading it. Every clause below is now a POSITIVE engine statement, and so is every ABSENCE: no
    // `cspBlocks` means policy_allows said yes, no `trustedTypes` means no TT requirement reaches this sink,
    // and no `delivery` means the source declared none (server-injected page state the attacker writes
    // directly) rather than one the engine forgot. The one thing this card must never do is fill a silence
    // with a plausible default — that is how it used to badge every finding HIGH.
    var blockers = _policyBlockers(item);
    var policyClause = blockers.length
      ? '<strong>the page\'s own policy blocks this vector</strong> — '
        + blockers.map(function (b) {
            return b.what === "CSP"
              ? 'CSP <code>' + esc(b.detail) + '</code>'
              : 'Trusted Types: the document requires a trusted type for the <code>' + esc(b.detail)
                + '</code> sink group, so the assignment throws before the payload is ever parsed unless a '
                + 'policy stringifies it';
          }).join(' &middot; and ')
      : 'the page CSP permits this vector, and no Trusted-Types requirement reaches this sink';
    var encClause = _encodeSentence(item);   // already HTML — see _encodeSentence
    var envHtml = '<div class="card-dims">reproduction envelope: '
      + '<strong>' + esc(_FIRES_ON[item.firesOn]
          || ('the engine reports this vector as `' + item.firesOn + '`, which this view has no sentence for'))
        + '</strong>'
      + ' · ' + policyClause
      + ' · ' + esc(_deliverySentence(item))
      + (encClause ? ' · ' + encClause : "")
      + '</div>';

    // ENGINE AGREEMENT verify: run the engine's EXACT poc against the REAL page in a sandboxed attacker
    // window; the sink firing apiclientsink is ground truth. WHETHER the delivery can be PERFORMED is
    // decided by the layer that performs it (startExploitProbe -> buildLiveDelivery), never re-decided
    // here. The old second predicate asked `/\{(hash|search|pm)\}/` of a record whose source reads
    // `location.hash`, so it was false for every finding and the card printed a reason ("needs a
    // client-deliverable source") that was not the real one. One decision, in the layer that owns it; its
    // own `pocWhy` is what the card reports when the mechanism cannot be performed.
    // The probe carries the engine's DECLARATION (`delivery` + `deliveryPrefix`) and no host-side source
    // taxonomy: `srcpath` and `gatefields` used to be sent here and the engine emits neither — they were
    // inputs to the deleted `{pm}` field-path builder, so they were `undefined` on every probe ever sent.
    // THE PROBE CARRIES ABSENCE AS ABSENCE. `item.delivery || ""` turned "the engine declared no delivery for
    // this source" into an empty string, which buildLiveDelivery then re-reads as the same thing through its
    // own `!delivery` — a round trip that works only because both ends happen to agree that "" is falsy. An
    // engine field the record legitimately omits is omitted here too, so the receiver's DCHECKs see the
    // record's real shape rather than this view's normalisation of it.
    var key = _findingKey(entry);
    var verifyHtml = "";
    if (entry.pageUrl) {
      var probeObj = { poc: item.poc, source: item.source, sinkName: item.sink,
                       sourceUrl: entry.sourceUrl, pageUrl: entry.pageUrl, findingId: key };
      if (item.delivery)       probeObj.delivery = item.delivery;
      if (item.deliveryPrefix) probeObj.deliveryPrefix = item.deliveryPrefix;
      if (item.cspBlocks)      probeObj.cspBlocks = item.cspBlocks;
      if (item.trustedTypes)   probeObj.trustedTypes = item.trustedTypes;
      var probe = JSON.stringify(probeObj);
      verifyHtml = '<div class="verify-row">'
        + '<button class="verify-btn" data-probe=\'' + esc(probe) + '\' data-key="' + esc(key) + '">Verify in real Chrome</button>'
        + '<span class="verify-hint">loads the real page with the engine’s EXACT payload in a sandboxed attacker window — the sink firing <code>apiclientsink</code> is ground-truth REAL EXPLOIT (no fire → engine/Chrome divergence)</span>'
        + '<div class="verify-result" data-key="' + esc(key) + '"></div></div>';
    } else {
      verifyHtml = '<div class="verify-na">no page url recorded for this finding — live verify delivers the payload to the page the sink was observed on</div>';
    }

    // POLICY-RELATIVE severity, over BOTH policy facts the engine emits — see _policyBlockers for why reading
    // only `cspBlocks` here was the same live defect as reading a name the engine never wrote.
    var sevBadge = blockers.length
      ? '<span class="badge badge-medium" title="' + esc('broke out and fired in the model, but the page\'s own policy ('
          + _blockerNames(blockers) + ') kills THIS vector on real Chrome — the sink is REAL and needs a '
          + 'policy-permitted vector') + '">POLICY-BLOCKED · ' + esc(_blockerNames(blockers)) + '</span>'
      : '<span class="badge badge-high">HIGH</span>';
    html += '<div class="card" data-finding-key="' + esc(key) + '">'
      + '<div class="card-label"><span class="badge badge-xss">XSS PoC</span> ' + sevBadge + ' ' + esc(item.sink) + '</div>'
      + srcHtml + pocHtml + envHtml
      + '<div class="card-meta">' + srcLink + '</div>'
      + verifyHtml
      + '</div>';
  }

  // PARKED SEARCHES — reached, searched this far, not broken out of YET. Shown because the alternative is
  // silence, and silence reads as "nothing is here" for a sink attacker input demonstrably reaches. The card
  // states what constrained the search — the bytes the source's own component percent-encodes — which is the
  // fact that tells a reader what would change the answer (an app that decodes its fragment breaks out with a
  // candidate that is parked here). It is deliberately not a severity: there is no verdict to render.
  if (parked.length) {
    html += '<div class="section-header">Reached, search parked <span class="badge badge-status">' + parked.length + '</span></div>';
    for (var pi = 0; pi < parked.length; pi++) {
      var pe = parked[pi], pit = pe.item;
      DCHECK(pit.sink && pit.source && typeof pit.tried === "number",
        "a parked @S record reached the popup without sink/source/tried — solve_json_array emits all three, "
        + "and without `tried` the card would say '0 breakouts run' about a search that has run (sink="
        + pit.sink + " source=" + pit.source + ")");
      var pEnc = _encodeSentence(pit);   // already HTML — see _encodeSentence
      var pSrc = pe.sourceUrl && /^https?:\/\//i.test(pe.sourceUrl)
        ? '<a href="' + esc(pe.sourceUrl) + '" target="_blank" title="' + esc(pe.sourceUrl) + '">' + esc(pe.srcLabel) + '</a>'
        : esc(pe.srcLabel);
      html += '<div class="card">'
        + '<div class="card-label"><span class="badge badge-status" title="an attacker source reaches this sink; no breakout has fired yet — this is NOT a finding that the sink is safe">PARKED</span> '
        + esc(pit.sink) + ' &larr; <code>' + esc(pit.source) + '</code></div>'
        // No `|| 0` beside `tried`: the DCHECK above states it is a number, and 0 is a real value this card
        // must be able to say (a sink reached but not yet searched) rather than one a default manufactures.
        + '<div class="card-dims">' + esc(String(pit.tried)) + ' breakout' + (pit.tried === 1 ? "" : "s")
        + ' run, none fired'
        + (pEnc ? ' — ' + pEnc : "")
        + '</div>'
        // The parked entry carries the SOURCE DECLARATION, not an envelope: there is no PoC yet, so there is
        // no firing vector or policy verdict to state about one. What it can say is how the attacker would
        // have to reach the victim if a candidate ever fires — which is the same declared fact the fired card
        // renders, from the same field.
        + '<div class="card-dims">' + esc(_deliverySentence(pit)) + '</div>'
        + '<div class="card-meta">' + pSrc + '</div>'
        + '</div>';
    }
  }

  container.innerHTML = html;
  container.querySelectorAll(".verify-btn").forEach(function (b) { b.addEventListener("click", function () { _handleVerify(b); }); });
}

// ── ENGINE-AGREEMENT live verify ─────────────────────────────────────────────
// The offscreen builds pocJs = the engine's EXACT poc (X9 -> apiclientsink) delivered to the REAL page via
// its source. We embed the sandboxed attacker page (poc-sandbox.html) and postMessage it the pocJs; the
// user clicks Run inside it (real user gesture for window.open). When the real sink fires, intercept.js's
// apiclientsink relays a hit keyed by the session marker -> REAL EXPLOIT (Chrome-confirmed).
var _verifySandboxes = new Map();   // pocId -> {pocJs, marker, resultEl}
var _verifyIdSeq = 0;
window.addEventListener("message", function (e) {
  var d = e.data;
  if (!d || typeof d !== "object" || (d.type !== "POC_READY" && d.type !== "POC_RAN")) return;
  for (var ent of _verifySandboxes.values()) {
    var ifr = document.querySelector('iframe[data-verify-id="' + ent.pocId + '"]');
    if (!ifr || ifr.contentWindow !== e.source) continue;
    // NO try/catch AROUND THE HANDOFF. This posts a plain object to a same-origin extension frame THIS view
    // created and whose contentWindow it has just identified, so the only way it throws is a bug in that
    // identification — and swallowing it left the sandbox waiting for a POC_SETUP that never came, which
    // reads to the user as a live-verify that simply never answers.
    if (d.type === "POC_READY") { e.source.postMessage({ type: "POC_SETUP", pocJs: ent.pocJs, marker: ent.marker }, "*"); }
    else if (d.type === "POC_RAN" && ent.resultEl) {
      ent.resultEl.textContent = d.error ? "PoC threw in the sandbox: " + d.error : "payload delivered — waiting for the sink to fire in Chrome…";
      _pollVerify(ent.resultEl, ent.marker, ent.blockers);
    }
    return;
  }
});
async function _handleVerify(btn) {
  // THE PROBE IS THIS VIEW'S OWN JSON, so a parse failure here is this file disagreeing with itself — never a
  // page state. `probe = {}` on the catch built a probe with no poc and no pageUrl and sent it anyway, and
  // startExploitProbe's "need the engine's poc" throw came back as a build error about the finding rather
  // than about the attribute that failed to round-trip.
  DCHECK(typeof btn.dataset.probe === "string" && btn.dataset.probe.length > 0,
         "a verify button carries no data-probe — renderSecurityPanel writes one on every button it creates, "
         + "so an empty one is that attribute failing to survive the innerHTML round trip");
  var probe = JSON.parse(btn.dataset.probe);
  var card = btn.closest(".card");
  var resultEl = card ? card.querySelector(".verify-result") : null;
  if (!resultEl) return;
  btn.disabled = true; var prev = btn.textContent; btn.textContent = "Building…"; resultEl.textContent = "Building the delivery from the engine’s poc…";
  try {
    var start = await new Promise(function (res) { chrome.runtime.sendMessage(Object.assign({ type: "EXPLOIT_PROBE_START", waitMs: 6000 }, probe), function (r) { res(r); }); });
    // NO pocJs IS AN ANSWER, AND `pocWhy` IS THAT ANSWER. The delivery layer states which engine-declared
    // mechanism it cannot perform (a planted cookie, an attacker-served referrer, a user-supplied file); the
    // finding itself stands — the engine fire-verified the breakout. Reporting only "no pocJs" read as a
    // broken build, which is a different claim from "this vector is not deliverable from a sandbox".
    if (!start || start.error || !start.pocJs) {
      resultEl.textContent = "not deliverable from this sandbox: "
        + ((start && (start.error || start.pocWhy)) || "the offscreen returned no PoC and no reason — an engine↔host contract gap");
      btn.disabled = false; btn.textContent = prev; return;
    }
    var pocId = "v" + (_verifyIdSeq++);
    // The probe carries the engine's policy facts (present only when the engine stated them), so the ONE
    // _policyBlockers reading serves the card and the verify verdict alike. Two spellings of "did the page's
    // policy kill this" is how the badge and the envelope came to disagree in the first place.
    _verifySandboxes.set(start.sessionId, { pocId: pocId, pocJs: start.pocJs, marker: start.sessionId,
                                            resultEl: resultEl, blockers: _policyBlockers(probe) });
    var ifr = document.createElement("iframe");
    ifr.setAttribute("data-verify-id", pocId);
    ifr.src = "poc-sandbox.html";
    ifr.style.cssText = "width:100%;height:120px;border:1px solid var(--border,#444);border-radius:6px;margin-top:6px;";
    resultEl.textContent = "click Run PoC inside the sandbox below (your click is the user gesture window.open needs):";
    resultEl.parentNode.appendChild(ifr);
    btn.textContent = prev; btn.disabled = false;
  } catch (err) {
    RETHROW_FATAL(err);   // an invariant abort is never reported as a verify that went wrong
    resultEl.textContent = "verify error: " + (err && err.message || err); btn.disabled = false; btn.textContent = prev;
  }
}
async function _pollVerify(resultEl, marker, blockers) {
  for (var i = 0; i < 20; i++) {
    await new Promise(function (r) { setTimeout(r, 400); });
    var snap = await new Promise(function (res) { chrome.runtime.sendMessage({ type: "EXPLOIT_PROBE_STATUS", sessionId: marker }, function (r) { res(r); }); });
    // A session that has expired out of the offscreen's LRU answers {error}; that is a real state and the
    // loop keeps polling the remaining attempts rather than claiming anything about the sink.
    if (!snap || snap.error) continue;
    // `hits` IS THE ONLY EVIDENCE, and `executed` NEVER EXISTED. This line read a second vocabulary —
    // `snap.executed`, an object of per-payload booleans — as a fallback source of "did it fire". Nothing in
    // this extension has ever WRITTEN it: startExploitProbe builds the session with {marker, status, pageUrl,
    // findingId, sourceUrl, sinkName, waitMs, hits, createdAt, finishedAt, error} and PROBE_HIT only ever
    // pushes onto `hits`, so the reply's own producer wrote `executed: ses.executed || null` — a default over
    // a field with no writer, which is what kept a dead vocabulary looking live. It is the `canVerify`/`shape`
    // defect exactly: a reader whose writer does not exist, hidden by a default. Deleted on both sides, and
    // `hits` is asserted rather than defaulted so the day EXPLOIT_PROBE_STATUS stops carrying it this crashes
    // instead of reporting NOT REPRODUCED for every finding forever.
    DCHECK(Array.isArray(snap.hits),
           "EXPLOIT_PROBE_STATUS answered without a hits array — the probe session is created with hits:[] "
           + "and PROBE_HIT only appends to it, so its absence is that reply broken and every live verify "
           + "would report NOT REPRODUCED no matter what real Chrome did");
    if (snap.hits.length) {
      resultEl.className = "verify-result verify-hit";
      // A FIRE THE ENGINE PREDICTED WOULD BE BLOCKED IS A DIVERGENCE, NOT AN AGREEMENT. §LIVE-VERIFY reads a
      // hit as ground truth, and it is — but "engine agrees with Chrome" is a claim about the MODEL, and the
      // model said this vector was dead. Saying it agreed there would hide a wrong policy read behind the
      // best-looking result the panel can print.
      resultEl.textContent = blockers.length
        ? "REAL EXPLOIT — apiclientsink fired in real Chrome. But the engine predicted this vector was dead ("
          + _blockerNames(blockers) + "), so its policy read DIVERGES from Chrome here: the exploit is real and "
          + "the model that called it blocked is wrong (an engine-fidelity bug to investigate)."
        : "REAL EXPLOIT — the engine’s payload fired the sink in real Chrome (apiclientsink relayed). Engine agrees with Chrome.";
      return;
    }
  }
  resultEl.className = "verify-result verify-miss";
  // POLICY-RELATIVE no-fire: when the engine already flagged the page's own policy — a CSP, a Trusted-Types
  // requirement, or both — as killing THIS vector, a non-fire is the EXPECTED, confirmed outcome (real sink,
  // dead vector), not an engine-fidelity divergence to chase. The last arm does not OFFER "CSP/Trusted-Types"
  // as a possibility: the engine answered both questions on the record, so an empty blocker list means both
  // are a No and the remaining explanation is a divergence. A no-fire is NEVER "safe" either — the sink stays
  // REAL and the search stays open.
  resultEl.textContent = blockers.length
    ? "BLOCKED AS PREDICTED (" + _blockerNames(blockers) + ") — apiclientsink never fired, and the engine said "
      + "this vector is dead on the real page: "
      + blockers.map(function (b) {
          return b.what === "CSP"
            ? "the page CSP blocks it (" + b.detail + ")"
            : "the document requires a trusted type for the '" + b.detail + "' sink group, so the assignment "
              + "throws before the payload is parsed";
        }).join("; and ")
      + ". The sink is REAL; it needs a policy-permitted vector. A policy-relative result, NOT an "
      + "engine-fidelity bug — and NOT a statement that the sink is safe."
    : "NOT REPRODUCED — apiclientsink never fired, and the engine reported neither a blocking CSP nor a Trusted-Types requirement for this vector, so the engine’s model diverges from Chrome here (an engine-fidelity bug to investigate). Not a statement that the sink is safe.";
}
