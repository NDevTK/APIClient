/* Popup security panel — extracted from popup.js (classic script, shares the popup global scope + DOM).
   Renders the @S securityFindings (source -> sink -> poc) and drives live-verify: _handleVerify opens the
   sandboxed attacker popup, _pollVerify + the message listener report REAL EXPLOIT / NOT REPRODUCED. */
// ─── Security Panel ──────────────────────────────────────────────────────────

// Stable-ish key for a finding card within a tab. The engine emits one record per
// (sink, source); key by that + the script the sink lives in.
function _findingKey(entry) {
  const it = entry.item || {};
  return (entry.sourceUrl || "(inline)") + "|" + (it.sink || "?") + "|" + (it.source || "") + "|" + (it.poc || "");
}


function renderSecurityPanel() {
  const container = document.getElementById("security-findings");
  const empty = document.getElementById("security-empty");

  const findings = tabData?.securityFindings || [];
  // A securitySink is one of TWO records (see the split below): a FIRED PoC — a concrete candidate driven
  // through the real code + branches + filters that BROKE OUT at the sink, self-verifying by replay — or a
  // PARKED search on a sink attacker input demonstrably reaches. Neither carries a verdict to compute: a
  // proven exploit is HIGH, full stop, and the absence of either record is NOT "safe".
  let secCount = 0;
  for (let i = 0; i < findings.length; i++) secCount += (findings[i].securitySinks || []).length;
  const fp = findings.length + ":" + secCount;
  if (fp === _lastSecFp) return;
  _lastSecFp = fp;

  // The container is NOT cleared here. A contract DCHECK below throws where the engine's record does not
  // carry what a card claims, and clearing first would leave the panel EMPTY on that throw — an empty Vulns
  // panel reads as "nothing is here", which is the "safe" verdict solve.h forbids, arrived at by a crash
  // instead of by a claim. The clear happens where a replacement is ready (the early return, and the final
  // assignment), so a failed render leaves the LAST honest cards standing beside a loud console @WHY.

  // TWO STATES, NEVER ONE LIST. The engine reports every sink an attacker source REACHES, and a sink whose
  // breakout has not been solved carries "search":"parked" instead of a poc. Rendering both as "Working XSS
  // PoCs" would badge a parked search HIGH with an empty payload — a claim the engine never made. They are
  // separated here for the same reason the engine emits them apart: a fired PoC is proof, a parked search is
  // an open lead, and neither is a statement that the sink is safe.
  var allItems = [], parked = [];
  for (var fi = 0; fi < findings.length; fi++) {
    var f = findings[fi];
    var srcLabel = f.sourceUrl ? _shortUrl(f.sourceUrl) : "(unknown)";
    for (var si = 0; si < (f.securitySinks || []).length; si++) {
      var it = f.securitySinks[si];
      var e = { item: it, sourceUrl: f.sourceUrl, srcLabel: srcLabel, pageUrl: f.pageUrl };
      (it && it.search === "parked" ? parked : allItems).push(e);
    }
  }

  if (!allItems.length && !parked.length) { container.innerHTML = ""; empty.style.display = "block"; return; }
  empty.style.display = "none";

  var html = allItems.length
    ? '<div class="section-header">Working XSS PoCs <span class="badge badge-status">' + allItems.length + '</span></div>'
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
    // writes {sink, source, poc, cspBlocks?} for a fired sink and {sink, source, search:"parked", tried,
    // sourceEncodes?} for a parked one — and nothing else. This card used to read `shape`, `evidence`,
    // `cspBlocked`, `cspReason` and `csp`: five names from a contract that no longer exists, so every card
    // silently dropped its source line AND its CSP verdict, and the live-verify button (gated on `shape`)
    // could not appear for any finding the engine has ever emitted. A bridge edge asserts its contract, per
    // CLAUDE.md §Architecture, so drift like that crashes where it is born instead of quietly rendering less.
    DCHECK(item.sink && item.source && item.poc,
      "an @S record reached the popup without sink/source/poc — solve_json_array emits all three for a fired "
      + "sink, so a card is about to claim a working PoC it cannot show (sink=" + item.sink + " source="
      + item.source + ")");

    var srcHtml = '<div class="card-value" title="the attacker-controlled source whose bytes reach this sink">'
      + 'source: <code>' + esc(item.source || "?") + '</code></div>';
    var pocHtml = item.poc
      ? '<div class="card-poc"><span class="poc-lbl">breakout input</span> <code class="poc-payload">' + esc(item.poc) + '</code></div>'
      : "";

    // §@S(d): EVERY PoC CARRIES ITS REPRODUCTION ENVELOPE — browser, needs-a-click, is-stored, required CSP/TT
    // state — because a payload without it is not reproducible by the person reading it. The engine's record
    // carries the CSP half and NOTHING ELSE, so the other three are stated as UNREPORTED rather than implied:
    // silence here would read as "auto-fires, no policy", and a reader who assumes that cannot reproduce a
    // click-gated or stored sink. ENGINE GAP — solve.c must emit, beside `poc`:
    //   • firesOn — the vector's real semantics (parse-time auto-fire vs interaction-required). The kind is
    //     ALREADY decided one line above the CSP check (POLICY_EVAL / POLICY_JAVASCRIPT_URL /
    //     POLICY_INLINE_HANDLER); it is computed and then thrown away.
    //   • stored — whether this is a two-stage plant+victim-load PoC (§@S(b)), with both deliveries.
    //   • trustedTypes — whether a TT policy gates this sink (browser/core/html/trusted_types.c models it).
    // Absence of `cspBlocks` IS a positive engine statement (policy_allows returned true), so that half is
    // reported as a fact, not as silence.
    var envHtml = '<div class="card-dims">reproduction envelope: '
      + (item.cspBlocks
          ? '<strong>CSP blocks this vector</strong> — <code>' + esc(item.cspBlocks) + '</code>'
          : 'the page CSP permits this vector')
      + ' · firing vector (auto vs needs-a-click), stored/two-stage and Trusted-Types state are NOT YET REPORTED'
      + ' by the engine — their absence is not a statement that the PoC auto-fires under no policy</div>';

    // ENGINE AGREEMENT verify: run the engine's EXACT poc against the REAL page in a sandboxed attacker
    // window; the sink firing apiclientsink is ground truth. WHETHER the source is client-deliverable is
    // decided by the layer that BUILDS the delivery (startExploitProbe -> buildPocFromShape), never
    // re-decided here. The old second predicate asked `/\{(hash|search|pm)\}/` of a record whose source
    // reads `location.hash`, so it was false for every finding and the card printed a reason ("needs a
    // client-deliverable source") that was not the real one. One decision, in the layer that owns it; its
    // own error is what the card reports when the delivery cannot be built.
    var key = _findingKey(entry);
    var verifyHtml = "";
    if (item.poc && entry.pageUrl) {
      var probe = JSON.stringify({ poc: item.poc, source: item.source, srcpath: item.srcpath, gatefields: item.gatefields, sinkName: item.sink, sourceUrl: entry.sourceUrl, pageUrl: entry.pageUrl, findingId: key, cspBlocks: item.cspBlocks || "" });
      verifyHtml = '<div class="verify-row">'
        + '<button class="verify-btn" data-probe=\'' + esc(probe) + '\' data-key="' + esc(key) + '">Verify in real Chrome</button>'
        + '<span class="verify-hint">loads the real page with the engine’s EXACT payload in a sandboxed attacker window — the sink firing <code>apiclientsink</code> is ground-truth REAL EXPLOIT (no fire → engine/Chrome divergence)</span>'
        + '<div class="verify-result" data-key="' + esc(key) + '"></div></div>';
    } else if (item.poc) {
      verifyHtml = '<div class="verify-na">no page url recorded for this finding — live verify delivers the payload to the page the sink was observed on</div>';
    }

    // POLICY-RELATIVE severity: a model breakout the page's CSP blocks on real Chrome is NOT a clean HIGH XSS —
    // it needs a policy-permitted vector. The engine states that by emitting the POLICY that blocks it.
    var sevBadge = item.cspBlocks
      ? '<span class="badge badge-medium" title="broke out in the model, but the page CSP blocks THIS vector on real Chrome — needs a policy-permitted vector">CSP-BLOCKED</span>'
      : '<span class="badge badge-high">HIGH</span>';
    html += '<div class="card" data-finding-key="' + esc(key) + '">'
      + '<div class="card-label"><span class="badge badge-xss">XSS PoC</span> ' + sevBadge + ' ' + esc(item.sink || "?") + '</div>'
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
      var pSrc = pe.sourceUrl && /^https?:\/\//i.test(pe.sourceUrl)
        ? '<a href="' + esc(pe.sourceUrl) + '" target="_blank" title="' + esc(pe.sourceUrl) + '">' + esc(pe.srcLabel) + '</a>'
        : esc(pe.srcLabel);
      html += '<div class="card">'
        + '<div class="card-label"><span class="badge badge-status" title="an attacker source reaches this sink; no breakout has fired yet — this is NOT a finding that the sink is safe">PARKED</span> '
        + esc(pit.sink || "?") + ' &larr; <code>' + esc(pit.source || "?") + '</code></div>'
        + '<div class="card-dims">' + esc(String(pit.tried || 0)) + ' breakout' + (pit.tried === 1 ? "" : "s")
        + ' run, none fired'
        + (pit.sourceEncodes
            ? ' — the browser percent-encodes <code>' + esc(pit.sourceEncodes) + '</code> in this source, so a candidate needing those bytes arrives escaped'
            : "")
        + '</div>'
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
    if (d.type === "POC_READY") { try { e.source.postMessage({ type: "POC_SETUP", pocJs: ent.pocJs, marker: ent.marker }, "*"); } catch (_) {} }
    else if (d.type === "POC_RAN" && ent.resultEl) {
      ent.resultEl.textContent = d.error ? "PoC threw in the sandbox: " + d.error : "payload delivered — waiting for the sink to fire in Chrome…";
      _pollVerify(ent.resultEl, ent.marker, ent.cspBlocks);
    }
    return;
  }
});
async function _handleVerify(btn) {
  var probe = {}; try { probe = JSON.parse(btn.dataset.probe || "{}"); } catch (_) {}
  var card = btn.closest(".card");
  var resultEl = card ? card.querySelector(".verify-result") : null;
  if (!resultEl) return;
  btn.disabled = true; var prev = btn.textContent; btn.textContent = "Building…"; resultEl.textContent = "Building the delivery from the engine’s poc…";
  try {
    var start = await new Promise(function (res) { chrome.runtime.sendMessage(Object.assign({ type: "EXPLOIT_PROBE_START", waitMs: 6000 }, probe), function (r) { res(r); }); });
    if (!start || start.error || !start.pocJs) { resultEl.textContent = "cannot build a client-deliverable PoC: " + ((start && start.error) || "no pocJs"); btn.disabled = false; btn.textContent = prev; return; }
    var pocId = "v" + (_verifyIdSeq++);
    _verifySandboxes.set(start.sessionId, { pocId: pocId, pocJs: start.pocJs, marker: start.sessionId, resultEl: resultEl, cspBlocks: probe.cspBlocks || "" });
    var ifr = document.createElement("iframe");
    ifr.setAttribute("data-verify-id", pocId);
    ifr.src = "poc-sandbox.html";
    ifr.style.cssText = "width:100%;height:120px;border:1px solid var(--border,#444);border-radius:6px;margin-top:6px;";
    resultEl.textContent = "click Run PoC inside the sandbox below (your click is the user gesture window.open needs):";
    resultEl.parentNode.appendChild(ifr);
    btn.textContent = prev; btn.disabled = false;
  } catch (err) { resultEl.textContent = "verify error: " + (err && err.message || err); btn.disabled = false; btn.textContent = prev; }
}
async function _pollVerify(resultEl, marker, cspBlocks) {
  for (var i = 0; i < 20; i++) {
    await new Promise(function (r) { setTimeout(r, 400); });
    var snap = await new Promise(function (res) { chrome.runtime.sendMessage({ type: "EXPLOIT_PROBE_STATUS", sessionId: marker }, function (r) { res(r); }); });
    if (!snap || snap.error) continue;
    var hits = (snap.hits || []).length || (snap.executed ? Object.keys(snap.executed).filter(function (k) { return snap.executed[k]; }).length : 0);
    if (hits) { resultEl.className = "verify-result verify-hit"; resultEl.textContent = "REAL EXPLOIT — the engine’s payload fired the sink in real Chrome (apiclientsink relayed). Engine agrees with Chrome."; return; }
  }
  resultEl.className = "verify-result verify-miss";
  // POLICY-RELATIVE no-fire: when the engine already flagged the page CSP as blocking THIS vector, a non-fire
  // is the EXPECTED, confirmed outcome (real sink, dead vector) — not an engine-fidelity divergence to chase.
  // A no-fire is NEVER "safe" either — the sink stays REAL and the search stays open.
  resultEl.textContent = cspBlocks
    ? "CSP BLOCKED as predicted — apiclientsink never fired because the page CSP blocks this vector (" + cspBlocks + "). The sink is REAL; it needs a policy-permitted vector. A policy-relative result, NOT an engine-fidelity bug."
    : "NOT REPRODUCED — apiclientsink never fired. Either CSP/Trusted-Types blocked it, or the engine’s model diverges from Chrome here (an engine-fidelity bug to investigate). Not a statement that the sink is safe.";
}
