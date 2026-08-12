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
    // "analyzing" requires ACTUAL forward motion, not just a fiber sitting in
    // the queue: a queued-but-not-resumed fiber with stale progress IS the
    // stall (observed: rem 1713, queued 1, running 0, msSinceProgress 85s — a
    // fiber parked, never resumed, no @DD advancing). So progress recency
    // (running OR recent progress) is the liveness signal; a stale queue alone
    // is NOT liveness. running>0 means a grind callMain is mid-flight (always
    // live); recent progress means @DD advanced within STALE_MS.
    var live = (running > 0) || (sinceProgress < STALE_MS);
    var state;
    if (rem === 0) state = "complete";
    else if (rem < 0) state = live ? "analyzing" : "unknown";
    // rem<0 = the grind hasn't reported a remaining count yet (total may be
    // known from @DTOTAL before the first @DS). NOT "stalled" (no rem to say
    // work-is-left-and-idle) — "analyzing" if live, else "unknown".
    else if (live) state = "analyzing";
    else state = "stalled";   // concrete rem>0, no forward motion (queued-but-parked counts here)
    var driven = (total >= 0 && rem >= 0) ? (total - rem) : -1;
    var head = (typeof c.head === "number") ? c.head : -1;
    /* headDone = the page's HIGH-VALUE head (net-reaching/endpoint orphans,
       sorted first) is fully driven → the continuous-session scheduler can
       rotate to another page's head before grinding this page's completeness
       tail. true once driven >= head (head known), or at complete. */
    var headDone = (state === "complete") || (head >= 0 && driven >= 0 && driven >= head);
    return {
      state: state, rem: rem, total: total, head: head,
      driven: driven,
      pct: (total > 0 && rem >= 0) ? Math.floor(((total - rem) / total) * 100) : -1,
      headDone: headDone,
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
