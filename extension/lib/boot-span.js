/* lib/boot-span.js — where a document's wall time went before its engine instance stepped.

   The trusted zone marks each phase of a document's boot at the event itself, keyed by the document's name.
   The marks are a diagnostic readout only: wall time is read here and handed to nothing that accounts,
   ranks, admits or asserts, and no engine statement is ever recorded as a phase time. A phase is marked at
   its first occurrence (a re-delivered seed or a later step does not move it). A document's marks live as
   long as the zone's other per-document records, the offscreen's session. */

/* In boot order. `firstStep` is the dispatch the engine's own `instanceUs` span opens at. */
const BOOT_SPAN_PHASES = Object.freeze([
  "seed",           // offscreen-brain.js admitted the document's CONTENT_SEED
  "loaded",         // bridge.js's navigationLoad of the seed settled
  "queued",         // the document joined `_waiting` for an instance
  "provision",      // engineCreate asked renderer-host.js for a renderer
  "rendererReady",  // rendererLaunch answered: the frame's hello arrived and the pipe is bound
  "init",           // `Init` answered rc 0
  "rootInstalled",  // the `Load` pump answered 0
  "begun",          // `Begin` answered: the frontier is seeded
  "firstStep",      // the first `Step` was dispatched
]);

/* One clock, the system's, so the offsets and `t0EpochMs` below are on the clock a driver on the same machine
   reads. */
const _bootSpans = new Map();   // documentId -> { t0: epoch ms of the first mark, at: Map(phase -> ms since t0) }

function bootSpanMark(documentId, phase) {
  DCHECK(typeof documentId === "string" && documentId !== "",
         "a boot phase was marked for no document name — every caller holds the browser's documentId or the " +
         "name engineCreate minted");
  DCHECK(BOOT_SPAN_PHASES.includes(phase), "a boot phase `" + phase + "` is not in BOOT_SPAN_PHASES");
  const now = Date.now();
  let s = _bootSpans.get(documentId);
  if (s === undefined) {
    s = { t0: now, at: new Map() };
    _bootSpans.set(documentId, s);
  }
  if (!s.at.has(phase)) s.at.set(phase, now - s.t0);
}

/* The readout for one document: null when no phase was marked for it, otherwise every phase's offset in
   milliseconds from the first mark (null for a phase not reached) and `t0EpochMs`, the first mark itself, so
   a driver can place the span against its own navigation. */
function bootSpanOf(documentId) {
  const s = _bootSpans.get(documentId);
  if (s === undefined) return null;
  const out = { t0EpochMs: s.t0 };
  for (const p of BOOT_SPAN_PHASES) out[p] = s.at.has(p) ? s.at.get(p) : null;
  return out;
}
