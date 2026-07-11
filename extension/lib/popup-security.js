/* Popup security panel — extracted from popup.js (classic script, shares the popup global scope + DOM).
   Renders the @S securityFindings (source -> sink -> poc) and drives live-verify: _handleVerify opens the
   sandboxed attacker popup, _pollVerify + the message listener report REAL EXPLOIT / NOT REPRODUCED. */
// ─── Security Panel ──────────────────────────────────────────────────────────

// Stable-ish key for a finding card within a tab. The engine emits a working PoC
// per (sink, source-shape); key by that + source url.
function _findingKey(entry) {
  const it = entry.item || {};
  return (entry.sourceUrl || "(inline)") + "|" + (it.sink || it.type || "?") + "|" + (it.shape || "") + "|" + (it.poc || "");
}


function renderSecurityPanel() {
  const container = document.getElementById("security-findings");
  const empty = document.getElementById("security-empty");

  const findings = tabData?.securityFindings || [];
  // Each securitySink IS a WORKING PoC: the engine emits it ONLY when a concrete
  // candidate, driven through the real code + branches + filters, BROKE OUT at the
  // sink (self-verifying by replay). There is no severity/verdict to compute — a
  // proven XSS exploit is HIGH, full stop; absence of a finding is NOT "safe".
  let secCount = 0;
  for (let i = 0; i < findings.length; i++) secCount += (findings[i].securitySinks || []).length;
  const fp = findings.length + ":" + secCount;
  if (fp === _lastSecFp) return;
  _lastSecFp = fp;

  container.innerHTML = "";

  var allItems = [];
  for (var fi = 0; fi < findings.length; fi++) {
    var f = findings[fi];
    var srcLabel = f.sourceUrl ? _shortUrl(f.sourceUrl) : "(unknown)";
    for (var si = 0; si < (f.securitySinks || []).length; si++)
      allItems.push({ item: f.securitySinks[si], sourceUrl: f.sourceUrl, srcLabel: srcLabel, pageUrl: f.pageUrl });
  }

  if (!allItems.length) { empty.style.display = "block"; return; }
  empty.style.display = "none";

  var html = '<div class="section-header">Working XSS PoCs <span class="badge badge-status">' + allItems.length + '</span></div>';

  for (var i = 0; i < allItems.length; i++) {
    var entry = allItems[i];
    var item = entry.item;

    var srcLink = entry.sourceUrl && /^https?:\/\//i.test(entry.sourceUrl)
      ? '<a href="' + esc(entry.sourceUrl) + '" target="_blank" title="' + esc(entry.sourceUrl) + '">' + esc(entry.srcLabel) + '</a>'
      : esc(entry.srcLabel);
    if (entry.pageUrl && entry.pageUrl !== entry.sourceUrl)
      srcLink += ' <span class="page-context" title="' + esc(entry.pageUrl) + '">in ' + esc(_shortUrl(entry.pageUrl)) + '</span>';

    // shape = which external source(s) reach the sink (e.g. "<h1>{hash}"); poc = the
    // exact input that breaks out; evidence = one-line proof from the forced-exec run.
    var shapeHtml = item.shape ? '<div class="card-value" title="external source(s) that reach this sink, transforms flattened">source&nbsp;shape: <code>' + esc(item.shape) + '</code></div>' : "";
    var pocHtml = item.poc
      ? '<div class="card-poc"><span class="poc-lbl">breakout input</span> <code class="poc-payload">' + esc(item.poc) + '</code></div>'
      : "";
    var evHtml = item.evidence ? '<div class="card-dims">' + esc(item.evidence) + '</div>' : "";

    // ENGINE AGREEMENT verify: run the engine's EXACT poc against the REAL page in a sandboxed attacker
    // window; the sink firing apiclientsink is ground truth. Only offered when the source is client-
    // deliverable (hash/search/pm) and we know the page url.
    var key = _findingKey(entry);
    var canVerify = item.poc && item.shape && /\{(hash|search|pm)\}/.test(item.shape) && entry.pageUrl;
    var verifyHtml = "";
    if (canVerify) {
      var probe = JSON.stringify({ poc: item.poc, shape: item.shape, srcpath: item.srcpath, gatefields: item.gatefields, sinkName: item.sink, sourceUrl: entry.sourceUrl, pageUrl: entry.pageUrl, findingId: key, cspBlocked: !!item.cspBlocked, cspReason: item.cspReason || "" });
      verifyHtml = '<div class="verify-row">'
        + '<button class="verify-btn" data-probe=\'' + esc(probe) + '\' data-key="' + esc(key) + '">Verify in real Chrome</button>'
        + '<span class="verify-hint">loads the real page with the engine’s EXACT payload in a sandboxed attacker window — the sink firing <code>apiclientsink</code> is ground-truth REAL EXPLOIT (no fire → engine/Chrome divergence)</span>'
        + '<div class="verify-result" data-key="' + esc(key) + '"></div></div>';
    } else if (item.poc) {
      verifyHtml = '<div class="verify-na">live-verify needs a client-deliverable source (hash/search/pm) + a page url</div>';
    }

    // POLICY-RELATIVE severity: a model breakout the page's CSP blocks on real Chrome is NOT a clean HIGH XSS —
    // it needs a policy-permitted vector. Show CSP-BLOCKED (medium) + the per-sink-vector reason from the engine.
    var sevBadge = item.cspBlocked
      ? '<span class="badge badge-medium" title="broke out in the model, but the page CSP blocks THIS vector on real Chrome — needs a policy-permitted vector">CSP-BLOCKED</span>'
      : '<span class="badge badge-high">HIGH</span>';
    var cspHtml = item.cspBlocked
      ? '<div class="card-dims"' + (item.csp ? ' title="' + esc(item.csp) + '"' : "") + '>⚠ ' + esc(item.cspReason || "the page CSP blocks this vector") + '</div>'
      : "";
    html += '<div class="card" data-finding-key="' + esc(key) + '">'
      + '<div class="card-label"><span class="badge badge-xss">XSS PoC</span> ' + sevBadge + ' ' + esc(item.sink || item.type || "?") + '</div>'
      + shapeHtml + pocHtml + evHtml + cspHtml
      + '<div class="card-meta">' + srcLink + '</div>'
      + verifyHtml
      + '</div>';
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
      _pollVerify(ent.resultEl, ent.marker, ent.cspBlocked, ent.cspReason);
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
    _verifySandboxes.set(start.sessionId, { pocId: pocId, pocJs: start.pocJs, marker: start.sessionId, resultEl: resultEl, cspBlocked: !!probe.cspBlocked, cspReason: probe.cspReason || "" });
    var ifr = document.createElement("iframe");
    ifr.setAttribute("data-verify-id", pocId);
    ifr.src = "poc-sandbox.html";
    ifr.style.cssText = "width:100%;height:120px;border:1px solid var(--border,#444);border-radius:6px;margin-top:6px;";
    resultEl.textContent = "click Run PoC inside the sandbox below (your click is the user gesture window.open needs):";
    resultEl.parentNode.appendChild(ifr);
    btn.textContent = prev; btn.disabled = false;
  } catch (err) { resultEl.textContent = "verify error: " + (err && err.message || err); btn.disabled = false; btn.textContent = prev; }
}
async function _pollVerify(resultEl, marker, cspBlocked, cspReason) {
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
  resultEl.textContent = cspBlocked
    ? "CSP BLOCKED as predicted — apiclientsink never fired because the page CSP blocks this vector (" + (cspReason || "policy-constrained") + "). The sink is REAL; it needs a policy-permitted vector. A policy-relative result, NOT an engine-fidelity bug."
    : "NOT REPRODUCED — apiclientsink never fired. Either CSP/Trusted-Types blocked it, or the engine’s model diverges from Chrome here (an engine-fidelity bug to investigate).";
}
