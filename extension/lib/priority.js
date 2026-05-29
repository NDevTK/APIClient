/* Picker heuristics — ORDER only, never COVERAGE.
 *
 * Every function here decides WHICH pending work item to run NEXT. None of
 * them adds, removes, or skips a work item — they only sort. Deleting this
 * file would not lose any learned endpoint or security finding; the system
 * would simply explore the same work set in arbitrary order. That invariant
 * is what allows tuning/swapping these heuristics without correctness review:
 * a different comparator changes WHEN something is learned, not WHETHER.
 *
 * The functions are pure: they take observed signals (productivity delta,
 * reaches-host-edge bit, vts, recency, etc.) and return ordering. They do
 * not mutate, do not close over scheduler state, do not have side effects.
 *
 * Three comparators live here:
 *   flowCmp(a, b, activePageKey)
 *     — paused-JSPI-fiber ordering. Used by ast-thread.js _yieldDrain to
 *       pick the next fiber to resume across all suspended page analyses.
 *   schedCmp(a, b)
 *     — BFS schedule ordering. Used by ast-thread.js _pickJob to pick the
 *       next decision-string schedule to run from the work queue.
 *   deepRoundCmp(a, b)
 *     — cross-page resumable-grind ordering. Used by _resumeIncompleteDeep
 *       to pick the next page-grind to advance.
 *
 * The engine-side qjs_deep_relcmp (orphan @T function ordering inside the
 * deep grind) lives in C — it can't share this file but follows the same
 * discipline: lexicographic dimensions, no weighted sums, no magic numbers.
 *
 * Loaded by ast-thread.js via importScripts; attaches comparators to self.
 */

