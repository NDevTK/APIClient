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
 * reaches-host-edge bit, virtual time, recency, etc.) and return ordering.
 * They do not mutate, do not close over scheduler state, do not have side
 * effects. flowCmp/flowWeight DO use weighted virtual time (WEIGHTED FAIR
 * QUEUEING) rather than strict lexicographic tiers — weights here allocate
 * CPU PROPORTIONALLY among runnable fibers, which is still ORDER ONLY (they
 * change WHEN a flow runs, never WHETHER any work item is covered). schedCmp
 * stays lexicographic (no weighted sums) because it orders a discrete
 * frontier set, not a continuous CPU share.
 *
 * Three comparators live here:
 *   flowCmp(a, b, activePageKey)  [+ flowWeight(entry)]
 *     — paused-JSPI-fiber ordering by WEIGHTED FAIR QUEUEING. Used by
 *       ast-thread.js to pick the next fiber to resume across all suspended
 *       page analyses; the scheduler advances each flow's virtual time by
 *       sliceCost / flowWeight after each resume.
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
  /* Paused JSPI fiber comparator — WEIGHTED FAIR QUEUEING. Negative result =
     a wins (run first). This is NOT strict lexicographic priority: the old
     design made `recentEndpoints` a STRICT tier, so a background page emitting
     1 more endpoint per cycle than another could monopolize the CPU and leave
     the other only anti-starvation crumbs. WFQ instead gives every runnable
     flow a CPU share PROPORTIONAL to its weight (= marginal value), so a
     flatter page still makes proportional progress while a productive page
     gets correspondingly more. Tiers:
       1. Active-page focus, STRICT but ONLY DURING HIGH-VALUE WORK — the page
          the user is ON wins every comparison WHILE it is still surfacing
          endpoints (a live review, or a deep grind whose net-reaching HEAD is
          not yet done: `ctx.headDone !== true`). This keeps the current page
          INSTANT for what matters (its endpoints) without letting its LOW-value
          completeness TAIL freeze the rest of the session: once the foreground
          page's head is done, its tail drops to plain WFQ and competes on value
          like any background, so a freshly-opened background tab's HIGH-VOI head
          (big UCB explore bonus) can preempt the foreground's near-zero-value
          tail. The head/tail boundary is the engine's existing net-reaching
          prefix (headDone) — no magic threshold. (Among equal-focus fibers and
          all background fibers, WFQ decides.)
       2. Virtual time `ctx.flowVt` ASCENDING — the WFQ core. The flow that has
          received the least service RELATIVE TO ITS WEIGHT (smallest virtual
          time) runs next. The scheduler (ast-thread.js) advances a flow's
          flowVt by sliceCost / weight after each resume, where weight =
          flowWeight() below. Endpoint production (goal #1) and host-edge reach
          raise the weight, so a productive flow's virtual time grows slower ⇒
          it is picked more often ⇒ more CPU — proportionally, not absolutely.
       3. Anti-starvation (oldest-waiting `ts`) — tiebreak when two flows have
          equal virtual time (e.g. two freshly-created flows both at the
          system virtual time). */
  flowCmp: function (a, b, activePageKey) {
    // Focus is strict only while the active page is still doing HIGH-value work
    // (headDone !== true). Its low-value completeness tail competes by WFQ so a
    // background's high-VOI head can preempt it.
    var aFoc = (activePageKey && a.ctx && a.ctx.pageKey === activePageKey && a.ctx.headDone !== true) ? 1 : 0;
    var bFoc = (activePageKey && b.ctx && b.ctx.pageKey === activePageKey && b.ctx.headDone !== true) ? 1 : 0;
    if (aFoc !== bFoc) return bFoc - aFoc;
    var aVt = (a.ctx && typeof a.ctx.flowVt === "number") ? a.ctx.flowVt : 0;
    var bVt = (b.ctx && typeof b.ctx.flowVt === "number") ? b.ctx.flowVt : 0;
    if (aVt !== bVt) return aVt - bVt;   // smaller virtual time served first
    return a.ts - b.ts;
  },

  /* WFQ weight for a fiber = its EXPECTED MARGINAL VALUE toward the goals
     (value of information), estimated OPTIMISTICALLY under uncertainty so
     under-sampled work is explored, not starved. Higher weight ⇒
     proportionally more CPU. Composed of:
       - a FAIRNESS FLOOR of 1: every flow, even one currently producing
         nothing, has weight ≥ 1 so its virtual time still advances at a
         bounded rate and it is never permanently starved (the WFQ analogue of
         the old anti-starvation tier, but giving a real proportional share
         rather than only tiebreak crumbs).
       - a +1 HOST-EDGE-REACH bonus when the fiber's current pc can reach a
         host edge (engine `@Y reaches` bit): such a fiber is positioned to
         emit an endpoint/sink imminently, so it earns more CPU NOW.
       - the observed ENDPOINT RATE (`ctx.epRate`, an EWMA of @H emissions per
         resume maintained by the scheduler): the load-bearing EXPLOIT signal —
         a flow actively surfacing endpoints (goal #1) gets CPU in proportion
         to how fast it is surfacing them, and decays back toward the floor as
         it goes flat (so a page that has surfaced its surface yields CPU to a
         fresher one).
       - the UCB1 EXPLORE bonus (`ctx.exploreBonus` = √(2·ln N / nᵢ), N = total
         resumes, nᵢ = this flow's resumes; computed by the scheduler): the
         optimism-under-uncertainty term that makes a NEWLY-OPENED or rarely-
         run page the HIGHEST-value thing to sample (its productivity is
         unknown — exactly what we should explore), then SHRINKS as the page is
         sampled so weight converges to its true observed rate. This is what
         turns WFQ from greedy-exploit into productive continuous-session
         learning: a fresh tab gets a burst to discover its surface instead of
         languishing at the floor until it happens to emit. The √ and ln are
         the canonical UCB1 confidence radius, not tunable coverage magic —
         order only, never coverage. */
  flowWeight: function (entry) {
    // Value is RUN OUTPUT, never a static reachability prediction. A flow earns
    // weight from what executing it ACTUALLY emits — epRate, the EWMA of its @H
    // endpoint AND @S security/XSS output. The old "+reaches" term was a static
    // "can this reach a network edge" flag, which a static bit CANNOT answer (only
    // running the recursion/interprocedural calls can) and which is network-blind
    // to XSS (DOM/eval sinks aren't network edges) — a flag that can't do its job
    // isn't kept. Weight = epRate (exploit) + UCB-explore + a fairness floor.
    var epRate = (entry && entry.ctx && typeof entry.ctx.epRate === "number" && entry.ctx.epRate > 0) ? entry.ctx.epRate : 0;
    var explore = (entry && entry.ctx && typeof entry.ctx.exploreBonus === "number" && entry.ctx.exploreBonus > 0) ? entry.ctx.exploreBonus : 0;
    return 1 + epRate + explore;
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
    // No static reach-signature: "this frontier's flipped arm reaches a network/sink
    // edge" is the same broken reachability flag (only RUNNING the arm tells, and it
    // is network-blind to XSS) — dropped. Order by OBSERVED signal only: a `deep`
    // frontier (its parent run emitted new @H/@S/@T) first, then the parent's
    // measured productivity, then enqueue order (anti-starvation).
    var aDeep = a && a.deep ? 1 : 0, bDeep = b && b.deep ? 1 : 0;
    if (aDeep !== bDeep) return bDeep - aDeep;
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
