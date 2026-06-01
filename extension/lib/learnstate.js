// Single source of truth for the learning-state verdict — the honest
// "a finding is genuinely MISSING vs still being LOOKED FOR vs the priority
// frontier STALLED" classification. Shared (DRY) by:
//   • ast-thread.js  — the worker heartbeat + harness `learnstate` command
//   • popup.js       — the user-facing deep-scan status line
// so the harness and the UI never drift on what "complete" means.
//
// Pure function: given the per-page grind counters, returns a terminal verdict.
//   • "complete"  — rem===0: every orphan driven. A finding absent NOW is a
//                   genuine gap (or correctly-not-an-endpoint, e.g. a
//                   <include-fragment src> whose connectedCallback never
//                   fetched — NOT an endpoint, by design; reading .src to
//                   "find" it would be the banned static-attribute shortcut).
//   • "analyzing" — rem>0 AND (a grind is running OR a fiber is queued OR the
//                   grind progressed within the staleness window): still
//                   looking — absence is NOT yet a gap.
//   • "stalled"   — rem>0 but nothing running/queued AND no progress for the
//                   staleness window: the priority frontier failed to advance
//                   (the prioritization-bug class) — absence is a SCHEDULING
//                   failure to surface, distinct from a real coverage gap.
//   • "unknown"   — no grind has reported yet (BFS-only / pre-grind page).
//
// STALE_MS is a DIAGNOSTIC threshold (how long with work-left-and-idle counts
// as stalled), never an analysis cap — it bounds nothing the engine does.
(function (root) {
  var STALE_MS = 8000;
  function learningStateOf(c) {
    c = c || {};
    var rem = (typeof c.rem === "number") ? c.rem : -1;
    var total = (typeof c.total === "number") ? c.total : -1;
    var running = c.running | 0;
    var queued = c.queued | 0;
    var sinceProgress = (typeof c.msSinceProgress === "number" && c.msSinceProgress >= 0) ? c.msSinceProgress : Infinity;
    var state;
    if (rem === 0) state = "complete";
    else if (rem < 0) state = (running > 0 || queued > 0 || sinceProgress < STALE_MS) ? "analyzing" : "unknown";
    // rem<0 = the grind hasn't reported a remaining count yet (total may be
    // known from @DTOTAL before the first @DS). That is NOT "stalled" (we have
    // no rem to say work-is-left-and-idle) — it's "analyzing" if anything is
    // live, else "unknown". Only a CONCRETE rem>0 with nothing live = stalled.
    else if (running > 0 || queued > 0 || sinceProgress < STALE_MS) state = "analyzing";
    else state = "stalled";
    return {
      state: state, rem: rem, total: total,
      driven: (total >= 0 && rem >= 0) ? (total - rem) : -1,
      pct: (total > 0 && rem >= 0) ? Math.floor(((total - rem) / total) * 100) : -1,
      running: running, queued: queued
    };
  }
  // A short human label for the UI (popup), derived from the same verdict.
  function learningLabelOf(v) {
    if (!v) return "";
    switch (v.state) {
      case "complete": return "Deep scan complete — " + (v.total >= 0 ? v.total : "?") + " unused functions explored";
      case "analyzing": return "Deep scan: " + (v.driven >= 0 ? v.driven : "?") + "/" + (v.total >= 0 ? v.total : "?") +
        (v.pct >= 0 ? " (" + v.pct + "%)" : "") + " — learning hidden endpoints…";
      case "stalled": return "Deep scan STALLED at " + (v.driven >= 0 ? v.driven : "?") + "/" + (v.total >= 0 ? v.total : "?") +
        " — priority frontier not advancing (scheduling issue)";
      default: return "";
    }
  }
  var api = { learningStateOf: learningStateOf, learningLabelOf: learningLabelOf, STALE_MS: STALE_MS };
  if (typeof module !== "undefined" && module.exports) module.exports = api;
  root.LearnState = api;
})(typeof self !== "undefined" ? self : (typeof globalThis !== "undefined" ? globalThis : this));