self._priorityCmp = {
  /* Paused JSPI fiber comparator. Negative result = a wins (run first).
     Lexicographic, no weighted sums. Both @H (endpoints) and @S (sinks)
     come from the SAME forced execution — one engine, two views per
     CLAUDE.md "The engine is one execution, two views" — but the
     ordering reflects CLAUDE.md's goal-priority list (endpoint
     completeness is goal #1, security finding detection is goal #10),
     so when two fibers each emitted records this cycle the endpoint
     emitter resumes first:
       1. Active-page focus (the user's current page wins all ties)
       2. Reaches host edge from current pc (engine-emitted reaches bit)
       3. Recent ENDPOINT emissions (@H — fetch / XHR / WebSocket /
          sendBeacon). Outranks one emitting structural candidates (@T)
          or security sinks (@S) of equal count — same execution still
          captures both, just resumes the endpoint-producing fiber's
          continuation first.
       4. Recent SECONDARY emissions (@T + @S). Beats a fiber emitting
          nothing — keeps sink-discovery moving when no endpoints are
          coming out — but loses to ANY endpoint emitter.
       5. Page visit recency (most-recently-viewed wins)
       6. Anti-starvation (oldest-waiting breaks ties so nothing starves) */
  flowCmp: function (a, b, activePageKey) {
    var aFoc = (activePageKey && a.ctx && a.ctx.pageKey === activePageKey) ? 1 : 0;
    var bFoc = (activePageKey && b.ctx && b.ctx.pageKey === activePageKey) ? 1 : 0;
    if (aFoc !== bFoc) return bFoc - aFoc;
    var aR = a.reaches ? 1 : 0, bR = b.reaches ? 1 : 0;
    if (aR !== bR) return bR - aR;
    var aEp = a.recentEndpoints | 0, bEp = b.recentEndpoints | 0;
    if (aEp !== bEp) return bEp - aEp;
    var aSec = a.recentSecondary | 0, bSec = b.recentSecondary | 0;
    if (aSec !== bSec) return bSec - aSec;
    var aV = (a.ctx && a.ctx.vts) || 0, bV = (b.ctx && b.ctx.vts) || 0;
    if (aV !== bV) return bV - aV;
    return a.ts - b.ts;
  },

  /* BFS schedule comparator. Negative result = a wins. Input shape:
       { sched: <decision-string>, key: <edge-key>, deep: <bool>,
         frontierSig: <int>, productivity: <int>, ts: <enqueue ms> }
     OR a bare string (legacy seed schedule, treated as all-zero —
     lowest priority + earliest enqueue tiebreak). Algorithmic priority
     — no depth/length heuristic. Lexicographic:
       1. `deep` bypass (frontiers from a fruitful run — emitted new
          @H/@S/@T). Boolean carry-over from the parent run's
          productivity check.
       2. Frontier reach-signature (higher first) — the engine's STATIC
          prediction of this specific frontier's flipped arm: 2 = the
          flip explores an arm reaching a NETWORK edge (an endpoint,
          goal #1), 1 = reaches a host edge that isn't a net one
          (sink-ish), 0 = unknown. This is "pick the function-internal
          code path most likely to be an endpoint NEXT" — intra-function
          priority, not FIFO over a function's branches.
       3. Parent-run productivity (higher first) — the OBSERVED signal:
          the PARENT schedule of this frontier emitted N new @H/@S/@T,
          so this lineage is producing. Comes after the per-frontier
          static signature because the signature is specific to THIS
          branch's arm while productivity is about the lineage.
       4. Enqueue timestamp (oldest first — anti-starvation). */
  schedCmp: function (a, b) {
    var aDeep = a && a.deep ? 1 : 0, bDeep = b && b.deep ? 1 : 0;
    if (aDeep !== bDeep) return bDeep - aDeep;
    var aS = (a && typeof a.frontierSig === "number") ? a.frontierSig : 0;
    var bS = (b && typeof b.frontierSig === "number") ? b.frontierSig : 0;
    if (aS !== bS) return bS - aS;
    var aP = (a && typeof a.productivity === "number") ? a.productivity : 0;
    var bP = (b && typeof b.productivity === "number") ? b.productivity : 0;
    if (aP !== bP) return bP - aP;
    var aT = (a && typeof a.ts === "number") ? a.ts : 0;
    var bT = (b && typeof b.ts === "number") ? b.ts : 0;
    return aT - bT;
  },

  /* Cross-page deep-grind rotation comparator. Input shape: an IDB prog
     record { key, vts, ts, driven }. Most-recently-VIEWED page first
     (vts is the visit/activation timestamp; ts is the per-batch update
     time which would unfairly bump a slowly-grinding old page). */
  deepRoundCmp: function (a, b) {
    var aV = (a && (a.vts || a.ts)) || 0;
    var bV = (b && (b.vts || b.ts)) || 0;
    return bV - aV;
  },

  /* Brain-side review-queue picker. Input is the array of pending tabIds and
     a tab → metadata lookup (so the comparator stays pure). Picks the tab
     the user activated MOST RECENTLY — switching to a tab while another is
     analyzing means the new tab's results are what the user is waiting for,
     so it jumps ahead of older queued tabs. Returns the picked tabId and
     splices it out of the queue (mutation here is necessary since picking
     from a queue is the operation; the COMPARISON itself is still pure). */
  pickFromReviewQueue: function (queue, getLastActivatedTs) {
    if (!queue || queue.length === 0) return null;
    var bestI = 0;
    var bestTs = (getLastActivatedTs(queue[0]) | 0);
    for (var k = 1; k < queue.length; k++) {
      var ts = (getLastActivatedTs(queue[k]) | 0);
      if (ts > bestTs) { bestI = k; bestTs = ts; }
    }
    var tabId = queue[bestI];
    queue.splice(bestI, 1);
    return tabId;
  },

  /* JSPI paused-fiber picker. Same role as pickFromReviewQueue but for the
     wasm-fiber scheduler — picks the highest-priority paused fiber from
     `_fiberQ` according to flowCmp. Splices it out of the queue and returns
     it. Caller invokes the fiber's `wake` callback (Promise resolve for the
     one-worker JSPI model; postMessage signal for a hypothetical multi-
     worker setup — see flowCmp's identity-based comparison: it doesn't read
     the wake handle, so this picker works regardless of how `wake` is
     materialized). Mutates `queue` because picking IS the operation. */
  pickFromFiberQueue: function (queue, activePageKey) {
    if (!queue || queue.length === 0) return null;
    var self_ = this;
    var bestI = 0;
    for (var k = 1; k < queue.length; k++) {
      if (self_.flowCmp(queue[k], queue[bestI], activePageKey) < 0) bestI = k;
    }
    var fiber = queue[bestI];
    queue.splice(bestI, 1);
    return fiber;
  },
};
